#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require

layout(location=0) in vec2 position;
out vec3 tc, tc_left, tc_down;
out vec3 equation_tc_assuming_left, equation_tc_assuming_right;
out float element_x_idx;
out float element_sum_idx;

uniform sampler2DArray diff_flow_tex, diffusivity_tex;
uniform usampler2DArray equation_red_tex;

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

	tc = vec3(position, gl_InstanceID);
	tc_left = vec3(tc.x - 0.5f / textureSize(diffusivity_tex, 0).x, tc.y, gl_InstanceID);
	tc_down = vec3(tc.x, tc.y - 0.5f / textureSize(diffusivity_tex, 0).y, gl_InstanceID);

	// The equation textures have half the horizontal width, so we need to adjust the texel centers.
	// It becomes extra tricky since the SOR texture might be of odd size, and then
	// the equation texture is not exactly half the size.
	vec2 element_idx = position * textureSize(diff_flow_tex, 0).xy - 0.5f;
	float equation_texel_number_assuming_left = element_idx.x / 2.0f;
	float equation_texel_number_assuming_right = (element_idx.x - 1.0f) / 2.0f;
	equation_tc_assuming_left.x = (equation_texel_number_assuming_left + 0.5f) / textureSize(equation_red_tex, 0).x;
	equation_tc_assuming_right.x = (equation_texel_number_assuming_right + 0.5f) / textureSize(equation_red_tex, 0).x;
	equation_tc_assuming_left.y = tc.y;
	equation_tc_assuming_right.y = tc.y;
	equation_tc_assuming_left.z = gl_InstanceID;
	equation_tc_assuming_right.z = gl_InstanceID;

	element_x_idx = element_idx.x;
	element_sum_idx = element_idx.x + element_idx.y;
}
