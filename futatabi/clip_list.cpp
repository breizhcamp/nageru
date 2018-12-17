#include "clip_list.h"

#include "mainwindow.h"
#include "shared/timebase.h"
#include "ui_mainwindow.h"

#include <math.h>
#include <string>
#include <vector>

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

int ClipList::rowCount(const QModelIndex &parent) const
{
	if (parent.isValid())
		return 0;
	return clips.size();
}

int PlayList::rowCount(const QModelIndex &parent) const
{
	if (parent.isValid())
		return 0;
	return clips.size();
}

int ClipList::columnCount(const QModelIndex &parent) const
{
	if (parent.isValid())
		return 0;
	return int(Column::NUM_NON_CAMERA_COLUMNS) + num_cameras;
}

int PlayList::columnCount(const QModelIndex &parent) const
{
	if (parent.isValid())
		return 0;
	return int(Column::NUM_COLUMNS);
}

QVariant ClipList::data(const QModelIndex &parent, int role) const
{
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
	default:
		if (is_camera_column(column)) {
			unsigned stream_idx = column - int(Column::CAMERA_1);
			return QString::fromStdString(clips[row].descriptions[stream_idx]);
		} else {
			return "";
		}
	}
}

QVariant PlayList::data(const QModelIndex &parent, int role) const
{
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
		case Column::FADE_TIME:
			return Qt::AlignRight + Qt::AlignVCenter;
		case Column::CAMERA:
			return Qt::AlignCenter;
		default:
			return Qt::AlignLeft + Qt::AlignVCenter;
		}
	}
	if (role == Qt::BackgroundRole) {
		if (Column(column) == Column::PLAYING) {
			auto it = current_progress.find(row);
			if (it != current_progress.end()) {
				double play_progress = it->second;

				// This only really works well for the first column, for whatever odd Qt reason.
				QLinearGradient grad(QPointF(0, 0), QPointF(1, 0));
				grad.setCoordinateMode(grad.QGradient::ObjectBoundingMode);
				grad.setColorAt(0.0f, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.2f));
				grad.setColorAt(play_progress, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.2f));
				if (play_progress + 0.01f <= 1.0f) {
					grad.setColorAt(play_progress + 0.01f, QColor::fromRgbF(0.0f, 0.0f, 1.0f, 0.0f));
				}
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
		return current_progress.count(row) ? "→" : "";
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
	case Column::FADE_TIME: {
		stringstream ss;
		ss.imbue(locale("C"));
		ss.precision(3);
		ss << fixed << clips[row].fade_time_seconds;
		return QString::fromStdString(ss.str());
	}
	default:
		return "";
	}
}

QVariant ClipList::headerData(int section, Qt::Orientation orientation, int role) const
{
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
	default:
		if (section >= int(Column::CAMERA_1) && section < int(Column::CAMERA_1) + num_cameras) {
			return QString::fromStdString("Camera " + to_string(section - int(Column::CAMERA_1) + 1));
		} else {
			return "";
		}
	}
}

QVariant PlayList::headerData(int section, Qt::Orientation orientation, int role) const
{
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
	case Column::FADE_TIME:
		return "Fade time";
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

	if (is_camera_column(column)) {
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
	} else {
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
	case Column::FADE_TIME:
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

	if (is_camera_column(column)) {
		unsigned stream_idx = column - int(Column::CAMERA_1);
		clips[row].descriptions[stream_idx] = value.toString().toStdString();
		emit_data_changed(row);
		return true;
	} else {
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
		if (!ok || camera_idx < 1 || camera_idx > num_cameras) {
			return false;
		}
		clips[row].stream_idx = camera_idx - 1;
		emit_data_changed(row);
		return true;
	}
	case Column::FADE_TIME: {
		bool ok;
		double val = value.toDouble(&ok);
		if (!ok || !(val >= 0.0)) {
			return false;
		}
		clips[row].fade_time_seconds = val;
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
	emit any_content_changed();
}

void PlayList::add_clip(const Clip &clip)
{
	beginInsertRows(QModelIndex(), clips.size(), clips.size());
	clips.push_back(clip);
	endInsertRows();
	emit any_content_changed();
}

void PlayList::duplicate_clips(size_t first, size_t last)
{
	beginInsertRows(QModelIndex(), first, last);
	clips.insert(clips.begin() + first, clips.begin() + first, clips.begin() + last + 1);
	endInsertRows();
	emit any_content_changed();
}

void PlayList::erase_clips(size_t first, size_t last)
{
	beginRemoveRows(QModelIndex(), first, last);
	clips.erase(clips.begin() + first, clips.begin() + last + 1);
	endRemoveRows();
	emit any_content_changed();
}

void PlayList::move_clips(size_t first, size_t last, int delta)
{
	if (delta == -1) {
		beginMoveRows(QModelIndex(), first, last, QModelIndex(), first - 1);
		rotate(clips.begin() + first - 1, clips.begin() + first, clips.begin() + last + 1);
	} else {
		beginMoveRows(QModelIndex(), first, last, QModelIndex(), first + (last - first + 1) + 1);
		first = clips.size() - first - 1;
		last = clips.size() - last - 1;
		rotate(clips.rbegin() + last - 1, clips.rbegin() + last, clips.rbegin() + first + 1);
	}
	endMoveRows();
	emit any_content_changed();
}

void ClipList::emit_data_changed(size_t row)
{
	emit dataChanged(index(row, 0), index(row, int(Column::NUM_NON_CAMERA_COLUMNS) + num_cameras));
	emit any_content_changed();
}

void PlayList::emit_data_changed(size_t row)
{
	emit dataChanged(index(row, 0), index(row, int(Column::NUM_COLUMNS)));
	emit any_content_changed();
}

void ClipList::change_num_cameras(size_t num_cameras)
{
	assert(num_cameras >= this->num_cameras);
	if (num_cameras == this->num_cameras) {
		return;
	}

	beginInsertColumns(QModelIndex(), int(Column::NUM_NON_CAMERA_COLUMNS) + this->num_cameras, int(Column::NUM_NON_CAMERA_COLUMNS) + num_cameras - 1);
	this->num_cameras = num_cameras;
	endInsertColumns();
	emit any_content_changed();
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

void PlayList::set_progress(const map<size_t, double> &progress)
{
	const int column = int(Column::PLAYING);
	map<size_t, double> old_progress = move(this->current_progress);
	this->current_progress = progress;

	for (auto it : old_progress) {
		size_t index = it.first;
		if (current_progress.count(index) == 0) {
			emit dataChanged(this->index(index, column), this->index(index, column));
		}
	}
	for (auto it : current_progress) {
		size_t index = it.first;
		emit dataChanged(this->index(index, column), this->index(index, column));
	}
}

namespace {

Clip deserialize_clip(const ClipProto &clip_proto)
{
	Clip clip;
	clip.pts_in = clip_proto.pts_in();
	clip.pts_out = clip_proto.pts_out();
	for (int camera_idx = 0; camera_idx < min(clip_proto.description_size(), MAX_STREAMS); ++camera_idx) {
		clip.descriptions[camera_idx] = clip_proto.description(camera_idx);
	}
	clip.stream_idx = clip_proto.stream_idx();
	clip.fade_time_seconds = clip_proto.fade_time_seconds();
	return clip;
}

void serialize_clip(const Clip &clip, ClipProto *clip_proto)
{
	clip_proto->set_pts_in(clip.pts_in);
	clip_proto->set_pts_out(clip.pts_out);
	for (int camera_idx = 0; camera_idx < MAX_STREAMS; ++camera_idx) {
		*clip_proto->add_description() = clip.descriptions[camera_idx];
	}
	clip_proto->set_stream_idx(clip.stream_idx);
	clip_proto->set_fade_time_seconds(clip.fade_time_seconds);
}

}  // namespace

ClipList::ClipList(const ClipListProto &serialized)
{
	for (const ClipProto &clip_proto : serialized.clip()) {
		clips.push_back(deserialize_clip(clip_proto));
	}
}

ClipListProto ClipList::serialize() const
{
	ClipListProto ret;
	for (const Clip &clip : clips) {
		serialize_clip(clip, ret.add_clip());
	}
	return ret;
}

PlayList::PlayList(const ClipListProto &serialized)
{
	for (const ClipProto &clip_proto : serialized.clip()) {
		clips.push_back(deserialize_clip(clip_proto));
	}
}

ClipListProto PlayList::serialize() const
{
	ClipListProto ret;
	for (const Clip &clip : clips) {
		serialize_clip(clip, ret.add_clip());
	}
	return ret;
}
