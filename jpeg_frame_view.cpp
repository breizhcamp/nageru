#include "jpeg_frame_view.h"

#include "post_to_main_thread.h"

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

void JPEGFrameView::update_frame()
{
	dirty = true;
	post_to_main_thread([this]{
		hide();
		show();
	});
}

void JPEGFrameView::resizeEvent(QResizeEvent *event)
{
	fitInView(&item, Qt::KeepAspectRatio);
}

void JPEGFrameView::paintEvent(QPaintEvent *event)
{
	if (dirty) {
		dirty = false;
		item.setPixmap(QPixmap(QString::fromStdString(filename_for_frame(stream_idx, pts))));
		fitInView(&item, Qt::KeepAspectRatio);
	}
	QGraphicsView::paintEvent(event);
}
