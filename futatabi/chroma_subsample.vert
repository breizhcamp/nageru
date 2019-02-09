#version 450 core

layout(location=0) in vec2 position;
out vec2 tc0, tc1;
uniform vec2 chroma_offset_0;
uniform vec2 chroma_offset_1;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	vec2 flipped_tc = position;
	tc0 = flipped_tc + chroma_offset_0;
	tc1 = flipped_tc + chroma_offset_1;
}

