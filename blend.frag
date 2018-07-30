#version 450 core

in vec2 tc;
out vec4 rgba;

uniform sampler2D image0_tex, image1_tex, flow_tex;
uniform float alpha, flow_consistency_tolerance;

void main()
{
	vec2 flow = texture(flow_tex, tc).xy;
	vec4 I_0 = texture(image0_tex, tc - alpha * flow);
	vec4 I_1 = texture(image1_tex, tc + (1.0f - alpha) * flow);

	// Occlusion reasoning:

	vec2 size = textureSize(image0_tex, 0);

	// Follow the flow back to the initial point (where we sample I_0 from), then forward again.
	// See how well we match the point we started at, which is out flow consistency.
	float d0 = alpha * length(size * (texture(flow_tex, tc - alpha * flow).xy - flow));

	// Same for d1.
	float d1 = (1.0f - alpha) * length(size * (texture(flow_tex, tc + (1.0f - alpha) * flow).xy - flow));

	if (max(d0, d1) < 3.0f) {  // Arbitrary constant, not all that tuned. The UW paper says 1.0 is fine for ground truth.
		// Both are visible, so blend.
		rgba = I_0 + alpha * (I_1 - I_0);
	} else if (d0 < d1) {
		rgba = I_0;
	} else {
		rgba = I_1;
	}

}
