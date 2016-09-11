#ifndef _INPUT_MAPPING_DIALOG_H
#define _INPUT_MAPPING_DIALOG_H

#include <QDialog>
#include <string>
#include <vector>
#include <sys/time.h>

#include "audio_mixer.h"
#include "mixer.h"

namespace Ui {
class InputMappingDialog;
}  // namespace Ui

class QComboBox;

class InputMappingDialog : public QDialog
{
	Q_OBJECT

public:
	InputMappingDialog();
	~InputMappingDialog();

private:
	void fill_ui_from_mapping(const InputMapping &mapping);
	void fill_row_from_bus(unsigned row, const InputMapping::Bus &bus);
	void setup_channel_choices_from_bus(unsigned row, const InputMapping::Bus &bus);
	void cell_changed(int row, int column);
	void card_selected(QComboBox *card_combo, unsigned row, int index);
	void channel_selected(unsigned row, unsigned channel, int index);
	void ok_clicked();
	void cancel_clicked();
	void add_clicked();
	void remove_clicked();
	void updown_clicked(int direction);
	void update_button_state();

	Ui::InputMappingDialog *ui;
	InputMapping mapping;  // Under edit. Will be committed on OK.

	// The old mapping. Will be re-committed on cancel, so that we
	// unhold all the unused devices (otherwise they would be
	// held forever).
	InputMapping old_mapping;

	std::map<DeviceSpec, DeviceInfo> devices;

	AudioMixer::state_changed_callback_t saved_callback;
};

#endif  // !defined(_INPUT_MAPPING_DIALOG_H)
