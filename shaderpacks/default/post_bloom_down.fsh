#version 150

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_src;
uniform vec2 u_texel;
uniform int u_first;

const float THRESHOLD = 1.3, KNEE = 0.5;	/* linear light */

void main()
{
	vec3 c = (texture(u_src, v_uv + u_texel * vec2(-1.0, -1.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(1.0, -1.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(-1.0, 1.0)).rgb +
		  texture(u_src, v_uv + u_texel * vec2(1.0, 1.0)).rgb) * 0.25;

	/* Only the brightest parts of the scene feed the glow. */
	if (u_first != 0) {
		float l = dot(c, vec3(0.2126, 0.7152, 0.0722));

		c = min(c, vec3(8.0)) * smoothstep(THRESHOLD - KNEE, THRESHOLD + KNEE, l);
	}
	f_color = vec4(c, 1.0);
}
