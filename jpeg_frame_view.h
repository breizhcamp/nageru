#ifndef _JPEG_FRAME_VIEW_H
#define _JPEG_FRAME_VIEW_H 1

#include <epoxy/gl.h>
#include <QGLWidget>

#include <stdint.h>

#include <movit/effect_chain.h>
#include <movit/flat_input.h>
#include <movit/ycbcr_input.h>

#include <memory>

struct JPEGID {
	unsigned stream_idx;
	int64_t pts;
	bool interpolated;
};
struct Frame {
	std::unique_ptr<uint8_t[]> y, cb, cr;
	unsigned width, height;
	unsigned chroma_subsampling_x, chroma_subsampling_y;
	unsigned pitch_y, pitch_chroma;
};
enum CacheMissBehavior {
	DECODE_IF_NOT_IN_CACHE,
	RETURN_NULLPTR_IF_NOT_IN_CACHE
};

std::string filename_for_frame(unsigned stream_idx, int64_t pts);
std::shared_ptr<Frame> decode_jpeg(const std::string &filename);
std::shared_ptr<Frame> decode_jpeg_with_cache(JPEGID id, CacheMissBehavior cache_miss_behavior, bool *did_decode);

class JPEGFrameView : public QGLWidget {
	Q_OBJECT

public:
	JPEGFrameView(QWidget *parent);

	void setFrame(unsigned stream_idx, int64_t pts, bool interpolated);
	static void insert_interpolated_frame(unsigned stream_idx, int64_t pts, std::shared_ptr<Frame> frame);

	void mousePressEvent(QMouseEvent *event) override;

	unsigned get_stream_idx() const { return current_stream_idx; }

	void setDecodedFrame(std::shared_ptr<Frame> frame);
	void set_overlay(const std::string &text);  // Blank for none.

signals:
	void clicked();

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	// The stream index of the latest frame we displayed.
	unsigned current_stream_idx;

	std::unique_ptr<movit::EffectChain> chain;
	std::shared_ptr<Frame> current_frame;  // So that we hold on to the pixels.
	movit::YCbCrInput *ycbcr_input;
	movit::YCbCrFormat ycbcr_format;

	static constexpr int overlay_base_width = 16, overlay_base_height = 16;
	int overlay_width = overlay_base_width, overlay_height = overlay_base_height;
	std::unique_ptr<QImage> overlay_image;  // If nullptr, no overlay.
	std::unique_ptr<movit::EffectChain> overlay_chain;  // Just to get the overlay on screen in the easiest way possible.
	movit::FlatInput *overlay_input;
	bool overlay_input_needs_refresh = false;

	int gl_width, gl_height;
};

#endif  // !defined(_JPEG_FRAME_VIEW_H)
