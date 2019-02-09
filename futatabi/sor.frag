#version 450 core

in vec3 tc, tc_left, tc_down;
in vec3 equation_tc_assuming_left, equation_tc_assuming_right;
in float element_x_idx, element_sum_idx;
out vec2 diff_flow;

uniform sampler2DArray diff_flow_tex, diffusivity_tex;
uniform usampler2DArray equation_red_tex, equation_black_tex;
uniform int phase;

uniform int num_nonzero_phases;

// See pack_floats_shared() in equations.frag.
vec2 unpack_floats_shared(uint c)
{
	// Recover the exponent, and multiply it in. Add one because
	// we have denormalized mantissas, then another one because we
	// already reduced the exponent by one. Then subtract 20, because
	// we are going to shift up the number by 20 below to recover the sign bits.
	float normalizer = uintBitsToFloat(((c >> 1) & 0x7f800000u) - (18 << 23));
	normalizer *= (1.0 / 2047.0);

	// Shift the values up so that we recover the sign bit, then normalize.
	float a = int(uint(c & 0x000fffu) << 20) * normalizer;
	float b = int(uint(c & 0xfff000u) << 8) * normalizer;

	return vec2(a, b);
}

float zero_if_outside_border(vec4 val)
{
	if (val.w < 1.0f) {
		// We hit the border (or more like half-way to it), so zero smoothness.
		return 0.0f;
	} else {
		return val.x;
	}
}

void main()
{
	// Red-black SOR: Every other pass, we update every other element in a
	// checkerboard pattern. This is rather suboptimal for the GPU, as it
	// just immediately throws away half of the warp, but it helps convergence
	// a _lot_ (rough testing indicates that five iterations of SOR is as good
	// as ~50 iterations of Jacobi). We could probably do better by reorganizing
	// the data into two-values-per-pixel, so-called “twinned buffering”;
	// seemingly, it helps Haswell by ~15% on the SOR code, but GTX 950 not at all
	// (at least not on 720p). Presumably the latter is already bandwidth bound.
	int color = int(round(element_sum_idx)) & 1;
	if (color != phase) discard;

	uvec4 equation;
	vec3 equation_tc;
	if ((int(round(element_x_idx)) & 1) == 0) {
		equation_tc = equation_tc_assuming_left;
	} else {
		equation_tc = equation_tc_assuming_right;
	}
	if (phase == 0) {
		equation = texture(equation_red_tex, equation_tc);
	} else {
		equation = texture(equation_black_tex, equation_tc);
	}
	float inv_A11 = uintBitsToFloat(equation.x);
	float A12 = uintBitsToFloat(equation.y);
	float inv_A22 = uintBitsToFloat(equation.z);
	vec2 b = unpack_floats_shared(equation.w);

	const float omega = 1.8;  // Marginally better than 1.6, it seems.

	if (num_nonzero_phases == 0) {
		// Simplified version of the code below, assuming diff_flow == 0.0f everywhere.
		diff_flow.x = omega * b.x * inv_A11;
		diff_flow.y = omega * b.y * inv_A22;
	} else {
		// Subtract the missing terms from the right-hand side
		// (it couldn't be done earlier, because we didn't know
		// the values of the neighboring pixels; they change for
		// each SOR iteration).
		float smooth_l = zero_if_outside_border(texture(diffusivity_tex, tc_left));
		float smooth_r = zero_if_outside_border(textureOffset(diffusivity_tex, tc_left, ivec2(1, 0)));
		float smooth_d = zero_if_outside_border(texture(diffusivity_tex, tc_down));
		float smooth_u = zero_if_outside_border(textureOffset(diffusivity_tex, tc_down, ivec2(0, 1)));
		b += smooth_l * textureOffset(diff_flow_tex, tc, ivec2(-1,  0)).xy;
		b += smooth_r * textureOffset(diff_flow_tex, tc, ivec2( 1,  0)).xy;
		b += smooth_d * textureOffset(diff_flow_tex, tc, ivec2( 0, -1)).xy;
		b += smooth_u * textureOffset(diff_flow_tex, tc, ivec2( 0,  1)).xy;

		if (num_nonzero_phases == 1) {
			diff_flow = vec2(0.0f);
		} else {
			diff_flow = texture(diff_flow_tex, tc).xy;
		}

		// From https://en.wikipedia.org/wiki/Successive_over-relaxation.
		float sigma_u = A12 * diff_flow.y;
		diff_flow.x += omega * ((b.x - sigma_u) * inv_A11 - diff_flow.x);
		float sigma_v = A12 * diff_flow.x;
		diff_flow.y += omega * ((b.y - sigma_v) * inv_A22 - diff_flow.y);
	}
}
