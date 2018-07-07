#version 450 core

in vec2 position;
out vec2 image_pos;
flat out vec2 flow_du;

uniform int width_patches;
uniform vec2 patch_size;  // In 0..1 coordinates.
uniform vec2 patch_spacing;  // In 0..1 coordinates.
uniform sampler2D flow_tex;

void main()
{
	int patch_x = gl_InstanceID % width_patches;
	int patch_y = gl_InstanceID / width_patches;

	// TODO: Lock the bottom left of the patch to an integer number of pixels?

	image_pos = patch_spacing * ivec2(patch_x, patch_y) + patch_size * position;

	// Find the flow value for this patch, and send it on to the fragment shader.
	flow_du = texelFetch(flow_tex, ivec2(patch_x, patch_y), 0).xy;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * image_pos.x - 1.0, 2.0 * image_pos.y - 1.0, -1.0, 1.0);
}
