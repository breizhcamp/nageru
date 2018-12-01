#version 450 core

in vec3 tc;
out float g;
const float eps_sq = 0.001 * 0.001;

uniform sampler2DArray flow_tex, diff_flow_tex;

// Relative weighting of smoothness term.
uniform float alpha;

uniform bool zero_diff_flow;

// This must be a macro, since the offset needs to be a constant expression.
#define get_flow(x_offs, y_offs) \
	(textureOffset(flow_tex, tc, ivec2((x_offs), (y_offs))).xy + \
	textureOffset(diff_flow_tex, tc, ivec2((x_offs), (y_offs))).xy)

#define get_flow_no_diff(x_offs, y_offs) \
	textureOffset(flow_tex, tc, ivec2((x_offs), (y_offs))).xy

float diffusivity(float u_x, float u_y, float v_x, float v_y)
{
	return alpha * inversesqrt(u_x * u_x + u_y * u_y + v_x * v_x + v_y * v_y + eps_sq);
}

void main()
{
	// Find diffusivity (g) for this pixel, using central differences.
	if (zero_diff_flow) {
		vec2 uv_x = get_flow_no_diff(1, 0) - get_flow_no_diff(-1,  0);
		vec2 uv_y = get_flow_no_diff(0, 1) - get_flow_no_diff( 0, -1);
		g = diffusivity(uv_x.x, uv_y.x, uv_x.y, uv_y.y);
	} else {
		vec2 uv_x = get_flow(1, 0) - get_flow(-1,  0);
		vec2 uv_y = get_flow(0, 1) - get_flow( 0, -1);
		g = diffusivity(uv_x.x, uv_y.x, uv_x.y, uv_y.y);
	}
}
