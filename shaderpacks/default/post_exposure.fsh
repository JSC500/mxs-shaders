#version 150

/*
 Red is the average scene brightness, green the distance in focus. Both are eased
 over time the way eyes adjust.
*/

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_color, u_src, u_depth;
uniform float u_proj_a, u_proj_b;
uniform int u_first;

const float ADAPT = 0.02;	/* per frame */
const float FOCUS_ADAPT = 0.06;
const float FOCUS_MAX = 200.0;

void main()
{
	float sum = 0.0, focus = 0.0;
	vec2 last = texture(u_src, vec2(0.5)).rg;
	int x, y;

	for (y = 0; y < 9; y++) {
		for (x = 0; x < 16; x++) {
			vec3 c = texture(u_color, (vec2(x, y) + 0.5) / vec2(16.0, 9.0)).rgb;

			sum += log(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.001));
		}
	}
	sum /= 144.0;

	/* A small cross at the center, so one thin object can't grab the focus. */
	for (x = -1; x <= 1; x++) {
		for (y = -1; y <= 1; y++) {
			float d = texture(u_depth, vec2(0.5) + vec2(x, y) * vec2(0.02, 0.04)).r;

			focus += d >= 1.0 ? FOCUS_MAX : min(u_proj_b / (d * 2.0 - 1.0 + u_proj_a), FOCUS_MAX);
		}
	}
	focus /= 9.0;

	f_color = u_first != 0 ? vec4(sum, focus, 0.0, 1.0) : vec4(mix(last.r, sum, ADAPT), mix(last.g, focus, FOCUS_ADAPT), 0.0, 1.0);
}
