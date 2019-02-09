#ifndef _MIDI_MAPPING_DIALOG_H
#define _MIDI_MAPPING_DIALOG_H

#include <stdbool.h>
#include <QDialog>
#include <QString>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "midi_mapper.h"

class QEvent;
class QObject;

namespace Ui {
class MIDIMappingDialog;
}  // namespace Ui

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
	void preview() override {}
	void queue() override {}
	void play() override {}
	void toggle_lock() override {}
	void jog(int delta) override {}
	void switch_camera(unsigned camera_idx) override {}
	void set_master_speed(float speed) override {}
	void cue_in() override {}
	void cue_out() override {}

	// Raw events; used for the editor dialog only.
	void controller_changed(unsigned controller) override;
	void note_on(unsigned note) override;

private:
	void ok_clicked();
	void cancel_clicked();
	void save_clicked();
	void load_clicked();

	void add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number);
	
	enum class ControlType { CONTROLLER, CONTROLLER_LIGHT, BUTTON, BUTTON_LIGHT, CAMERA_BUTTON, CAMERA_BUTTON_LIGHT };
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
		QSpinBox *spinner2;  // Value for button lights, min value for controller lights.
		QSpinBox *spinner3;  // Max value for controller lights.
		int field_number;  // In MIDIMappingBusProto.
	};
	struct InstantiatedComboBox {
		QComboBox *combo_box;
		int field_number;  // In MIDIMappingProto.
	};
	std::vector<InstantiatedSpinner> controller_spinners;
	std::vector<InstantiatedSpinner> controller_light_spinners;
	std::vector<InstantiatedSpinner> button_spinners;
	std::vector<InstantiatedSpinner> button_light_spinners;
	std::vector<InstantiatedSpinner> camera_button_spinners;  // One per camera.
	std::vector<InstantiatedSpinner> camera_button_light_spinners;  // One per camera.
	std::vector<InstantiatedComboBox> bank_combo_boxes;

	// Keyed on field number.
	std::map<unsigned, QSpinBox *> spinners;
};

#endif  // !defined(_MIDI_MAPPING_DIALOG_H)
