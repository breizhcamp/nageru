#version 450 core

in vec2 tc;
out vec2 gradients;

uniform sampler2D tex;
uniform float inv_width, inv_height;

void main()
{
	// There are two common Sobel filters, horizontal and vertical
	// (see e.g. Wikipedia, or the OpenCV documentation):
	//
	//  [-1 0 1]     [ 1  2  1]
	//  [-2 0 2]     [ 0  0  0]
	//  [-1 0 1]     [-1 -2 -1]
	// Horizontal     Vertical
	//
	// Computing both at once allows us to get away with eight
	// texture samples instead of twelve.

	float x_left   = tc.x - inv_width;
	float x_mid    = tc.x; 
	float x_right  = tc.x + inv_width;

	float y_top    = tc.y + inv_height;  // Note the bottom-left coordinate system.
	float y_mid    = tc.y;
	float y_bottom = tc.y - inv_height;
 
	float top_left     = texture(tex, vec2(x_left,  y_top)).x;
	float left         = texture(tex, vec2(x_left,  y_mid)).x;
	float bottom_left  = texture(tex, vec2(x_left,  y_bottom)).x;

	float top          = texture(tex, vec2(x_mid,   y_top)).x;
	float bottom       = texture(tex, vec2(x_mid,   y_bottom)).x;

	float top_right    = texture(tex, vec2(x_right, y_top)).x;
	float right        = texture(tex, vec2(x_right, y_mid)).x;
	float bottom_right = texture(tex, vec2(x_right, y_bottom)).x;

	gradients.x = (top_right + 2.0f * right + bottom_right) - (top_left + 2.0f * left + bottom_left);
	gradients.y = (top_left + 2.0 * top + top_right) - (bottom_left + 2.0f * bottom + bottom_right);
}
