#version 450 core

in vec2 tc;
out vec2 out_flow;

uniform sampler2D tex;

void main()
{
//	out_flow = texture(tex, tc).xy;
	out_flow = tc.xy;
}
