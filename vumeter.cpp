#include <QPainter>

#include "vumeter.h"
#include "vu_common.h"

using namespace std;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::resizeEvent(QResizeEvent *event)
{
	recalculate_pixmaps();
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	int on_pos = lrint(lufs_to_pos(level_lu, height()));
	QRect off_rect(0, 0, width(), on_pos);
	QRect on_rect(0, on_pos, width(), height() - on_pos);

	painter.drawPixmap(off_rect, off_pixmap, off_rect);
	painter.drawPixmap(on_rect, on_pixmap, on_rect);
}

void VUMeter::recalculate_pixmaps()
{
	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	draw_vu_meter(on_painter, width(), height(), 0, true, min_level, max_level);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	draw_vu_meter(off_painter, width(), height(), 0, false, min_level, max_level);
}
