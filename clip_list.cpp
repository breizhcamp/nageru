#include "mainwindow.h"

#include <math.h>
#include <string>
#include <vector>

#include "clip_list.h"
#include "timebase.h"
#include "ui_mainwindow.h"

using namespace std;

string pts_to_string(int64_t pts)
{
	int64_t t = lrint((pts / double(TIMEBASE)) * 1e3);  // In milliseconds.
	int ms = t % 1000;
	t /= 1000;
	int sec = t % 60;
	t /= 60;
	int min = t % 60;
	t /= 60;
	int hour = t;

	char buf[256];
	snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", hour, min, sec, ms);
	return buf;
}

string duration_to_string(int64_t pts_diff)
{
	int64_t t = lrint((pts_diff / double(TIMEBASE)) * 1e3);  // In milliseconds.
	int ms = t % 1000;
	t /= 1000;
	int sec = t % 60;
	t /= 60;
	int min = t;

	char buf[256];
	snprintf(buf, sizeof(buf), "%d:%02d.%03d", min, sec, ms);
	return buf;
}

int ClipList::rowCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return clips.size();
}

int PlayList::rowCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return clips.size();
}

int ClipList::columnCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return int(Column::NUM_COLUMNS);
}

int PlayList::columnCount(const QModelIndex &parent) const {
	if (parent.isValid()) return 0;
	return int(Column::NUM_COLUMNS);
}

QVariant ClipList::data(const QModelIndex &parent, int role) const {
	if (!parent.isValid())
		return QVariant();
	const int row = parent.row(), column = parent.column();
	if (size_t(row) >= clips.size())
		return QVariant();

	if (role == Qt::TextAlignmentRole) {
		switch (Column(column)) {
		case Column::IN:
		case Column::OUT:
		case Column::DURATION:
			return Qt::AlignRight + Qt::AlignVCenter;
		default:
			return Qt::AlignLeft + Qt::AlignVCenter;
		}
	}

	if (role != Qt::DisplayRole && role != Qt::EditRole)
		return QVariant();

	switch (Column(column)) {
	case Column::IN:
		return QString::fromStdString(pts_to_string(clips[row].pts_in));
	case Column::OUT:
		if (clips[row].pts_out >= 0) {
			return QString::fromStdString(pts_to_string(clips[row].pts_out));
		} else {
			return QVariant();
		}
	case Column::DURATION:
		if (clips[row].pts_out >= 0) {
			return QString::fromStdString(duration_to_string(clips[row].pts_out - clips[row].pts_in));
		} else {
			return QVariant();
		}
	case Column::CAMERA_1:
	case Column::CAMERA_2:
	case Column::CAMERA_3:
	case Column::CAMERA_4: {
		unsigned stream_idx = column - int(Column::CAMERA_1);
		return QString::fromStdString(clips[row].descriptions[stream_idx]);
	}
	default:
		return "";
	}
}

QVariant PlayList::data(const QModelIndex &parent, int role) const {
	if (!parent.isValid())
		return QVariant();
	const int row = parent.row(), column = parent.column();
	if (size_t(row) >= clips.size())
		return QVariant();

	if (role == Qt::TextAlignmentRole) {
		switch (Column(column)) {
		case Column::PLAYING:
			return Qt::AlignCenter;
		case Column::IN:
		case Column::OUT:
		case Column::DURATION:
			return Qt::AlignRight + Qt::AlignVCenter;
		case Column::CAMERA:
			return Qt::AlignCenter;
		default:
			return Qt::AlignLeft + Qt::AlignVCenter;
		}
	}
	if (role == Qt::BackgroundRole) {
		if (Column(column) == Column::PLAYING) {
			if (row == currently_playing_index) {
				// This only really works well for the first column, for whatever odd Qt reason.
				QLinearGradient grad(QPointF(0, 0), QPointF(1, 0));
				grad.setCoordinateMode(grad.QGradient::ObjectBoundingMode);
				grad.setColorAt(0.0f, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.2f));
				grad.setColorAt(play_progress, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.2f));
				grad.setColorAt(play_progress + 0.01f, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.0f));
				return QBrush(grad);
			} else {
				return QVariant();
			}
		} else {
			return QVariant();
		}
	}

	if (role != Qt::DisplayRole && role != Qt::EditRole)
		return QVariant();

	switch (Column(column)) {
	case Column::PLAYING:
		return (row == currently_playing_index) ? "→" : "";
	case Column::IN:
		return QString::fromStdString(pts_to_string(clips[row].pts_in));
	case Column::OUT:
		if (clips[row].pts_out >= 0) {
			return QString::fromStdString(pts_to_string(clips[row].pts_out));
		} else {
			return QVariant();
		}
	case Column::DURATION:
		if (clips[row].pts_out >= 0) {
			return QString::fromStdString(duration_to_string(clips[row].pts_out - clips[row].pts_in));
		} else {
			return QVariant();
		}
	case Column::CAMERA:
		return qlonglong(clips[row].stream_idx + 1);
	case Column::DESCRIPTION:
		return QString::fromStdString(clips[row].descriptions[clips[row].stream_idx]);
	default:
		return "";
	}
}

QVariant ClipList::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role != Qt::DisplayRole)
		return QVariant();
	if (orientation != Qt::Horizontal)
		return QVariant();

	switch (Column(section)) {
	case Column::IN:
		return "In";
	case Column::OUT:
		return "Out";
	case Column::DURATION:
		return "Duration";
	case Column::CAMERA_1:
		return "Camera 1";
	case Column::CAMERA_2:
		return "Camera 2";
	case Column::CAMERA_3:
		return "Camera 3";
	case Column::CAMERA_4:
		return "Camera 4";
	default:
		return "";
	}
}

QVariant PlayList::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role != Qt::DisplayRole)
		return QVariant();
	if (orientation != Qt::Horizontal)
		return QVariant();

	switch (Column(section)) {
	case Column::PLAYING:
		return "";
	case Column::IN:
		return "In";
	case Column::OUT:
		return "Out";
	case Column::DURATION:
		return "Duration";
	case Column::CAMERA:
		return "Camera";
	case Column::DESCRIPTION:
		return "Description";
	default:
		return "";
	}
}

Qt::ItemFlags ClipList::flags(const QModelIndex &index) const
{
	if (!index.isValid())
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	const int row = index.row(), column = index.column();
	if (size_t(row) >= clips.size())
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;

	switch (Column(column)) {
	case Column::CAMERA_1:
	case Column::CAMERA_2:
	case Column::CAMERA_3:
	case Column::CAMERA_4:
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
	default:
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	}
}

Qt::ItemFlags PlayList::flags(const QModelIndex &index) const
{
	if (!index.isValid())
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	const int row = index.row(), column = index.column();
	if (size_t(row) >= clips.size())
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;

	switch (Column(column)) {
	case Column::DESCRIPTION:
	case Column::CAMERA:
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
	default:
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	}
}

bool ClipList::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (!index.isValid() || role != Qt::EditRole) {
		return false;
	}

	const int row = index.row(), column = index.column();
	if (size_t(row) >= clips.size())
		return false;

	switch (Column(column)) {
	case Column::CAMERA_1:
	case Column::CAMERA_2:
	case Column::CAMERA_3:
	case Column::CAMERA_4: {
		unsigned stream_idx = column - int(Column::CAMERA_1);
		clips[row].descriptions[stream_idx] = value.toString().toStdString();
		emit_data_changed(row);
		return true;
	}
	default:
		return false;
	}
}

bool PlayList::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (!index.isValid() || role != Qt::EditRole) {
		return false;
	}

	const int row = index.row(), column = index.column();
	if (size_t(row) >= clips.size())
		return false;

	switch (Column(column)) {
	case Column::DESCRIPTION:
		clips[row].descriptions[clips[row].stream_idx] = value.toString().toStdString();
		emit_data_changed(row);
		return true;
	case Column::CAMERA: {
		bool ok;
		int camera_idx = value.toInt(&ok);
		if (!ok || camera_idx < 1 || camera_idx > NUM_CAMERAS) {
			return false;
		}
		clips[row].stream_idx = camera_idx - 1;
		emit_data_changed(row);
		return true;
	}
	default:
		return false;
	}
}

void ClipList::add_clip(const Clip &clip)
{
	beginInsertRows(QModelIndex(), clips.size(), clips.size());
	clips.push_back(clip);
	endInsertRows();
}

void PlayList::add_clip(const Clip &clip)
{
	beginInsertRows(QModelIndex(), clips.size(), clips.size());
	clips.push_back(clip);
	endInsertRows();
}

void PlayList::duplicate_clips(size_t first, size_t last)
{
	beginInsertRows(QModelIndex(), first, last);
	clips.insert(clips.begin() + first, clips.begin() + first, clips.begin() + last + 1);
	endInsertRows();
}

void PlayList::erase_clips(size_t first, size_t last)
{
	beginRemoveRows(QModelIndex(), first, last);
	clips.erase(clips.begin() + first, clips.begin() + last + 1);
	endRemoveRows();
}

void PlayList::move_clips(size_t first, size_t last, int delta)
{
	if (delta == -1) {
		beginMoveRows(QModelIndex(), first, last, QModelIndex(), first - 1);
		rotate(clips.begin() + first - 1, clips.begin() + first, clips.begin() + last + 1);
	} else {
		beginMoveRows(QModelIndex(), first, last, QModelIndex(), first + (last-first+1) + 1);
		first = clips.size() - first - 1;
		last = clips.size() - last - 1;
		rotate(clips.rbegin() + last - 1, clips.rbegin() + last, clips.rbegin() + first + 1);
	}
	endMoveRows();
}

void ClipList::emit_data_changed(size_t row)
{
	emit dataChanged(index(row, 0), index(row, int(Column::NUM_COLUMNS)));
}

void PlayList::emit_data_changed(size_t row)
{
	emit dataChanged(index(row, 0), index(row, int(Column::NUM_COLUMNS)));
}

void PlayList::set_currently_playing(int index, double progress)
{
	int old_index = currently_playing_index;
	int column = int(Column::PLAYING);
	if (index != old_index) {
		currently_playing_index = index;
		play_progress = progress;
		if (old_index != -1) {
			emit dataChanged(this->index(old_index, column), this->index(old_index, column));
		}
		if (index != -1) {
			emit dataChanged(this->index(index, column), this->index(index, column));
		}
	} else if (index != -1 && fabs(progress - play_progress) > 1e-3) {
		play_progress = progress;
		emit dataChanged(this->index(index, column), this->index(index, column));
	}
}
