#version 450 core

in vec3 tc0, tc_left0, tc_down0;
in vec3 tc1, tc_left1, tc_down1;
in float line_offset;
out uvec4 equation_red, equation_black;

uniform sampler2DArray I_x_y_tex, I_t_tex;
uniform sampler2DArray diff_flow_tex, base_flow_tex;
uniform sampler2DArray beta_0_tex;
uniform sampler2DArray diffusivity_tex;

// Relative weighting of intensity term.
uniform float delta;

// Relative weighting of gradient term.
uniform float gamma;

uniform bool zero_diff_flow;

// Similar to packHalf2x16, but the two values share exponent, and are stored
// as 12-bit fixed point numbers multiplied by that exponent (the leading one
// can't be implicit in this kind of format). This allows us to store a much
// greater range of numbers (8-bit, ie., full fp32 range), and also gives us an
// extra mantissa bit. (Well, ostensibly two, but because the numbers have to
// be stored denormalized, we only really gain one.)
//
// The price we pay is that if the numbers are of very different magnitudes,
// the smaller number gets less precision.
uint pack_floats_shared(float a, float b)
{
	float greatest = max(abs(a), abs(b));

	// Find the exponent, increase it by one, and negate it.
	// E.g., if the nonbiased exponent is 3, the number is between
	// 2^3 and 2^4, so our normalization factor to get within -1..1
	// is going to be 2^-4.
	//
	// exponent -= 127;
	// exponent = -(exponent + 1);
	// exponent += 127;
	//
	// is the same as
	//
	// exponent = 252 - exponent;
	uint e = floatBitsToUint(greatest) & 0x7f800000u;
	float normalizer = uintBitsToFloat((252 << 23) - e);

	// The exponent is the same range as fp32, so just copy it
	// verbatim, shifted up to where the sign bit used to be.
	e <<= 1;

	// Quantize to 12 bits.
	uint qa = uint(int(round(a * (normalizer * 2047.0))));
	uint qb = uint(int(round(b * (normalizer * 2047.0))));

	return (qa & 0xfffu) | ((qb & 0xfffu) << 12) | e;
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

uvec4 compute_equation(vec3 tc, vec3 tc_left, vec3 tc_down)
{
	// Read the flow (on top of the u0/v0 flow).
	float du, dv;
	if (zero_diff_flow) {
		du = dv = 0.0f;
	} else {
		vec2 diff_flow = texture(diff_flow_tex, tc).xy;
		du = diff_flow.x;
		dv = diff_flow.y;
	}

	// Read the first derivatives.
	vec2 I_x_y = texture(I_x_y_tex, tc).xy;
	float I_x = I_x_y.x;
	float I_y = I_x_y.y;
	float I_t = texture(I_t_tex, tc).x;

	// E_I term. Note that we don't square β_0, in line with DeepFlow;
	// it's probably an error (see variational_refinement.txt),
	// but squaring it seems to give worse results.
	float beta_0 = texture(beta_0_tex, tc).x;
	float k1 = delta * beta_0 * inversesqrt(beta_0 * (I_x * du + I_y * dv + I_t) * (I_x * du + I_y * dv + I_t) + 1e-6);
	float A11 = k1 * I_x * I_x;
	float A12 = k1 * I_x * I_y;
	float A22 = k1 * I_y * I_y;
	float b1 = -k1 * I_t * I_x;
	float b2 = -k1 * I_t * I_y;

	// Compute the second derivatives. First I_xx and I_xy.
	vec2 I_x_y_m2 = textureOffset(I_x_y_tex, tc, ivec2(-2,  0)).xy;
	vec2 I_x_y_m1 = textureOffset(I_x_y_tex, tc, ivec2(-1,  0)).xy;
	vec2 I_x_y_p1 = textureOffset(I_x_y_tex, tc, ivec2( 1,  0)).xy;
	vec2 I_x_y_p2 = textureOffset(I_x_y_tex, tc, ivec2( 2,  0)).xy;
	vec2 I_xx_yx = (I_x_y_p1 - I_x_y_m1) * (2.0/3.0) + (I_x_y_m2 - I_x_y_p2) * (1.0/12.0);
	float I_xx = I_xx_yx.x;
	float I_xy = I_xx_yx.y;

	// And now I_yy; I_yx = I_xy, bar rounding differences, so we don't
	// bother computing it. We still have to sample the x component,
	// though, but we can throw it away immediately.
	float I_y_m2 = textureOffset(I_x_y_tex, tc, ivec2(0, -2)).y;
	float I_y_m1 = textureOffset(I_x_y_tex, tc, ivec2(0, -1)).y;
	float I_y_p1 = textureOffset(I_x_y_tex, tc, ivec2(0,  1)).y;
	float I_y_p2 = textureOffset(I_x_y_tex, tc, ivec2(0,  2)).y;
	float I_yy = (I_y_p1 - I_y_m1) * (2.0/3.0) + (I_y_m2 - I_y_p2) * (1.0/12.0);

	// Finally I_xt and I_yt. (We compute these as I_tx and I_yt.)
	vec2 I_t_m2 = textureOffset(I_t_tex, tc, ivec2(-2,  0)).xy;
	vec2 I_t_m1 = textureOffset(I_t_tex, tc, ivec2(-1,  0)).xy;
	vec2 I_t_p1 = textureOffset(I_t_tex, tc, ivec2( 1,  0)).xy;
	vec2 I_t_p2 = textureOffset(I_t_tex, tc, ivec2( 2,  0)).xy;
	vec2 I_tx_ty = (I_t_p1 - I_t_m1) * (2.0/3.0) + (I_t_m2 - I_t_p2) * (1.0/12.0);
	float I_xt = I_tx_ty.x;
	float I_yt = I_tx_ty.y;

	// E_G term. Same normalization as beta_0 (see derivatives.frag).
	float beta_x = 1.0 / (I_xx * I_xx + I_xy * I_xy + 1e-7);
	float beta_y = 1.0 / (I_xy * I_xy + I_yy * I_yy + 1e-7);
	float k2 = gamma * inversesqrt(
		beta_x * (I_xx * du + I_xy * dv + I_xt) * (I_xx * du + I_xy * dv + I_xt) +
		beta_y * (I_xy * du + I_yy * dv + I_yt) * (I_xy * du + I_yy * dv + I_yt) +
		1e-6);
	float k_x = k2 * beta_x;
	float k_y = k2 * beta_y;
	A11 += k_x * I_xx * I_xx + k_y * I_xy * I_xy;
	A12 += k_x * I_xx * I_xy + k_y * I_xy * I_yy;
	A22 += k_x * I_xy * I_xy + k_y * I_yy * I_yy;
	b1 -= k_x * I_xx * I_xt + k_y * I_xy * I_yt;
	b2 -= k_x * I_xy * I_xt + k_y * I_yy * I_yt;

	// E_S term, sans the part on the right-hand side that deals with
	// the neighboring pixels. The gamma is multiplied in in smoothness.frag.
	//
	// Note that we sample in-between two texels, which gives us the 0.5 *
	// (x[-1] + x[0]) part for free. If one of the texels is a border
	// texel, it will have zero alpha, and zero_if_outside_border() will
	// set smoothness to zero.
	float smooth_l = zero_if_outside_border(texture(diffusivity_tex, tc_left));
	float smooth_r = zero_if_outside_border(textureOffset(diffusivity_tex, tc_left, ivec2(1, 0)));
	float smooth_d = zero_if_outside_border(texture(diffusivity_tex, tc_down));
	float smooth_u = zero_if_outside_border(textureOffset(diffusivity_tex, tc_down, ivec2(0, 1)));
	A11 += smooth_l + smooth_r + smooth_d + smooth_u;
	A22 += smooth_l + smooth_r + smooth_d + smooth_u;

	// Laplacian of (u0, v0).
	vec2 laplacian =
		smooth_l * textureOffset(base_flow_tex, tc, ivec2(-1,  0)).xy +
		smooth_r * textureOffset(base_flow_tex, tc, ivec2( 1,  0)).xy +
		smooth_d * textureOffset(base_flow_tex, tc, ivec2( 0, -1)).xy +
		smooth_u * textureOffset(base_flow_tex, tc, ivec2( 0,  1)).xy -
		(smooth_l + smooth_r + smooth_d + smooth_u) * texture(base_flow_tex, tc).xy;
	b1 += laplacian.x;
	b2 += laplacian.y;

	// Encode the equation down into four uint32s.
	uvec4 ret;
	ret.x = floatBitsToUint(1.0 / A11);
	ret.y = floatBitsToUint(A12);
	ret.z = floatBitsToUint(1.0 / A22);
	ret.w = pack_floats_shared(b1, b2);
	return ret;
}

void main()
{
	uvec4 eq0 = compute_equation(tc0, tc_left0, tc_down0);
	uvec4 eq1 = compute_equation(tc1, tc_left1, tc_down1);

	if ((int(round(line_offset)) & 1) == 1) {
		// Odd line, so the right value is red.
		equation_red = eq1;
		equation_black = eq0;
	} else {
		equation_red = eq0;
		equation_black = eq1;
	}
}
