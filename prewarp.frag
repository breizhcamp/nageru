#version 450 core

// Warps I_1 according to the flow, then computes the mean and difference to I_0.

in vec3 tc;
out float I, I_t;
out vec2 normalized_flow;

uniform sampler2DArray image_tex, flow_tex;

void main()
{
	vec3 flow = texture(flow_tex, tc).xyz;
	flow.xy /= flow.z;  // Normalize the sum coming out of the densification.

	float I_0 = texture(image_tex, tc).x;
	float I_w = texture(image_tex, vec3(tc.xy + flow.xy, 1.0f - tc.z)).x;  // NOTE: This is effectively a reverse warp since texture() is a gather operation and flow is conceptually scatter.

	I = 0.5f * (I_0 + I_w);
	I_t = I_w - I_0;
	normalized_flow = flow.xy * textureSize(image_tex, 0).xy;
}
