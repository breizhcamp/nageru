#version 130

in vec2 position;
in vec2 texcoord;
out vec2 y_tc0, y_tc1, cbcr_tc0, cbcr_tc1;
uniform vec2 foo_luma_offset_0;
uniform vec2 foo_luma_offset_1;
uniform vec2 foo_chroma_offset_0;
uniform vec2 foo_chroma_offset_1;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	vec2 flipped_tc = texcoord;
	y_tc0 = flipped_tc + foo_luma_offset_0;
	y_tc1 = flipped_tc + foo_luma_offset_1;
	cbcr_tc0 = flipped_tc + foo_chroma_offset_0;
	cbcr_tc1 = flipped_tc + foo_chroma_offset_1;
};
