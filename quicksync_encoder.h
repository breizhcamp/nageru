// Hardware H.264 encoding via VAAPI. Heavily modified based on example
// code by Intel. Intel's original copyright and license is reproduced below:
//
// Copyright (c) 2007-2013 Intel Corporation. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sub license, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice (including the
// next paragraph) shall be included in all copies or substantial portions
// of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
// IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef _H264ENCODE_H
#define _H264ENCODE_H

#include <epoxy/gl.h>
#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class AudioEncoder;
class Mux;
class QSurface;
class QuickSyncEncoderImpl;
class RefCountedFrame;
class X264Encoder;

namespace movit {
class ResourcePool;
}  // namespace movit

// This is just a pimpl, because including anything X11-related in a .h file
// tends to trip up Qt. All the real logic is in QuickSyncEncoderImpl, defined in the
// .cpp file.
class QuickSyncEncoder {
public:
        QuickSyncEncoder(const std::string &filename, movit::ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, AVOutputFormat *oformat, X264Encoder *x264_encoder);
        ~QuickSyncEncoder();

	void set_stream_mux(Mux *mux);  // Does not take ownership. Must be called unless x264 is used for the stream.
	void add_audio(int64_t pts, std::vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames);
	void shutdown();  // Blocking. Does not require an OpenGL context.
	void release_gl_resources();  // Requires an OpenGL context. Must be run after shutdown.
	int64_t global_delay() const;  // So we never get negative dts.

private:
	std::unique_ptr<QuickSyncEncoderImpl> impl;
};

#endif
