#version 150

in vec3 interp_normal, interp_tangent, interp_viewer;
in vec2 interp_texcoord;

uniform sampler2D u_tex, u_nmap, u_smap;
uniform vec3 u_fog_color;
uniform float u_fog_density;
uniform vec3 u_light_position;
uniform vec4 u_ambient;
uniform vec3 u_diffuse;
uniform float u_shininess;
uniform vec3 u_specular_color;
uniform vec3 u_view_up;
uniform int u_nbones;	/* shared with the game's vertex shader, nonzero on skinned models */

out vec4 f_fragcolor;

#include "common.glsl"

/*
 The shadow map is a frame old and a moving bike has already left it, so a
 skinned model can't shadow itself accurately. It ignores anything this close
 along the sun's direction, about a bike's length, but still darkens under trees
 and other riders.
*/
const float SKINNED_SELF_SHADOW_IGNORE = 2.2;

const float SUN_WRAP = 0.15;
const vec3 GROUND_BOUNCE = vec3(0.38, 0.32, 0.26);
const float SKY_SHEEN = 0.25;
const float SHADED_RIDER_AMBIENT = 0.8;
const float SOLID_ALPHA = 0.97;

void main()
{
	vec4 tex, s;
	vec3 n, t, up, light, albedo, lit, sky, ambient;
	float ndl, sun, power, alpha, hemi, shade, edge;
	mat3 m;

	tex = texture(u_tex, interp_texcoord);
	alpha = u_ambient.a * tex.a;
	/* Around the cut-out edge the alpha becomes coverage, which MSAA turns into a smooth line. */
	edge = clamp((alpha - 0.75) / max(fwidth(alpha), 0.0001) + 0.5, 0.0, 1.0);
	if (edge <= 0.0)
		discard;
	if (u_shadow_pass != 0) {
		if (alpha < 0.75)
			discard;
		/* Water and glass don't cast, or they shadow themselves and flicker. */
		if (alpha < SOLID_ALPHA)
			discard;
		f_fragcolor = vec4(0.0);
		return;
	}

	light = normalize(u_light_position);

	n = normalize(interp_normal);
	t = normalize(interp_tangent - n * dot(n, interp_tangent));
	m = mat3(t, cross(n, t), n);
	n = normalize(m * (texture(u_nmap, interp_texcoord).rgb * 2.0 - 1.0));

	albedo = to_linear(tex.rgb);

	/* Bright from the sky, darker and warmer from the ground, so round shapes read as round. */
	up = dot(u_view_up, u_view_up) > 0.5 ? u_view_up : vec3(0.0, 1.0, 0.0);
	hemi = dot(n, up) * 0.5 + 0.5;
	sky = sky_light(u_ambient.rgb);
	ambient = mix(sky * GROUND_BOUNCE, sky, hemi);

	shade = sun_shadow(interp_viewer, n, u_nbones != 0 || alpha < SOLID_ALPHA ? SKINNED_SELF_SHADOW_IGNORE : 0.0);
	ndl = max(dot(n, light), 0.0) * shade;

	/* A rider in shade gets no bounce from sunlit ground, keep them in step with it. */
	if (u_nbones != 0)
		ambient *= mix(SHADED_RIDER_AMBIENT, 1.0, shade);

	/* Wrapping the sun past the edge softens the hard light-to-dark line. */
	sun = clamp((dot(n, light) + SUN_WRAP) / (1.0 + SUN_WRAP), 0.0, 1.0) * shade;

	lit = albedo * (ambient + to_linear(u_diffuse) * sun * sun_boost());

	/*
	 The spec map is either a real map with the spec color set to the sun, or
	 white with the spec color set to the material, so multiplying covers both.
	*/
	s = texture(u_smap, interp_texcoord);
	power = s.a * 128.0 + 0.125;
	if (power >= 127.875)
		power = u_shininess;
	if (ndl > 0.0)
		lit += to_linear(s.rgb * u_specular_color) * ndl *
		       specular(n, interp_viewer, light, power) * fresnel(n, interp_viewer, 0.04) * 8.0;

	/*
	 A faint sky reflection at grazing angles. Skinned models only: scenery
	 without a spec map gets a white default that would read as snow on trees.
	*/
	if (u_nbones != 0)
		lit += sky * s.rgb * to_linear(u_specular_color) * fresnel(n, interp_viewer, 0.02) * hemi * SKY_SHEEN;

	f_fragcolor = vec4(finish_color(lit, u_fog_color, u_fog_density), min(alpha, edge));
}
