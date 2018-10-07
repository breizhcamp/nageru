#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <stdbool.h>
#include <sys/types.h>

#include <QLabel>
#include <QMainWindow>

#include "clip_list.h"
#include "db.h"
#include "state.pb.h"

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
	QLabel *disk_free_label;
	Player *preview_player, *live_player;
	DB db;

	// State when doing a scrub operation on a timestamp with the mouse.
	bool scrubbing = false;
	int scrub_x_origin;  // In pixels on the viewport.
	int64_t scrub_pts_origin;

	// Which element (e.g. pts_in on clip 4) we are scrubbing.
	enum ScrubType { SCRUBBING_CLIP_LIST, SCRUBBING_PLAYLIST } scrub_type;
	int scrub_row;
	int scrub_column;

	// Used to keep track of small mouse wheel motions on the camera index in the playlist.
	int last_mousewheel_camera_row = -1;
	int leftover_angle_degrees = 0;

	// Some operations, notably scrubbing and scrolling, happen in so large increments
	// that we want to group them instead of saving to disk every single time.
	// If they happen (ie., we get a callback from the model that it's changed) while
	// currently_deferring_model_changes, we fire off this timer. If it manages to elapse
	// before some other event happens, we count the event. (If the other event is of the
	// same kind, we just fire off the timer anew instead of taking any action.)
	QTimer *defer_timeout;
	std::string deferred_change_id;
	StateProto deferred_state;

	// Before a change that should be deferred (see above), currently_deferring_model_changes
	// must be set to true, and current_change_id must be given contents describing what's
	// changed to avoid accidental grouping.
	bool currently_deferring_model_changes = false;
	std::string current_change_id;

	void cue_in_clicked();
	void cue_out_clicked();
	void queue_clicked();
	void preview_clicked();
	void preview_angle_clicked(unsigned stream_idx);
	void play_clicked();
	void live_player_clip_done();
	Clip live_player_get_next_clip();
	void live_player_clip_progress(double played_this_clip, double total_length);
	void playlist_duplicate();
	void playlist_remove();
	void playlist_move(int delta);

	void defer_timer_expired();
	void content_changed();  // In clip_list or play_list.
	void state_changed(const StateProto &state);  // Called post-filtering.

	enum Rounding { FIRST_AT_OR_AFTER, LAST_BEFORE };
	void preview_single_frame(int64_t pts, unsigned stream_idx, Rounding rounding);

	// Also covers when the playlist itself changes.
	void playlist_selection_changed();

	void resizeEvent(QResizeEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

	void report_disk_space(off_t free_bytes, double estimated_seconds_left);
	void exit_triggered();

private slots:
	void relayout();
};

extern MainWindow *global_mainwindow;

#endif

