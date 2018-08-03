#version 450 core

in vec2 tc;
out uint packed_gradients;

uniform sampler2D tex;

uint pack_gradients(float x, float y, float v)
{
	x = clamp(x, -0.5f, 0.5f);
	y = clamp(y, -0.5f, 0.5f);

	uint vi = uint(round(v * 255.0f));
	uint xi = uint(round((x + 0.5f) * 4095.0f));
	uint yi = uint(round((y + 0.5f) * 4095.0f));
	return vi | (xi << 8) | (yi << 20);
}

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

	float top_left     = textureOffset(tex, tc, ivec2(-1,  1)).x;  // Note the bottom-left coordinate system.
	float left         = textureOffset(tex, tc, ivec2(-1,  0)).x;
	float bottom_left  = textureOffset(tex, tc, ivec2(-1, -1)).x;

	float top          = textureOffset(tex, tc, ivec2( 0,  1)).x;
	float bottom       = textureOffset(tex, tc, ivec2( 0, -1)).x;

	float top_right    = textureOffset(tex, tc, ivec2( 1,  1)).x;
	float right        = textureOffset(tex, tc, ivec2( 1,  0)).x;
	float bottom_right = textureOffset(tex, tc, ivec2( 1, -1)).x;

	vec2 gradients;
	gradients.x = (top_right + 2.0f * right + bottom_right) - (top_left + 2.0f * left + bottom_left);
	gradients.y = (top_left + 2.0 * top + top_right) - (bottom_left + 2.0f * bottom + bottom_right);

	// Normalize so that we have a normalized unit of intensity levels per pixel.
	gradients.x *= 0.125;
	gradients.y *= 0.125;

	// Also store the actual pixel value, so that we get it “for free”
	// when we sample the gradients in motion_search.frag later.
	float center = texture(tex, tc).x;

	// Pack everything into a single 32-bit value, using simple fixed-point.
	packed_gradients = pack_gradients(gradients.x, gradients.y, center);
}
