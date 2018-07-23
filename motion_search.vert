#version 450 core

in vec2 position;
out vec2 flow_tc;
out vec2 patch_bottom_left_texel;  // Center of bottom-left texel of patch.

uniform vec2 inv_flow_size, inv_image_size;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	flow_tc = position;

	vec2 patch_bottom_left = position - vec2(0.5, 0.5) * inv_flow_size;
	patch_bottom_left_texel = patch_bottom_left + vec2(0.5, 0.5) * inv_image_size;
}
