#include "ycbcr_converter.h"

#include <movit/mix_effect.h>
#include <movit/ycbcr_input.h>

#include "jpeg_frame.h"

using namespace std;
using namespace movit;

YCbCrConverter::YCbCrConverter(YCbCrConverter::OutputMode output_mode, ResourcePool *resource_pool)
{
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	ycbcr_format.luma_coefficients = YCBCR_REC_709;
	ycbcr_format.full_range = false;
	ycbcr_format.num_levels = 256;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 1;
	ycbcr_format.cb_x_position = 0.0f;  // H.264 -- _not_ JPEG, even though our input is MJPEG-encoded
	ycbcr_format.cb_y_position = 0.5f;  // Irrelevant.
	ycbcr_format.cr_x_position = 0.0f;
	ycbcr_format.cr_y_position = 0.5f;

	YCbCrFormat ycbcr_output_format = ycbcr_format;
	ycbcr_output_format.chroma_subsampling_x = 1;

	// Planar Y'CbCr decoding chain.
	planar_chain.reset(new EffectChain(1280, 720, resource_pool));
	ycbcr_planar_input = (YCbCrInput *)planar_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, 1280, 720, YCBCR_INPUT_PLANAR));
	if (output_mode == OUTPUT_TO_RGBA) {
		planar_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
		planar_chain->set_output_origin(OUTPUT_ORIGIN_BOTTOM_LEFT);
	} else {
		assert(output_mode == OUTPUT_TO_DUAL_YCBCR);

		// One full Y'CbCr texture (for interpolation), one that's just Y (throwing away the
		// Cb and Cr channels). The second copy is sort of redundant, but it's the easiest way
		// of getting the gray data into a layered texture.
		planar_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		planar_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		planar_chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	}
	planar_chain->set_dither_bits(8);
	planar_chain->finalize();

	// Semiplanar Y'CbCr decoding chain (for images coming from VA-API).
	semiplanar_chain.reset(new EffectChain(1280, 720, resource_pool));
	ycbcr_semiplanar_input = (YCbCrInput *)semiplanar_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, 1280, 720, YCBCR_INPUT_SPLIT_Y_AND_CBCR));
	if (output_mode == OUTPUT_TO_RGBA) {
		semiplanar_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
		semiplanar_chain->set_output_origin(OUTPUT_ORIGIN_BOTTOM_LEFT);
	} else {
		// See above.
		semiplanar_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		semiplanar_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		semiplanar_chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	}
	semiplanar_chain->set_dither_bits(8);
	semiplanar_chain->finalize();
}

EffectChain *YCbCrConverter::prepare_chain_for_conversion(shared_ptr<Frame> frame)
{
	if (frame->is_semiplanar) {
		setup_input_for_frame(frame, ycbcr_format, ycbcr_semiplanar_input);
		return semiplanar_chain.get();
	} else {
		setup_input_for_frame(frame, ycbcr_format, ycbcr_planar_input);
		return planar_chain.get();
	}
}

void setup_input_for_frame(shared_ptr<Frame> frame, const YCbCrFormat &ycbcr_format, YCbCrInput *input)
{
	YCbCrFormat format_copy = ycbcr_format;
	format_copy.chroma_subsampling_x = frame->chroma_subsampling_x;
	format_copy.chroma_subsampling_y = frame->chroma_subsampling_y;
	input->change_ycbcr_format(format_copy);

	input->set_width(frame->width);
	input->set_height(frame->height);
	input->set_pixel_data(0, frame->y.get());
	input->set_pitch(0, frame->pitch_y);
	if (frame->is_semiplanar) {
		input->set_pixel_data(1, frame->cbcr.get());
		input->set_pitch(1, frame->pitch_chroma);
	} else {
		input->set_pixel_data(1, frame->cb.get());
		input->set_pixel_data(2, frame->cr.get());
		input->set_pitch(1, frame->pitch_chroma);
		input->set_pitch(2, frame->pitch_chroma);
	}
}
