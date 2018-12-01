#version 450 core

layout(location=0) in vec2 position;
out vec2 image_pos;
flat out vec2 flow, I_0_check_offset, I_1_check_offset;

uniform vec2 splat_size;  // In 0..1 coordinates.
uniform vec2 inv_flow_size;
uniform float alpha;
uniform sampler2DArray flow_tex;  // 0 = forward flow, 1 = backward flow.

void main()
{
	int instance = gl_InstanceID;
	int num_pixels_per_layer = textureSize(flow_tex, 0).x * textureSize(flow_tex, 0).y;
	int src_layer;
	if (instance >= num_pixels_per_layer) {
		instance -= num_pixels_per_layer;
		src_layer = 1;
	} else {
		src_layer = 0;
	}
	int x = instance % textureSize(flow_tex, 0).x;
	int y = instance / textureSize(flow_tex, 0).x;

	// Find out where to splat this to.
	vec2 full_flow = texelFetch(flow_tex, ivec3(x, y, src_layer), 0).xy;
	float splat_alpha;
	if (src_layer == 1) {  // Reverse flow.
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
