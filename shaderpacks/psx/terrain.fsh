#version 150

/* The ground: chunky textures, keeping the normal map so ruts can still be read. */

in vec2 interp_texcoord;
in vec2 interp_hcoord;
in vec3 interp_viewer;
in mat3 interp_modelview;
noperspective in vec2 interp_affine;

uniform sampler2D u_tex, u_nmap, u_heights;
uniform float u_heights_dim_inv;
uniform float u_spacing;
uniform vec3 u_fog_color;
uniform float u_fog_density;
uniform vec3 u_light_position;
uniform vec4 u_ambient;
uniform vec3 u_diffuse;

out vec4 f_fragcolor;

#include "common.glsl"
#include "psx_texture.glsl"

/* The ground's triangles are large up close, full warping would tear the track apart. */
const float AFFINE = 0.35;

/* Smaller than on models, the ground's texture repeats close together. */
const float TEXTURE_SIZE = 128.0;

float ground_height(float dx, float dz)
{
	return texture(u_heights, (interp_hcoord + 0.5 + vec2(dx, dz)) * u_heights_dim_inv).r;
}

vec3 ground_normal()
{
	return normalize(vec3(ground_height(-0.5, 0.0) - ground_height(0.5, 0.0), u_spacing,
			      ground_height(0.0, -0.5) - ground_height(0.0, 0.5)));
}

/* The normal map's axes follow world x and z, tilted to lie on the ground. */
vec3 tilt_to_ground(vec3 n)
{
	vec3 up = ground_normal();
	vec3 forward = normalize(vec3(0.0, -up.z, up.y));

	return cross(up, forward) * n.x + up * n.y + forward * n.z;
}

void main()
{
	vec2 uv = mix(interp_texcoord, interp_affine, AFFINE);
	vec4 nmap;
	vec3 n, lit;
	float sun;

	if (u_shadow_pass != 0) {
		f_fragcolor = vec4(0.0);
		return;
	}

	/* Green holds a terrain shading term, the normal's y is rebuilt from x and z. */
	nmap = psx_texture(u_nmap, uv, TEXTURE_SIZE);
	n = nmap.rgb * 2.0 - 1.0;
	n.y = sqrt(max(1.0 - n.z * n.z - n.x * n.x, 0.0));
	n = normalize(interp_modelview * tilt_to_ground(n));

	sun = max(dot(n, normalize(u_light_position)), 0.0) * sun_shadow(interp_viewer, n, 0.0);
	lit = to_linear(psx_texture(u_tex, uv, TEXTURE_SIZE).rgb) * (sky_light(u_ambient.rgb) + to_linear(u_diffuse) * sun * nmap.y * sun_boost());
	f_fragcolor = vec4(finish_color(lit, u_fog_color, u_fog_density), u_ambient.a);
}
