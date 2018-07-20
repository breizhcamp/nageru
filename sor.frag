#version 450 core

in vec2 tc;
out vec2 diff_flow;

uniform sampler2D diff_flow_tex, smoothness_x_tex, smoothness_y_tex;
uniform usampler2D equation_tex;

void main()
{
	uvec4 equation = texture(equation_tex, tc);
	float inv_A11 = uintBitsToFloat(equation.x);
	float A12 = uintBitsToFloat(equation.y);
	float inv_A22 = uintBitsToFloat(equation.z);
	vec2 b = unpackHalf2x16(equation.w);

	// Subtract the missing terms from the right-hand side
	// (it couldn't be done earlier, because we didn't know
	// the values of the neighboring pixels; they change for
	// each SOR iteration).
	// TODO: Multiply by some gamma.
	float smooth_l = textureOffset(smoothness_x_tex, tc, ivec2(-1,  0)).x;
	float smooth_r = texture(smoothness_x_tex, tc).x;
	float smooth_d = textureOffset(smoothness_y_tex, tc, ivec2( 0, -1)).x;
	float smooth_u = texture(smoothness_y_tex, tc).x;
	b += smooth_l * textureOffset(diff_flow_tex, tc, ivec2(-1,  0)).xy;
	b += smooth_r * textureOffset(diff_flow_tex, tc, ivec2( 1,  0)).xy;
	b += smooth_d * textureOffset(diff_flow_tex, tc, ivec2( 0, -1)).xy;
	b += smooth_u * textureOffset(diff_flow_tex, tc, ivec2( 0,  1)).xy;

	const float omega = 1.6;
	diff_flow = texture(diff_flow_tex, tc).xy;

	// From https://en.wikipedia.org/wiki/Successive_over-relaxation.
	float sigma_u = A12 * diff_flow.y;
	diff_flow.x += omega * ((b.x - sigma_u) * inv_A11 - diff_flow.x);
	float sigma_v = A12 * diff_flow.x;
	diff_flow.y += omega * ((b.y - sigma_v) * inv_A22 - diff_flow.y);
}
