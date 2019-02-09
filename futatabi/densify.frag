#version 450 core

in vec2 image_pos;
flat in int image0_layer, image1_layer;
flat in vec2 flow_du;
flat in float mean_diff;
out vec3 flow_contribution;

uniform sampler2DArray image_tex;

void main()
{
	// Equation (3) from the paper. We're using additive blending, so the
	// sum will happen automatically for us, and normalization happens on
	// next read.
	//
	// Note that equation (2) says 1 for the minimum error, but the code says 2.0.
	// And it says L2 norm, but really, the code does absolute value even for
	// L2 error norm (it uses a square root formula for L1 norm).
	float diff = texture(image_tex, vec3(image_pos, image0_layer)).x - texture(image_tex, vec3(image_pos + flow_du, image1_layer)).x;
	diff -= mean_diff;
	float weight = 1.0 / max(abs(diff), 2.0 / 255.0);
	flow_contribution = vec3(flow_du.x * weight, flow_du.y * weight, weight);
}
