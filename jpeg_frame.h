#ifndef _JPEG_FRAME_H
#define _JPEG_FRAME_H 1

#include <memory>

struct Frame {
	bool is_semiplanar = false;
	std::unique_ptr<uint8_t[]> y;
	std::unique_ptr<uint8_t[]> cb, cr; // For planar.
	std::unique_ptr<uint8_t[]> cbcr;  // For semiplanar.
	unsigned width, height;
	unsigned chroma_subsampling_x, chroma_subsampling_y;
	unsigned pitch_y, pitch_chroma;
};

#endif   // !defined(_JPEG_FRAME_H)
