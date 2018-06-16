#ifndef _CLIP_LIST_H
#define _CLIP_LIST_H 1

#include <QAbstractTableModel>

#include <stdint.h>

#include <vector>
#include <string>

#include "defs.h"

struct Clip {
	int64_t pts_in = -1, pts_out = -1;
	std::string descriptions[NUM_CAMERAS];
	unsigned stream_idx = 0;  // For the playlist only.
};

class DataChangedReceiver {
public:
	virtual void emit_data_changed(size_t row) = 0;
};

// Like a smart pointer to a Clip, but emits dataChanged when it goes out of scope.
struct ClipProxy {
public:
	ClipProxy(Clip &clip, DataChangedReceiver *clip_list, size_t row)
		: clip(clip), clip_list(clip_list), row(row) {}
	~ClipProxy() {
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
	ClipList() {}

	enum class Column {
		IN,
		OUT,
		DURATION,
		CAMERA_1,
		CAMERA_2,
		CAMERA_3,
		CAMERA_4,
		NUM_COLUMNS
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

	ClipProxy clip(size_t index) { return ClipProxy(clips[index], this, index); }
	const Clip *clip(size_t index) const { return &clips[index]; }

	ClipProxy back() { return clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	void emit_data_changed(size_t row) override;

private:
	std::vector<Clip> clips;
};

class PlayList : public QAbstractTableModel, public DataChangedReceiver {
	Q_OBJECT

public:
	PlayList() {}

	enum class Column {
		PLAYING,
		IN,
		OUT,
		DURATION,
		CAMERA,
		DESCRIPTION,
		NUM_COLUMNS
	};

	int rowCount(const QModelIndex &parent) const override;
	int columnCount(const QModelIndex &parent) const override;
	QVariant data(const QModelIndex &parent, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

	void add_clip(const Clip &clip);
	size_t size() const { return clips.size(); }
	bool empty() const { return clips.empty(); }

	ClipProxy clip(size_t index) { return ClipProxy(clips[index], this, index); }
	const Clip *clip(size_t index) const { return &clips[index]; }

	ClipProxy back() { return clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	void set_currently_playing(int index);  // -1 = none.
	int get_currently_playing() const { return currently_playing_index; }

	void emit_data_changed(size_t row) override;

private:
	std::vector<Clip> clips;
	int currently_playing_index = -1;
};

#endif  // !defined (_CLIP_LIST_H)
