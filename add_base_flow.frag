#version 450 core

in vec3 tc;
out vec2 diff_flow;

uniform sampler2DArray diff_flow_tex;

void main()
{
	diff_flow = texture(diff_flow_tex, tc).xy;
}
