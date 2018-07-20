#version 450 core

in vec2 tc;
out vec2 diff_flow;

uniform sampler2D diff_flow_tex;

void main()
{
	diff_flow = texture(diff_flow_tex, tc).xy;
}
