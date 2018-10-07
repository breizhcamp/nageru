#include "mainwindow.h"

#include "clip_list.h"
#include "disk_space_estimator.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "timebase.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QMouseEvent>
#include <QWheelEvent>
#include <QShortcut>
#include <QTimer>

#include <sqlite3.h>

using namespace std;
using namespace std::placeholders;

MainWindow *global_mainwindow = nullptr;
ClipList *cliplist_clips;
PlayList *playlist_clips;

extern int64_t current_pts;
extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];

MainWindow::MainWindow()
	: ui(new Ui::MainWindow),
	  db("futatabi.db")
{
	global_mainwindow = this;
	ui->setupUi(this);

	// The menus.
	connect(ui->exit_action, &QAction::triggered, this, &MainWindow::exit_triggered);

	global_disk_space_estimator = new DiskSpaceEstimator(bind(&MainWindow::report_disk_space, this, _1, _2));
	disk_free_label = new QLabel(this);
	disk_free_label->setStyleSheet("QLabel {padding-right: 5px;}");
	ui->menuBar->setCornerWidget(disk_free_label);

	StateProto state = db.get_state();

	cliplist_clips = new ClipList(state.clip_list());
	ui->clip_list->setModel(cliplist_clips);
	connect(cliplist_clips, &ClipList::any_content_changed, this, &MainWindow::content_changed);

	playlist_clips = new PlayList(state.play_list());
	ui->playlist->setModel(playlist_clips);
	connect(playlist_clips, &PlayList::any_content_changed, this, &MainWindow::content_changed);

	// For scrubbing in the pts columns.
	ui->clip_list->viewport()->installEventFilter(this);
	ui->playlist->viewport()->installEventFilter(this);

	QShortcut *cue_in = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(cue_in, &QShortcut::activated, ui->cue_in_btn, &QPushButton::click);
	connect(ui->cue_in_btn, &QPushButton::clicked, this, &MainWindow::cue_in_clicked);

	QShortcut *cue_out = new QShortcut(QKeySequence(Qt::Key_S), this);
	connect(cue_out, &QShortcut::activated, ui->cue_out_btn, &QPushButton::click);
	connect(ui->cue_out_btn, &QPushButton::clicked, this, &MainWindow::cue_out_clicked);

	QShortcut *queue = new QShortcut(QKeySequence(Qt::Key_Q), this);
	connect(queue, &QShortcut::activated, ui->queue_btn, &QPushButton::click);
	connect(ui->queue_btn, &QPushButton::clicked, this, &MainWindow::queue_clicked);

	QShortcut *preview = new QShortcut(QKeySequence(Qt::Key_W), this);
	connect(preview, &QShortcut::activated, ui->preview_btn, &QPushButton::click);
	connect(ui->preview_btn, &QPushButton::clicked, this, &MainWindow::preview_clicked);

	QShortcut *play = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(play, &QShortcut::activated, ui->play_btn, &QPushButton::click);
	connect(ui->play_btn, &QPushButton::clicked, this, &MainWindow::play_clicked);

	QShortcut *preview_1 = new QShortcut(QKeySequence(Qt::Key_1), this);
	connect(preview_1, &QShortcut::activated, ui->preview_1_btn, &QPushButton::click);
	connect(ui->input1_display, &JPEGFrameView::clicked, ui->preview_1_btn, &QPushButton::click);
	connect(ui->preview_1_btn, &QPushButton::clicked, [this]{ preview_angle_clicked(0); });
	ui->input1_display->set_overlay("1");

	QShortcut *preview_2 = new QShortcut(QKeySequence(Qt::Key_2), this);
	connect(preview_2, &QShortcut::activated, ui->preview_2_btn, &QPushButton::click);
	connect(ui->input2_display, &JPEGFrameView::clicked, ui->preview_2_btn, &QPushButton::click);
	connect(ui->preview_2_btn, &QPushButton::clicked, [this]{ preview_angle_clicked(1); });
	ui->input2_display->set_overlay("2");

	QShortcut *preview_3 = new QShortcut(QKeySequence(Qt::Key_3), this);
	connect(preview_3, &QShortcut::activated, ui->preview_3_btn, &QPushButton::click);
	connect(ui->input3_display, &JPEGFrameView::clicked, ui->preview_3_btn, &QPushButton::click);
	connect(ui->preview_3_btn, &QPushButton::clicked, [this]{ preview_angle_clicked(2); });
	ui->input3_display->set_overlay("3");

	QShortcut *preview_4 = new QShortcut(QKeySequence(Qt::Key_4), this);
	connect(preview_4, &QShortcut::activated, ui->preview_4_btn, &QPushButton::click);
	connect(ui->input4_display, &JPEGFrameView::clicked, ui->preview_4_btn, &QPushButton::click);
	connect(ui->preview_4_btn, &QPushButton::clicked, [this]{ preview_angle_clicked(3); });
	ui->input4_display->set_overlay("4");

	connect(ui->playlist_duplicate_btn, &QPushButton::clicked, this, &MainWindow::playlist_duplicate);

	connect(ui->playlist_remove_btn, &QPushButton::clicked, this, &MainWindow::playlist_remove);
	QShortcut *delete_key = new QShortcut(QKeySequence(Qt::Key_Delete), ui->playlist);
	connect(delete_key, &QShortcut::activated, [this] {
		if (ui->playlist->hasFocus()) {
			playlist_remove();
		}
	});

	// TODO: support drag-and-drop.
	connect(ui->playlist_move_up_btn, &QPushButton::clicked, [this]{ playlist_move(-1); });
	connect(ui->playlist_move_down_btn, &QPushButton::clicked, [this]{ playlist_move(1); });

	connect(ui->playlist->selectionModel(), &QItemSelectionModel::selectionChanged,
		this, &MainWindow::playlist_selection_changed);
	playlist_selection_changed();  // First time set-up.

	preview_player = new Player(ui->preview_display, /*also_output_to_stream=*/false);
	live_player = new Player(ui->live_display, /*also_output_to_stream=*/true);
	live_player->set_done_callback([this]{
		post_to_main_thread([this]{
			live_player_clip_done();
		});
	});
	live_player->set_next_clip_callback(bind(&MainWindow::live_player_get_next_clip, this));
	live_player->set_progress_callback([this](double played_this_clip, double total_length) {
		post_to_main_thread([this, played_this_clip, total_length] {
			live_player_clip_progress(played_this_clip, total_length);
		});
	});

	defer_timeout = new QTimer(this);
	defer_timeout->setSingleShot(true);
	connect(defer_timeout, &QTimer::timeout, this, &MainWindow::defer_timer_expired);
}

void MainWindow::cue_in_clicked()
{
	if (!cliplist_clips->empty() && cliplist_clips->back()->pts_out < 0) {
		cliplist_clips->mutable_back()->pts_in = current_pts;
		return;
	}
	Clip clip;
	clip.pts_in = current_pts;
	cliplist_clips->add_clip(clip);
	playlist_selection_changed();
}

void MainWindow::cue_out_clicked()
{
	if (!cliplist_clips->empty()) {
		cliplist_clips->mutable_back()->pts_out = current_pts;
		// TODO: select the row in the clip list?
	}
}

void MainWindow::queue_clicked()
{
	if (cliplist_clips->empty()) {
		return;
	}

	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		Clip clip = *cliplist_clips->back();
		clip.stream_idx = 0;
		if (clip.pts_out != -1) {
			playlist_clips->add_clip(clip);
			playlist_selection_changed();
		}
		return;
	}

	QModelIndex index = selected->currentIndex();
	Clip clip = *cliplist_clips->clip(index.row());
	if (index.column() >= int(ClipList::Column::CAMERA_1) &&
	    index.column() <= int(ClipList::Column::CAMERA_4)) {
		clip.stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
	} else {
		clip.stream_idx = ui->preview_display->get_stream_idx();
	}

	if (clip.pts_out != -1) {
		playlist_clips->add_clip(clip);
		playlist_selection_changed();
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
	unsigned stream_idx;
	if (index.column() >= int(ClipList::Column::CAMERA_1) &&
	    index.column() <= int(ClipList::Column::CAMERA_4)) {
		stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
	} else {
		stream_idx = ui->preview_display->get_stream_idx();
	}
	preview_player->play_clip(*cliplist_clips->clip(index.row()), stream_idx);
}

void MainWindow::preview_angle_clicked(unsigned stream_idx)
{
	preview_player->override_angle(stream_idx);

	// Change the selection if we were previewing a clip from the clip list.
	// (The only other thing we could be showing is a pts scrub, and if so,
	// that would be selected.)
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (selected->hasSelection()) {
		QModelIndex cell = selected->selectedIndexes()[0];
		int column = int(ClipList::Column::CAMERA_1) + stream_idx;
		selected->setCurrentIndex(cell.sibling(cell.row(), column), QItemSelectionModel::ClearAndSelect);
	}
}

void MainWindow::playlist_duplicate()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}
	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	playlist_clips->duplicate_clips(first, last);
	playlist_selection_changed();
}

void MainWindow::playlist_remove()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}
	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	playlist_clips->erase_clips(first, last);

	// TODO: select the next one in the list?

	playlist_selection_changed();
}

void MainWindow::playlist_move(int delta)
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}

	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	if ((delta == -1 && first == 0) ||
	    (delta == 1 && size_t(last) == playlist_clips->size() - 1)) {
		// Should have been grayed out, but OK.
		return;
	}

	playlist_clips->move_clips(first, last, delta);
	playlist_selection_changed();
}

void MainWindow::defer_timer_expired()
{
	state_changed(deferred_state);
}

void MainWindow::content_changed()
{
	if (defer_timeout->isActive() &&
	    (!currently_deferring_model_changes || deferred_change_id != current_change_id)) {
		// There's some deferred event waiting, but this event is unrelated.
		// So it's time to short-circuit that timer and do the work it wanted to do.
		defer_timeout->stop();
		state_changed(deferred_state);
	}
	StateProto state;
	*state.mutable_clip_list() = cliplist_clips->serialize();
	*state.mutable_play_list() = playlist_clips->serialize();
	if (currently_deferring_model_changes) {
		deferred_change_id = current_change_id;
		deferred_state = std::move(state);
		defer_timeout->start(200);
		return;
	}
	state_changed(state);
}

void MainWindow::state_changed(const StateProto &state)
{
	db.store_state(state);
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
	playlist_clips->set_currently_playing(row, 0.0f);
	playlist_selection_changed();
}

void MainWindow::live_player_clip_done()
{
	int row = playlist_clips->get_currently_playing();
	if (row == -1 || row == int(playlist_clips->size()) - 1) {
		ui->live_label->setText("Current output (paused)");
		playlist_clips->set_currently_playing(-1, 0.0f);
	} else {
		playlist_clips->set_currently_playing(row + 1, 0.0f);
	}
}

Clip MainWindow::live_player_get_next_clip()
{
	// FIXME: threading
	int row = playlist_clips->get_currently_playing();
	if (row != -1 && row < int(playlist_clips->size()) - 1) {
		return *playlist_clips->clip(row + 1);
	} else {
		return Clip();
	}
}

void MainWindow::live_player_clip_progress(double played_this_clip, double total_length)
{
	playlist_clips->set_currently_playing(playlist_clips->get_currently_playing(), played_this_clip / total_length);

	double remaining = total_length - played_this_clip;
	for (int row = playlist_clips->get_currently_playing() + 1; row < int(playlist_clips->size()); ++row) {
		const Clip clip = *playlist_clips->clip(row);
		remaining += double(clip.pts_out - clip.pts_in) / TIMEBASE / 0.5;   // FIXME: stop hardcoding speed.
	}
	int remaining_ms = lrint(remaining * 1e3);

	int ms = remaining_ms % 1000;
	remaining_ms /= 1000;
	int s = remaining_ms % 60;
	remaining_ms /= 60;
	int m = remaining_ms;

	char buf[256];
	snprintf(buf, sizeof(buf), "Current output (%d:%02d.%03d left)", m, s, ms);
	ui->live_label->setText(buf);
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
	ui->live_display->setMinimumWidth(ui->live_display->height() * 16 / 9);
	ui->preview_display->setMinimumWidth(ui->preview_display->height() * 16 / 9);
}

void set_pts_in(int64_t pts, int64_t current_pts, ClipProxy &clip)
{
	pts = std::max<int64_t>(pts, 0);
	if (clip->pts_out == -1) {
		pts = std::min(pts, current_pts);
	} else {
		pts = std::min(pts, clip->pts_out);
	}
	clip->pts_in = pts;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	constexpr int dead_zone_pixels = 3;  // To avoid that simple clicks get misinterpreted.
	constexpr int scrub_sensitivity = 100;  // pts units per pixel.
	constexpr int wheel_sensitivity = 100;  // pts units per degree.
	constexpr int camera_degrees_per_pixel = 15;  // One click of most mice.

	unsigned stream_idx = ui->preview_display->get_stream_idx();

	if (event->type() != QEvent::Wheel) {
		last_mousewheel_camera_row = -1;
	}

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
			currently_deferring_model_changes = true;
			if (scrub_type == SCRUBBING_CLIP_LIST) {
				ClipProxy clip = cliplist_clips->mutable_clip(scrub_row);
				if (scrub_column == int(ClipList::Column::IN)) {
					current_change_id = "cliplist:in:" + to_string(scrub_row);
					set_pts_in(pts, current_pts, clip);
					preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
				} else {
					current_change_id = "cliplist:out" + to_string(scrub_row);
					pts = std::max(pts, clip->pts_in);
					pts = std::min(pts, current_pts);
					clip->pts_out = pts;
					preview_single_frame(pts, stream_idx, LAST_BEFORE);
				}
			} else {
				ClipProxy clip = playlist_clips->mutable_clip(scrub_row);
				if (scrub_column == int(PlayList::Column::IN)) {
					current_change_id = "playlist:in:" + to_string(scrub_row);
					set_pts_in(pts, current_pts, clip);
					preview_single_frame(pts, clip->stream_idx, FIRST_AT_OR_AFTER);
				} else {
					current_change_id = "playlist:out:" + to_string(scrub_row);
					pts = std::max(pts, clip->pts_in);
					pts = std::min(pts, current_pts);
					clip->pts_out = pts;
					preview_single_frame(pts, clip->stream_idx, LAST_BEFORE);
				}
			}
			currently_deferring_model_changes = false;

			return true;  // Don't use this mouse movement for selecting things.
		}
	} else if (event->type() == QEvent::Wheel) {
		QWheelEvent *wheel = (QWheelEvent *)event;

		QTableView *destination;
		int in_column, out_column, camera_column;
		if (watched == ui->clip_list->viewport()) {
			destination = ui->clip_list;
			in_column = int(ClipList::Column::IN);
			out_column = int(ClipList::Column::OUT);
			camera_column = -1;
			last_mousewheel_camera_row = -1;
		} else if (watched == ui->playlist->viewport()) {
			destination = ui->playlist;
			in_column = int(PlayList::Column::IN);
			out_column = int(PlayList::Column::OUT);
			camera_column = int(PlayList::Column::CAMERA);
		} else {
			last_mousewheel_camera_row = -1;
			return false;
		}
		int column = destination->columnAt(wheel->x());
		int row = destination->rowAt(wheel->y());
		if (column == -1 || row == -1) return false;

		currently_deferring_model_changes = true;
		{
			current_change_id = (watched == ui->clip_list->viewport()) ? "cliplist:" : "playlist:";
			ClipProxy clip = (watched == ui->clip_list->viewport()) ?
				cliplist_clips->mutable_clip(row) : playlist_clips->mutable_clip(row);
			if (watched == ui->playlist->viewport()) {
				stream_idx = clip->stream_idx;
			}

			if (column != camera_column) {
				last_mousewheel_camera_row = -1;
			}
			if (column == in_column) {
				current_change_id += "in:" + to_string(row);
				int64_t pts = clip->pts_in + wheel->angleDelta().y() * wheel_sensitivity;
				set_pts_in(pts, current_pts, clip);
				preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
			} else if (column == out_column) {
				current_change_id += "out:" + to_string(row);
				int64_t pts = clip->pts_out + wheel->angleDelta().y() * wheel_sensitivity;
				pts = std::max(pts, clip->pts_in);
				pts = std::min(pts, current_pts);
				clip->pts_out = pts;
				preview_single_frame(pts, stream_idx, LAST_BEFORE);
			} else if (column == camera_column) {
				current_change_id += "camera:" + to_string(row);
				int angle_degrees = wheel->angleDelta().y();
				if (last_mousewheel_camera_row == row) {
					angle_degrees += leftover_angle_degrees;
				}

				int stream_idx = clip->stream_idx + angle_degrees / camera_degrees_per_pixel;
				stream_idx = std::max(stream_idx, 0);
				stream_idx = std::min(stream_idx, NUM_CAMERAS - 1);
				clip->stream_idx = stream_idx;

				last_mousewheel_camera_row = row;
				leftover_angle_degrees = angle_degrees % camera_degrees_per_pixel;

				// Don't update the live view, that's rarely what the operator wants.
			}
		}
		currently_deferring_model_changes = false;
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

void MainWindow::playlist_selection_changed()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	bool any_selected = selected->hasSelection();
	ui->playlist_duplicate_btn->setEnabled(any_selected);
	ui->playlist_remove_btn->setEnabled(any_selected);
	ui->playlist_move_up_btn->setEnabled(
		any_selected && selected->selectedRows().front().row() > 0);
	ui->playlist_move_down_btn->setEnabled(
		any_selected && selected->selectedRows().back().row() < int(playlist_clips->size()) - 1);
	ui->play_btn->setEnabled(!playlist_clips->empty());
}

void MainWindow::report_disk_space(off_t free_bytes, double estimated_seconds_left)
{
	char time_str[256];
	if (estimated_seconds_left < 60.0) {
		strcpy(time_str, "<font color=\"red\">Less than a minute</font>");
	} else if (estimated_seconds_left < 1800.0) {  // Less than half an hour: Xm Ys (red).
		int s = lrintf(estimated_seconds_left);
		int m = s / 60;
		s %= 60;
		snprintf(time_str, sizeof(time_str), "<font color=\"red\">%dm %ds</font>", m, s);
	} else if (estimated_seconds_left < 3600.0) {  // Less than an hour: Xm.
		int m = lrintf(estimated_seconds_left / 60.0);
		snprintf(time_str, sizeof(time_str), "%dm", m);
	} else if (estimated_seconds_left < 36000.0) {  // Less than ten hours: Xh Ym.
		int m = lrintf(estimated_seconds_left / 60.0);
		int h = m / 60;
		m %= 60;
		snprintf(time_str, sizeof(time_str), "%dh %dm", h, m);
	} else {  // More than ten hours: Xh.
		int h = lrintf(estimated_seconds_left / 3600.0);
		snprintf(time_str, sizeof(time_str), "%dh", h);
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "Disk free: %'.0f MB (approx. %s)", free_bytes / 1048576.0, time_str);

	std::string label = buf;

	post_to_main_thread([this, label]{
			disk_free_label->setText(QString::fromStdString(label));
			ui->menuBar->setCornerWidget(disk_free_label);  // Need to set this again for the sizing to get right.
			});
}

void MainWindow::exit_triggered()
{
	close();
}

