#version 450 core

in vec3 tc;
out vec2 flow;

uniform sampler2DArray flow_tex;
uniform vec2 scale_factor;

void main()
{
	flow = texture(flow_tex, tc).xy * scale_factor;
}
