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
ClipList *clips;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	clips = new ClipList;
	ui->clip_list->setModel(clips);

	// TODO: Make these into buttons.
	// TODO: These are too big for lambdas.
	QShortcut *cue_in = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(cue_in, &QShortcut::activated, []{
		if (!clips->empty() && clips->back()->pts_out < 0) {
			clips->back()->pts_in = current_pts;
			return;
		}
		Clip clip;
		clip.pts_in = current_pts;
		clips->add_clip(clip);
	});

	QShortcut *cue_out = new QShortcut(QKeySequence(Qt::Key_S), this);
	connect(cue_out, &QShortcut::activated, []{
		if (!clips->empty()) {
			clips->back()->pts_out = current_pts;
			// TODO: select the row in the clip list?
		}
	});

	QShortcut *preview_shortcut = new QShortcut(QKeySequence(Qt::Key_W), this);
	connect(preview_shortcut, &QShortcut::activated, this, &MainWindow::preview_clicked);
}

void MainWindow::preview_clicked()
{
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		play_clip(*clips->back(), 0);
		return;
	}

	QModelIndex index = selected->currentIndex();
	if (index.column() >= ClipList::Column::CAMERA_1 &&
	    index.column() <= ClipList::Column::CAMERA_4) {
		unsigned stream_idx = index.column() - ClipList::Column::CAMERA_1;
		play_clip(*clips->clip(index.row()), stream_idx);
	}
}
