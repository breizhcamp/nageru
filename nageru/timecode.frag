#version 130

in vec2 tc0;
uniform sampler2D tex;
out vec4 Y, CbCr, YCbCr;

void main() {
	vec4 gray = texture(tex, tc0);;
	gray.r = gray.r * ((235.0-16.0)/255.0) + 16.0/255.0;  // Limited-range Y'CbCr.
	CbCr = vec4(128.0/255.0, 128.0/255.0, 0.0, 1.0);;
	Y = gray.rrra;
	YCbCr = vec4(Y.r, CbCr.r, CbCr.g, CbCr.a);
}
