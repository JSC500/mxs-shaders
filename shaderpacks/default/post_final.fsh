#version 150

/* The scene in linear light, and it leaves that way: post_lens.fsh finishes it. */

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_color, u_ao, u_bloom, u_depth, u_shafts;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;
uniform float u_camera_height;	/* above the ground around the rider */
uniform float u_fog_amount;	/* the fog setting in mxs-shaders.ini, 1 at 100 */
uniform vec3 u_up, u_sun, u_fog_color;

#include "common.glsl"

const float AO_STRENGTH = 0.85;
const float CONTACT_STRENGTH = 0.6;
const float BLOOM_STRENGTH = 0.12;


/* Haze per game unit at ground level, thinning by e every FOG_HEIGHT units. */
const float FOG_DENSITY = 0.0002;
const float FOG_HEIGHT = 14.0;
const float SKY_DISTANCE = 900.0;	/* how much air a ray to the sky goes through */
const float FOG_MAX_BRIGHT = 0.85;	/* gamma encoded, near white would undo to a huge value */
const float FOG_MAX_DESCENT = 2.0;	/* in FOG_HEIGHTs, the air stops thickening below that */

const vec3 SUN_SCATTER = vec3(1.0, 0.82, 0.6);
const float FOG_SUN = 0.6;	/* how much the haze brightens toward the sun */
const float SHAFT_STRENGTH = 0.35;
const float SHAFT_AMBIENT = 0.12;	/* beams away from the sun, nearly invisible */

vec3 atmosphere(vec3 c)
{
	float depth = texture(u_depth, v_uv).r;
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 ray = vec3(ndc.x / u_fx, ndc.y / u_fy, -1.0);
	float dist = SKY_DISTANCE * length(ray);
	vec3 dir = normalize(ray);
	vec2 shafts = texture(u_shafts, v_uv).rg;
	float climb, thinning, optical, sun;

	if (depth < 1.0)
		dist = u_proj_b / (depth * 2.0 - 1.0 + u_proj_a) * length(ray);

	/* Density falls with height, so this is its exact integral along the ray. */
	climb = max(dot(dir, normalize(u_up)) * dist / FOG_HEIGHT, -FOG_MAX_DESCENT);
	thinning = abs(climb) < 0.001 ? 1.0 : (1.0 - exp(-climb)) / climb;
	optical = FOG_DENSITY * u_fog_amount * exp(-u_camera_height / FOG_HEIGHT) * dist * thinning;

	sun = pow(max(dot(dir, normalize(u_sun)), 0.0), 8.0) * (1.0 - u_night);
	c = mix(scene_from_gamma(min(u_fog_color, vec3(FOG_MAX_BRIGHT))) + SUN_SCATTER * sun * FOG_SUN, c, exp(-optical));
	return c + SUN_SCATTER * shafts.r * shafts.g * (SHAFT_AMBIENT + sun) * SHAFT_STRENGTH * (1.0 - u_night);
}

void main()
{
	vec3 c = texture(u_color, v_uv).rgb;
	vec2 occlusion = pow(texture(u_ao, v_uv).rg, vec2(2.2));


	c *= mix(1.0, occlusion.r, AO_STRENGTH);
	c *= mix(1.0, occlusion.g, CONTACT_STRENGTH);
	c = atmosphere(c);
	c += texture(u_bloom, v_uv).rgb * BLOOM_STRENGTH;

	f_color = vec4(c, 1.0);
}
