#ifndef _MIDI_MAPPER_H
#define _MIDI_MAPPER_H 1

// MIDIMapper in Futatabi is much the same as MIDIMapper in Nageru
// (it incoming MIDI messages from mixer controllers interprets them
// according to a user-defined mapping, and calls back into a receiver),
// and shares a fair amount of support code with it. However, it is
// also somewhat different; there are no audio buses, in particular.
// Also, DJ controllers typically have more buttons than audio controllers
// since there's only one (or maybe two) channels, so banks are less
// important, and thus, there's no highlighting. Also, the controllers
// are somewhat different, e.g., you have jog to deal with.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "defs.h"
#include "shared/midi_device.h"

class MIDIMappingProto;

// Interface for receiving interpreted controller messages.
class ControllerReceiver {
public:
	virtual ~ControllerReceiver() {}

	virtual void preview() = 0;
	virtual void queue() = 0;
	virtual void play() = 0;
	virtual void toggle_lock() = 0;
	virtual void jog(int delta) = 0;
	virtual void switch_camera(unsigned camera_idx) = 0;
	virtual void set_master_speed(float speed) = 0;
	virtual void cue_in() = 0;
	virtual void cue_out() = 0;

	// Raw events; used for the editor dialog only.
	virtual void controller_changed(unsigned controller) = 0;
	virtual void note_on(unsigned note) = 0;
};

class MIDIMapper : public MIDIReceiver {
public:
	MIDIMapper(ControllerReceiver *receiver);
	virtual ~MIDIMapper();
	void set_midi_mapping(const MIDIMappingProto &new_mapping);
	void start_thread();
	const MIDIMappingProto &get_current_mapping() const;

	// Overwrites and returns the previous value.
	ControllerReceiver *set_receiver(ControllerReceiver *new_receiver);

	void refresh_lights();

	void set_preview_enabled(bool enabled) {
		preview_enabled_light = enabled;
		refresh_lights();
	}
	void set_queue_enabled(bool enabled) {
		queue_enabled_light = enabled;
		refresh_lights();
	}
	void set_play_enabled(bool enabled) {
		play_enabled_light = enabled;
		refresh_lights();
	}
	void set_locked(bool locked) {
		locked_light = locked;
		refresh_lights();
	}
	void highlight_camera_input(int stream_idx) {  // -1 for none.
		current_highlighted_camera = stream_idx;
		refresh_lights();
	}

	// MIDIReceiver.
	void controller_received(int controller, int value) override;
	void note_on_received(int note) override;
	void update_num_subscribers(unsigned num_subscribers) override {}

private:
	void match_controller(int controller, int field_number, int bank_field_number, float value, std::function<void(float)> func);
	void match_button(int note, int field_number, int bank_field_number, std::function<void()> func);
	bool has_active_controller(int field_number, int bank_field_number);  // Also works for buttons.
	bool bank_mismatch(int bank_field_number);

	void update_lights_lock_held();

	std::atomic<bool> should_quit{false};
	int should_quit_fd;

	mutable std::mutex mu;
	ControllerReceiver *receiver;  // Under <mu>.
	std::unique_ptr<MIDIMappingProto> mapping_proto;  // Under <mu>.
	int num_controller_banks;  // Under <mu>.
	std::atomic<int> current_controller_bank{0};

	std::atomic<bool> preview_enabled_light{false};
	std::atomic<bool> queue_enabled_light{false};
	std::atomic<bool> play_enabled_light{false};
	std::atomic<bool> locked_light{true};
	std::atomic<int> current_highlighted_camera{-1};

	MIDIDevice midi_device;
};

bool load_midi_mapping_from_file(const std::string &filename, MIDIMappingProto *new_mapping);
bool save_midi_mapping_to_file(const MIDIMappingProto &mapping_proto, const std::string &filename);

#endif  // !defined(_MIDI_MAPPER_H)
