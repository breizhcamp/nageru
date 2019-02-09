#ifndef _CLIP_LIST_H
#define _CLIP_LIST_H 1

#include "defs.h"
#include "state.pb.h"

#include <QAbstractTableModel>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

struct Clip {
	int64_t pts_in = -1, pts_out = -1;  // pts_in is inclusive, pts_out is exclusive.
	std::string descriptions[MAX_STREAMS];

	// These are for the playlist only.
	unsigned stream_idx = 0;
	double fade_time_seconds = 0.5;
	double speed = 0.5;
};
struct ClipWithID {
	Clip clip;
	uint64_t id;  // Used for progress callback only. Immutable.
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
	ClipList(const ClipListProto &serialized);

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

	void change_num_cameras(size_t num_cameras);  // Defaults to 2. Cannot decrease.
	void emit_data_changed(size_t row) override;

	bool is_camera_column(int column) const
	{
		return (column >= int(Column::CAMERA_1) && column < int(Column::CAMERA_1) + int(num_cameras));
	}

signals:
	void any_content_changed();

private:
	std::vector<Clip> clips;
	size_t num_cameras = 2;
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
		SPEED,
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

	ClipProxy mutable_clip(size_t index) { return ClipProxy(clips[index].clip, this, index); }
	const Clip *clip(size_t index) const { return &clips[index].clip; }
	const ClipWithID *clip_with_id(size_t index) const { return &clips[index]; }

	ClipProxy mutable_back() { return mutable_clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	void set_progress(const std::map<uint64_t, double> &progress);

	ClipListProto serialize() const;

	void change_num_cameras(size_t num_cameras)  // Defaults to 2. Cannot decrease.
	{
		this->num_cameras = num_cameras;
	}

	void emit_data_changed(size_t row) override;

signals:
	void any_content_changed();

private:
	std::vector<ClipWithID> clips;
	double play_progress = 0.0;
	std::map<uint64_t, double> current_progress;
	size_t num_cameras = 2;
	uint64_t clip_counter = 1000000;  // Used for generating IDs. Starting at a high number to avoid any kind of bugs treating IDs as rows.
};

#endif  // !defined (_CLIP_LIST_H)
