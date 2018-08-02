#version 450 core

layout(location=0) in vec2 position;
out vec2 tc;

uniform float z;
uniform vec2 sample_offset;

void main()
{
	// Moving the position is equivalent to moving the texture coordinate,
	// but cheaper -- as it means some of the fullscreen quad can be clipped away.
	vec2 adjusted_pos = position - sample_offset;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * adjusted_pos.x - 1.0, 2.0 * adjusted_pos.y - 1.0, 2.0f * (z - 0.5f), 1.0);

	tc = position;
}
