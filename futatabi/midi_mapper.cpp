#include "midi_mapper.h"

#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <functional>
#include <map>
#include <thread>

#include "defs.h"
#include "futatabi_midi_mapping.pb.h"
#include "shared/midi_mapper_util.h"
#include "shared/text_proto.h"

using namespace google::protobuf;
using namespace std;
using namespace std::placeholders;

MIDIMapper::MIDIMapper(ControllerReceiver *receiver)
	: receiver(receiver), mapping_proto(new MIDIMappingProto), midi_device(this)
{
}

MIDIMapper::~MIDIMapper() {}

bool load_midi_mapping_from_file(const string &filename, MIDIMappingProto *new_mapping)
{
	return load_proto_from_file(filename, new_mapping);
}

bool save_midi_mapping_to_file(const MIDIMappingProto &mapping_proto, const string &filename)
{
	return save_proto_to_file(mapping_proto, filename);
}

void MIDIMapper::set_midi_mapping(const MIDIMappingProto &new_mapping)
{
	lock_guard<mutex> lock(mu);
	if (mapping_proto) {
		mapping_proto->CopyFrom(new_mapping);
	} else {
		mapping_proto.reset(new MIDIMappingProto(new_mapping));
	}

	num_controller_banks = min(max(mapping_proto->num_controller_banks(), 1), 5);
        current_controller_bank = 0;
}

void MIDIMapper::start_thread()
{
	midi_device.start_thread();
}

const MIDIMappingProto &MIDIMapper::get_current_mapping() const
{
	lock_guard<mutex> lock(mu);
	return *mapping_proto;
}

ControllerReceiver *MIDIMapper::set_receiver(ControllerReceiver *new_receiver)
{
	lock_guard<mutex> lock(mu);
	swap(receiver, new_receiver);
	return new_receiver;  // Now old receiver.
}

void MIDIMapper::controller_received(int controller, int value_int)
{
	int delta_value = value_int - 64;  // For infinite controllers such as jog.
	float value = map_controller_to_float(controller, value_int);

	receiver->controller_changed(controller);

	match_controller(controller, MIDIMappingProto::kJogFieldNumber, MIDIMappingProto::kJogBankFieldNumber,
		delta_value, bind(&ControllerReceiver::jog, receiver, _1));

	// Speed goes from 0.0 to 2.0 (the receiver will clamp).
	match_controller(controller, MIDIMappingProto::kMasterSpeedFieldNumber, MIDIMappingProto::kMasterSpeedBankFieldNumber,
		value * 2.0, bind(&ControllerReceiver::set_master_speed, receiver, _1));
}

void MIDIMapper::note_on_received(int note)
{
	lock_guard<mutex> lock(mu);
	receiver->note_on(note);

	if (mapping_proto->has_prev_bank() &&
	    mapping_proto->prev_bank().note_number() == note) {
		current_controller_bank = (current_controller_bank + num_controller_banks - 1) % num_controller_banks;
		update_lights_lock_held();
	}
	if (mapping_proto->has_next_bank() &&
	    mapping_proto->next_bank().note_number() == note) {
		current_controller_bank = (current_controller_bank + 1) % num_controller_banks;
		update_lights_lock_held();
	}
	if (mapping_proto->has_select_bank_1() &&
	    mapping_proto->select_bank_1().note_number() == note) {
		current_controller_bank = 0;
		update_lights_lock_held();
	}
	if (mapping_proto->has_select_bank_2() &&
	    mapping_proto->select_bank_2().note_number() == note &&
	    num_controller_banks >= 2) {
		current_controller_bank = 1;
		update_lights_lock_held();
	}
	if (mapping_proto->has_select_bank_3() &&
	    mapping_proto->select_bank_3().note_number() == note &&
	    num_controller_banks >= 3) {
		current_controller_bank = 2;
		update_lights_lock_held();
	}
	if (mapping_proto->has_select_bank_4() &&
	    mapping_proto->select_bank_4().note_number() == note &&
	    num_controller_banks >= 4) {
		current_controller_bank = 3;
		update_lights_lock_held();
	}
	if (mapping_proto->has_select_bank_5() &&
	    mapping_proto->select_bank_5().note_number() == note &&
	    num_controller_banks >= 5) {
		current_controller_bank = 4;
		update_lights_lock_held();
	}

	match_button(note, MIDIMappingProto::kPreviewFieldNumber, MIDIMappingProto::kPreviewBankFieldNumber,
		bind(&ControllerReceiver::preview, receiver));
	match_button(note, MIDIMappingProto::kQueueFieldNumber, MIDIMappingProto::kQueueBankFieldNumber,
		bind(&ControllerReceiver::queue, receiver));
	match_button(note, MIDIMappingProto::kPlayFieldNumber, MIDIMappingProto::kPlayBankFieldNumber,
		bind(&ControllerReceiver::play, receiver));
	match_button(note, MIDIMappingProto::kToggleLockFieldNumber, MIDIMappingProto::kToggleLockBankFieldNumber,
		bind(&ControllerReceiver::toggle_lock, receiver));

	unsigned num_cameras = std::min(MAX_STREAMS, mapping_proto->camera_size());
	for (unsigned camera_idx = 0; camera_idx < num_cameras; ++camera_idx) {
		const CameraMIDIMappingProto &camera = mapping_proto->camera(camera_idx);
		if (match_bank_helper(camera, CameraMIDIMappingProto::kBankFieldNumber, current_controller_bank) &&
		    match_button_helper(camera, CameraMIDIMappingProto::kButtonFieldNumber, note)) {
			receiver->switch_camera(camera_idx);
		}
	}

	match_button(note, MIDIMappingProto::kCueInFieldNumber, MIDIMappingProto::kCueInBankFieldNumber,
		bind(&ControllerReceiver::cue_in, receiver));
	match_button(note, MIDIMappingProto::kCueOutFieldNumber, MIDIMappingProto::kCueOutBankFieldNumber,
		bind(&ControllerReceiver::cue_out, receiver));
}

void MIDIMapper::match_controller(int controller, int field_number, int bank_field_number, float value, function<void(float)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	if (match_controller_helper(*mapping_proto, field_number, controller)) {
		func(value);
	}
}

void MIDIMapper::match_button(int note, int field_number, int bank_field_number, function<void()> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	if (match_button_helper(*mapping_proto, field_number, note)) {
		func();
	}
}

bool MIDIMapper::has_active_controller(int field_number, int bank_field_number)
{
	if (bank_mismatch(bank_field_number)) {
		return false;
	}

	const FieldDescriptor *descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *reflection = mapping_proto->GetReflection();
	return reflection->HasField(*mapping_proto, descriptor);
}

bool MIDIMapper::bank_mismatch(int bank_field_number)
{
	return !match_bank_helper(*mapping_proto, bank_field_number, current_controller_bank);
}

void MIDIMapper::refresh_lights()
{
	lock_guard<mutex> lock(mu);
	update_lights_lock_held();
}

void MIDIMapper::update_lights_lock_held()
{
	map<unsigned, uint8_t> active_lights;  // Desired state.
	if (current_controller_bank == 0) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kBank1IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 1) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kBank2IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 2) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kBank3IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 3) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kBank4IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 4) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kBank5IsSelectedFieldNumber, &active_lights);
	}
	if (preview_enabled_light == On) {  // Playing.
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kPreviewPlayingFieldNumber, &active_lights);
	} else if (preview_enabled_light == Blinking) {  // Preview ready.
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kPreviewReadyFieldNumber, &active_lights);
	}
	if (queue_enabled_light) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kQueueEnabledFieldNumber, &active_lights);
	}
	if (play_enabled_light == On) {  // Playing.
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kPlayingFieldNumber, &active_lights);
	} else if (play_enabled_light == Blinking) {  // Play ready.
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kPlayReadyFieldNumber, &active_lights);
	}
	if (locked_light == On) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kLockedFieldNumber, &active_lights);
	} else if (locked_light == Blinking) {
		activate_mapped_light(*mapping_proto, MIDIMappingProto::kLockedBlinkingFieldNumber, &active_lights);
	}
	if (current_highlighted_camera >= 0 && current_highlighted_camera < mapping_proto->camera_size()) {
		const CameraMIDIMappingProto &camera = mapping_proto->camera(current_highlighted_camera);
		activate_mapped_light(camera, CameraMIDIMappingProto::kIsCurrentFieldNumber, &active_lights);
	}

	// These are always enabled right now.
	activate_mapped_light(*mapping_proto, MIDIMappingProto::kCueInFieldNumber, &active_lights);
	activate_mapped_light(*mapping_proto, MIDIMappingProto::kCueOutFieldNumber, &active_lights);

	midi_device.update_lights(active_lights);
}
