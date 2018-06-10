#include "mainwindow.h"

#include "clip_list.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QShortcut>

using namespace std;

MainWindow *global_mainwindow = nullptr;
extern int64_t current_pts;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	ClipList *clips = new ClipList;
	ui->clip_list->setModel(clips);

	// TODO: Make these into buttons.
	// TODO: These are too big for lambdas.
	QShortcut *cue_in = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(cue_in, &QShortcut::activated, [clips]{
		if (!clips->empty() && clips->back()->pts_out < 0) {
			clips->back()->pts_in = current_pts;
			return;
		}
		Clip clip;
		clip.pts_in = current_pts;
		clips->add_clip(clip);
	});

	QShortcut *cue_out = new QShortcut(QKeySequence(Qt::Key_S), this);
	connect(cue_out, &QShortcut::activated, [clips]{
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
	printf("preview\n");
}
