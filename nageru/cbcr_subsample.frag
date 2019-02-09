#version 130

in vec2 tc0, tc1;
uniform sampler2D cbcr_tex;
out vec4 FragColor, FragColor2;
void main() {
	FragColor = 0.5 * (texture(cbcr_tex, tc0) + texture(cbcr_tex, tc1));
	FragColor2 = FragColor;
}
