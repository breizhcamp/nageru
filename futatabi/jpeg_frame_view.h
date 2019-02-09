#ifndef _JPEG_FRAME_VIEW_H
#define _JPEG_FRAME_VIEW_H 1

#include "frame_on_disk.h"
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

enum CacheMissBehavior {
	DECODE_IF_NOT_IN_CACHE,
	RETURN_NULLPTR_IF_NOT_IN_CACHE
};

std::shared_ptr<Frame> decode_jpeg(const std::string &jpeg);
std::shared_ptr<Frame> decode_jpeg_with_cache(FrameOnDisk id, CacheMissBehavior cache_miss_behavior, FrameReader *frame_reader, bool *did_decode);
std::shared_ptr<Frame> get_black_frame();

class JPEGFrameView : public QGLWidget {
	Q_OBJECT

public:
	JPEGFrameView(QWidget *parent);

	void setFrame(unsigned stream_idx, FrameOnDisk frame, FrameOnDisk secondary_frame = {}, float fade_alpha = 0.0f);
	void setFrame(std::shared_ptr<Frame> frame);

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
	static void jpeg_decoder_thread_func();

	FrameReader frame_reader;

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
