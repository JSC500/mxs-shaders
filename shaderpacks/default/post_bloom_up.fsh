#version 150

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_src;
uniform vec2 u_texel;

void main()
{
	vec3 c = (texture(u_src, v_uv + u_texel * vec2(-1.0, 0.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(1.0, 0.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(0.0, -1.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(0.0, 1.0)).rgb) * 0.25;

	f_color = vec4(c * 0.7, 1.0);
}
