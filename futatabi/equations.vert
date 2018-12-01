#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require

layout(location=0) in vec2 position;
out vec3 tc0, tc_left0, tc_down0;
out vec3 tc1, tc_left1, tc_down1;
out float line_offset;

uniform sampler2DArray diffusivity_tex;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	gl_Layer = gl_InstanceID;

	const vec2 half_texel = 0.5f / textureSize(diffusivity_tex, 0).xy;

	vec2 tc = position;
	vec2 tc_left = vec2(tc.x - half_texel.x, tc.y);
	vec2 tc_down = vec2(tc.x, tc.y - half_texel.y);

	// Adjust for different texel centers.
	tc0 = vec3(tc.x - half_texel.x, tc.y, gl_InstanceID);
	tc_left0 = vec3(tc_left.x - half_texel.x, tc_left.y, gl_InstanceID);
	tc_down0 = vec3(tc_down.x - half_texel.x, tc_down.y, gl_InstanceID);

	tc1 = vec3(tc.x + half_texel.x, tc.y, gl_InstanceID);
	tc_left1 = vec3(tc_left.x + half_texel.x, tc_left.y, gl_InstanceID);
	tc_down1 = vec3(tc_down.x + half_texel.x, tc_down.y, gl_InstanceID);

	line_offset = position.y * textureSize(diffusivity_tex, 0).y - 0.5f;
}
