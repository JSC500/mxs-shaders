#version 150

/*
 Red is the average scene brightness, green the distance in focus, blue how much of
 the sun is in sight for the lens flare. All are eased over time, the first two the
 way eyes adjust, the sun quickly so a flare grows and fades instead of blinking.
*/

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_color, u_src, u_depth;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;
uniform vec3 u_sun;
uniform int u_first;

const float ADAPT = 0.02;	/* per frame */
const float FOCUS_ADAPT = 0.06;
const float FOCUS_MAX = 200.0;
const float SUN_ADAPT = 0.25;
const float SUN_SPREAD = 0.012;	/* of the screen, the patch of sky checked around the sun */

/* The share of sky around the sun, fading out as it leaves the screen. */
float sun_visible()
{
	vec3 sun = normalize(u_sun);
	vec2 uv;
	float sky = 0.0, edge;
	int x, y;

	if (sun.z > -0.05)
		return 0.0;
	uv = vec2(sun.x * u_fx, sun.y * u_fy) / -sun.z * 0.5 + 0.5;
	edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
	if (edge <= 0.0)
		return 0.0;
	for (y = -2; y <= 2; y++)
		for (x = -2; x <= 2; x++)
			sky += step(1.0, texture(u_depth, uv + vec2(x, y) * SUN_SPREAD).r);
	return sky / 25.0 * smoothstep(0.0, 0.08, edge);
}

void main()
{
	float sum = 0.0, focus = 0.0, sun = sun_visible();
	vec3 last = texture(u_src, vec2(0.5)).rgb;
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

	f_color = u_first != 0 ? vec4(sum, focus, sun, 1.0) :
		vec4(mix(last.r, sum, ADAPT), mix(last.g, focus, FOCUS_ADAPT), mix(last.b, sun, SUN_ADAPT), 1.0);
}
