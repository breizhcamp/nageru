#ifndef _ABOUTDIALOG_H
#define _ABOUTDIALOG_H 1

#include <QDialog>
#include <QString>

#include <string>

class QObject;

namespace Ui {
class AboutDialog;
}  // namespace Ui

class AboutDialog : public QDialog
{
	Q_OBJECT

public:
	AboutDialog(const std::string &program, const std::string &subheading);

private:
	Ui::AboutDialog *ui;
};

#endif  // !defined(_ABOUTDIALOG_H)
