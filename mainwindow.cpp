#include "mainwindow.h"

#include "clip_list.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QMouseEvent>
#include <QWheelEvent>
#include <QShortcut>

using namespace std;

MainWindow *global_mainwindow = nullptr;
ClipList *cliplist_clips;
PlayList *playlist_clips;

extern int64_t current_pts;
extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	cliplist_clips = new ClipList();
	ui->clip_list->setModel(cliplist_clips);

	playlist_clips = new PlayList();
	ui->playlist->setModel(playlist_clips);

	// For scrubbing in the pts columns.
	ui->clip_list->viewport()->installEventFilter(this);
	ui->playlist->viewport()->installEventFilter(this);

	// TODO: These are too big for lambdas.
	QShortcut *cue_in = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(cue_in, &QShortcut::activated, ui->cue_in_btn, &QPushButton::click);
	connect(ui->cue_in_btn, &QPushButton::clicked, []{
		if (!cliplist_clips->empty() && cliplist_clips->back()->pts_out < 0) {
			cliplist_clips->back()->pts_in = current_pts;
			return;
		}
		Clip clip;
		clip.pts_in = current_pts;
		cliplist_clips->add_clip(clip);
	});

	QShortcut *cue_out = new QShortcut(QKeySequence(Qt::Key_S), this);
	connect(cue_out, &QShortcut::activated, ui->cue_out_btn, &QPushButton::click);
	connect(ui->cue_out_btn, &QPushButton::clicked, []{
		if (!cliplist_clips->empty()) {
			cliplist_clips->back()->pts_out = current_pts;
			// TODO: select the row in the clip list?
		}
	});

	QShortcut *queue = new QShortcut(QKeySequence(Qt::Key_Q), this);
	connect(queue, &QShortcut::activated, ui->queue_btn, &QPushButton::click);
	connect(ui->queue_btn, &QPushButton::clicked, this, &MainWindow::queue_clicked);

	QShortcut *preview = new QShortcut(QKeySequence(Qt::Key_W), this);
	connect(preview, &QShortcut::activated, ui->preview_btn, &QPushButton::click);
	connect(ui->preview_btn, &QPushButton::clicked, this, &MainWindow::preview_clicked);

	QShortcut *play = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(play, &QShortcut::activated, ui->play_btn, &QPushButton::click);
	connect(ui->play_btn, &QPushButton::clicked, this, &MainWindow::play_clicked);

	preview_player = new Player(ui->preview_display);
	live_player = new Player(ui->live_display);
	live_player->set_done_callback([this]{
		post_to_main_thread([this]{
			live_player_clip_done();
		});
	});
}

void MainWindow::queue_clicked()
{
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		Clip clip = *cliplist_clips->back();
		clip.stream_idx = 0;
		playlist_clips->add_clip(clip);
		return;
	}

	QModelIndex index = selected->currentIndex();
	if (index.column() >= int(ClipList::Column::CAMERA_1) &&
	    index.column() <= int(ClipList::Column::CAMERA_4)) {
		Clip clip = *cliplist_clips->clip(index.row());
		clip.stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
		playlist_clips->add_clip(clip);
	}
}

void MainWindow::preview_clicked()
{
	if (cliplist_clips->empty()) return;

	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		preview_player->play_clip(*cliplist_clips->back(), 0);
		return;
	}

	QModelIndex index = selected->currentIndex();
	if (index.column() >= int(ClipList::Column::CAMERA_1) &&
	    index.column() <= int(ClipList::Column::CAMERA_4)) {
		unsigned stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
		preview_player->play_clip(*cliplist_clips->clip(index.row()), stream_idx);
	}
}

void MainWindow::play_clicked()
{
	if (playlist_clips->empty()) return;

	QItemSelectionModel *selected = ui->playlist->selectionModel();
	int row;
	if (!selected->hasSelection()) {
		row = 0;
	} else {
		row = selected->selectedRows(0)[0].row();
	}

	const Clip &clip = *playlist_clips->clip(row);
	live_player->play_clip(clip, clip.stream_idx);
	playlist_clips->set_currently_playing(row);
}

void MainWindow::live_player_clip_done()
{
	int row = playlist_clips->get_currently_playing();
	if (row != -1 && row < int(playlist_clips->size()) - 1) {
		++row;
		const Clip &clip = *playlist_clips->clip(row);
		live_player->play_clip(clip, clip.stream_idx);
		playlist_clips->set_currently_playing(row);
	} else {
		playlist_clips->set_currently_playing(-1);
	}
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);

	// Ask for a relayout, but only after the event loop is done doing relayout
	// on everything else.
	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

void MainWindow::relayout()
{
	ui->live_display->setMinimumHeight(ui->live_display->width() * 9 / 16);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	constexpr int dead_zone_pixels = 3;  // To avoid that simple clicks get misinterpreted.
	constexpr int scrub_sensitivity = 100;  // pts units per pixel.
	constexpr int wheel_sensitivity = 100;  // pts units per degree.

	unsigned stream_idx = ui->preview_display->get_stream_idx();

	if (event->type() == QEvent::MouseButtonPress) {
		QMouseEvent *mouse = (QMouseEvent *)event;

		QTableView *destination;
		ScrubType type;

		if (watched == ui->clip_list->viewport()) {
			destination = ui->clip_list;
			type = SCRUBBING_CLIP_LIST;
		} else if (watched == ui->playlist->viewport()) {
			destination = ui->playlist;
			type = SCRUBBING_PLAYLIST;
		} else {
			return false;
		}
		int column = destination->columnAt(mouse->x());
		int row = destination->rowAt(mouse->y());
		if (column == -1 || row == -1) return false;

		if (type == SCRUBBING_CLIP_LIST) {
			if (ClipList::Column(column) == ClipList::Column::IN) {
				scrub_pts_origin = cliplist_clips->clip(row)->pts_in;
				preview_single_frame(scrub_pts_origin, stream_idx, FIRST_AT_OR_AFTER);
			} else if (ClipList::Column(column) == ClipList::Column::OUT) {
				scrub_pts_origin = cliplist_clips->clip(row)->pts_out;
				preview_single_frame(scrub_pts_origin, stream_idx, LAST_BEFORE);
			} else {
				return false;
			}
		} else {
			if (PlayList::Column(column) == PlayList::Column::IN) {
				scrub_pts_origin = playlist_clips->clip(row)->pts_in;
				preview_single_frame(scrub_pts_origin, stream_idx, FIRST_AT_OR_AFTER);
			} else if (PlayList::Column(column) == PlayList::Column::OUT) {
				scrub_pts_origin = playlist_clips->clip(row)->pts_out;
				preview_single_frame(scrub_pts_origin, stream_idx, LAST_BEFORE);
			} else {
				return false;
			}
		}

		scrubbing = true;
		scrub_row = row;
		scrub_column = column;
		scrub_x_origin = mouse->x();
		scrub_type = type;
	} else if (event->type() == QEvent::MouseMove) {
		if (scrubbing) {
			QMouseEvent *mouse = (QMouseEvent *)event;
			int offset = mouse->x() - scrub_x_origin;
			int adjusted_offset;
			if (offset >= dead_zone_pixels) {
				adjusted_offset = offset - dead_zone_pixels;
			} else if (offset < -dead_zone_pixels) {
				adjusted_offset = offset + dead_zone_pixels;
			} else {
				adjusted_offset = 0;
			}

			int64_t pts = scrub_pts_origin + adjusted_offset * scrub_sensitivity;

			if (scrub_type == SCRUBBING_CLIP_LIST) {
				if (scrub_column == int(ClipList::Column::IN)) {
					pts = std::max<int64_t>(pts, 0);
					pts = std::min(pts, cliplist_clips->clip(scrub_row)->pts_out);
					cliplist_clips->clip(scrub_row)->pts_in = pts;
					preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
				} else {
					pts = std::max(pts, cliplist_clips->clip(scrub_row)->pts_in);
					pts = std::min(pts, current_pts);
					cliplist_clips->clip(scrub_row)->pts_out = pts;
					preview_single_frame(pts, stream_idx, LAST_BEFORE);
				}
			} else {
				if (scrub_column == int(PlayList::Column::IN)) {
					pts = std::max<int64_t>(pts, 0);
					pts = std::min(pts, playlist_clips->clip(scrub_row)->pts_out);
					playlist_clips->clip(scrub_row)->pts_in = pts;
					preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
				} else {
					pts = std::max(pts, playlist_clips->clip(scrub_row)->pts_in);
					pts = std::min(pts, current_pts);
					playlist_clips->clip(scrub_row)->pts_out = pts;
					preview_single_frame(pts, stream_idx, LAST_BEFORE);
				}
			}

			return true;  // Don't use this mouse movement for selecting things.
		}
	} else if (event->type() == QEvent::Wheel) {
		QWheelEvent *wheel = (QWheelEvent *)event;

		QTableView *destination;
		int in_column, out_column;
		if (watched == ui->clip_list->viewport()) {
			destination = ui->clip_list;
			in_column = int(ClipList::Column::IN);
			out_column = int(ClipList::Column::OUT);
		} else if (watched == ui->playlist->viewport()) {
			destination = ui->playlist;
			in_column = int(PlayList::Column::IN);
			out_column = int(PlayList::Column::OUT);
		} else {
			return false;
		}
		int column = destination->columnAt(wheel->x());
		int row = destination->rowAt(wheel->y());
		if (column == -1 || row == -1) return false;

		ClipProxy clip = (watched == ui->clip_list->viewport()) ?
			cliplist_clips->clip(row) : playlist_clips->clip(row);

		if (column == in_column) {
			int64_t pts = clip->pts_in + wheel->angleDelta().y() * wheel_sensitivity;
			pts = std::max<int64_t>(pts, 0);
			pts = std::min(pts, clip->pts_out);
			clip->pts_in = pts;
			preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
		} else if (column == out_column) {
			int64_t pts = clip->pts_out + wheel->angleDelta().y() * wheel_sensitivity;
			pts = std::max(pts, clip->pts_in);
			pts = std::min(pts, current_pts);
			clip->pts_out = pts;
			preview_single_frame(pts, stream_idx, LAST_BEFORE);
		}
	} else if (event->type() == QEvent::MouseButtonRelease) {
		scrubbing = false;
	}
	return false;
}

void MainWindow::preview_single_frame(int64_t pts, unsigned stream_idx, MainWindow::Rounding rounding)
{
	if (rounding == LAST_BEFORE) {
		lock_guard<mutex> lock(frame_mu);
		if (frames[stream_idx].empty()) return;
		auto it = lower_bound(frames[stream_idx].begin(), frames[stream_idx].end(), pts);
		if (it != frames[stream_idx].end()) {
			pts = *it;
		}
	} else {
		assert(rounding == FIRST_AT_OR_AFTER);
		lock_guard<mutex> lock(frame_mu);
		if (frames[stream_idx].empty()) return;
		auto it = upper_bound(frames[stream_idx].begin(), frames[stream_idx].end(), pts - 1);
		if (it != frames[stream_idx].end()) {
			pts = *it;
		}
	}

	Clip fake_clip;
	fake_clip.pts_in = pts;
	fake_clip.pts_out = pts + 1;
	preview_player->play_clip(fake_clip, stream_idx);
}
