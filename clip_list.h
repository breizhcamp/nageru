#ifndef _CLIP_LIST_H
#define _CLIP_LIST_H 1

#include <QAbstractTableModel>

#include <stdint.h>

#include <vector>
#include <string>

struct Clip {
	int64_t pts_in = -1, pts_out = -1;
	std::vector<std::string> descriptions;  // One per camera.
};

class ClipList : public QAbstractTableModel {
	Q_OBJECT

public:
	enum Column {
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

	void add_clip(const Clip &clip);
	size_t size() const { return clips.size(); }
	bool empty() const { return clips.empty(); }

	// Like a smart pointer to a Clip, but emits dataChanged when it goes out of scope.
	struct ClipProxy {
	public:
		ClipProxy(Clip &clip, ClipList *clip_list, size_t row)
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
		ClipList *clip_list;
		size_t row;
	};

	ClipProxy clip(size_t index) { return ClipProxy(clips[index], this, index); }
	const Clip *clip(size_t index) const { return &clips[index]; }

	ClipProxy back() { return clip(size() - 1); }
	const Clip *back() const { return clip(size() - 1); }

	void emit_data_changed(size_t row);

private:
	std::vector<Clip> clips;
};

#endif  // !defined (_CLIP_LIST_H)
