#version 150

/*
 The game draws the sky, particles and HUD with this one shader. The mod says
 which is being drawn through u_sprite_mode, and the HUD stays exactly as it was.
*/

in vec4 interp_color;
in vec2 interp_texcoord;

uniform sampler2D u_tex;
uniform vec3 u_fog_color;
uniform float u_fog_density;
uniform mat4 u_projection;
uniform int u_sprite_mode;	/* 0 normal, 1 sky, 2 particle */
uniform vec3 u_sun_view;
uniform vec3 u_screen;

/*
 Set by the mod for each face of the skybox once it has been read. Columns are the
 face's corners at the texture's low and high u and v: (lo, lo), (hi, lo), (lo, hi),
 (hi, hi), and their w is lo u, lo v, hi u, hi v.
*/
uniform int u_sky_face_on;
uniform mat4 u_sky_face;

out vec4 f_fragcolor;

#include "common.glsl"
#include "sky.glsl"

const vec3 SUN_GLOW = vec3(1.0, 0.86, 0.66);
const float PARTICLE_SHADE = 0.55;

vec3 view_ray()
{
	vec2 ndc = gl_FragCoord.xy / u_screen.xy * 2.0 - 1.0;

	return normalize(vec3(ndc.x / u_projection[0][0], ndc.y / u_projection[1][1], -1.0));
}

void main()
{
	vec4 color = interp_color * texture(u_tex, interp_texcoord);

	/* The atmosphere above the skyline, the painted scenery below it in the light of the time of day. */
	if (u_sprite_mode == 1 && u_sky_face_on != 0) {
		vec2 st = (interp_texcoord - vec2(u_sky_face[0].w, u_sky_face[1].w)) /
			  (vec2(u_sky_face[2].w, u_sky_face[3].w) - vec2(u_sky_face[0].w, u_sky_face[1].w));
		vec3 dir = normalize(mix(mix(u_sky_face[0].xyz, u_sky_face[1].xyz, st.x), mix(u_sky_face[2].xyz, u_sky_face[3].xyz, st.x), st.y));
		vec3 painted = u_hdr != 0 ? scene_from_gamma(color.rgb) : color.rgb;

		f_fragcolor = vec4(mix(painted * painted_light(), clear_sky(dir), skyline_open(dir)), color.a);
		return;
	}

	color.rgb = apply_fog(color.rgb, u_fog_color, u_fog_density);

	if (u_sprite_mode == 1 && dot(u_sun_view, u_sun_view) > 0.0) {
		/*
		 A wide warm haze only. The sky texture has its own sun, slightly off the
		 lighting direction, so a tight core here would show as a second disc.
		*/
		float d = max(dot(view_ray(), normalize(u_sun_view)), 0.0);

		color.rgb += SUN_GLOW * (0.1 * pow(d, 6.0) + 0.1 * pow(d, 32.0)) * (1.0 - u_night) * sun_through_clouds(u_sun_view);
		if (u_hdr != 0)
			color.rgb = scene_from_gamma(color.rgb);
	} else if (u_sprite_mode == 2) {
		/* Dust in shade shouldn't glow as if it were in the sun. */
		float z = -u_projection[3][2] / (gl_FragCoord.z * 2.0 - 1.0 + u_projection[2][2]);
		vec2 ndc = gl_FragCoord.xy / u_screen.xy * 2.0 - 1.0;
		vec3 pos = vec3(-ndc.x * z / u_projection[0][0], -ndc.y * z / u_projection[1][1], z);

		color.rgb *= (1.0 - u_ambient_loss) * PARTICLE_SHADE +
			     light_tint() * (1.0 - PARTICLE_SHADE) * sun_shadow_fast(pos) * cloud_shadow(pos, u_sun_view);
	}

	f_fragcolor = color;
}
