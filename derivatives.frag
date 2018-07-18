#version 450 core

in vec2 tc;
out vec2 derivatives;

uniform sampler2D tex;

void main()
{
	float x_m2 = textureOffset(tex, tc, ivec2(-2,  0)).x;
	float x_m1 = textureOffset(tex, tc, ivec2(-1,  0)).x;
	float x_p1 = textureOffset(tex, tc, ivec2( 1,  0)).x;
	float x_p2 = textureOffset(tex, tc, ivec2( 2,  0)).x;

	float y_m2 = textureOffset(tex, tc, ivec2( 0, -2)).x;
	float y_m1 = textureOffset(tex, tc, ivec2( 0, -1)).x;
	float y_p1 = textureOffset(tex, tc, ivec2( 0,  1)).x;
	float y_p2 = textureOffset(tex, tc, ivec2( 0,  2)).x;

	derivatives.x = (x_p1 - x_m1) * (2.0/3.0) + (x_m2 - x_p2) * (1.0/12.0);
	derivatives.y = (y_p1 - y_m1) * (2.0/3.0) + (y_m2 - y_p2) * (1.0/12.0);
}
