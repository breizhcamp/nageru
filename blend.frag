#version 450 core

in vec3 tc;
out vec4 rgba;

uniform sampler2DArray image_tex;
uniform sampler2D flow_tex;
uniform float alpha;

void main()
{
	vec2 flow = texture(flow_tex, tc.xy).xy;
	vec4 I_0 = texture(image_tex, vec3(tc.xy - alpha * flow, 0));
	vec4 I_1 = texture(image_tex, vec3(tc.xy + (1.0f - alpha) * flow, 1));

	// Occlusion reasoning:

	vec2 size = textureSize(image_tex, 0).xy;

	// Follow the flow back to the initial point (where we sample I_0 from), then forward again.
	// See how well we match the point we started at, which is out flow consistency.
	float d0 = alpha * length(size * (texture(flow_tex, vec2(tc.xy - alpha * flow)).xy - flow));

	// Same for d1.
	float d1 = (1.0f - alpha) * length(size * (texture(flow_tex, vec2(tc.xy + (1.0f - alpha) * flow)).xy - flow));

	if (max(d0, d1) < 3.0f) {  // Arbitrary constant, not all that tuned. The UW paper says 1.0 is fine for ground truth.
		// Both are visible, so blend.
		rgba = I_0 + alpha * (I_1 - I_0);
	} else if (d0 < d1) {
		rgba = I_0;
	} else {
		rgba = I_1;
	}

}
