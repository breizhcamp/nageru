#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require

layout(location=0) in vec2 position;
out vec3 tc;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	tc.xy = position;
	tc.z = gl_InstanceID;

	gl_Layer = gl_InstanceID;
}
