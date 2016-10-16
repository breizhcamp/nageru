#ifndef _MIDI_MAPPING_DIALOG_H
#define _MIDI_MAPPING_DIALOG_H

#include <QDialog>
#include <string>
#include <vector>
#include <sys/time.h>

#include "audio_mixer.h"
#include "midi_mapper.h"
#include "mixer.h"

namespace Ui {
class MIDIMappingDialog;
}  // namespace Ui

class MIDIMapper;
class MIDIMappingProto;
class QComboBox;
class QSpinBox;
class QTreeWidgetItem;

class MIDIMappingDialog : public QDialog, public ControllerReceiver
{
	Q_OBJECT

public:
	MIDIMappingDialog(MIDIMapper *mapper);
	~MIDIMappingDialog();

	// For use in midi_mapping_dialog.cpp only.
	struct Control {
		std::string label;
		int field_number;  // In MIDIMappingBusProto.
		int bank_field_number;  // In MIDIMappingProto.
	};

	// ControllerReceiver interface. We only implement the raw events.
	// All values are [0.0, 1.0].
	void set_locut(float value) override {}
	void set_limiter_threshold(float value) override {}
	void set_makeup_gain(float value) override {}

	void set_treble(unsigned bus_idx, float value) override {}
	void set_mid(unsigned bus_idx, float value) override {}
	void set_bass(unsigned bus_idx, float value) override {}
	void set_gain(unsigned bus_idx, float value) override {}
	void set_compressor_threshold(unsigned bus_idx, float value) override {}
	void set_fader(unsigned bus_idx, float value) override {}

	void toggle_locut(unsigned bus_idx) override {}
	void toggle_auto_gain_staging(unsigned bus_idx) override {}
	void toggle_compressor(unsigned bus_idx) override {}
	void clear_peak(unsigned bus_idx) override {}

	// Raw events; used for the editor dialog only.
	void controller_changed(unsigned controller) override;
	void note_on(unsigned note) override;

public slots:
	void ok_clicked();
	void cancel_clicked();
	void save_clicked();
	void load_clicked();

private:
	static constexpr unsigned num_buses = 8;

	void add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number);
	
	enum class ControlType { CONTROLLER, BUTTON };
	void add_controls(const std::string &heading, ControlType control_type,
	                  const MIDIMappingProto &mapping_proto, const std::vector<Control> &controls);
	void fill_controls_from_mapping(const MIDIMappingProto &mapping_proto);

	std::unique_ptr<MIDIMappingProto> construct_mapping_proto_from_ui();

	Ui::MIDIMappingDialog *ui;
	MIDIMapper *mapper;
	ControllerReceiver *old_receiver;

	// All controllers actually laid out on the grid (we need to store them
	// so that we can move values back and forth between the controls and
	// the protobuf on save/load).
	struct InstantiatedSpinner {
		QSpinBox *spinner;
		unsigned bus_idx;
		int field_number;  // In MIDIMappingBusProto.
	};
	struct InstantiatedComboBox {
		QComboBox *combo_box;
		int field_number;  // In MIDIMappingProto.
	};
	std::vector<InstantiatedSpinner> controller_spinners;
	std::vector<InstantiatedSpinner> button_spinners;
	std::vector<InstantiatedComboBox> bank_combo_boxes;
};

#endif  // !defined(_MIDI_MAPPING_DIALOG_H)