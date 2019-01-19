#include "midi_mapping_dialog.h"

#include <assert.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <stdio.h>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>

#include "shared/controller_spin_box.h"
#include "midi_mapper.h"
#include "futatabi_midi_mapping.pb.h"
#include "shared/midi_mapper_util.h"
#include "shared/post_to_main_thread.h"
#include "ui_midi_mapping.h"

class QObject;

using namespace google::protobuf;
using namespace std;

vector<MIDIMappingDialog::Control> controllers = {
	{ "Jog",          MIDIMappingProto::kJogFieldNumber,
	                  MIDIMappingProto::kJogBankFieldNumber },
	{ "Master speed", MIDIMappingProto::kMasterSpeedFieldNumber,
	                  MIDIMappingProto::kMasterSpeedBankFieldNumber },
};
vector<MIDIMappingDialog::Control> controller_lights = {
	{ "Master speed light", MIDIMappingProto::kMasterSpeedLightFieldNumber, 0 },
};
vector<MIDIMappingDialog::Control> buttons = {
	{ "Preview",      MIDIMappingProto::kPreviewFieldNumber,
	                  MIDIMappingProto::kPreviewBankFieldNumber },
	{ "Queue",        MIDIMappingProto::kQueueFieldNumber,
	                  MIDIMappingProto::kQueueBankFieldNumber },
	{ "Play",         MIDIMappingProto::kPlayFieldNumber,
	                  MIDIMappingProto::kPlayBankFieldNumber },
	{ "Lock master speed", MIDIMappingProto::kToggleLockFieldNumber,
	                  MIDIMappingProto::kToggleLockBankFieldNumber },
	{ "Cue in",       MIDIMappingProto::kCueInFieldNumber,
	                  MIDIMappingProto::kCueInBankFieldNumber },
	{ "Cue out",      MIDIMappingProto::kCueOutFieldNumber,
	                  MIDIMappingProto::kCueOutBankFieldNumber },
	{ "Previous bank", MIDIMappingProto::kPrevBankFieldNumber, 0 },
	{ "Next bank",     MIDIMappingProto::kNextBankFieldNumber, 0 },
	{ "Select bank 1", MIDIMappingProto::kSelectBank1FieldNumber, 0 },
	{ "Select bank 2", MIDIMappingProto::kSelectBank2FieldNumber, 0 },
	{ "Select bank 3", MIDIMappingProto::kSelectBank3FieldNumber, 0 },
	{ "Select bank 4", MIDIMappingProto::kSelectBank4FieldNumber, 0 },
	{ "Select bank 5", MIDIMappingProto::kSelectBank5FieldNumber, 0 },
};
vector<MIDIMappingDialog::Control> button_lights = {
        { "Preview playing",      MIDIMappingProto::kPreviewPlayingFieldNumber, 0 },
        { "Preview ready",        MIDIMappingProto::kPreviewReadyFieldNumber, 0 },
        { "Queue button enabled", MIDIMappingProto::kQueueEnabledFieldNumber, 0 },
        { "Playing",              MIDIMappingProto::kPlayingFieldNumber, 0 },
        { "Play ready",           MIDIMappingProto::kPlayReadyFieldNumber, 0 },
        { "Master speed locked",  MIDIMappingProto::kLockedFieldNumber, 0 },
        { "Master speed locked (blinking)",
	                          MIDIMappingProto::kLockedBlinkingFieldNumber, 0 },
        { "Cue in enabled",       MIDIMappingProto::kCueInEnabledFieldNumber, 0 },
        { "Cue out enabled",      MIDIMappingProto::kCueOutEnabledFieldNumber, 0 },
        { "Bank 1 is selected",   MIDIMappingProto::kBank1IsSelectedFieldNumber, 0 },
        { "Bank 2 is selected",   MIDIMappingProto::kBank2IsSelectedFieldNumber, 0 },
        { "Bank 3 is selected",   MIDIMappingProto::kBank3IsSelectedFieldNumber, 0 },
        { "Bank 4 is selected",   MIDIMappingProto::kBank4IsSelectedFieldNumber, 0 },
        { "Bank 5 is selected",   MIDIMappingProto::kBank5IsSelectedFieldNumber, 0 },
};

namespace {

int get_bank(const MIDIMappingProto &mapping_proto, int bank_field_number, int default_value)
{
	const FieldDescriptor *bank_descriptor = mapping_proto.GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = mapping_proto.GetReflection();
	if (!reflection->HasField(mapping_proto, bank_descriptor)) {
		return default_value;
	}
	return reflection->GetInt32(mapping_proto, bank_descriptor);
}

}  // namespace

MIDIMappingDialog::MIDIMappingDialog(MIDIMapper *mapper)
	: ui(new Ui::MIDIMappingDialog),
          mapper(mapper)
{
	ui->setupUi(this);

	const MIDIMappingProto mapping_proto = mapper->get_current_mapping();  // Take a copy.
	old_receiver = mapper->set_receiver(this);

	QStringList labels;
	labels << "";
	labels << "Controller bank";
	labels << "";
	labels << "";
	labels << "";
	labels << "";
	ui->treeWidget->setColumnCount(6);
	ui->treeWidget->setHeaderLabels(labels);

	vector<MIDIMappingDialog::Control> camera_select_buttons;
	vector<MIDIMappingDialog::Control> camera_is_selected_lights;
	for (size_t camera_idx = 0; camera_idx < MAX_STREAMS; ++camera_idx) {
		char str[256];
		snprintf(str, sizeof(str), "Switch to camera %zu", camera_idx + 1);
		camera_select_buttons.emplace_back(Control{ str, CameraMIDIMappingProto::kButtonFieldNumber, 0 });

		snprintf(str, sizeof(str), "Camera %zu is current", camera_idx + 1);
		camera_is_selected_lights.emplace_back(Control{ str, CameraMIDIMappingProto::kIsCurrentFieldNumber, 0 });
	}

	add_controls("Controllers",               ControlType::CONTROLLER,          mapping_proto, controllers);
	add_controls("Controller lights",         ControlType::CONTROLLER_LIGHT,    mapping_proto, controller_lights);
	add_controls("Buttons",                   ControlType::BUTTON,              mapping_proto, buttons);
	add_controls("Button lights",             ControlType::BUTTON_LIGHT,        mapping_proto, button_lights);
	add_controls("Camera select buttons",     ControlType::CAMERA_BUTTON,       mapping_proto, camera_select_buttons);
	add_controls("Camera is selected lights", ControlType::CAMERA_BUTTON_LIGHT, mapping_proto, camera_is_selected_lights);
	fill_controls_from_mapping(mapping_proto);

	// Auto-resize every column but the last.
	for (unsigned column_idx = 0; column_idx < 5; ++column_idx) {
		ui->treeWidget->resizeColumnToContents(column_idx);
	}

	connect(ui->ok_cancel_buttons, &QDialogButtonBox::accepted, this, &MIDIMappingDialog::ok_clicked);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::rejected, this, &MIDIMappingDialog::cancel_clicked);
	connect(ui->save_button, &QPushButton::clicked, this, &MIDIMappingDialog::save_clicked);
	connect(ui->load_button, &QPushButton::clicked, this, &MIDIMappingDialog::load_clicked);
}

MIDIMappingDialog::~MIDIMappingDialog()
{
	mapper->set_receiver(old_receiver);
}

void MIDIMappingDialog::ok_clicked()
{
	unique_ptr<MIDIMappingProto> new_mapping = construct_mapping_proto_from_ui();
	mapper->set_midi_mapping(*new_mapping);
	mapper->set_receiver(old_receiver);
	accept();
}

void MIDIMappingDialog::cancel_clicked()
{
	mapper->set_receiver(old_receiver);
	reject();
}

void MIDIMappingDialog::save_clicked()
{
	QFileDialog::Options options;
	unique_ptr<MIDIMappingProto> new_mapping = construct_mapping_proto_from_ui();
	QString filename = QFileDialog::getSaveFileName(this,
		"Save MIDI mapping", QString(), tr("Mapping files (*.midimapping)"), /*selectedFilter=*/nullptr, options);
	if (!filename.endsWith(".midimapping")) {
		filename += ".midimapping";
	}
	if (!save_midi_mapping_to_file(*new_mapping, filename.toStdString())) {
		QMessageBox box;
		box.setText("Could not save mapping to '" + filename + "'. Check that you have the right permissions and try again.");
		box.exec();
	}
}

void MIDIMappingDialog::load_clicked()
{
	QFileDialog::Options options;
	QString filename = QFileDialog::getOpenFileName(this,
		"Load MIDI mapping", QString(), tr("Mapping files (*.midimapping)"), /*selectedFilter=*/nullptr, options);
	MIDIMappingProto new_mapping;
	if (!load_midi_mapping_from_file(filename.toStdString(), &new_mapping)) {
		QMessageBox box;
		box.setText("Could not load mapping from '" + filename + "'. Check that the file exists, has the right permissions and is valid.");
		box.exec();
		return;
	}

	fill_controls_from_mapping(new_mapping);
}

namespace {

template<class T, class Proto>
T *get_mutable_message(Proto *proto, int field_number)
{
	const FieldDescriptor *descriptor = proto->GetDescriptor()->FindFieldByNumber(field_number);
	const Reflection *bus_reflection = proto->GetReflection();
	return static_cast<T *>(bus_reflection->MutableMessage(proto, descriptor));
}

}  // namespace

unique_ptr<MIDIMappingProto> MIDIMappingDialog::construct_mapping_proto_from_ui()
{
	unique_ptr<MIDIMappingProto> mapping_proto(new MIDIMappingProto);
	for (const InstantiatedSpinner &is : controller_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDIControllerProto *controller_proto =
			get_mutable_message<MIDIControllerProto>(mapping_proto.get(), is.field_number);
		controller_proto->set_controller_number(val);
	}
	for (const InstantiatedSpinner &is : controller_light_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDIControllerProto *controller_proto =
			get_mutable_message<MIDIControllerProto>(mapping_proto.get(), is.field_number);
		controller_proto->set_controller_number(val);

		// HACK: We only have one of these right now, so min/max is a given;
		// no need to store proto field numbers.
		int val2 = is.spinner2->value();
		if (val2 != -1) {
			mapping_proto->set_master_speed_light_min(val2);
		}
		int val3 = is.spinner3->value();
		if (val3 != -1) {
			mapping_proto->set_master_speed_light_max(val3);
		}
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDIButtonProto *button_proto =
			get_mutable_message<MIDIButtonProto>(mapping_proto.get(), is.field_number);
		button_proto->set_note_number(val);
	}
	for (const InstantiatedSpinner &is : button_light_spinners) {
		const int val = is.spinner->value();
		if (val == -1) {
			continue;
		}

		MIDILightProto *light_proto =
			get_mutable_message<MIDILightProto>(mapping_proto.get(), is.field_number);
		light_proto->set_note_number(val);

		int val2 = is.spinner2->value();
		if (val2 != -1) {
			light_proto->set_velocity(val2);
		}
	}
	int highest_bank_used = 0;  // 1-indexed.
	for (const InstantiatedComboBox &ic : bank_combo_boxes) {
		const int val = ic.combo_box->currentIndex();
		highest_bank_used = std::max(highest_bank_used, val);
		if (val == 0) {
			continue;
		}

		const FieldDescriptor *descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(ic.field_number);
		const Reflection *bus_reflection = mapping_proto->GetReflection();
		bus_reflection->SetInt32(mapping_proto.get(), descriptor, val - 1);
	}
	mapping_proto->set_num_controller_banks(highest_bank_used);

	size_t num_cameras_used = 0;
	for (size_t camera_idx = 0; camera_idx < MAX_STREAMS; ++camera_idx) {
		if (camera_button_spinners[camera_idx].spinner->value() != -1) {
			num_cameras_used = camera_idx + 1;
		} else if (camera_button_light_spinners[camera_idx].spinner->value() != -1) {
			num_cameras_used = camera_idx + 1;
		}
	}
	for (size_t camera_idx = 0; camera_idx < num_cameras_used; ++camera_idx) {
		CameraMIDIMappingProto *camera_proto = mapping_proto->add_camera();
	
		{	
			const InstantiatedSpinner &is = camera_button_spinners[camera_idx];
			MIDIButtonProto *button_proto =
				get_mutable_message<MIDIButtonProto>(camera_proto, is.field_number);
			int val = is.spinner->value();
			if (val != -1) {
				button_proto->set_note_number(val);
			}
		}
		{	
			const InstantiatedSpinner &is = camera_button_light_spinners[camera_idx];
			MIDILightProto *light_proto =
				get_mutable_message<MIDILightProto>(camera_proto, is.field_number);
			int val = is.spinner->value();
			if (val != -1) {
				light_proto->set_note_number(val);
			}

			int val2 = is.spinner2->value();
			if (val2 != -1) {
				light_proto->set_velocity(val2);
			}
		}
	}

	return mapping_proto;
}

void MIDIMappingDialog::add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number)
{
	if (bank_field_number == 0) {
		return;
	}
	QComboBox *bank_selector = new QComboBox(this);
	bank_selector->addItems(QStringList() << "" << "Bank 1" << "Bank 2" << "Bank 3" << "Bank 4" << "Bank 5");
	bank_selector->setAutoFillBackground(true);

	bank_combo_boxes.push_back(InstantiatedComboBox{ bank_selector, bank_field_number });

	ui->treeWidget->setItemWidget(item, 1, bank_selector);
}

void MIDIMappingDialog::add_controls(const string &heading,
                                     MIDIMappingDialog::ControlType control_type,
                                     const MIDIMappingProto &mapping_proto,
                                     const vector<MIDIMappingDialog::Control> &controls)
{
	QTreeWidgetItem *heading_item = new QTreeWidgetItem(ui->treeWidget);
	heading_item->setText(0, QString::fromStdString(heading));
	if (control_type == ControlType::BUTTON_LIGHT) {
		heading_item->setText(3, "Velocity");
	} else if (control_type == ControlType::CONTROLLER_LIGHT) {
		heading_item->setText(3, "Min");
		heading_item->setText(4, "Max");
	} else {
		heading_item->setFirstColumnSpanned(true);
	}
	heading_item->setExpanded(true);
	for (const Control &control : controls) {
		QTreeWidgetItem *item = new QTreeWidgetItem(heading_item);
		heading_item->addChild(item);
		add_bank_selector(item, mapping_proto, control.bank_field_number);
		item->setText(0, QString::fromStdString(control.label + "   "));

		QSpinBox *spinner;
		if (control_type == ControlType::CONTROLLER) {
			spinner = new ControllerSpinBox(this);
			spinner->setRange(-1, 128);  // 128 for pitch bend.
		} else {
			spinner = new QSpinBox(this);
			spinner->setRange(-1, 127);
		}
		spinner->setAutoFillBackground(true);
		spinner->setSpecialValueText("\u200d");  // Zero-width joiner (ie., empty).
		ui->treeWidget->setItemWidget(item, 2, spinner);

		if (control_type == ControlType::CONTROLLER) {
			controller_spinners.push_back(InstantiatedSpinner{ spinner, nullptr, nullptr, control.field_number });
		} else if (control_type == ControlType::CONTROLLER_LIGHT) {
			QSpinBox *spinner2 = new QSpinBox(this);
			spinner2->setRange(-1, 127);
			spinner2->setAutoFillBackground(true);
			spinner2->setSpecialValueText("\u200d");  // Zero-width joiner (ie., empty).

			QSpinBox *spinner3 = new QSpinBox(this);
			spinner3->setRange(-1, 127);
			spinner3->setAutoFillBackground(true);
			spinner3->setSpecialValueText("\u200d");  // Zero-width joiner (ie., empty).

			ui->treeWidget->setItemWidget(item, 3, spinner2);
			ui->treeWidget->setItemWidget(item, 4, spinner3);

			controller_light_spinners.push_back(InstantiatedSpinner{ spinner, spinner2, spinner3, control.field_number });
		} else if (control_type == ControlType::BUTTON) {
			button_spinners.push_back(InstantiatedSpinner{ spinner, nullptr, nullptr, control.field_number });
		} else if (control_type == ControlType::CAMERA_BUTTON) {
			camera_button_spinners.push_back(InstantiatedSpinner{ spinner, nullptr, nullptr, control.field_number });
		} else {
			assert(control_type == ControlType::BUTTON_LIGHT || control_type == ControlType::CAMERA_BUTTON_LIGHT);
			QSpinBox *spinner2 = new QSpinBox(this);
			spinner2->setRange(-1, 127);
			spinner2->setAutoFillBackground(true);
			spinner2->setSpecialValueText("\u200d");  // Zero-width joiner (ie., empty).
			ui->treeWidget->setItemWidget(item, 3, spinner2);
			if (control_type == ControlType::BUTTON_LIGHT) {
				button_light_spinners.push_back(InstantiatedSpinner{ spinner, spinner2, nullptr, control.field_number });
			} else {
				assert(control_type == ControlType::CAMERA_BUTTON_LIGHT);
				camera_button_light_spinners.push_back(InstantiatedSpinner{ spinner, spinner2, nullptr, control.field_number });
			}
		}
		spinners[control.field_number] = spinner;
	}
	ui->treeWidget->addTopLevelItem(heading_item);
}

void MIDIMappingDialog::fill_controls_from_mapping(const MIDIMappingProto &mapping_proto)
{
	for (const InstantiatedSpinner &is : controller_spinners) {
		is.spinner->setValue(get_controller_mapping_helper(mapping_proto, is.field_number, -1));
	}
	for (const InstantiatedSpinner &is : controller_light_spinners) {
		is.spinner->setValue(get_controller_mapping_helper(mapping_proto, is.field_number, -1));

		// HACK: We only have one of these right now, so min/max is a given;
		// no need to store proto field numbers.
		if (mapping_proto.has_master_speed_light_min()) {
			is.spinner2->setValue(mapping_proto.master_speed_light_min());
		}
		if (mapping_proto.has_master_speed_light_max()) {
			is.spinner3->setValue(mapping_proto.master_speed_light_max());
		}
	}
	for (const InstantiatedSpinner &is : button_spinners) {
		is.spinner->setValue(get_button_mapping_helper(mapping_proto, is.field_number, -1));
	}
	for (const InstantiatedSpinner &is : button_light_spinners) {
		MIDILightProto light_proto = get_light_mapping_helper(mapping_proto, is.field_number);
		if (light_proto.has_note_number()) {
			is.spinner->setValue(light_proto.note_number());
		} else {
			is.spinner->setValue(-1);
		}
		if (light_proto.has_velocity()) {
			is.spinner2->setValue(light_proto.velocity());
		} else {
			is.spinner2->setValue(-1);
		}
	}
	for (size_t camera_idx = 0; camera_idx < MAX_STREAMS; ++camera_idx) {
		CameraMIDIMappingProto camera_proto;
		if (camera_idx < size_t(mapping_proto.camera_size())) {
			camera_proto = mapping_proto.camera(camera_idx);
		}
		{
			const InstantiatedSpinner &is = camera_button_spinners[camera_idx];
			is.spinner->setValue(get_button_mapping_helper(camera_proto, is.field_number, -1));
		}
		{
			const InstantiatedSpinner &is = camera_button_light_spinners[camera_idx];
			const MIDILightProto &light_proto = get_light_mapping_helper(camera_proto, is.field_number);
			if (light_proto.has_note_number()) {
				is.spinner->setValue(light_proto.note_number());
			} else {
				is.spinner->setValue(-1);
			}
			if (light_proto.has_velocity()) {
				is.spinner2->setValue(light_proto.velocity());
			} else {
				is.spinner2->setValue(-1);
			}
		}
	}
	for (const InstantiatedComboBox &ic : bank_combo_boxes) {
		ic.combo_box->setCurrentIndex(get_bank(mapping_proto, ic.field_number, -1) + 1);
	}
}

void MIDIMappingDialog::controller_changed(unsigned controller)
{
	post_to_main_thread([=]{
		for (const InstantiatedSpinner &is : controller_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(controller);
				is.spinner->selectAll();
			}
		}
		for (const InstantiatedSpinner &is : controller_light_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(controller);
				is.spinner->selectAll();
			}
		}
	});
}

void MIDIMappingDialog::note_on(unsigned note)
{
	post_to_main_thread([=]{
		for (const InstantiatedSpinner &is : button_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(note);
				is.spinner->selectAll();
			}
		}
		for (const InstantiatedSpinner &is : button_light_spinners) {
			if (is.spinner->hasFocus()) {
				is.spinner->setValue(note);
				is.spinner->selectAll();
			}
		}
	});
}
