#version 450 core

in vec2 image_pos;
flat in vec2 flow, I_0_check_offset, I_1_check_offset;
out vec2 out_flow;

uniform sampler2DArray gray_tex;

void main()
{
	out_flow = flow;

	// TODO: Check if we are sampling out-of-image.
	float I_0 = texture(gray_tex, vec3(image_pos + I_0_check_offset, 0)).r;
	float I_1 = texture(gray_tex, vec3(image_pos + I_1_check_offset, 1)).r;
	float diff = abs(I_1 - I_0);
	gl_FragDepth = 0.125 * diff.x;  // Make sure we stay well under the 1.0 maximum.
}
