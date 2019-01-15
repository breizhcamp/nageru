#ifndef _MIDI_MAPPER_UTIL_H
#define _MIDI_MAPPER_UTIL_H 1

#include "midi_mapping.pb.h"
#include "shared/midi_device.h"

#include <google/protobuf/descriptor.h>

template <class Proto>
inline bool match_controller_helper(const Proto &msg, int field_number, int controller)
{
	using namespace google::protobuf;
	const FieldDescriptor *descriptor = msg.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *reflection = msg.GetReflection();
	if (!reflection->HasField(msg, descriptor)) {
		return false;
	}
	const MIDIControllerProto &controller_proto =
		static_cast<const MIDIControllerProto &>(reflection->GetMessage(msg, descriptor));
	return (controller_proto.controller_number() == controller);
}

template <class Proto>
inline bool match_button_helper(const Proto &msg, int field_number, int note)
{
	using namespace google::protobuf;
	const FieldDescriptor *descriptor = msg.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *reflection = msg.GetReflection();
	if (!reflection->HasField(msg, descriptor)) {
		return false;
	}
	const MIDIButtonProto &button_proto =
		static_cast<const MIDIButtonProto &>(reflection->GetMessage(msg, descriptor));
	return (button_proto.note_number() == note);
}

template <class Proto>
inline bool match_bank_helper(const Proto &msg, int bank_field_number, int bank)
{
	using namespace google::protobuf;
	const FieldDescriptor *bank_descriptor = msg.GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = msg.GetReflection();
	if (!reflection->HasField(msg, bank_descriptor)) {
		// No bank set => in all banks.
		return true;
	}
	return reflection->GetInt32(msg, bank_descriptor) == bank;
}

// Find what MIDI note the given light (as given by field_number) is mapped to, and enable it.
template <class Proto>
void activate_mapped_light(const Proto &msg, int field_number, std::set<unsigned> *active_lights)
{
	using namespace google::protobuf;
	const FieldDescriptor *descriptor = msg.GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *reflection = msg.GetReflection();
	if (!reflection->HasField(msg, descriptor)) {
		return;
	}
	const MIDILightProto &light_proto =
		static_cast<const MIDILightProto &>(reflection->GetMessage(msg, descriptor));
	active_lights->insert(light_proto.note_number());
}

inline double map_controller_to_float(int controller, int val)
{
	if (controller == MIDIReceiver::PITCH_BEND_CONTROLLER) {
		// We supposedly go from -8192 to 8191 (inclusive), but there are
		// controllers that only have 10-bit precision and do the upconversion
		// to 14-bit wrong (just padding with zeros), making 8176 the highest
		// attainable value. We solve this by making the effective range
		// -8176..8176 (inclusive).
		if (val <= -8176) {
			return 0.0;
		} else if (val >= 8176) {
			return 1.0;
		} else {
			return 0.5 * (double(val) / 8176.0) + 0.5;
		}
	}

	// Slightly hackish mapping so that we can represent exactly 0.0, 0.5 and 1.0.
	if (val <= 0) {
		return 0.0;
	} else if (val >= 127) {
		return 1.0;
	} else {
		return (val + 0.5) / 127.0;
	}
}

#endif  // !defined(_MIDI_MAPPER_UTIL_H)
