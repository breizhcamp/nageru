#version 450 core

in vec2 position;
out vec2 image_pos;
flat out vec2 flow_du;
flat out float mean_diff;

uniform int width_patches;
uniform vec2 patch_size;  // In 0..1 coordinates.
uniform vec2 patch_spacing;  // In 0..1 coordinates.
uniform sampler2D flow_tex;
uniform vec2 flow_size;

void main()
{
	int patch_x = gl_InstanceID % width_patches;
	int patch_y = gl_InstanceID / width_patches;

	// Convert the patch index to being the full 0..1 range, to match where
	// the motion search puts the patches. We don't bother with the locking
	// to texel centers, though.
	vec2 patch_center = ivec2(patch_x, patch_y) / (flow_size - 1.0);

	// Increase the patch size a bit; since patch spacing is not necessarily
	// an integer number of pixels, and we don't use conservative rasterization,
	// we could be missing the outer edges of the patch. And it seemingly helps
	// a little bit in general to have some more candidates as well -- although
	// this is measured without variational refinement, so it might be moot
	// with it.
	//
	// This maps [0.0,1.0] to [-0.25,1.25], ie. extends the patch by 25% in
	// all directions.
	vec2 grown_pos = (position * 1.5) - 0.25;

	image_pos = patch_center + patch_size * (grown_pos - 0.5f);

	// Find the flow value for this patch, and send it on to the fragment shader.
	vec3 flow_du_and_mean_diff = texelFetch(flow_tex, ivec2(patch_x, patch_y), 0).xyz;
	flow_du = flow_du_and_mean_diff.xy;
	mean_diff = flow_du_and_mean_diff.z;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * image_pos.x - 1.0, 2.0 * image_pos.y - 1.0, -1.0, 1.0);
}
