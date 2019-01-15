#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "clip_list.h"
#include "db.h"
#include "midi_mapper.h"
#include "state.pb.h"

#include <QLabel>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <deque>
#include <memory>
#include <mutex>
#include <stdbool.h>
#include <string>
#include <sys/types.h>
#include <utility>

namespace Ui {
class MainWindow;
}  // namespace Ui

struct FrameOnDisk;
class JPEGFrameView;
class Player;
class QPushButton;
class QTableView;

class MainWindow : public QMainWindow, public ControllerReceiver {
	Q_OBJECT

public:
	MainWindow();
	~MainWindow();

	// HTTP callback. TODO: Does perhaps not belong to MainWindow?
	std::pair<std::string, std::string> get_queue_status() const;

	void display_frame(unsigned stream_idx, const FrameOnDisk &frame);

	// ControllerReceiver interface.
	void preview() override;
	void queue() override;
	void play() override;
	void jog(int delta) override;
	void switch_camera(unsigned camera_idx) override;
	void set_master_speed(float speed) override;
	void cue_in() override;
	void cue_out() override;

	// Raw receivers are not used.
	void controller_changed(unsigned controller) override {}
	void note_on(unsigned note) override {}

private:
	Ui::MainWindow *ui;

	QLabel *disk_free_label;
	std::unique_ptr<Player> preview_player, live_player;
	DB db;
	unsigned num_cameras;

	// State when doing a scrub operation on a timestamp with the mouse.
	bool scrubbing = false;
	int scrub_x_origin;  // In pixels on the viewport.
	int64_t scrub_pts_origin;

	// Which element (e.g. pts_in on clip 4) we are scrubbing.
	enum ScrubType { SCRUBBING_CLIP_LIST,
	                 SCRUBBING_PLAYLIST } scrub_type;
	int scrub_row;
	int scrub_column;

	// Used to keep track of small mouse wheel motions on the camera index in the playlist.
	int last_mousewheel_camera_row = -1;
	int leftover_angle_degrees = 0;

	// Normally, jog is only allowed if in the focus (well, selection) is
	// on the in or out pts columns. However, changing camera (even when
	// using a MIDI button) on the clip list changes the highlight,
	// and we'd like to keep on jogging. Thus, as a special case, if you
	// change to a camera column on the clip list (and don't change which
	// clip you're looking at), the last column you were at will be stored here.
	// If you then try to jog, we'll fetch the value from here and highlight it.
	// Doing pretty much anything else is going to reset it back to -1, though.
	int hidden_jog_column = -1;

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

	struct FrameAndDisplay {
		QFrame *frame;
		JPEGFrameView *display;
		QPushButton *preview_btn;
	};
	std::vector<FrameAndDisplay> displays;

	// Used to get tally information, if a tally URL is set.
	QNetworkAccessManager http;
	QNetworkReply *http_reply = nullptr;

	MIDIMapper midi_mapper;

	void change_num_cameras();
	void cue_in_clicked();
	void cue_out_clicked();
	void queue_clicked();
	void preview_clicked();
	void preview_angle_clicked(unsigned stream_idx);
	void play_clicked();
	void stop_clicked();
	void speed_slider_changed(int percent);
	void live_player_done();
	void live_player_clip_progress(const std::map<uint64_t, double> &progress, double time_remaining);
	void set_output_status(const std::string &status);
	void playlist_duplicate();
	void playlist_remove();
	void playlist_move(int delta);

	enum JogDestination { JOG_CLIP_LIST, JOG_PLAYLIST };
	void jog_internal(JogDestination jog_destination, int column, int row, int stream_idx, int pts_delta);

	void defer_timer_expired();
	void content_changed();  // In clip_list or play_list.
	void state_changed(const StateProto &state);  // Called post-filtering.
	void save_settings();

	enum Rounding { FIRST_AT_OR_AFTER,
	                LAST_BEFORE };
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
	void padding_toggled(double seconds, bool checked);

	void highlight_camera_input(int stream_idx);
	void enable_or_disable_preview_button();
	void enable_or_disable_queue_button();

	template<class Model>
	void replace_model(QTableView *view, Model **model, Model *new_model);

	void start_tally();
	void tally_received();

private slots:
	void relayout();
};

extern MainWindow *global_mainwindow;

#endif
