#version 450 core

in vec2 position;
out vec2 tc;
out float element_sum_idx;

uniform sampler2D diff_flow_tex;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	tc = position;

	vec2 element_idx = position * textureSize(diff_flow_tex, 0) - 0.5;
	element_sum_idx = element_idx.x + element_idx.y;
}
