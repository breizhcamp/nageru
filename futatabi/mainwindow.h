#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "clip_list.h"
#include "db.h"
#include "state.pb.h"

#include <deque>
#include <memory>
#include <mutex>
#include <QLabel>
#include <QMainWindow>
#include <stdbool.h>
#include <sys/types.h>
#include <string>
#include <utility>

namespace Ui {
class MainWindow;
}  // namespace Ui

class Player;
class QTableView;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow();
	~MainWindow();

	// HTTP callback. TODO: Does perhaps not belong to MainWindow?
	std::pair<std::string, std::string> get_queue_status() const;

//private:
	Ui::MainWindow *ui;

private:
	QLabel *disk_free_label;
	std::unique_ptr<Player> preview_player, live_player;
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

	// NOTE: The undo stack always has the current state on top.
	std::deque<StateProto> undo_stack, redo_stack;

	// Before a change that should be deferred (see above), currently_deferring_model_changes
	// must be set to true, and current_change_id must be given contents describing what's
	// changed to avoid accidental grouping.
	bool currently_deferring_model_changes = false;
	std::string current_change_id;

	mutable std::mutex queue_status_mu;
	std::string queue_status;  // Under queue_status_mu.

	void cue_in_clicked();
	void cue_out_clicked();
	void queue_clicked();
	void preview_clicked();
	void preview_angle_clicked(unsigned stream_idx);
	void play_clicked();
	void stop_clicked();
	void live_player_clip_done();
	std::pair<Clip, size_t> live_player_get_next_clip();
	void live_player_clip_progress(const std::map<size_t, double> &progress);
	void set_output_status(const std::string &status);
	void playlist_duplicate();
	void playlist_remove();
	void playlist_move(int delta);

	void defer_timer_expired();
	void content_changed();  // In clip_list or play_list.
	void state_changed(const StateProto &state);  // Called post-filtering.
	void save_settings();

	enum Rounding { FIRST_AT_OR_AFTER, LAST_BEFORE };
	void preview_single_frame(int64_t pts, unsigned stream_idx, Rounding rounding);

	// Also covers when the playlist itself changes.
	void playlist_selection_changed();

	void clip_list_selection_changed(const QModelIndex &current, const QModelIndex &previous);

	void resizeEvent(QResizeEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

	void report_disk_space(off_t free_bytes, double estimated_seconds_left);
	void exit_triggered();
	void export_cliplist_clip_multitrack_triggered();
	void export_playlist_clip_interpolated_triggered();
	void manual_triggered();
	void about_triggered();
	void undo_triggered();
	void redo_triggered();
	void quality_toggled(int quality, bool checked);

	void highlight_camera_input(int stream_idx);

	template <class Model>
	void replace_model(QTableView *view, Model **model, Model *new_model);

private slots:
	void relayout();
};

extern MainWindow *global_mainwindow;

#endif
