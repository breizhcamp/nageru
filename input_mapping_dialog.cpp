#include "input_mapping_dialog.h"

#include "ui_input_mapping.h"

#include <QComboBox>

using namespace std;
using namespace std::placeholders;

InputMappingDialog::InputMappingDialog()
	: ui(new Ui::InputMappingDialog),
	  mapping(global_audio_mixer->get_input_mapping()),
	  old_mapping(mapping),
	  devices(global_audio_mixer->get_devices())
{
	ui->setupUi(this);
	ui->table->setSelectionBehavior(QAbstractItemView::SelectRows);
	ui->table->setSelectionMode(QAbstractItemView::SingleSelection);  // Makes implementing moving easier for now.

	fill_ui_from_mapping(mapping);
	connect(ui->table, &QTableWidget::cellChanged, this, &InputMappingDialog::cell_changed);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::accepted, this, &InputMappingDialog::ok_clicked);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::rejected, this, &InputMappingDialog::cancel_clicked);
	connect(ui->add_button, &QPushButton::clicked, this, &InputMappingDialog::add_clicked);
	connect(ui->remove_button, &QPushButton::clicked, this, &InputMappingDialog::remove_clicked);
	connect(ui->up_button, &QPushButton::clicked, bind(&InputMappingDialog::updown_clicked, this, -1));
	connect(ui->down_button, &QPushButton::clicked, bind(&InputMappingDialog::updown_clicked, this, 1));

	update_button_state();
	connect(ui->table, &QTableWidget::itemSelectionChanged, this, &InputMappingDialog::update_button_state);
}

void InputMappingDialog::fill_ui_from_mapping(const InputMapping &mapping)
{
	ui->table->verticalHeader()->hide();
	ui->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionsClickable(false);

	ui->table->setRowCount(mapping.buses.size());
	for (unsigned row = 0; row < mapping.buses.size(); ++row) {
		fill_row_from_bus(row, mapping.buses[row]);
	}
}

void InputMappingDialog::fill_row_from_bus(unsigned row, const InputMapping::Bus &bus)
{
	QString name(QString::fromStdString(bus.name));
	ui->table->setItem(row, 0, new QTableWidgetItem(name));

	// Card choices.
	QComboBox *card_combo = new QComboBox;
	unsigned current_index = 0;
	card_combo->addItem(QString("(none)   "));
	for (const auto &spec_and_info : devices) {
		++current_index;
		card_combo->addItem(
			QString::fromStdString(spec_and_info.second.name + "   "),
			qulonglong(DeviceSpec_to_key(spec_and_info.first)));
		if (bus.device == spec_and_info.first) {
			card_combo->setCurrentIndex(current_index);
		}
	}
	connect(card_combo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		bind(&InputMappingDialog::card_selected, this, card_combo, row, _1));
	ui->table->setCellWidget(row, 1, card_combo);

	setup_channel_choices_from_bus(row, bus);
}

void InputMappingDialog::setup_channel_choices_from_bus(unsigned row, const InputMapping::Bus &bus)
{
	// Left and right channel.
	for (unsigned channel = 0; channel < 2; ++channel) {
		QComboBox *channel_combo = new QComboBox;
		channel_combo->addItem(QString("(none)"));
		if (bus.device.type == InputSourceType::CAPTURE_CARD ||
		    bus.device.type == InputSourceType::ALSA_INPUT) {
			auto device_it = devices.find(bus.device);
			assert(device_it != devices.end());
			unsigned num_device_channels = device_it->second.num_channels;
			for (unsigned source = 0; source < num_device_channels; ++source) {
				char buf[256];
				snprintf(buf, sizeof(buf), "Channel %u   ", source + 1);
				channel_combo->addItem(QString(buf));
			}
			channel_combo->setCurrentIndex(bus.source_channel[channel] + 1);
		} else {
			channel_combo->setCurrentIndex(0);
		}
		connect(channel_combo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		        bind(&InputMappingDialog::channel_selected, this, row, channel, _1));
		ui->table->setCellWidget(row, 2 + channel, channel_combo);
	}
}

void InputMappingDialog::ok_clicked()
{
	global_audio_mixer->set_input_mapping(mapping);
	accept();
}

void InputMappingDialog::cancel_clicked()
{
	global_audio_mixer->set_input_mapping(old_mapping);
	reject();
}

void InputMappingDialog::cell_changed(int row, int column)
{
	if (column != 0) {
		// Spurious; only really the name column should fire these.
		return;
	}
	mapping.buses[row].name = ui->table->item(row, column)->text().toStdString();
}

void InputMappingDialog::card_selected(QComboBox *card_combo, unsigned row, int index)
{
	uint64_t key = card_combo->itemData(index).toULongLong();
	mapping.buses[row].device = key_to_DeviceSpec(key);
	setup_channel_choices_from_bus(row, mapping.buses[row]);
}

void InputMappingDialog::channel_selected(unsigned row, unsigned channel, int index)
{
	mapping.buses[row].source_channel[channel] = index - 1;
}

void InputMappingDialog::add_clicked()
{
	QTableWidgetSelectionRange all(0, 0, ui->table->rowCount() - 1, ui->table->columnCount() - 1);
	ui->table->setRangeSelected(all, false);

	InputMapping::Bus new_bus;
	new_bus.name = "New input";
	new_bus.device.type = InputSourceType::SILENCE;
	mapping.buses.push_back(new_bus);
	ui->table->setRowCount(mapping.buses.size());

	unsigned row = mapping.buses.size() - 1;
	fill_row_from_bus(row, new_bus);
	ui->table->editItem(ui->table->item(row, 0));  // Start editing the name.
	update_button_state();
}

void InputMappingDialog::remove_clicked()
{
	assert(ui->table->rowCount() != 0);

	set<int, greater<int>> rows_to_delete;  // Need to remove in reverse order.
	for (const QTableWidgetSelectionRange &range : ui->table->selectedRanges()) {
		for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
			rows_to_delete.insert(row);
		}
	}
	if (rows_to_delete.empty()) {
		rows_to_delete.insert(ui->table->rowCount() - 1);
	}

	for (int row : rows_to_delete) {
		ui->table->removeRow(row);
		mapping.buses.erase(mapping.buses.begin() + row);
	}
	update_button_state();
}

void InputMappingDialog::updown_clicked(int direction)
{
	assert(ui->table->selectedRanges().size() == 1);
	const QTableWidgetSelectionRange &range = ui->table->selectedRanges()[0];
	int a_row = range.bottomRow();
	int b_row = range.bottomRow() + direction;

	swap(mapping.buses[a_row], mapping.buses[b_row]);
	fill_row_from_bus(a_row, mapping.buses[a_row]);
	fill_row_from_bus(b_row, mapping.buses[b_row]);

	QTableWidgetSelectionRange a_sel(a_row, 0, a_row, ui->table->columnCount() - 1);
	QTableWidgetSelectionRange b_sel(b_row, 0, b_row, ui->table->columnCount() - 1);
	ui->table->setRangeSelected(a_sel, false);
	ui->table->setRangeSelected(b_sel, true);
}

void InputMappingDialog::update_button_state()
{
	ui->add_button->setDisabled(mapping.buses.size() >= MAX_BUSES);
	ui->remove_button->setDisabled(mapping.buses.size() == 0);
	ui->up_button->setDisabled(
		ui->table->selectedRanges().empty() ||
		ui->table->selectedRanges()[0].bottomRow() == 0);
	ui->down_button->setDisabled(
		ui->table->selectedRanges().empty() ||
		ui->table->selectedRanges()[0].bottomRow() == ui->table->rowCount() - 1);
}
