#include "mainwindow.h"
#include "window.h"
#include <thread>

#include "context.h"
#include "mixer.h"

#include "ui_mainwindow.cpp"

using std::thread;

MainWindow::MainWindow()
{
	Ui::MainWindow *ui = new Ui::MainWindow;
	ui->setupUi(this);
	connect(ui->cut_btn, SIGNAL(clicked()), this, SLOT(cut()));

	ui->me_live->set_output(Mixer::OUTPUT_LIVE);
	ui->me_preview->set_output(Mixer::OUTPUT_PREVIEW);
}

void MainWindow::cut()
{
	static int i = 0;
	global_mixer->cut(Mixer::Source((++i) % 3));
}
