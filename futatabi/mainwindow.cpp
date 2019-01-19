#include "mainwindow.h"

#include "clip_list.h"
#include "export.h"
#include "flags.h"
#include "frame_on_disk.h"
#include "player.h"
#include "futatabi_midi_mapping.pb.h"
#include "midi_mapping_dialog.h"
#include "shared/aboutdialog.h"
#include "shared/disk_space_estimator.h"
#include "shared/post_to_main_thread.h"
#include "shared/timebase.h"
#include "ui_mainwindow.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QShortcut>
#include <QTimer>
#include <QWheelEvent>
#include <future>
#include <sqlite3.h>
#include <string>
#include <vector>

using namespace std;
using namespace std::placeholders;

MainWindow *global_mainwindow = nullptr;
static ClipList *cliplist_clips;
static PlayList *playlist_clips;

extern int64_t current_pts;

namespace {

void set_pts_in(int64_t pts, int64_t current_pts, ClipProxy &clip)
{
	pts = std::max<int64_t>(pts, 0);
	if (clip->pts_out == -1) {
		pts = std::min(pts, current_pts);
	} else {
		pts = std::min(pts, clip->pts_out);
	}
	clip->pts_in = pts;
}

}  // namespace

MainWindow::MainWindow()
	: ui(new Ui::MainWindow),
	  db(global_flags.working_directory + "/futatabi.db"),
	  midi_mapper(this)
{
	global_mainwindow = this;
	ui->setupUi(this);

	// Load settings from database.
	SettingsProto settings = db.get_settings();
	if (!global_flags.interpolation_quality_set) {
		if (settings.interpolation_quality() != 0) {
			global_flags.interpolation_quality = settings.interpolation_quality() - 1;
		}
	}
	if (!global_flags.cue_point_padding_set) {
		global_flags.cue_point_padding_seconds = settings.cue_point_padding_seconds();  // Default 0 is fine.
	}
	if (global_flags.interpolation_quality == 0) {
		// Allocate something just for simplicity; we won't be using it
		// unless the user changes runtime, in which case 1 is fine.
		flow_initialized_interpolation_quality = 1;
	} else {
		flow_initialized_interpolation_quality = global_flags.interpolation_quality;
	}
	save_settings();

	// The menus.
	connect(ui->midi_mapping_action, &QAction::triggered, this, &MainWindow::midi_mapping_triggered);
	connect(ui->exit_action, &QAction::triggered, this, &MainWindow::exit_triggered);
	connect(ui->export_cliplist_clip_multitrack_action, &QAction::triggered, this, &MainWindow::export_cliplist_clip_multitrack_triggered);
	connect(ui->export_playlist_clip_interpolated_action, &QAction::triggered, this, &MainWindow::export_playlist_clip_interpolated_triggered);
	connect(ui->manual_action, &QAction::triggered, this, &MainWindow::manual_triggered);
	connect(ui->about_action, &QAction::triggered, this, &MainWindow::about_triggered);
	connect(ui->undo_action, &QAction::triggered, this, &MainWindow::undo_triggered);
	connect(ui->redo_action, &QAction::triggered, this, &MainWindow::redo_triggered);
	ui->undo_action->setEnabled(false);
	ui->redo_action->setEnabled(false);

	// The quality group.
	QActionGroup *quality_group = new QActionGroup(ui->interpolation_menu);
	quality_group->addAction(ui->quality_0_action);
	quality_group->addAction(ui->quality_1_action);
	quality_group->addAction(ui->quality_2_action);
	quality_group->addAction(ui->quality_3_action);
	quality_group->addAction(ui->quality_4_action);
	if (global_flags.interpolation_quality == 0) {
		ui->quality_0_action->setChecked(true);
	} else if (global_flags.interpolation_quality == 1) {
		ui->quality_1_action->setChecked(true);
	} else if (global_flags.interpolation_quality == 2) {
		ui->quality_2_action->setChecked(true);
	} else if (global_flags.interpolation_quality == 3) {
		ui->quality_3_action->setChecked(true);
	} else if (global_flags.interpolation_quality == 4) {
		ui->quality_4_action->setChecked(true);
	} else {
		assert(false);
	}
	connect(ui->quality_0_action, &QAction::toggled, bind(&MainWindow::quality_toggled, this, 0, _1));
	connect(ui->quality_1_action, &QAction::toggled, bind(&MainWindow::quality_toggled, this, 1, _1));
	connect(ui->quality_2_action, &QAction::toggled, bind(&MainWindow::quality_toggled, this, 2, _1));
	connect(ui->quality_3_action, &QAction::toggled, bind(&MainWindow::quality_toggled, this, 3, _1));
	connect(ui->quality_4_action, &QAction::toggled, bind(&MainWindow::quality_toggled, this, 4, _1));

	// The cue point padding group.
	QActionGroup *padding_group = new QActionGroup(ui->interpolation_menu);
	padding_group->addAction(ui->padding_0_action);
	padding_group->addAction(ui->padding_1_action);
	padding_group->addAction(ui->padding_2_action);
	padding_group->addAction(ui->padding_5_action);
	if (global_flags.cue_point_padding_seconds <= 1e-3) {
		ui->padding_0_action->setChecked(true);
	} else if (fabs(global_flags.cue_point_padding_seconds - 1.0) < 1e-3) {
		ui->padding_1_action->setChecked(true);
	} else if (fabs(global_flags.cue_point_padding_seconds - 2.0) < 1e-3) {
		ui->padding_2_action->setChecked(true);
	} else if (fabs(global_flags.cue_point_padding_seconds - 5.0) < 1e-3) {
		ui->padding_5_action->setChecked(true);
	} else {
		// Nothing to check, which is fine.
	}
	connect(ui->padding_0_action, &QAction::toggled, bind(&MainWindow::padding_toggled, this, 0.0, _1));
	connect(ui->padding_1_action, &QAction::toggled, bind(&MainWindow::padding_toggled, this, 1.0, _1));
	connect(ui->padding_2_action, &QAction::toggled, bind(&MainWindow::padding_toggled, this, 2.0, _1));
	connect(ui->padding_5_action, &QAction::toggled, bind(&MainWindow::padding_toggled, this, 5.0, _1));

	global_disk_space_estimator = new DiskSpaceEstimator(bind(&MainWindow::report_disk_space, this, _1, _2));
	disk_free_label = new QLabel(this);
	disk_free_label->setStyleSheet("QLabel {padding-right: 5px;}");
	ui->menuBar->setCornerWidget(disk_free_label);

	StateProto state = db.get_state();
	undo_stack.push_back(state);  // The undo stack always has the current state on top.

	cliplist_clips = new ClipList(state.clip_list());
	ui->clip_list->setModel(cliplist_clips);
	connect(cliplist_clips, &ClipList::any_content_changed, this, &MainWindow::content_changed);

	playlist_clips = new PlayList(state.play_list());
	ui->playlist->setModel(playlist_clips);
	connect(playlist_clips, &PlayList::any_content_changed, this, &MainWindow::content_changed);

	// For un-highlighting when we lose focus.
	ui->clip_list->installEventFilter(this);

	// For scrubbing in the pts columns.
	ui->clip_list->viewport()->installEventFilter(this);
	ui->playlist->viewport()->installEventFilter(this);

	QShortcut *cue_in = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(cue_in, &QShortcut::activated, ui->cue_in_btn, &QPushButton::click);
	connect(ui->cue_in_btn, &QPushButton::clicked, this, &MainWindow::cue_in_clicked);

	QShortcut *cue_out = new QShortcut(QKeySequence(Qt::Key_S), this);
	connect(cue_out, &QShortcut::activated, ui->cue_out_btn, &QPushButton::click);
	connect(ui->cue_out_btn, &QPushButton::clicked, this, &MainWindow::cue_out_clicked);

	QShortcut *queue = new QShortcut(QKeySequence(Qt::Key_Q), this);
	connect(queue, &QShortcut::activated, ui->queue_btn, &QPushButton::click);
	connect(ui->queue_btn, &QPushButton::clicked, this, &MainWindow::queue_clicked);

	QShortcut *preview = new QShortcut(QKeySequence(Qt::Key_W), this);
	connect(preview, &QShortcut::activated, ui->preview_btn, &QPushButton::click);
	connect(ui->preview_btn, &QPushButton::clicked, this, &MainWindow::preview_clicked);

	QShortcut *play = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(play, &QShortcut::activated, ui->play_btn, &QPushButton::click);
	connect(ui->play_btn, &QPushButton::clicked, this, &MainWindow::play_clicked);

	connect(ui->stop_btn, &QPushButton::clicked, this, &MainWindow::stop_clicked);
	ui->stop_btn->setEnabled(false);

	connect(ui->speed_slider, &QAbstractSlider::valueChanged, this, &MainWindow::speed_slider_changed);
	connect(ui->speed_lock_btn, &QPushButton::clicked, this, &MainWindow::speed_lock_clicked);

	connect(ui->playlist_duplicate_btn, &QPushButton::clicked, this, &MainWindow::playlist_duplicate);

	connect(ui->playlist_remove_btn, &QPushButton::clicked, this, &MainWindow::playlist_remove);
	QShortcut *delete_key = new QShortcut(QKeySequence(Qt::Key_Delete), ui->playlist);
	connect(delete_key, &QShortcut::activated, [this] {
		if (ui->playlist->hasFocus()) {
			playlist_remove();
		}
	});

	// TODO: support drag-and-drop.
	connect(ui->playlist_move_up_btn, &QPushButton::clicked, [this] { playlist_move(-1); });
	connect(ui->playlist_move_down_btn, &QPushButton::clicked, [this] { playlist_move(1); });

	connect(ui->playlist->selectionModel(), &QItemSelectionModel::selectionChanged,
	        this, &MainWindow::playlist_selection_changed);
	playlist_selection_changed();  // First time set-up.

	preview_player.reset(new Player(ui->preview_display, Player::NO_STREAM_OUTPUT));
	preview_player->set_done_callback([this] {
		post_to_main_thread([this] {
			preview_player_done();
		});
	});

	live_player.reset(new Player(ui->live_display, Player::HTTPD_STREAM_OUTPUT));
	live_player->set_done_callback([this] {
		post_to_main_thread([this] {
			live_player_done();
		});
	});
	live_player->set_progress_callback([this](const map<uint64_t, double> &progress, double time_remaining) {
		post_to_main_thread([this, progress, time_remaining] {
			live_player_clip_progress(progress, time_remaining);
		});
	});
	set_output_status("paused");
	enable_or_disable_queue_button();

	defer_timeout = new QTimer(this);
	defer_timeout->setSingleShot(true);
	connect(defer_timeout, &QTimer::timeout, this, &MainWindow::defer_timer_expired);
	ui->undo_action->setEnabled(true);

	lock_blink_timeout = new QTimer(this);
	lock_blink_timeout->setSingleShot(true);
	connect(lock_blink_timeout, &QTimer::timeout, this, &MainWindow::lock_blink_timer_expired);

	connect(ui->clip_list->selectionModel(), &QItemSelectionModel::currentChanged,
	        this, &MainWindow::clip_list_selection_changed);
	enable_or_disable_queue_button();

	// Find out how many cameras we have in the existing frames;
	// if none, we start with two cameras.
	num_cameras = 2;
	{
		lock_guard<mutex> lock(frame_mu);
		for (size_t stream_idx = 2; stream_idx < MAX_STREAMS; ++stream_idx) {
			if (!frames[stream_idx].empty()) {
				num_cameras = stream_idx + 1;
			}
		}
	}
	change_num_cameras();

	if (!global_flags.tally_url.empty()) {
		start_tally();
	}

	if (!global_flags.midi_mapping_filename.empty()) {
		MIDIMappingProto midi_mapping;
		if (!load_midi_mapping_from_file(global_flags.midi_mapping_filename, &midi_mapping)) {
			fprintf(stderr, "Couldn't load MIDI mapping '%s'; exiting.\n",
				global_flags.midi_mapping_filename.c_str());
			exit(1);
		}
		midi_mapper.set_midi_mapping(midi_mapping);
	}
	midi_mapper.refresh_lights();
	midi_mapper.start_thread();
}

void MainWindow::change_num_cameras()
{
	assert(num_cameras >= displays.size());  // We only add, never remove.

	// Make new display rows.
	unsigned display_rows = (num_cameras + 1) / 2;
	ui->video_displays->setStretch(1, display_rows);
	for (unsigned i = displays.size(); i < num_cameras; ++i) {
		QFrame *frame = new QFrame(this);
		frame->setAutoFillBackground(true);

		QLayout *layout = new QGridLayout(frame);
		frame->setLayout(layout);
		layout->setContentsMargins(3, 3, 3, 3);

		JPEGFrameView *display = new JPEGFrameView(frame);
		display->setAutoFillBackground(true);
		layout->addWidget(display);

		ui->input_displays->addWidget(frame, i / 2, i % 2);
		display->set_overlay(to_string(i + 1));

		QPushButton *preview_btn = new QPushButton(this);
		preview_btn->setMaximumSize(20, 17);
		preview_btn->setText(QString::fromStdString(to_string(i + 1)));
		ui->preview_layout->addWidget(preview_btn);

		displays.emplace_back(FrameAndDisplay{ frame, display, preview_btn });

		connect(display, &JPEGFrameView::clicked, preview_btn, &QPushButton::click);
		QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_1 + i), this);
		connect(shortcut, &QShortcut::activated, preview_btn, &QPushButton::click);

		connect(preview_btn, &QPushButton::clicked, [this, i] { preview_angle_clicked(i); });
	}

	cliplist_clips->change_num_cameras(num_cameras);
	playlist_clips->change_num_cameras(num_cameras);

	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
	// Empty so that we can forward-declare Player in the .h file.
}

void MainWindow::cue_in_clicked()
{
	if (!cliplist_clips->empty() && cliplist_clips->back()->pts_out < 0) {
		cliplist_clips->mutable_back()->pts_in = current_pts;
	} else {
		Clip clip;
		clip.pts_in = max<int64_t>(current_pts - lrint(global_flags.cue_point_padding_seconds * TIMEBASE), 0);
		cliplist_clips->add_clip(clip);
		playlist_selection_changed();
	}

	// Select the item so that we can jog it.
	ui->clip_list->setFocus();
	QModelIndex index = cliplist_clips->index(cliplist_clips->size() - 1, int(ClipList::Column::IN));
	ui->clip_list->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
	ui->clip_list->scrollToBottom();
	enable_or_disable_queue_button();
}

void MainWindow::cue_out_clicked()
{
	if (cliplist_clips->empty()) {
		return;
	}

	cliplist_clips->mutable_back()->pts_out = current_pts + lrint(global_flags.cue_point_padding_seconds * TIMEBASE);

	// Select the item so that we can jog it.
	ui->clip_list->setFocus();
	QModelIndex index = cliplist_clips->index(cliplist_clips->size() - 1, int(ClipList::Column::OUT));
	ui->clip_list->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
	ui->clip_list->scrollToBottom();
	enable_or_disable_queue_button();
}

void MainWindow::queue_clicked()
{
	// See also enable_or_disable_queue_button().

	if (cliplist_clips->empty()) {
		return;
	}

	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		Clip clip = *cliplist_clips->back();
		clip.stream_idx = 0;
		if (clip.pts_out != -1) {
			playlist_clips->add_clip(clip);
			playlist_selection_changed();
			ui->playlist->scrollToBottom();
		}
		return;
	}

	QModelIndex index = selected->currentIndex();
	Clip clip = *cliplist_clips->clip(index.row());
	if (cliplist_clips->is_camera_column(index.column())) {
		clip.stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
	} else {
		clip.stream_idx = ui->preview_display->get_stream_idx();
	}

	if (clip.pts_out != -1) {
		playlist_clips->add_clip(clip);
		playlist_selection_changed();
		ui->playlist->scrollToBottom();
		if (!ui->playlist->selectionModel()->hasSelection()) {
			// TODO: Figure out why this doesn't always seem to actually select the row.
			QModelIndex bottom = playlist_clips->index(playlist_clips->size() - 1, 0);
			ui->playlist->setCurrentIndex(bottom);
		}
	}
}

void MainWindow::preview_clicked()
{
	// See also enable_or_disable_preview_button().

	if (ui->playlist->hasFocus()) {
		// Allow the playlist as preview iff it has focus and something is selected.
		QItemSelectionModel *selected = ui->playlist->selectionModel();
		if (selected->hasSelection()) {
			QModelIndex index = selected->currentIndex();
			const Clip &clip = *playlist_clips->clip(index.row());
			preview_player->play(clip);
			preview_playing = true;
			enable_or_disable_preview_button();
			return;
		}
	}

	if (cliplist_clips->empty())
		return;

	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		preview_player->play(*cliplist_clips->back());
		preview_playing = true;
		enable_or_disable_preview_button();
		return;
	}

	QModelIndex index = selected->currentIndex();
	Clip clip = *cliplist_clips->clip(index.row());
	if (cliplist_clips->is_camera_column(index.column())) {
		clip.stream_idx = index.column() - int(ClipList::Column::CAMERA_1);
	} else {
		clip.stream_idx = ui->preview_display->get_stream_idx();
	}
	preview_player->play(clip);
	preview_playing = true;
	enable_or_disable_preview_button();
}

void MainWindow::preview_angle_clicked(unsigned stream_idx)
{
	preview_player->override_angle(stream_idx);

	// Change the selection if we were previewing a clip from the clip list.
	// (The only other thing we could be showing is a pts scrub, and if so,
	// that would be selected.)
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (selected->hasSelection()) {
		QModelIndex cell = selected->selectedIndexes()[0];
		int column = int(ClipList::Column::CAMERA_1) + stream_idx;
		selected->setCurrentIndex(cell.sibling(cell.row(), column), QItemSelectionModel::ClearAndSelect);
	}
}

void MainWindow::playlist_duplicate()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}
	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	playlist_clips->duplicate_clips(first, last);
	playlist_selection_changed();
}

void MainWindow::playlist_remove()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}
	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	playlist_clips->erase_clips(first, last);

	// TODO: select the next one in the list?

	playlist_selection_changed();
}

void MainWindow::playlist_move(int delta)
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		// Should have been grayed out, but OK.
		return;
	}

	QModelIndexList rows = selected->selectedRows();
	int first = rows.front().row(), last = rows.back().row();
	if ((delta == -1 && first == 0) ||
	    (delta == 1 && size_t(last) == playlist_clips->size() - 1)) {
		// Should have been grayed out, but OK.
		return;
	}

	playlist_clips->move_clips(first, last, delta);
	playlist_selection_changed();
}

void MainWindow::jog_internal(JogDestination jog_destination, int row, int column, int stream_idx, int pts_delta)
{
	constexpr int camera_pts_per_pixel = 1500;  // One click of most mice (15 degrees), multiplied by the default wheel_sensitivity.

	int in_column, out_column, camera_column;
	if (jog_destination == JOG_CLIP_LIST) {
		in_column = int(ClipList::Column::IN);
		out_column = int(ClipList::Column::OUT);
		camera_column = -1;
	} else if (jog_destination == JOG_PLAYLIST) {
		in_column = int(PlayList::Column::IN);
		out_column = int(PlayList::Column::OUT);
		camera_column = int(PlayList::Column::CAMERA);
	} else {
		assert(false);
	}

	currently_deferring_model_changes = true;
	{
		current_change_id = (jog_destination == JOG_CLIP_LIST) ? "cliplist:" : "playlist:";
		ClipProxy clip = (jog_destination == JOG_CLIP_LIST) ? cliplist_clips->mutable_clip(row) : playlist_clips->mutable_clip(row);
		if (jog_destination == JOG_PLAYLIST) {
			stream_idx = clip->stream_idx;
		}

		if (column == in_column) {
			current_change_id += "in:" + to_string(row);
			int64_t pts = clip->pts_in + pts_delta;
			set_pts_in(pts, current_pts, clip);
			preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
		} else if (column == out_column) {
			current_change_id += "out:" + to_string(row);
			int64_t pts = clip->pts_out + pts_delta;
			pts = std::max(pts, clip->pts_in);
			pts = std::min(pts, current_pts);
			clip->pts_out = pts;
			preview_single_frame(pts, stream_idx, LAST_BEFORE);
		} else if (column == camera_column) {
			current_change_id += "camera:" + to_string(row);
			int angle_degrees = pts_delta;
			if (last_mousewheel_camera_row == row) {
				angle_degrees += leftover_angle_degrees;
			}

			int stream_idx = clip->stream_idx + angle_degrees / camera_pts_per_pixel;
			stream_idx = std::max(stream_idx, 0);
			stream_idx = std::min<int>(stream_idx, num_cameras - 1);
			clip->stream_idx = stream_idx;

			last_mousewheel_camera_row = row;
			leftover_angle_degrees = angle_degrees % camera_pts_per_pixel;

			// Don't update the live view, that's rarely what the operator wants.
		}
	}
	currently_deferring_model_changes = false;
}

void MainWindow::defer_timer_expired()
{
	state_changed(deferred_state);
}

void MainWindow::content_changed()
{
	// If we are playing, update the part of the playlist that's not playing yet.
	vector<ClipWithID> clips;
	for (unsigned row = 0; row < playlist_clips->size(); ++row) {
		clips.emplace_back(*playlist_clips->clip_with_id(row));
	}
	live_player->splice_play(clips);

	// Serialize the state.
	if (defer_timeout->isActive() &&
	    (!currently_deferring_model_changes || deferred_change_id != current_change_id)) {
		// There's some deferred event waiting, but this event is unrelated.
		// So it's time to short-circuit that timer and do the work it wanted to do.
		defer_timeout->stop();
		state_changed(deferred_state);
	}
	StateProto state;
	*state.mutable_clip_list() = cliplist_clips->serialize();
	*state.mutable_play_list() = playlist_clips->serialize();
	if (currently_deferring_model_changes) {
		deferred_change_id = current_change_id;
		deferred_state = std::move(state);
		defer_timeout->start(200);
		return;
	}
	state_changed(state);
}

void MainWindow::state_changed(const StateProto &state)
{
	db.store_state(state);

	redo_stack.clear();
	ui->redo_action->setEnabled(false);

	undo_stack.push_back(state);
	ui->undo_action->setEnabled(undo_stack.size() > 1);

	// Make sure it doesn't grow without bounds.
	while (undo_stack.size() >= 100) {
		undo_stack.pop_front();
	}
}

void MainWindow::save_settings()
{
	SettingsProto settings;
	settings.set_interpolation_quality(global_flags.interpolation_quality + 1);
	settings.set_cue_point_padding_seconds(global_flags.cue_point_padding_seconds);
	db.store_settings(settings);
}

void MainWindow::lock_blink_timer_expired()
{
	midi_mapper.set_locked(MIDIMapper::LightState(ui->speed_lock_btn->isChecked()));  // Presumably On, or the timer should have been canceled.
}

void MainWindow::play_clicked()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		return;
	}
	unsigned start_row = selected->selectedRows(0)[0].row();

	vector<ClipWithID> clips;
	for (unsigned row = start_row; row < playlist_clips->size(); ++row) {
		clips.emplace_back(*playlist_clips->clip_with_id(row));
	}
	live_player->play(clips);
	playlist_clips->set_progress({ { start_row, 0.0f } });
	ui->playlist->selectionModel()->clear();
	ui->stop_btn->setEnabled(true);
	playlist_selection_changed();
}

void MainWindow::stop_clicked()
{
	Clip fake_clip;
	fake_clip.pts_in = 0;
	fake_clip.pts_out = 0;
	playlist_clips->set_progress({});
	live_player->play(fake_clip);
	ui->stop_btn->setEnabled(false);
	playlist_selection_changed();
}

void MainWindow::speed_slider_changed(int percent)
{
	float speed = percent / 100.0f;
	ui->speed_lock_btn->setText(QString::fromStdString(" " + to_string(percent) + "%"));
	live_player->set_master_speed(speed);
	midi_mapper.set_speed_light(speed);
}

void MainWindow::speed_lock_clicked()
{
	// TODO: Make for a less abrupt transition if we're not already at 100%.
	ui->speed_slider->setValue(100);  // Also actually sets the master speed and updates the label.
	ui->speed_slider->setEnabled(!ui->speed_lock_btn->isChecked());
	midi_mapper.set_locked(MIDIMapper::LightState(ui->speed_lock_btn->isChecked()));
	lock_blink_timeout->stop();
}

void MainWindow::preview_player_done()
{
	preview_playing = false;
	enable_or_disable_preview_button();
}

void MainWindow::live_player_done()
{
	playlist_clips->set_progress({});
	ui->stop_btn->setEnabled(false);
	playlist_selection_changed();
}

void MainWindow::live_player_clip_progress(const map<uint64_t, double> &progress, double time_remaining)
{
	playlist_clips->set_progress(progress);
	set_output_status(format_duration(time_remaining) + " left");
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);

	// Ask for a relayout, but only after the event loop is done doing relayout
	// on everything else.
	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

void MainWindow::relayout()
{
	ui->live_display->setMinimumWidth(ui->live_display->height() * 16 / 9);
	ui->preview_display->setMinimumWidth(ui->preview_display->height() * 16 / 9);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	constexpr int dead_zone_pixels = 3;  // To avoid that simple clicks get misinterpreted.
	int scrub_sensitivity = 100;  // pts units per pixel.
	int wheel_sensitivity = 100;  // pts units per degree.

	if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
		enable_or_disable_preview_button();
		hidden_jog_column = -1;
	}

	unsigned stream_idx = ui->preview_display->get_stream_idx();

	if (watched == ui->clip_list) {
		if (event->type() == QEvent::FocusOut) {
			highlight_camera_input(-1);
		}
		return false;
	}

	if (event->type() != QEvent::Wheel) {
		last_mousewheel_camera_row = -1;
	}

	if (event->type() == QEvent::MouseButtonPress) {
		QMouseEvent *mouse = (QMouseEvent *)event;

		QTableView *destination;
		ScrubType type;

		if (watched == ui->clip_list->viewport()) {
			destination = ui->clip_list;
			type = SCRUBBING_CLIP_LIST;
		} else if (watched == ui->playlist->viewport()) {
			destination = ui->playlist;
			type = SCRUBBING_PLAYLIST;
		} else {
			return false;
		}
		int column = destination->columnAt(mouse->x());
		int row = destination->rowAt(mouse->y());
		if (column == -1 || row == -1)
			return false;

		if (type == SCRUBBING_CLIP_LIST) {
			if (ClipList::Column(column) == ClipList::Column::IN) {
				scrub_pts_origin = cliplist_clips->clip(row)->pts_in;
				preview_single_frame(scrub_pts_origin, stream_idx, FIRST_AT_OR_AFTER);
			} else if (ClipList::Column(column) == ClipList::Column::OUT) {
				scrub_pts_origin = cliplist_clips->clip(row)->pts_out;
				preview_single_frame(scrub_pts_origin, stream_idx, LAST_BEFORE);
			} else {
				return false;
			}
		} else {
			if (PlayList::Column(column) == PlayList::Column::IN) {
				scrub_pts_origin = playlist_clips->clip(row)->pts_in;
				preview_single_frame(scrub_pts_origin, stream_idx, FIRST_AT_OR_AFTER);
			} else if (PlayList::Column(column) == PlayList::Column::OUT) {
				scrub_pts_origin = playlist_clips->clip(row)->pts_out;
				preview_single_frame(scrub_pts_origin, stream_idx, LAST_BEFORE);
			} else {
				return false;
			}
		}

		scrubbing = true;
		scrub_row = row;
		scrub_column = column;
		scrub_x_origin = mouse->x();
		scrub_type = type;
	} else if (event->type() == QEvent::MouseMove) {
		QMouseEvent *mouse = (QMouseEvent *)event;
		if (mouse->modifiers() & Qt::KeyboardModifier::ShiftModifier) {
			scrub_sensitivity *= 10;
			wheel_sensitivity *= 10;
		}
		if (mouse->modifiers() & Qt::KeyboardModifier::AltModifier) {  // Note: Shift + Alt cancel each other out.
			scrub_sensitivity /= 10;
			wheel_sensitivity /= 10;
		}
		if (scrubbing) {
			int offset = mouse->x() - scrub_x_origin;
			int adjusted_offset;
			if (offset >= dead_zone_pixels) {
				adjusted_offset = offset - dead_zone_pixels;
			} else if (offset < -dead_zone_pixels) {
				adjusted_offset = offset + dead_zone_pixels;
			} else {
				adjusted_offset = 0;
			}

			int64_t pts = scrub_pts_origin + adjusted_offset * scrub_sensitivity;
			currently_deferring_model_changes = true;
			if (scrub_type == SCRUBBING_CLIP_LIST) {
				ClipProxy clip = cliplist_clips->mutable_clip(scrub_row);
				if (scrub_column == int(ClipList::Column::IN)) {
					current_change_id = "cliplist:in:" + to_string(scrub_row);
					set_pts_in(pts, current_pts, clip);
					preview_single_frame(pts, stream_idx, FIRST_AT_OR_AFTER);
				} else {
					current_change_id = "cliplist:out" + to_string(scrub_row);
					pts = std::max(pts, clip->pts_in);
					pts = std::min(pts, current_pts);
					clip->pts_out = pts;
					preview_single_frame(pts, stream_idx, LAST_BEFORE);
				}
			} else {
				ClipProxy clip = playlist_clips->mutable_clip(scrub_row);
				if (scrub_column == int(PlayList::Column::IN)) {
					current_change_id = "playlist:in:" + to_string(scrub_row);
					set_pts_in(pts, current_pts, clip);
					preview_single_frame(pts, clip->stream_idx, FIRST_AT_OR_AFTER);
				} else {
					current_change_id = "playlist:out:" + to_string(scrub_row);
					pts = std::max(pts, clip->pts_in);
					pts = std::min(pts, current_pts);
					clip->pts_out = pts;
					preview_single_frame(pts, clip->stream_idx, LAST_BEFORE);
				}
			}
			currently_deferring_model_changes = false;

			return true;  // Don't use this mouse movement for selecting things.
		}
	} else if (event->type() == QEvent::Wheel) {
		QWheelEvent *wheel = (QWheelEvent *)event;
		int angle_delta = wheel->angleDelta().y();
		if (wheel->modifiers() & Qt::KeyboardModifier::ShiftModifier) {
			scrub_sensitivity *= 10;
			wheel_sensitivity *= 10;
		}
		if (wheel->modifiers() & Qt::KeyboardModifier::AltModifier) {  // Note: Shift + Alt cancel each other out.
			scrub_sensitivity /= 10;
			wheel_sensitivity /= 10;
			angle_delta = wheel->angleDelta().x();  // Qt ickiness.
		}

		QTableView *destination;
		JogDestination jog_destination;
		if (watched == ui->clip_list->viewport()) {
			destination = ui->clip_list;
			jog_destination = JOG_CLIP_LIST;
			last_mousewheel_camera_row = -1;
		} else if (watched == ui->playlist->viewport()) {
			destination = ui->playlist;
			jog_destination = JOG_PLAYLIST;
			if (destination->columnAt(wheel->x()) != int(PlayList::Column::CAMERA)) {
				last_mousewheel_camera_row = -1;
			}
		} else {
			last_mousewheel_camera_row = -1;
			return false;
		}

		int column = destination->columnAt(wheel->x());
		int row = destination->rowAt(wheel->y());
		if (column == -1 || row == -1)
			return false;

		// Only adjust pts with the wheel if the given row is selected.
		if (!destination->hasFocus() ||
		    row != destination->selectionModel()->currentIndex().row()) {
			return false;
		}

		jog_internal(jog_destination, row, column, stream_idx, angle_delta * wheel_sensitivity);
		return true;  // Don't scroll.
	} else if (event->type() == QEvent::MouseButtonRelease) {
		scrubbing = false;
	}
	return false;
}

void MainWindow::preview_single_frame(int64_t pts, unsigned stream_idx, MainWindow::Rounding rounding)
{
	if (rounding == LAST_BEFORE) {
		lock_guard<mutex> lock(frame_mu);
		if (frames[stream_idx].empty())
			return;
		auto it = find_last_frame_before(frames[stream_idx], pts);
		if (it != frames[stream_idx].end()) {
			pts = it->pts;
		}
	} else {
		assert(rounding == FIRST_AT_OR_AFTER);
		lock_guard<mutex> lock(frame_mu);
		if (frames[stream_idx].empty())
			return;
		auto it = find_first_frame_at_or_after(frames[stream_idx], pts);
		if (it != frames[stream_idx].end()) {
			pts = it->pts;
		}
	}

	Clip fake_clip;
	fake_clip.pts_in = pts;
	fake_clip.pts_out = pts + 1;
	fake_clip.stream_idx = stream_idx;
	preview_player->play(fake_clip);
}

void MainWindow::playlist_selection_changed()
{
	enable_or_disable_preview_button();

	QItemSelectionModel *selected = ui->playlist->selectionModel();
	bool any_selected = selected->hasSelection();
	ui->playlist_duplicate_btn->setEnabled(any_selected);
	ui->playlist_remove_btn->setEnabled(any_selected);
	ui->playlist_move_up_btn->setEnabled(
		any_selected && selected->selectedRows().front().row() > 0);
	ui->playlist_move_down_btn->setEnabled(
		any_selected && selected->selectedRows().back().row() < int(playlist_clips->size()) - 1);

	ui->play_btn->setEnabled(any_selected);
	if (ui->stop_btn->isEnabled()) {  // Playing.
		midi_mapper.set_play_enabled(MIDIMapper::On);
	} else if (any_selected) {
		midi_mapper.set_play_enabled(MIDIMapper::Blinking);
	} else {
		midi_mapper.set_play_enabled(MIDIMapper::Off);
	}

	if (!any_selected) {
		set_output_status("paused");
	} else {
		vector<ClipWithID> clips;
		for (size_t row = selected->selectedRows().front().row(); row < playlist_clips->size(); ++row) {
			clips.emplace_back(*playlist_clips->clip_with_id(row));
		}
		double remaining = compute_total_time(clips);
		set_output_status(format_duration(remaining) + " ready");
	}
}

void MainWindow::clip_list_selection_changed(const QModelIndex &current, const QModelIndex &previous)
{
	int camera_selected = -1;
	if (cliplist_clips->is_camera_column(current.column())) {
		camera_selected = current.column() - int(ClipList::Column::CAMERA_1);

		// See the comment on hidden_jog_column.
		if (current.row() != previous.row()) {
			hidden_jog_column = -1;
		} else if (hidden_jog_column == -1) {
			hidden_jog_column = previous.column();
		}
	} else {
		hidden_jog_column = -1;
	}
	highlight_camera_input(camera_selected);
	enable_or_disable_queue_button();
}

void MainWindow::report_disk_space(off_t free_bytes, double estimated_seconds_left)
{
	char time_str[256];
	if (estimated_seconds_left < 60.0) {
		strcpy(time_str, "<font color=\"red\">Less than a minute</font>");
	} else if (estimated_seconds_left < 1800.0) {  // Less than half an hour: Xm Ys (red).
		int s = lrintf(estimated_seconds_left);
		int m = s / 60;
		s %= 60;
		snprintf(time_str, sizeof(time_str), "<font color=\"red\">%dm %ds</font>", m, s);
	} else if (estimated_seconds_left < 3600.0) {  // Less than an hour: Xm.
		int m = lrintf(estimated_seconds_left / 60.0);
		snprintf(time_str, sizeof(time_str), "%dm", m);
	} else if (estimated_seconds_left < 36000.0) {  // Less than ten hours: Xh Ym.
		int m = lrintf(estimated_seconds_left / 60.0);
		int h = m / 60;
		m %= 60;
		snprintf(time_str, sizeof(time_str), "%dh %dm", h, m);
	} else {  // More than ten hours: Xh.
		int h = lrintf(estimated_seconds_left / 3600.0);
		snprintf(time_str, sizeof(time_str), "%dh", h);
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "Disk free: %'.0f MB (approx. %s)", free_bytes / 1048576.0, time_str);

	std::string label = buf;

	post_to_main_thread([this, label] {
		disk_free_label->setText(QString::fromStdString(label));
		ui->menuBar->setCornerWidget(disk_free_label);  // Need to set this again for the sizing to get right.
	});
}

void MainWindow::midi_mapping_triggered()
{
	MIDIMappingDialog(&midi_mapper).exec();
}

void MainWindow::exit_triggered()
{
	close();
}

void MainWindow::export_cliplist_clip_multitrack_triggered()
{
	QItemSelectionModel *selected = ui->clip_list->selectionModel();
	if (!selected->hasSelection()) {
		QMessageBox msgbox;
		msgbox.setText("No clip selected in the clip list. Select one and try exporting again.");
		msgbox.exec();
		return;
	}

	QModelIndex index = selected->currentIndex();
	Clip clip = *cliplist_clips->clip(index.row());
	QString filename = QFileDialog::getSaveFileName(this,
		"Export multitrack clip", QString(), tr("Matroska video files (*.mkv)"));
	if (filename.isNull()) {
		// Cancel.
		return;
	}
	if (!filename.endsWith(".mkv")) {
		filename += ".mkv";
	}
	export_multitrack_clip(filename.toStdString(), clip);
}

void MainWindow::export_playlist_clip_interpolated_triggered()
{
	QItemSelectionModel *selected = ui->playlist->selectionModel();
	if (!selected->hasSelection()) {
		QMessageBox msgbox;
		msgbox.setText("No clip selected in the playlist. Select one and try exporting again.");
		msgbox.exec();
		return;
	}

	QString filename = QFileDialog::getSaveFileName(this,
		"Export interpolated clip", QString(), tr("Matroska video files (*.mkv)"));
	if (filename.isNull()) {
		// Cancel.
		return;
	}
	if (!filename.endsWith(".mkv")) {
		filename += ".mkv";
	}

	vector<Clip> clips;
	QModelIndexList rows = selected->selectedRows();
	for (QModelIndex index : rows) {
		clips.push_back(*playlist_clips->clip(index.row()));
	}
	export_interpolated_clip(filename.toStdString(), clips);
}

void MainWindow::manual_triggered()
{
	if (!QDesktopServices::openUrl(QUrl("https://nageru.sesse.net/doc/"))) {
		QMessageBox msgbox;
		msgbox.setText("Could not launch manual in web browser.\nPlease see https://nageru.sesse.net/doc/ manually.");
		msgbox.exec();
	}
}

void MainWindow::about_triggered()
{
	AboutDialog("Futatabi", "Multicamera slow motion video server").exec();
}

void MainWindow::undo_triggered()
{
	// Finish any deferred action.
	if (defer_timeout->isActive()) {
		defer_timeout->stop();
		state_changed(deferred_state);
	}

	StateProto redo_state;
	*redo_state.mutable_clip_list() = cliplist_clips->serialize();
	*redo_state.mutable_play_list() = playlist_clips->serialize();
	redo_stack.push_back(std::move(redo_state));
	ui->redo_action->setEnabled(true);

	assert(undo_stack.size() > 1);

	// Pop off the current state, which is always at the top of the stack.
	undo_stack.pop_back();

	StateProto state = undo_stack.back();
	ui->undo_action->setEnabled(undo_stack.size() > 1);

	replace_model(ui->clip_list, &cliplist_clips, new ClipList(state.clip_list()));
	replace_model(ui->playlist, &playlist_clips, new PlayList(state.play_list()));

	db.store_state(state);
}

void MainWindow::redo_triggered()
{
	assert(!redo_stack.empty());

	ui->undo_action->setEnabled(true);
	ui->redo_action->setEnabled(true);

	undo_stack.push_back(std::move(redo_stack.back()));
	redo_stack.pop_back();
	ui->undo_action->setEnabled(true);
	ui->redo_action->setEnabled(!redo_stack.empty());

	const StateProto &state = undo_stack.back();
	replace_model(ui->clip_list, &cliplist_clips, new ClipList(state.clip_list()));
	replace_model(ui->playlist, &playlist_clips, new PlayList(state.play_list()));

	db.store_state(state);
}

void MainWindow::quality_toggled(int quality, bool checked)
{
	if (!checked) {
		return;
	}
	global_flags.interpolation_quality = quality;
	if (quality != 0 &&  // Turning interpolation off is always possible.
	    quality != flow_initialized_interpolation_quality) {
		QMessageBox msgbox;
		msgbox.setText(QString::fromStdString(
			"The interpolation quality for the main output cannot be changed at runtime, "
			"except being turned completely off; it will take effect for exported files "
			"only until next restart. The live output quality thus remains at " +
			to_string(flow_initialized_interpolation_quality) + "."));
		msgbox.exec();
	}

	save_settings();
}

void MainWindow::padding_toggled(double seconds, bool checked)
{
	if (!checked) {
		return;
	}
	global_flags.cue_point_padding_seconds = seconds;
	save_settings();
}

void MainWindow::highlight_camera_input(int stream_idx)
{
	for (unsigned i = 0; i < num_cameras; ++i) {
		if (unsigned(stream_idx) == i) {
			displays[i].frame->setStyleSheet("background: rgb(0,255,0)");
		} else {
			displays[i].frame->setStyleSheet("");
		}
	}
	midi_mapper.highlight_camera_input(stream_idx);
}

void MainWindow::enable_or_disable_preview_button()
{
	// Follows the logic in preview_clicked().

	if (ui->playlist->hasFocus()) {
		// Allow the playlist as preview iff it has focus and something is selected.
		// TODO: Is this part really relevant?
		QItemSelectionModel *selected = ui->playlist->selectionModel();
		if (selected->hasSelection()) {
			ui->preview_btn->setEnabled(true);
			midi_mapper.set_preview_enabled(preview_playing ? MIDIMapper::On : MIDIMapper::Blinking);
			return;
		}
	}

	// TODO: Perhaps only enable this if something is actually selected.
	ui->preview_btn->setEnabled(!cliplist_clips->empty());
	if (preview_playing) {
		midi_mapper.set_preview_enabled(MIDIMapper::On);
	} else {
		midi_mapper.set_preview_enabled(cliplist_clips->empty() ? MIDIMapper::Off : MIDIMapper::Blinking);
	}
}

void MainWindow::enable_or_disable_queue_button()
{
	// Follows the logic in queue_clicked().
	// TODO: Perhaps only enable this if something is actually selected.

	bool enabled;

	if (cliplist_clips->empty()) {
		enabled = false;
	} else {
		QItemSelectionModel *selected = ui->clip_list->selectionModel();
		if (!selected->hasSelection()) {
			Clip clip = *cliplist_clips->back();
			enabled = clip.pts_out != -1;
		} else {
			QModelIndex index = selected->currentIndex();
			Clip clip = *cliplist_clips->clip(index.row());
			enabled = clip.pts_out != -1;
		}
	}

	ui->queue_btn->setEnabled(enabled);
	midi_mapper.set_queue_enabled(enabled);
}

void MainWindow::set_output_status(const string &status)
{
	ui->live_label->setText(QString::fromStdString("Current output (" + status + ")"));
	if (live_player != nullptr) {
		live_player->set_pause_status(status);
	}

	lock_guard<mutex> lock(queue_status_mu);
	queue_status = status;
}

pair<string, string> MainWindow::get_queue_status() const
{
	lock_guard<mutex> lock(queue_status_mu);
	return { queue_status, "text/plain" };
}

void MainWindow::display_frame(unsigned stream_idx, const FrameOnDisk &frame)
{
	if (stream_idx >= MAX_STREAMS) {
		fprintf(stderr, "WARNING: Ignoring too-high stream index %u.\n", stream_idx);
		return;
	}
	if (stream_idx >= num_cameras) {
		post_to_main_thread_and_wait([this, stream_idx] {
			num_cameras = stream_idx + 1;
			change_num_cameras();
		});
	}
	displays[stream_idx].display->setFrame(stream_idx, frame);
}

void MainWindow::preview()
{
	post_to_main_thread([this] {
		preview_clicked();
	});
}

void MainWindow::queue()
{
	post_to_main_thread([this] {
		queue_clicked();
	});
}

void MainWindow::play()
{
	post_to_main_thread([this] {
		play_clicked();
	});
}

void MainWindow::toggle_lock()
{
	post_to_main_thread([this] {
		ui->speed_lock_btn->setChecked(!ui->speed_lock_btn->isChecked());
		speed_lock_clicked();
	});
}

void MainWindow::jog(int delta)
{
	post_to_main_thread([this, delta] {
		int64_t pts_delta = delta * (TIMEBASE / 60);  // One click = frame at 60 fps.
		if (ui->playlist->hasFocus()) {
			QModelIndex selected = ui->playlist->selectionModel()->currentIndex();
			if (selected.column() != -1 && selected.row() != -1) {
				jog_internal(JOG_PLAYLIST, selected.row(), selected.column(), /*stream_idx=*/-1, pts_delta);
			}
		} else if (ui->clip_list->hasFocus()) {
			QModelIndex selected = ui->clip_list->selectionModel()->currentIndex();
			if (cliplist_clips->is_camera_column(selected.column()) &&
			    hidden_jog_column != -1) {
				// See the definition on hidden_jog_column.
				selected = selected.sibling(selected.row(), hidden_jog_column);
				ui->clip_list->selectionModel()->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect);
				hidden_jog_column = -1;
			}
			if (selected.column() != -1 && selected.row() != -1) {
				jog_internal(JOG_CLIP_LIST, selected.row(), selected.column(), ui->preview_display->get_stream_idx(), pts_delta);
			}
		}
	});
}

void MainWindow::switch_camera(unsigned camera_idx)
{
	post_to_main_thread([this, camera_idx] {
		if (camera_idx < num_cameras) {  // TODO: Also make this change a highlighted clip?
			preview_angle_clicked(camera_idx);
		}
	});
}

void MainWindow::set_master_speed(float speed)
{
	speed = min(max(speed, 0.1f), 2.0f);

	post_to_main_thread([this, speed] {
		if (ui->speed_lock_btn->isChecked()) {
			midi_mapper.set_locked(MIDIMapper::Blinking);
			lock_blink_timeout->start(1000);
			return;
		}

		int percent = lrintf(speed * 100.0f);
		ui->speed_slider->blockSignals(true);
		ui->speed_slider->setValue(percent);
		ui->speed_slider->blockSignals(false);
		ui->speed_lock_btn->setText(QString::fromStdString(" " + to_string(percent) + "%"));

		live_player->set_master_speed(speed);
		midi_mapper.set_speed_light(speed);
	});
}

void MainWindow::cue_in()
{
	post_to_main_thread([this] { cue_in_clicked(); });
}

void MainWindow::cue_out()
{
	post_to_main_thread([this] { cue_out_clicked(); });
}

template<class Model>
void MainWindow::replace_model(QTableView *view, Model **model, Model *new_model)
{
	QItemSelectionModel *old_selection_model = view->selectionModel();
	view->setModel(new_model);
	delete *model;
	delete old_selection_model;
	*model = new_model;
	connect(new_model, &Model::any_content_changed, this, &MainWindow::content_changed);
}

void MainWindow::start_tally()
{
	http_reply = http.get(QNetworkRequest(QString::fromStdString(global_flags.tally_url)));
	connect(http_reply, &QNetworkReply::finished, this, &MainWindow::tally_received);
}

void MainWindow::tally_received()
{
	unsigned time_to_next_tally_ms;
	if (http_reply->error()) {
		fprintf(stderr, "HTTP get of '%s' failed: %s\n", global_flags.tally_url.c_str(),
		        http_reply->errorString().toStdString().c_str());
		ui->live_frame->setStyleSheet("");
		time_to_next_tally_ms = 1000;
	} else {
		string contents = http_reply->readAll().toStdString();
		ui->live_frame->setStyleSheet(QString::fromStdString("background: " + contents));
		time_to_next_tally_ms = 100;
	}
	http_reply->deleteLater();
	http_reply = nullptr;

	QTimer::singleShot(time_to_next_tally_ms, this, &MainWindow::start_tally);
}
