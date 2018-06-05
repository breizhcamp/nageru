#include "jpeg_frame_view.h"

#include <QGraphicsPixmapItem>
#include <QPixmap>

using namespace std;

string filename_for_frame(unsigned stream_idx, int64_t pts);

JPEGFrameView::JPEGFrameView(QWidget *parent)
	: QGraphicsView(parent) {
	scene.addItem(&item);
	setScene(&scene);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void JPEGFrameView::update()
{
	item.setPixmap(QPixmap(QString::fromStdString(filename_for_frame(stream_idx, pts))));
	fitInView(&item, Qt::KeepAspectRatio);
}

void JPEGFrameView::resizeEvent(QResizeEvent *event)
{
	fitInView(&item, Qt::KeepAspectRatio);
}
