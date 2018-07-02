#version 450 core

in vec2 position;
in vec2 texcoord;
out vec2 flow_tc;
out vec2 patch_bottom_left_texel;  // Center of bottom-left texel of patch.

uniform float inv_flow_width, inv_flow_height;
uniform float inv_image_width, inv_image_height;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	flow_tc = texcoord;

	vec2 patch_bottom_left = texcoord - vec2(0.5, 0.5) * vec2(inv_flow_width, inv_flow_height);
	patch_bottom_left_texel = patch_bottom_left + vec2(0.5, 0.5) * vec2(inv_image_width, inv_image_height);
}
