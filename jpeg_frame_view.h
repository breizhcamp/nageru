#ifndef _JPEG_FRAME_VIEW_H
#define _JPEG_FRAME_VIEW_H 1

#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>

#include <stdint.h>

class JPEGFrameView : public QGraphicsView {
	Q_OBJECT

public:
	JPEGFrameView(QWidget *parent);

	void setFrame(unsigned stream_idx, int64_t pts)
	{
		this->stream_idx = stream_idx;
		this->pts = pts;
		update();
	}

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void update();

	QGraphicsPixmapItem item;
	QGraphicsScene scene;

	unsigned stream_idx;
	int64_t pts;
};

#endif  // !defined(_JPEG_FRAME_VIEW_H)
