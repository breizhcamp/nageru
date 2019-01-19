#ifndef _CONTROLLER_SPIN_BOX_H
#define _CONTROLLER_SPIN_BOX_H 1

// ControllerSpinBox is like QSpinBox, except it has a second special value
// "PB" (in addition to the standard minimum value of -1, representing blank),
// representing the virtual pitch bend controller.

#include <QSpinBox>
#include <QString>

#include "shared/midi_device.h"

class ControllerSpinBox : public QSpinBox {
	Q_OBJECT

public:
	ControllerSpinBox(QWidget *parent) : QSpinBox(parent) {}

	int valueFromText(const QString &text) const override
	{
		if (text == "PB") {
			return MIDIReceiver::PITCH_BEND_CONTROLLER;
		} else {
			return QSpinBox::valueFromText(text);
		}
	}

	QString textFromValue(int value) const override
	{
		if (value == MIDIReceiver::PITCH_BEND_CONTROLLER) {
			return "PB";
		} else {
			return QSpinBox::textFromValue(value);
		}
	}

	QValidator::State validate(QString &input, int &pos) const override
	{
		if (input == "PB") {
			return QValidator::Acceptable;
		} else {
			return QSpinBox::validate(input, pos);
		}
	}
};

#endif  // !defined(_CONTROLLER_SPIN_BOX_H)
