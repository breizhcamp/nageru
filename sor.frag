#version 450 core

in vec2 tc;
out vec2 diff_flow;

uniform sampler2D diff_flow_tex, smoothness_x_tex, smoothness_y_tex;
uniform usampler2D equation_tex;

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

void main()
{
	uvec4 equation = texture(equation_tex, tc);
	float inv_A11 = uintBitsToFloat(equation.x);
	float A12 = uintBitsToFloat(equation.y);
	float inv_A22 = uintBitsToFloat(equation.z);
	vec2 b = unpack_floats_shared(equation.w);

	// Subtract the missing terms from the right-hand side
	// (it couldn't be done earlier, because we didn't know
	// the values of the neighboring pixels; they change for
	// each SOR iteration).
	float smooth_l = textureOffset(smoothness_x_tex, tc, ivec2(-1,  0)).x;
	float smooth_r = texture(smoothness_x_tex, tc).x;
	float smooth_d = textureOffset(smoothness_y_tex, tc, ivec2( 0, -1)).x;
	float smooth_u = texture(smoothness_y_tex, tc).x;
	b += smooth_l * textureOffset(diff_flow_tex, tc, ivec2(-1,  0)).xy;
	b += smooth_r * textureOffset(diff_flow_tex, tc, ivec2( 1,  0)).xy;
	b += smooth_d * textureOffset(diff_flow_tex, tc, ivec2( 0, -1)).xy;
	b += smooth_u * textureOffset(diff_flow_tex, tc, ivec2( 0,  1)).xy;

	// FIXME: omega=1.6 seems to make our entire system diverge.
	// Is this because we do Gauss-Seidel instead of Jacobi?
	// Figure out what's going on.
	const float omega = 1.0;
	diff_flow = texture(diff_flow_tex, tc).xy;

	// From https://en.wikipedia.org/wiki/Successive_over-relaxation.
	float sigma_u = A12 * diff_flow.y;
	diff_flow.x += omega * ((b.x - sigma_u) * inv_A11 - diff_flow.x);
	float sigma_v = A12 * diff_flow.x;
	diff_flow.y += omega * ((b.y - sigma_v) * inv_A22 - diff_flow.y);
}
