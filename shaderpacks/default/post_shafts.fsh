#version 150

/*
 Light shafts at half resolution. Each view ray is walked through the sun's shadow
 map, so the beams are the real shadows of trees, even with the sun off screen.
 Red is how much of the ray is in sunlight, green how much of the range it covers.
*/

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_depth;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;

#include "common.glsl"

const int STEPS = 12;
const float START = 2.0;	/* the bike right below the camera isn't part of the beams */
const float RANGE = 80.0;	/* inside the wide shadow map */

void main()
{
	float depth = texture(u_depth, v_uv).r;
	float end = RANGE, noise, sum = 0.0;
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 ray = vec3(ndc.x / u_fx, ndc.y / u_fy, -1.0);
	int i;

	if (u_shadow_on == 0) {
		f_color = vec4(1.0, 0.0, 0.0, 1.0);
		return;
	}
	if (depth < 1.0)
		end = min(u_proj_b / (depth * 2.0 - 1.0 + u_proj_a), RANGE);
	if (end <= START) {
		f_color = vec4(1.0, 0.0, 0.0, 1.0);
		return;
	}

	/* Blue noise style offsets, which the blur turns into smooth beams. */
	noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	for (i = 0; i < STEPS; i++) {
		float t = mix(START, end, (float(i) + noise) / float(STEPS));
		vec3 c = shadow_coord(u_shadow_matrix, ray * t);

		if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0)
			sum += 1.0;
		else
			sum += texture(u_shadow_map, vec3(c.xy, c.z - 0.00015));
	}
	f_color = vec4(sum / float(STEPS), (end - START) / (RANGE - START), 0.0, 1.0);
}
