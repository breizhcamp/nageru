#version 450 core
in vec2 tc0, tc1;
uniform sampler2D cbcr_tex;
out float Cb, Cr;
void main() {
	vec2 result = 0.5 * (texture(cbcr_tex, tc0).rg + texture(cbcr_tex, tc1).rg);
	Cb = result.r;
	Cr = result.g;
}

