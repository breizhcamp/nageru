#include "vu_common.h"

#include <math.h>
#include <algorithm>

#include <QBrush>
#include <QColor>
#include <QPainter>

using namespace std;

int lufs_to_pos(float level_lu, int height)
{       
	const float min_level = 9.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -18.0f;

	// Handle -inf.
	if (level_lu < max_level) {
		return height - 1;
	}

	int y = lrintf(height * (level_lu - min_level) / (max_level - min_level));
	y = max(y, 0);
	y = min(y, height - 1);
	return y;
}

void draw_vu_meter(QPainter &painter, int width, int height, int margin, bool is_on)
{
	painter.fillRect(margin, 0, width - 2 * margin, height, Qt::black);

	// TODO: QLinearGradient is not gamma-correct; we might want to correct for that.
	QLinearGradient on(0, 0, 0, height);
	on.setColorAt(0.0f, QColor(255, 0, 0));
	on.setColorAt(0.5f, QColor(255, 255, 0));
	on.setColorAt(1.0f, QColor(0, 255, 0));
	QColor off(80, 80, 80);

	// Draw bars colored up until the level, then gray from there.
	for (int level = -18; level < 9; ++level) {
		int min_y = lufs_to_pos(level + 1.0f, height) + 1;
		int max_y = lufs_to_pos(level, height) - 1;

		if (is_on) {
			painter.fillRect(margin, min_y, width - 2 * margin, max_y - min_y, on);
		} else {
			painter.fillRect(margin, min_y, width - 2 * margin, max_y - min_y, off);
		}
	}
}
