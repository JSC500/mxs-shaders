#version 150

/* Bikes, riders and objects: chunky warped textures with simple lighting. */

in vec3 interp_normal, interp_tangent, interp_viewer;
in vec2 interp_texcoord;
noperspective in vec2 interp_affine;

uniform sampler2D u_tex, u_nmap, u_smap;
uniform vec3 u_fog_color;
uniform float u_fog_density;
uniform vec3 u_light_position;
uniform vec4 u_ambient;
uniform vec3 u_diffuse;
uniform int u_nbones;

out vec4 f_fragcolor;

#include "common.glsl"
#include "psx_texture.glsl"

const float SKINNED_SELF_SHADOW_IGNORE = 2.2;	/* see the default pack */
const float TEXTURE_SIZE = 256.0;

void main()
{
	vec4 tex = psx_texture(u_tex, interp_affine, TEXTURE_SIZE);
	float alpha = u_ambient.a * tex.a;
	vec3 n, lit;
	float sun;

	/* The console's transparency was on or off. */
	if (alpha < 0.5)
		discard;
	if (u_shadow_pass != 0) {
		f_fragcolor = vec4(0.0);
		return;
	}

	n = normalize(interp_normal);
	sun = max(dot(n, normalize(u_light_position)), 0.0) * sun_shadow(interp_viewer, n, u_nbones != 0 ? SKINNED_SELF_SHADOW_IGNORE : 0.0);
	lit = to_linear(tex.rgb) * (sky_light(u_ambient.rgb) + to_linear(u_diffuse) * sun * sun_boost());
	f_fragcolor = vec4(finish_color(lit, u_fog_color, u_fog_density), alpha);
}
