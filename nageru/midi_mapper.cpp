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
#include <thread>

#include "audio_mixer.h"
#include "midi_mapping.pb.h"
#include "shared/text_proto.h"

using namespace google::protobuf;
using namespace std;
using namespace std::placeholders;

namespace {

double map_controller_to_float(int val)
{
	// Slightly hackish mapping so that we can represent exactly 0.0, 0.5 and 1.0.
	if (val <= 0) {
		return 0.0;
	} else if (val >= 127) {
		return 1.0;
	} else {
		return (val + 0.5) / 127.0;
	}
}

}  // namespace

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

	receiver->clear_all_highlights();
	update_highlights();
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
	const float value = map_controller_to_float(value_int);

	receiver->controller_changed(controller);

	// Global controllers.
	match_controller(controller, MIDIMappingBusProto::kLocutFieldNumber, MIDIMappingProto::kLocutBankFieldNumber,
		value, bind(&ControllerReceiver::set_locut, receiver, _2));
	match_controller(controller, MIDIMappingBusProto::kLimiterThresholdFieldNumber, MIDIMappingProto::kLimiterThresholdBankFieldNumber,
		value, bind(&ControllerReceiver::set_limiter_threshold, receiver, _2));
	match_controller(controller, MIDIMappingBusProto::kMakeupGainFieldNumber, MIDIMappingProto::kMakeupGainBankFieldNumber,
		value, bind(&ControllerReceiver::set_makeup_gain, receiver, _2));

	// Bus controllers.
	match_controller(controller, MIDIMappingBusProto::kStereoWidthFieldNumber, MIDIMappingProto::kStereoWidthBankFieldNumber,
		value, bind(&ControllerReceiver::set_stereo_width, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber,
		value, bind(&ControllerReceiver::set_treble, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kMidFieldNumber, MIDIMappingProto::kMidBankFieldNumber,
		value, bind(&ControllerReceiver::set_mid, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kBassFieldNumber, MIDIMappingProto::kBassBankFieldNumber,
		value, bind(&ControllerReceiver::set_bass, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kGainFieldNumber, MIDIMappingProto::kGainBankFieldNumber,
		value, bind(&ControllerReceiver::set_gain, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kCompressorThresholdFieldNumber, MIDIMappingProto::kCompressorThresholdBankFieldNumber,
		value, bind(&ControllerReceiver::set_compressor_threshold, receiver, _1, _2));
	match_controller(controller, MIDIMappingBusProto::kFaderFieldNumber, MIDIMappingProto::kFaderBankFieldNumber,
		value, bind(&ControllerReceiver::set_fader, receiver, _1, _2));
}

void MIDIMapper::note_on_received(int note)
{
	lock_guard<mutex> lock(mu);
	receiver->note_on(note);

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);
		if (bus_mapping.has_prev_bank() &&
		    bus_mapping.prev_bank().note_number() == note) {
			current_controller_bank = (current_controller_bank + num_controller_banks - 1) % num_controller_banks;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_next_bank() &&
		    bus_mapping.next_bank().note_number() == note) {
			current_controller_bank = (current_controller_bank + 1) % num_controller_banks;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_select_bank_1() &&
		    bus_mapping.select_bank_1().note_number() == note) {
			current_controller_bank = 0;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_select_bank_2() &&
		    bus_mapping.select_bank_2().note_number() == note &&
		    num_controller_banks >= 2) {
			current_controller_bank = 1;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_select_bank_3() &&
		    bus_mapping.select_bank_3().note_number() == note &&
		    num_controller_banks >= 3) {
			current_controller_bank = 2;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_select_bank_4() &&
		    bus_mapping.select_bank_4().note_number() == note &&
		    num_controller_banks >= 4) {
			current_controller_bank = 3;
			update_highlights();
			update_lights_lock_held();
		}
		if (bus_mapping.has_select_bank_5() &&
		    bus_mapping.select_bank_5().note_number() == note &&
		    num_controller_banks >= 5) {
			current_controller_bank = 4;
			update_highlights();
			update_lights_lock_held();
		}
	}

	match_button(note, MIDIMappingBusProto::kToggleLocutFieldNumber, MIDIMappingProto::kToggleLocutBankFieldNumber,
		bind(&ControllerReceiver::toggle_locut, receiver, _1));
	match_button(note, MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber, MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber,
		bind(&ControllerReceiver::toggle_auto_gain_staging, receiver, _1));
	match_button(note, MIDIMappingBusProto::kToggleCompressorFieldNumber, MIDIMappingProto::kToggleCompressorBankFieldNumber,
		bind(&ControllerReceiver::toggle_compressor, receiver, _1));
	match_button(note, MIDIMappingBusProto::kClearPeakFieldNumber, MIDIMappingProto::kClearPeakBankFieldNumber,
		bind(&ControllerReceiver::clear_peak, receiver, _1));
	match_button(note, MIDIMappingBusProto::kToggleMuteFieldNumber, MIDIMappingProto::kClearPeakBankFieldNumber,
		bind(&ControllerReceiver::toggle_mute, receiver, _1));
	match_button(note, MIDIMappingBusProto::kToggleLimiterFieldNumber, MIDIMappingProto::kToggleLimiterBankFieldNumber,
		bind(&ControllerReceiver::toggle_limiter, receiver));
	match_button(note, MIDIMappingBusProto::kToggleAutoMakeupGainFieldNumber, MIDIMappingProto::kToggleAutoMakeupGainBankFieldNumber,
		bind(&ControllerReceiver::toggle_auto_makeup_gain, receiver));
}

void MIDIMapper::update_num_subscribers(unsigned num_subscribers)
{
	num_subscribed_ports = num_subscribers;
	update_highlights();
}

void MIDIMapper::match_controller(int controller, int field_number, int bank_field_number, float value, function<void(unsigned, float)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIControllerProto &controller_proto =
			static_cast<const MIDIControllerProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (controller_proto.controller_number() == controller) {
			func(bus_idx, value);
		}
	}
}

void MIDIMapper::match_button(int note, int field_number, int bank_field_number, function<void(unsigned)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIButtonProto &button_proto =
			static_cast<const MIDIButtonProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (button_proto.note_number() == note) {
			func(bus_idx);
		}
	}
}

bool MIDIMapper::has_active_controller(unsigned bus_idx, int field_number, int bank_field_number)
{
	if (bank_mismatch(bank_field_number)) {
		return false;
	}

	const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);
	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	return bus_reflection->HasField(bus_mapping, descriptor);
}

bool MIDIMapper::bank_mismatch(int bank_field_number)
{
	const FieldDescriptor *bank_descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = mapping_proto->GetReflection();
	return (reflection->HasField(*mapping_proto, bank_descriptor) &&
 	        reflection->GetInt32(*mapping_proto, bank_descriptor) != current_controller_bank);
}

void MIDIMapper::refresh_highlights()
{
	receiver->clear_all_highlights();
	update_highlights();
}

void MIDIMapper::refresh_lights()
{
	lock_guard<mutex> lock(mu);
	update_lights_lock_held();
}

void MIDIMapper::update_highlights()
{
	if (num_subscribed_ports.load() == 0) {
		receiver->clear_all_highlights();
		return;
	}

	// Global controllers.
	bool highlight_locut = false;
	bool highlight_limiter_threshold = false;
	bool highlight_makeup_gain = false;
	bool highlight_toggle_limiter = false;
	bool highlight_toggle_auto_makeup_gain = false;
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kLocutFieldNumber, MIDIMappingProto::kLocutBankFieldNumber)) {
			highlight_locut = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kLimiterThresholdFieldNumber, MIDIMappingProto::kLimiterThresholdBankFieldNumber)) {
			highlight_limiter_threshold = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kMakeupGainFieldNumber, MIDIMappingProto::kMakeupGainBankFieldNumber)) {
			highlight_makeup_gain = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleLimiterFieldNumber, MIDIMappingProto::kToggleLimiterBankFieldNumber)) {
			highlight_toggle_limiter = true;
		}
		if (has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleAutoMakeupGainFieldNumber, MIDIMappingProto::kToggleAutoMakeupGainBankFieldNumber)) {
			highlight_toggle_auto_makeup_gain = true;
		}
	}
	receiver->highlight_locut(highlight_locut);
	receiver->highlight_limiter_threshold(highlight_limiter_threshold);
	receiver->highlight_makeup_gain(highlight_makeup_gain);
	receiver->highlight_toggle_limiter(highlight_toggle_limiter);
	receiver->highlight_toggle_auto_makeup_gain(highlight_toggle_auto_makeup_gain);

	// Per-bus controllers.
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		receiver->highlight_stereo_width(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kStereoWidthFieldNumber, MIDIMappingProto::kStereoWidthBankFieldNumber));
		receiver->highlight_treble(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber));
		receiver->highlight_mid(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kMidFieldNumber, MIDIMappingProto::kMidBankFieldNumber));
		receiver->highlight_bass(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kBassFieldNumber, MIDIMappingProto::kBassBankFieldNumber));
		receiver->highlight_gain(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kGainFieldNumber, MIDIMappingProto::kGainBankFieldNumber));
		receiver->highlight_compressor_threshold(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kCompressorThresholdFieldNumber, MIDIMappingProto::kCompressorThresholdBankFieldNumber));
		receiver->highlight_fader(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kFaderFieldNumber, MIDIMappingProto::kFaderBankFieldNumber));
		receiver->highlight_mute(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleMuteFieldNumber, MIDIMappingProto::kToggleMuteBankFieldNumber));
		receiver->highlight_toggle_locut(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleLocutFieldNumber, MIDIMappingProto::kToggleLocutBankFieldNumber));
		receiver->highlight_toggle_auto_gain_staging(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber, MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber));
		receiver->highlight_toggle_compressor(bus_idx, has_active_controller(
			bus_idx, MIDIMappingBusProto::kToggleCompressorFieldNumber, MIDIMappingProto::kToggleCompressorBankFieldNumber));
	}
}

void MIDIMapper::update_lights_lock_held()
{
	if (global_audio_mixer == nullptr) {
		return;
	}

	set<unsigned> active_lights;  // Desired state.
	if (current_controller_bank == 0) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank1IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 1) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank2IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 2) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank3IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 3) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank4IsSelectedFieldNumber, &active_lights);
	}
	if (current_controller_bank == 4) {
		activate_lights_all_buses(MIDIMappingBusProto::kBank5IsSelectedFieldNumber, &active_lights);
	}
	if (global_audio_mixer->get_limiter_enabled()) {
		activate_lights_all_buses(MIDIMappingBusProto::kLimiterIsOnFieldNumber, &active_lights);
	}
	if (global_audio_mixer->get_final_makeup_gain_auto()) {
		activate_lights_all_buses(MIDIMappingBusProto::kAutoMakeupGainIsOnFieldNumber, &active_lights);
	}
	unsigned num_buses = min<unsigned>(global_audio_mixer->num_buses(), mapping_proto->bus_mapping_size());
	for (unsigned bus_idx = 0; bus_idx < num_buses; ++bus_idx) {
		if (global_audio_mixer->get_mute(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kIsMutedFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_locut_enabled(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kLocutIsOnFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_gain_staging_auto(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kAutoGainStagingIsOnFieldNumber, &active_lights);
		}
		if (global_audio_mixer->get_compressor_enabled(bus_idx)) {
			activate_lights(bus_idx, MIDIMappingBusProto::kCompressorIsOnFieldNumber, &active_lights);
		}
		if (has_peaked[bus_idx]) {
			activate_lights(bus_idx, MIDIMappingBusProto::kHasPeakedFieldNumber, &active_lights);
		}
	}

	midi_device.update_lights(active_lights);
}

void MIDIMapper::activate_lights(unsigned bus_idx, int field_number, set<unsigned> *active_lights)
{
	const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

	const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = bus_mapping.GetReflection();
	if (!bus_reflection->HasField(bus_mapping, descriptor)) {
		return;
	}
	const MIDILightProto &light_proto =
		static_cast<const MIDILightProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
	active_lights->insert(light_proto.note_number());
}

void MIDIMapper::activate_lights_all_buses(int field_number, set<unsigned> *active_lights)
{
	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDILightProto &light_proto =
			static_cast<const MIDILightProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		active_lights->insert(light_proto.note_number());
	}
}
