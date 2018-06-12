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
	if (display_type == ListDisplay::CLIP_LIST) {
		return int(ClipListColumn::NUM_COLUMNS);
	} else {
		return int(PlayListColumn::NUM_COLUMNS);
	}
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

	if (display_type == ListDisplay::CLIP_LIST) {
		switch (ClipListColumn(section)) {
		case ClipListColumn::IN:
			return "In";
		case ClipListColumn::OUT:
			return "Out";
		case ClipListColumn::DURATION:
			return "Duration";
		case ClipListColumn::CAMERA_1:
			return "Camera 1";
		case ClipListColumn::CAMERA_2:
			return "Camera 2";
		case ClipListColumn::CAMERA_3:
			return "Camera 3";
		case ClipListColumn::CAMERA_4:
			return "Camera 4";
		default:
			return "";
		}
	} else {
		switch (PlayListColumn(section)) {
		case PlayListColumn::IN:
			return "In";
		case PlayListColumn::OUT:
			return "Out";
		case PlayListColumn::DURATION:
			return "Duration";
		case PlayListColumn::CAMERA:
			return "Camera";
		case PlayListColumn::DESCRIPTION:
			return "Description";
		default:
			return "";
		}
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
	if (display_type == ListDisplay::CLIP_LIST) {
		emit dataChanged(index(row, 0), index(row, int(ClipListColumn::NUM_COLUMNS)));
	} else {
		emit dataChanged(index(row, 0), index(row, int(PlayListColumn::NUM_COLUMNS)));
	}
}
