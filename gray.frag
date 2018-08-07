#version 450 core

in vec3 tc;
out vec4 gray;

uniform sampler2DArray tex;

void main()
{
	vec4 color = texture(tex, tc);
	gray.rgb = vec3(dot(color.rgb, vec3(0.2126f, 0.7152f, 0.0722f)));  // Rec. 709.
	gray.a = color.a;
}
