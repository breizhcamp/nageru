#ifndef _JPEG_DESTROYER_H
#define _JPEG_DESTROYER_H 1

#include <jpeglib.h>

class JPEGDestroyer {
public:
	JPEGDestroyer(jpeg_decompress_struct *dinfo)
		: dinfo(dinfo) {}

	~JPEGDestroyer() {
		jpeg_destroy_decompress(dinfo);
	}

private:
	jpeg_decompress_struct *dinfo;
};

#endif  // !defined(_JPEG_DESTROYER_H)
