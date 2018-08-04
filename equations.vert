#version 450 core

layout(location=0) in vec2 position;
out vec2 tc0, tc_left0, tc_down0;
out vec2 tc1, tc_left1, tc_down1;
out float line_offset;

uniform sampler2D diffusivity_tex;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);

	const vec2 half_texel = 0.5f / textureSize(diffusivity_tex, 0);

	vec2 tc = position;
	vec2 tc_left = vec2(tc.x - half_texel.x, tc.y);
	vec2 tc_down = vec2(tc.x, tc.y - half_texel.y);

	// Adjust for different texel centers.
	tc0 = vec2(tc.x - half_texel.x, tc.y);
	tc_left0 = vec2(tc_left.x - half_texel.x, tc_left.y);
	tc_down0 = vec2(tc_down.x - half_texel.x, tc_down.y);

	tc1 = vec2(tc.x + half_texel.x, tc.y);
	tc_left1 = vec2(tc_left.x + half_texel.x, tc_left.y);
	tc_down1 = vec2(tc_down.x + half_texel.x, tc_down.y);

	line_offset = position.y * textureSize(diffusivity_tex, 0).y - 0.5f;
}
