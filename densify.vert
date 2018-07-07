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

	// Increase the patch size a bit; since patch spacing is not necessarily
	// an integer number of pixels, and we don't use conservative rasterization,
	// we could be missing the outer edges of the patch. And it seemingly helps
	// a little bit in general to have some more candidates as well -- although
	// this is measured without variational refinement, so it might be moot
	// with it.
	//
	// Tihs maps [0.0,1.0] to [-0.25 to 1.25), ie. extends the patch by 25% in
	// all directions.
	vec2 grown_pos = (position * 1.5) - vec2(0.25, 0.25);

	image_pos = patch_spacing * ivec2(patch_x, patch_y) + patch_size * grown_pos;

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
