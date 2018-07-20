#version 450 core

in vec2 tc;
out float smoothness_x, smoothness_y;
const float eps_sq = 0.001 * 0.001;

uniform sampler2D flow_tex, diff_flow_tex;

// This must be a macro, since the offset needs to be a constant expression.
#define get_flow(x_offs, y_offs) \
	(textureOffset(flow_tex, tc, ivec2((x_offs), (y_offs))).xy + \
	textureOffset(diff_flow_tex, tc, ivec2((x_offs), (y_offs))).xy)

float diffusivity(float u_x, float u_y, float v_x, float v_y)
{
	return -inversesqrt(u_x * u_x + u_y * u_y + v_x * v_x + v_y * v_y + eps_sq);
}

void main()
{
	float g, g_right, g_up;

	// These are shared between some of the diffusivities.
	vec2 flow_0_0 = get_flow(0, 0);
	vec2 flow_1_1 = get_flow(1, 1);

	// Find diffusivity (g) for this pixel, using central differences.
	{
		vec2 uv_x = get_flow(1, 0) - get_flow(-1,  0);
		vec2 uv_y = get_flow(0, 1) - get_flow( 0, -1);
		g = diffusivity(uv_x.x, uv_y.x, uv_x.y, uv_y.y);
	}

	// Now find diffusivity for the pixel to the right.
	{
		vec2 uv_x = get_flow(2, 0) - flow_0_0;
		vec2 uv_y = flow_1_1 - get_flow( 1, -1);
		g_right = diffusivity(uv_x.x, uv_y.x, uv_x.y, uv_y.y);
	}

	// And up.
	{
		vec2 uv_x = flow_1_1 - get_flow(-1,  1);
		vec2 uv_y = get_flow(0, 2) - flow_0_0;
		g_up = diffusivity(uv_x.x, uv_y.x, uv_x.y, uv_y.y);
	}

	smoothness_x = 0.5 * (g + g_right);
	smoothness_y = 0.5 * (g + g_up);
}
