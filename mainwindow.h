#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <stdbool.h>
#include <sys/types.h>
#include <QMainWindow>

namespace Ui {
class MainWindow;
}  // namespace Ui

class Player;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();

//private:
	Ui::MainWindow *ui;

private:
	Player *preview_player, *live_player;

	void queue_clicked();
	void preview_clicked();
	void play_clicked();
	void live_player_clip_done();

	void resizeEvent(QResizeEvent *event) override;

private slots:
	void relayout();
};

extern MainWindow *global_mainwindow;

#endif

