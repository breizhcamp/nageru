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
	int rowCount(const QModelIndex &parent) const override;
	int columnCount(const QModelIndex &parent) const override;
	QVariant data(const QModelIndex &parent, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

	void add_clip(int64_t pts_in);
	size_t size() const { return clips.size(); }

private:
	std::vector<Clip> clips;
};

#endif  // !defined (_CLIP_LIST_H)
