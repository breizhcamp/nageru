#include "mainwindow.h"

#include "clip_list.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

#include <QShortcut>

using namespace std;

MainWindow *global_mainwindow = nullptr;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	ClipList *clips = new ClipList;
	ui->clip_list->setModel(clips);

	QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_Q), this);
	connect(shortcut, &QShortcut::activated, [clips]{
		clips->add_clip(12345);
	});
}
