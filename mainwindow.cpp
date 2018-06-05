#include "mainwindow.h"

#include "ui_mainwindow.h"

MainWindow *global_mainwindow = nullptr;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);
}
