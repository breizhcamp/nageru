#version 450 core

in vec2 image_pos;
flat in vec2 flow, I_0_check_offset, I_1_check_offset;
out vec2 out_flow;

uniform sampler2D image0_tex, image1_tex;

void main()
{
	out_flow = flow;

	// TODO: Check if we are sampling out-of-image.
	// TODO: See whether using intensity values gives equally good results
	// as RGB, since the rest of our pipeline is intensity.
	vec3 I_0 = texture(image0_tex, image_pos + I_0_check_offset).rgb;
	vec3 I_1 = texture(image1_tex, image_pos + I_1_check_offset).rgb;
	vec3 diff = abs(I_1 - I_0);
	gl_FragDepth = diff.x + diff.y + diff.z;
}
