#version 450 core

in vec2 tc;
out vec2 derivatives;

uniform sampler2D tex;
uniform vec2 inv_image_size;

void main()
{
	float x_m2 = texture(tex, vec2(tc.x - 2.0 * inv_image_size.x), tc.y).x;
	float x_m1 = texture(tex, vec2(tc.x -       inv_image_size.x), tc.y).x;
	float x_p1 = texture(tex, vec2(tc.x +       inv_image_size.x), tc.y).x;
	float x_p2 = texture(tex, vec2(tc.x + 2.0 * inv_image_size.x), tc.y).x;

	float y_m2 = texture(tex, vec2(tc.x, tc.y - 2.0 * inv_image_size.y)).x;
	float y_m1 = texture(tex, vec2(tc.x, tc.y -       inv_image_size.y)).x;
	float y_p1 = texture(tex, vec2(tc.x, tc.y +       inv_image_size.y)).x;
	float y_p2 = texture(tex, vec2(tc.x, tc.y + 2.0 * inv_image_size.y)).x;

	derivatives.x = (x_p1 - x_m1) * (2.0/3.0) + (x_m2 - x_p2) * (1.0/12.0);
	derivatives.y = (y_p1 - y_m1) * (2.0/3.0) + (y_m2 - y_p2) * (1.0/12.0);
}
