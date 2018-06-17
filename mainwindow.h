#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <stdbool.h>
#include <sys/types.h>
#include <QMainWindow>

#include "clip_list.h"

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

	// State when doing a scrub operation on a timestamp with the mouse.
	bool scrubbing = false;
	int scrub_x_origin;  // In pixels on the viewport.
	int64_t scrub_pts_origin;

	// Which element (e.g. pts_in on clip 4) we are scrubbing.
	enum ScrubType { SCRUBBING_CLIP_LIST, SCRUBBING_PLAYLIST } scrub_type;
	int scrub_row;
	int scrub_column;

	void queue_clicked();
	void preview_clicked();
	void play_clicked();
	void live_player_clip_done();

	enum Rounding { FIRST_AT_OR_AFTER, LAST_BEFORE };
	void preview_single_frame(int64_t pts, unsigned stream_idx, Rounding rounding);

	void resizeEvent(QResizeEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void relayout();
};

extern MainWindow *global_mainwindow;

#endif

