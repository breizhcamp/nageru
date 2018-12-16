#ifndef _CLIP_LIST_H
#define _CLIP_LIST_H 1

#include "defs.h"
#include "state.pb.h"

#include <QAbstractTableModel>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

struct Clip {
	int64_t pts_in = -1, pts_out = -1;  // pts_in is inclusive, pts_out is exclusive.
	std::string descriptions[NUM_CAMERAS];
	unsigned stream_idx = 0;  // For the playlist only.
	double fade_time_seconds = 0.5;  // For the playlist only.
};

class DataChangedReceiver {
public:
	virtual ~DataChangedReceiver() {}
	virtual void emit_data_changed(size_t row) = 0;
};

// Like a smart pointer to a Clip, but emits dataChanged when it goes out of scope.
struct ClipProxy {
public:
	ClipProxy(Clip &clip, DataChangedReceiver *clip_list, size_t row)
		: clip(clip), clip_list(clip_list), row(row) {}
	~ClipProxy()
	{
		if (clip_list != nullptr) {
			clip_list->emit_data_changed(row);
		}
	}
	Clip *operator->() { return &clip; }
	Clip &operator*() { return clip; }

private:
	Clip &clip;
	DataChangedReceiver *clip_list;
	size_t row;
};

class ClipList : public QAbstractTableModel, public DataChangedReceiver {
	Q_OBJECT

public:
	explicit ClipList(const ClipListProto &serialized);

	enum class Column {
		IN,
		OUT,
		DURATION,
		CAMERA_1,  // Then CAMERA_2, CAMERA_3, etc. as needed.
		NUM_NON_CAMERA_COLUMNS = CAMERA_1
	};

	int rowCount(const QModelIndex &parent) const override;
	int columnCount(const QModelIndex &parent) const override;
	QVariant data(const QModelIndex &parent, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

	void add_clip(const Clip &clip);
	size_t size() const { return clips.size(); }
	bool empty() const { return clips.empty(); }

	ClipProxy mutable_clip(size_t index) { return ClipProxy(clips[index], this, index); }
	const Clip *clip(size_t index) const { return &clips[index]; }

	ClipProxy mutable_back() { return mutable_clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	ClipListProto serialize() const;

	void emit_data_changed(size_t row) override;

	static bool is_camera_column(int column)
	{
		return (column >= int(Column::CAMERA_1) && column < int(Column::CAMERA_1) + NUM_CAMERAS);
	}

signals:
	void any_content_changed();

private:
	std::vector<Clip> clips;
};

class PlayList : public QAbstractTableModel, public DataChangedReceiver {
	Q_OBJECT

public:
	explicit PlayList(const ClipListProto &serialized);

	enum class Column {
		PLAYING,
		IN,
		OUT,
		DURATION,
		CAMERA,
		DESCRIPTION,
		FADE_TIME,
		NUM_COLUMNS
	};

	int rowCount(const QModelIndex &parent) const override;
	int columnCount(const QModelIndex &parent) const override;
	QVariant data(const QModelIndex &parent, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

	void add_clip(const Clip &clip);

	// <last> is inclusive in all of these.
	void duplicate_clips(size_t first, size_t last);
	void erase_clips(size_t first, size_t last);
	// <delta> is -1 to move upwards, +1 to move downwards.
	void move_clips(size_t first, size_t last, int delta);

	size_t size() const { return clips.size(); }
	bool empty() const { return clips.empty(); }

	ClipProxy mutable_clip(size_t index) { return ClipProxy(clips[index], this, index); }
	const Clip *clip(size_t index) const { return &clips[index]; }

	ClipProxy mutable_back() { return mutable_clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	// TODO: Move these out of PlayList.
	void set_currently_playing(int index, double progress);  // -1 = none.
	int get_currently_playing() const { return currently_playing_index; }

	void set_progress(const std::map<size_t, double> &progress);

	ClipListProto serialize() const;

	void emit_data_changed(size_t row) override;

signals:
	void any_content_changed();

private:
	std::vector<Clip> clips;
	int currently_playing_index = -1;
	double play_progress = 0.0;
	std::map<size_t, double> current_progress;
};

#endif  // !defined (_CLIP_LIST_H)
