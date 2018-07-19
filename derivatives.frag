#version 450 core

in vec2 tc;
out vec2 derivatives;
out float beta_0;

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

	// The nudge term in the square root in the DeepFlow paper is ζ² = 0.1² = 0.01.
	// But this is assuming a 0..255 level. Given the nonlinearities in the expression
	// where β_0 appears, there's no 100% equivalent way to adjust this
	// constant that I can see, but taking it to (0.1/255)² ~= 1.53e-7 ~=
	// 1e-7 ought to be good enough. I guess the basic idea is that it
	// will only matter for near-zero derivatives anyway. I am a tiny
	// bit worried about fp16 precision when storing these numbers, but OK.
	beta_0 = 1.0 / (derivatives.x * derivatives.x + derivatives.y * derivatives.y + 1e-7);
}
