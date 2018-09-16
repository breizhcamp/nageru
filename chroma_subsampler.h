#ifndef _CHROMA_SUBSAMPLER_H
#define _CHROMA_SUBSAMPLER_H 1

#include <epoxy/gl.h>

#include "flow.h"

class ChromaSubsampler {
public:
	ChromaSubsampler();
	~ChromaSubsampler();

	// Subsamples chroma (packed Cb and Cr) 2x1 to yield chroma suitable for
	// planar 4:2:2. Chroma positioning is left (H.264 convention).
	// width and height are the dimensions (in pixels) of the input texture.
	void subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint cb_tex, GLuint cr_tex);

private:
	PersistentFBOSet<2> fbos;

	GLuint vao;
	GLuint vbo;  // Holds position data.

	GLuint cbcr_vs_obj, cbcr_fs_obj, cbcr_program;
	GLuint uniform_cbcr_tex;
	GLuint uniform_chroma_offset_0, uniform_chroma_offset_1;
};

#endif  // !defined(_CHROMA_SUBSAMPLER_H)
