#version 450 core

in vec2 tc;
out vec2 flow;

uniform sampler2D flow_tex;
uniform vec2 scale_factor;

void main()
{
	flow = texture(flow_tex, tc).xy * scale_factor;
}
