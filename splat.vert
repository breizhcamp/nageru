#version 450 core

layout(location=0) in vec2 position;
out vec2 image_pos;
flat out vec2 flow, I_0_check_offset, I_1_check_offset;

uniform bool invert_flow;
uniform vec2 splat_size;  // In 0..1 coordinates.
uniform vec2 inv_flow_size;
uniform float alpha;
uniform sampler2D flow_tex;

void main()
{
	int x = gl_InstanceID % textureSize(flow_tex, 0).x;
	int y = gl_InstanceID / textureSize(flow_tex, 0).x;

	// Find out where to splat this to.
	// TODO: See if we can move some of these calculations into uniforms.
	vec2 full_flow = texelFetch(flow_tex, ivec2(x, y), 0).xy;
	float splat_alpha;
	if (invert_flow) {
		full_flow = -full_flow;
		splat_alpha = 1.0f - alpha;
	} else {
		splat_alpha = alpha;
	}
	full_flow *= inv_flow_size;
	
	vec2 patch_center = (ivec2(x, y) + 0.5) * inv_flow_size + full_flow * splat_alpha;
	image_pos = patch_center + splat_size * (position - 0.5);

	flow = full_flow;
	I_0_check_offset = full_flow * -alpha;
	I_1_check_offset = full_flow * (1.0f - alpha);

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * image_pos.x - 1.0, 2.0 * image_pos.y - 1.0, -1.0, 1.0);
}
