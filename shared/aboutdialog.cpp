#include "aboutdialog.h"

#include <QDialogButtonBox>

#include "ui_aboutdialog.h"

using namespace std;

AboutDialog::AboutDialog(const string &program, const string &subheading)
	: ui(new Ui::AboutDialog)
{
	ui->setupUi(this);
	QString str = ui->header->text();
	str.replace("@NAGERU_VERSION@", NAGERU_VERSION);
	str.replace("@PROGRAM@", QString::fromStdString(program));
	str.replace("@SUBHEADING@", QString::fromStdString(subheading));
	ui->header->setText(str);

	connect(ui->button_box, &QDialogButtonBox::accepted, [this]{ this->close(); });
}

