#version 450 core

in vec2 position;
out vec2 flow_tc;
out vec2 patch_center;

uniform sampler2D flow_tex;

void main()
{
	// Patch placement: We want the outermost patches to have centers exactly in the
	// image corners, so that the bottom-left patch has centre (0,0) and the
	// upper-right patch has center (1,1). The position we get in is _almost_ there;
	// since the quad's corners are in (0,0) and (1,1), the fragment shader will get
	// centers in x=0.5/w, x=1.5/w and so on (and similar for y).
	//
	// In other words, find some f(x) = ax + b so that
	//
	//   a 0.5 / w + b = 0
	//   a (1.0 - 0.5 / w) + b = 1
	//
	// which gives
	//
	//   a = 1 / (w - 1)
	//   b = w / 2 (w - 1)
	vec2 flow_size = textureSize(flow_tex, 0);
	vec2 a = flow_size / (flow_size - 1);
	vec2 b = -1.0 / (2 * (flow_size - 1.0));
	patch_center = a * position + b;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	flow_tc = position;
}
