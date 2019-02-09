#version 450 core

in vec2 tc;
out vec2 out_flow;

uniform sampler2D tex;

void main()
{
	vec2 flow = texture(tex, tc).xy;
	if (flow.x > 100.0f) {
		// Don't copy unset flows around.
		discard;
	}
	out_flow = flow;
}
