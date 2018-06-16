#include "mainwindow.h"

#include "clip_list.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QShortcut>

using namespace std;

MainWindow *global_mainwindow = nullptr;
extern int64_t current_pts;
ClipList *cliplist_clips;
PlayList *playlist_clips;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	cliplist_clips = new ClipList();
	ui->clip_list->setModel(cliplist_clips);

	playlist_clips = new PlayList();
	ui->playlist->setModel(playlist_clips);

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
