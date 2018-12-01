#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require

layout(location=0) in vec2 position;
out vec3 flow_tc;
out vec2 patch_center;
flat out int ref_layer, search_layer;

uniform sampler2DArray flow_tex;
uniform vec2 out_flow_size;

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
	vec2 a = out_flow_size / (out_flow_size - 1);
	vec2 b = -1.0 / (2 * (out_flow_size - 1.0));
	patch_center = a * position + b;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	flow_tc = vec3(position, gl_InstanceID);

	gl_Layer = gl_InstanceID;

	// Forward flow (0) goes from 0 to 1. Backward flow (1) goes from 1 to 0.
	ref_layer = gl_InstanceID;
	search_layer = 1 - gl_InstanceID;
}
