#version 450 core

in vec2 tc;
out vec4 equation;

uniform sampler2D I_x_y_tex, I_t_tex;
uniform sampler2D diff_flow_tex, flow_tex;
uniform sampler2D beta_0_tex;
uniform sampler2D smoothness_x_tex, smoothness_y_tex;

// TODO: Consider a specialized version for the case where we know that du = dv = 0,
// since we run so few iterations.

// The base flow needs to be normalized.
// TODO: Should we perhaps reduce this to a separate two-component
// texture when calculating the derivatives?
vec2 normalize_flow(vec3 flow)
{
	return flow.xy / flow.z;
}

// This must be a macro, since the offset needs to be a constant expression.
#define get_flow(x_offs, y_offs) \
	(normalize_flow(textureOffset(flow_tex, tc, ivec2((x_offs), (y_offs))).xyz) + \
	textureOffset(diff_flow_tex, tc, ivec2((x_offs), (y_offs))).xy)

void main()
{
	// Read the flow (on top of the u0/v0 flow).
	vec2 diff_flow = texture(diff_flow_tex, tc).xy;
	float du = diff_flow.x;  // FIXME: convert to pixels?
	float dv = diff_flow.y;

	// Read the first derivatives.
	vec2 I_x_y = texture(I_x_y_tex, tc).xy;
	float I_x = I_x_y.x;
	float I_y = I_x_y.y;
	float I_t = texture(I_t_tex, tc).x;

	// E_I term. Note that we don't square β_0, in line with DeepFlow,
	// even though it's probably an error.
	//
	// TODO: Evaluate squaring β_0.
  	// FIXME: Should the penalizer be adjusted for 0..1 intensity range instead of 0..255?
	// TODO: Multiply by some alpha.
	float beta_0 = texture(beta_0_tex, tc).x;
	float k1 = beta_0 * inversesqrt(beta_0 * (I_x * du + I_y * dv + I_t) * (I_x * du + I_y * dv + I_t) + 1e-6);
	float A11 = k1 * I_x * I_x;
	float A12 = k1 * I_x * I_y;
	float A22 = k1 * I_y * I_y;
	float b1 = -k1 * I_t;
	float b2 = -k1 * I_t;

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

	// E_G term. Same TODOs as E_I. Same normalization as beta_0
	// (see derivatives.frag).
	float beta_x = 1.0 / (I_xx * I_xx + I_xy * I_xy + 1e-7);
	float beta_y = 1.0 / (I_xy * I_xy + I_yy * I_yy + 1e-7);
	float k2 = inversesqrt(
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
	// the neighboring pixels.
	// TODO: Multiply by some gamma.
	float smooth_l = textureOffset(smoothness_x_tex, tc, ivec2(-1, 0)).x;
	float smooth_r = texture(smoothness_x_tex, tc).x;
	float smooth_d = textureOffset(smoothness_y_tex, tc, ivec2(-1, 0)).x;
	float smooth_u = texture(smoothness_y_tex, tc).x;
	A11 -= smooth_l + smooth_r + smooth_d + smooth_u;
	A22 -= smooth_l + smooth_r + smooth_d + smooth_u;

	// Laplacian of (u0 + du, v0 + dv), sans the central term.
	vec2 laplacian =
		smooth_l * get_flow(-1, 0) +
		smooth_r * get_flow(1, 0) +
		smooth_d * get_flow(0, -1) +
		smooth_u * get_flow(0, 1);
	b1 -= laplacian.x;
	b2 -= laplacian.y;

	// The central term of the Laplacian, for (u0, v0) only.
	// (The central term for (du, dv) is what we are solving for.)
	vec2 central = (smooth_l + smooth_r + smooth_d + smooth_u) * normalize_flow(texture(flow_tex, tc).xyz);
	b1 += central.x;
	b2 += central.y;

	// Encode the equation down into four uint32s.
	equation.x = floatBitsToUint(A11);
	equation.y = floatBitsToUint(A12);
	equation.z = floatBitsToUint(A22);
	equation.w = packHalf2x16(vec2(b1, b2));
}
