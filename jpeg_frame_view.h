#ifndef _JPEG_FRAME_VIEW_H
#define _JPEG_FRAME_VIEW_H 1

#include "jpeg_frame.h"
#include "ycbcr_converter.h"

#include <QGLWidget>
#include <epoxy/gl.h>
#include <memory>
#include <movit/effect_chain.h>
#include <movit/flat_input.h>
#include <movit/mix_effect.h>
#include <movit/ycbcr_input.h>
#include <stdint.h>
#include <thread>

struct JPEGID {
	unsigned stream_idx;
	int64_t pts;
	bool interpolated;
};
enum CacheMissBehavior {
	DECODE_IF_NOT_IN_CACHE,
	RETURN_NULLPTR_IF_NOT_IN_CACHE
};

// This is, well, a hack. We hope for no collisions.
inline JPEGID create_jpegid_for_interpolated_fade(unsigned stream_idx, int64_t pts, unsigned secondary_stream_idx, int64_t secondary_pts)
{
	JPEGID id;
	id.stream_idx = (stream_idx << 8) | secondary_stream_idx;

	uint64_t rot = secondary_stream_idx;
	rot = (rot << 32) | (rot >> 32);
	id.pts = pts ^ int64_t(rot);
	id.interpolated = true;
	return id;
}

std::string filename_for_frame(unsigned stream_idx, int64_t pts);
std::shared_ptr<Frame> decode_jpeg(const std::string &filename);
std::shared_ptr<Frame> decode_jpeg_with_cache(JPEGID id, CacheMissBehavior cache_miss_behavior, bool *did_decode);

class JPEGFrameView : public QGLWidget {
	Q_OBJECT

public:
	JPEGFrameView(QWidget *parent);

	void setFrame(unsigned stream_idx, int64_t pts, bool interpolated, int secondary_stream_idx = -1, int64_t secondary_pts = -1, float fade_alpha = 0.0f);
	static void insert_interpolated_frame(JPEGID id, std::shared_ptr<Frame> frame);

	void mousePressEvent(QMouseEvent *event) override;

	unsigned get_stream_idx() const { return current_stream_idx; }

	void setDecodedFrame(std::shared_ptr<Frame> frame, std::shared_ptr<Frame> secondary_frame, float fade_alpha);
	void set_overlay(const std::string &text);  // Blank for none.

	static void shutdown();

signals:
	void clicked();

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	// The stream index of the latest frame we displayed.
	unsigned current_stream_idx = 0;

	std::unique_ptr<YCbCrConverter> ycbcr_converter;
	movit::EffectChain *current_chain = nullptr;  // Owned by ycbcr_converter.

	std::shared_ptr<Frame> current_frame;  // So that we hold on to the pixels.
	std::shared_ptr<Frame> current_secondary_frame;  // Same.

	static constexpr int overlay_base_width = 16, overlay_base_height = 16;
	int overlay_width = overlay_base_width, overlay_height = overlay_base_height;
	std::unique_ptr<QImage> overlay_image;  // If nullptr, no overlay.
	std::unique_ptr<movit::EffectChain> overlay_chain;  // Just to get the overlay on screen in the easiest way possible.
	movit::FlatInput *overlay_input;
	bool overlay_input_needs_refresh = false;

	int gl_width, gl_height;

	static std::thread jpeg_decoder_thread;
};

#endif  // !defined(_JPEG_FRAME_VIEW_H)
