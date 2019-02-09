#ifndef _YCBCR_CONVERTER_H
#define _YCBCR_CONVERTER_H 1

#include <epoxy/gl.h>
#include <memory>
#include <movit/ycbcr_input.h>

namespace movit {

class EffectChain;
class MixEffect;
class ResourcePool;
struct YCbCrFormat;

}  // namespace movit

struct Frame;

class YCbCrConverter {
public:
	enum OutputMode {
		OUTPUT_TO_RGBA,         // One texture (bottom-left origin): RGBA
		OUTPUT_TO_SEMIPLANAR,   // Two textures (top-left origin):   Y, CbCr
		OUTPUT_TO_DUAL_YCBCR    // Two textures (top-left origin):   Y'CbCr, Y'CbCr
	};
	YCbCrConverter(OutputMode output_mode, movit::ResourcePool *resource_pool);

	// Returns the appropriate chain for rendering.
	movit::EffectChain *prepare_chain_for_conversion(std::shared_ptr<Frame> frame);
	movit::EffectChain *prepare_chain_for_fade(std::shared_ptr<Frame> frame, std::shared_ptr<Frame> secondary_frame, float fade_alpha);

	// <tex> must be interleaved Y'CbCr.
	movit::EffectChain *prepare_chain_for_fade_from_texture(GLuint tex, unsigned width, unsigned height, std::shared_ptr<Frame> secondary_frame, float fade_alpha);

private:
	movit::YCbCrFormat ycbcr_format;

	// Effectively only converts from 4:2:2 to 4:4:4.
	// TODO: Have a separate version with ResampleEffect, for scaling?
	std::unique_ptr<movit::EffectChain> planar_chain, semiplanar_chain;
	movit::YCbCrInput *ycbcr_planar_input, *ycbcr_semiplanar_input;

	// These do fades, parametrized on whether the two inputs are planar
	// or semiplanar.
	struct FadeChain {
		std::unique_ptr<movit::EffectChain> chain;
		movit::YCbCrInput *input[2];
		movit::MixEffect *mix_effect;
	};
	FadeChain fade_chains[2][2];

	// These do fades, where the first input is interleaved and the second is
	// either planar or semiplanar.
	FadeChain interleaved_fade_chains[2];
};

// TODO: make private
void setup_input_for_frame(std::shared_ptr<Frame> frame, const movit::YCbCrFormat &ycbcr_format, movit::YCbCrInput *input);

#endif  // !defined(_YCBCR_CONVERTER_H)
