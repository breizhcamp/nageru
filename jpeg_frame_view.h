#ifndef _JPEG_FRAME_VIEW_H
#define _JPEG_FRAME_VIEW_H 1

#include <epoxy/gl.h>
#include <QGLWidget>

#include <stdint.h>

#include <movit/effect_chain.h>
#include <movit/ycbcr_input.h>

#include <memory>

std::string filename_for_frame(unsigned stream_idx, int64_t pts);

struct Frame {
	std::unique_ptr<uint8_t[]> y, cb, cr;
	unsigned width, height;
	unsigned chroma_subsampling_x, chroma_subsampling_y;
	unsigned pitch_y, pitch_chroma;
};

class JPEGFrameView : public QGLWidget {
	Q_OBJECT

public:
	JPEGFrameView(QWidget *parent);

	void setFrame(unsigned stream_idx, int64_t pts)
	{
		this->stream_idx = stream_idx;
		this->pts = pts;
		update_frame();
	}

	unsigned get_stream_idx() const { return stream_idx; }

	void setDecodedFrame(std::shared_ptr<Frame> frame);

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	void update_frame();

	unsigned stream_idx;
	int64_t pts;

	std::unique_ptr<movit::EffectChain> chain;
	std::shared_ptr<Frame> current_frame;  // So that we hold on to the pixels.
	movit::YCbCrInput *ycbcr_input;
	movit::YCbCrFormat ycbcr_format;
};

#endif  // !defined(_JPEG_FRAME_VIEW_H)
