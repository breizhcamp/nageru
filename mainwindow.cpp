#include "mainwindow.h"

#include "clip_list.h"
#include "player.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QShortcut>

using namespace std;

MainWindow *global_mainwindow = nullptr;
extern int64_t current_pts;
ClipList *cliplist_clips, *playlist_clips;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	cliplist_clips = new ClipList(ClipList::ListDisplay::CLIP_LIST);
	ui->clip_list->setModel(cliplist_clips);

	playlist_clips = new ClipList(ClipList::ListDisplay::PLAY_LIST);
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
	if (index.column() >= int(ClipList::ClipListColumn::CAMERA_1) &&
	    index.column() <= int(ClipList::ClipListColumn::CAMERA_4)) {
		Clip clip = *cliplist_clips->clip(index.row());
		clip.stream_idx = index.column() - int(ClipList::ClipListColumn::CAMERA_1);
		playlist_clips->add_clip(clip);
	}
}

void MainWindow::preview_clicked()
{
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		preview_player->play_clip(*cliplist_clips->back(), 0);
		return;
	}

	QModelIndex index = selected->currentIndex();
	if (index.column() >= int(ClipList::ClipListColumn::CAMERA_1) &&
	    index.column() <= int(ClipList::ClipListColumn::CAMERA_4)) {
		unsigned stream_idx = index.column() - int(ClipList::ClipListColumn::CAMERA_1);
		preview_player->play_clip(*cliplist_clips->clip(index.row()), stream_idx);
	}
}

void MainWindow::play_clicked()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		ui->playlist->selectRow(0);
		return;
	}
}
