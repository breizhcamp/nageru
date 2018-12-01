#version 450 core

in vec2 tc;
out vec2 out_flow;

uniform sampler2D left_tex, right_tex, up_tex, down_tex;

void main()
{
	// Some of these may contain “junk”, in the sense that they were
	// not written in the given pass, if they came from an edge.
	// Most of the time, this is benign, since it means we'll get
	// the previous value (left/right/up) again. However, if it were
	// bogus on the very first pass, we need to exclude it.
	// Thus the test for 100.0f (invalid flows are initialized to 1000,
	// all valid ones are less than 1).
	vec2 left = texture(left_tex, tc).xy;
	vec2 right = texture(right_tex, tc).xy;
	vec2 up = texture(up_tex, tc).xy;
	vec2 down = texture(down_tex, tc).xy;

	vec2 sum = vec2(0.0f);
	float num = 0.0f;
	if (left.x < 100.0f) {
		sum = left;
		num = 1.0f;
	}
	if (right.x < 100.0f) {
		sum += right;
		num += 1.0f;
	}
	if (up.x < 100.0f) {
		sum += up;
		num += 1.0f;
	}
	if (down.x < 100.0f) {
		sum += down;
		num += 1.0f;
	}

	// If _all_ of them were 0, this would mean the entire row _and_ column
	// would be devoid of flow. If so, the zero flow is fine for our purposes.
	if (num == 0.0f) {
		out_flow = vec2(0.0f);
	} else {
		out_flow = sum / num;
	}
}
