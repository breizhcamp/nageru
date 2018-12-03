#version 130

in vec2 y_tc0, y_tc1, cbcr_tc0, cbcr_tc1;
uniform sampler2D y_tex, cbcr_tex;
out vec4 FragColor;
void main() {
	float y0 = texture(y_tex, y_tc0).r;
	float y1 = texture(y_tex, y_tc1).r;
	vec2 cbcr0 = texture(cbcr_tex, cbcr_tc0).rg;
	vec2 cbcr1 = texture(cbcr_tex, cbcr_tc1).rg;
	vec2 cbcr = 0.5 * (cbcr0 + cbcr1);
	FragColor = vec4(cbcr.g, y0, cbcr.r, y1);
};
