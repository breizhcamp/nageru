#ifndef _FLOW2RGB_H
#define _FLOW2RGB_H 1

#include <math.h>
#include <stdint.h>

#include <algorithm>

// du and dv are in pixels.
inline void flow2rgb(float du, float dv, uint8_t *rr, uint8_t *gg, uint8_t *bb)
{
	float angle = atan2(dv, du);
	float magnitude = std::min(hypot(du, dv) / 20.0, 1.0);
	
	// HSV to RGB (from Wikipedia). Saturation is 1.
	float c = magnitude;
	float h = (angle + M_PI) * 6.0 / (2.0 * M_PI);
	float X = c * (1.0 - fabs(fmod(h, 2.0) - 1.0));
	float r = 0.0f, g = 0.0f, b = 0.0f;
	if (h < 1.0f) {
		r = c; g = X;
	} else if (h < 2.0f) {
		r = X; g = c;
	} else if (h < 3.0f) {
		g = c; b = X;
	} else if (h < 4.0f) {
		g = X; b = c;
	} else if (h < 5.0f) {
		r = X; b = c;
	} else if (h < 6.0f) {
		r = c; b = X;
	} else {
		// h is NaN, so black is fine.
	}
	float m = magnitude - c;
	r += m; g += m; b += m;
	r = std::max(std::min(r, 1.0f), 0.0f);
	g = std::max(std::min(g, 1.0f), 0.0f);
	b = std::max(std::min(b, 1.0f), 0.0f);
	*rr = lrintf(r * 255.0f);
	*gg = lrintf(g * 255.0f);
	*bb = lrintf(b * 255.0f);
}

#endif  // !defined(_FLOW2RGB_H)
