#include "mainwindow.h"

#include "clip_list.h"
#include "ui_mainwindow.h"

#include <string>
#include <vector>

using namespace std;

int ClipList::rowCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return clips.size();
}

int ClipList::columnCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return 7;
}

QVariant ClipList::data(const QModelIndex &parent, int role) const {
	if (!parent.isValid())
		return QVariant();
	if (role != Qt::DisplayRole)
		return QVariant();

	const int row = parent.row(), column = parent.column();
	if (size_t(row) >= clips.size())
		return QVariant();

	switch (column) {
	case 0:
		return qlonglong(clips[row].pts_in);
	case 1:
		if (clips[row].pts_out >= 0) {
			return qlonglong(clips[row].pts_out);
		} else {
			return QVariant();
		}
	case 2:
		if (clips[row].pts_out >= 0) {
			return qlonglong(clips[row].pts_out - clips[row].pts_in);
		} else {
			return QVariant();
		}
	default:
		return QVariant();
	}
}

QVariant ClipList::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role != Qt::DisplayRole)
		return QVariant();
	if (orientation != Qt::Horizontal)
		return QVariant();

	switch (section) {
	case 0:
		return "In";
	case 1:
		return "Out";
	case 2:
		return "Duration";
	case 3:
		return "Camera 1";
	case 4:
		return "Camera 2";
	case 5:
		return "Camera 3";
	case 6:
		return "Camera 4";
	default:
		return "";
	}
}

void ClipList::add_clip(const Clip &clip)
{
	beginInsertRows(QModelIndex(), clips.size(), clips.size());
	clips.push_back(clip);
	endInsertRows();
}

void ClipList::emit_data_changed(size_t row)
{
	emit dataChanged(index(row, 0), index(row, 6));
}
