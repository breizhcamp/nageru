#version 450 core

in vec2 tc;
out vec2 gradients;

uniform sampler2D tex;
uniform vec2 inv_image_size;

void main()
{
	// There are two common Sobel filters, horizontal and vertical
	// (see e.g. Wikipedia, or the OpenCV documentation):
	//
	//  [1 0 -1]     [-1 -2 -1]
	//  [2 0 -2]     [ 0  0  0]
	//  [1 0 -1]     [ 1  2  1]
	// Horizontal     Vertical
	//
	// Note that Wikipedia and OpenCV gives entirely opposite definitions
	// with regards to sign! This appears to be an error in the OpenCV
	// documentation, forgetting that for convolution, the filters must be
	// flipped. We have to flip the vertical matrix again comparing to
	// Wikipedia, though, since we have bottom-left origin (y = up)
	// and they define y as pointing downwards.
	//
	// Computing both directions at once allows us to get away with eight
	// texture samples instead of twelve.

	float x_left   = tc.x - inv_image_size.x;
	float x_mid    = tc.x; 
	float x_right  = tc.x + inv_image_size.x;

	float y_top    = tc.y + inv_image_size.y;  // Note the bottom-left coordinate system.
	float y_mid    = tc.y;
	float y_bottom = tc.y - inv_image_size.y;
 
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
