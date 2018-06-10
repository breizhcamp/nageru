#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <stdbool.h>
#include <sys/types.h>
#include <QMainWindow>

namespace Ui {
class MainWindow;
}  // namespace Ui

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();

//private:
	Ui::MainWindow *ui;

private:
	void preview_clicked();
};

extern MainWindow *global_mainwindow;

#endif

