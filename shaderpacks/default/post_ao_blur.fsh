#version 150

/* Removes the AO noise without bleeding it across depth edges. */

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_src, u_depth;
uniform vec2 u_dir, u_texel;

void main()
{
	float center = texture(u_depth, v_uv).r;
	float weight = 0.0;
	vec2 sum = vec2(0.0);
	int i;

	for (i = -3; i <= 3; i++) {
		vec2 uv = v_uv + u_dir * u_texel * float(i);
		float w = exp(-abs(texture(u_depth, uv).r - center) * 8000.0);

		sum += texture(u_src, uv).rg * w;
		weight += w;
	}
	f_color = vec4(sum / weight, 0.0, 1.0);
}
