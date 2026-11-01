#version 150

in vec2 interp_texcoord;
in vec2 interp_hcoord;
in vec3 interp_viewer;
in mat3 interp_modelview;

uniform sampler2D u_tex, u_nmap, u_smap, u_heights;
uniform float u_heights_dim_inv;
uniform float u_spacing;
uniform vec3 u_fog_color;
uniform float u_fog_density;
uniform vec3 u_light_position;
uniform vec4 u_ambient;
uniform vec3 u_diffuse;
uniform vec3 u_specular_color;
uniform float u_texscale;	/* set by the game for the vertex shader: texture per world unit */
uniform int u_parallax;		/* the parallax setting in mxs-shaders.ini, as a percentage */

out vec4 f_fragcolor;

#include "common.glsl"

float ground_height(float dx, float dz)
{
	return texture(u_heights, (interp_hcoord + 0.5 + vec2(dx, dz)) * u_heights_dim_inv).r;
}

/* Central differences half a height cell either side, with up as the y axis. */
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
	vec3 side = cross(up, forward);

	return side * n.x + up * n.y + forward * n.z;
}

/*
 The game has no height data for ruts and dirt, only the colour and normal maps it
 composites at runtime. Dark detail is usually low, so height comes from each
 texel's brightness against a blurred copy, which ignores slow changes like the
 edge between dirt and grass.
*/
const int PARALLAX_MIN_STEPS = 12, PARALLAX_MAX_STEPS = 32;	/* more at shallow angles, where steps show */
const float PARALLAX_DEPTH = 0.1;	/* game units at full strength */
const float PARALLAX_GAIN = 4.0;
const float PARALLAX_MIN_SLANT = 0.4;	/* limits how far a shallow ray can slide */
const float PARALLAX_BLUR_LOD = 4.0;	/* mip levels above the sampled one */
const float PARALLAX_FADE_START = 8.0, PARALLAX_FADE_END = 30.0;

float parallax_height(vec2 uv, float lod, float blurred)
{
	float l = dot(textureLod(u_tex, uv, lod).rgb, vec3(0.2126, 0.7152, 0.0722));

	return clamp(0.5 + (l - blurred) * PARALLAX_GAIN, 0.0, 1.0);
}

vec2 parallax_coord()
{
	float amount = float(u_parallax) * 0.01 *
		       (1.0 - smoothstep(PARALLAX_FADE_START, PARALLAX_FADE_END, length(interp_viewer)));
	vec2 size = vec2(textureSize(u_tex, 0));
	vec2 dx = dFdx(interp_texcoord) * size, dy = dFdy(interp_texcoord) * size;
	float lod = max(0.5 * log2(min(dot(dx, dx), dot(dy, dy)) + 1e-6), 0.0) + 1.0;	/* smoother than the colour, or it terraces */
	float layer, slant, depth, under, last_under, blurred;
	vec3 view, ground;
	vec2 step_uv, uv;
	int i, steps;

	if (amount <= 0.0)
		return interp_texcoord;

	/* Texture space is world x and z, so the view only has to be in world space. */
	view = normalize(transpose(interp_modelview) * -interp_viewer);
	ground = ground_normal();
	slant = dot(view, ground);
	steps = int(mix(float(PARALLAX_MAX_STEPS), float(PARALLAX_MIN_STEPS), clamp(slant, 0.0, 1.0)));
	layer = 1.0 / float(steps);
	step_uv = -view.xz / max(slant, PARALLAX_MIN_SLANT) * PARALLAX_DEPTH * amount * u_texscale * layer;

	blurred = dot(textureLod(u_tex, interp_texcoord, lod + PARALLAX_BLUR_LOD).rgb, vec3(0.2126, 0.7152, 0.0722));

	/* Walks down the ray until it is under the surface, then interpolates the hit. */
	uv = interp_texcoord;
	depth = 0.0;
	under = -(1.0 - parallax_height(uv, lod, blurred));
	for (i = 0; i < steps && under < 0.0; i++) {
		last_under = under;
		uv += step_uv;
		depth += layer;
		under = depth - (1.0 - parallax_height(uv, lod, blurred));
	}
	if (i == 0)
		return uv;
	return uv - step_uv * under / max(under - last_under, 1e-4) * step(0.0, under);
}

void main()
{
	vec4 nmap, s;
	vec3 n, up, light, albedo, lit, ambient;
	vec2 uv;
	float ndl, len, power;

	if (u_shadow_pass != 0) {
		f_fragcolor = vec4(0.0);
		return;
	}

	light = normalize(u_light_position);
	uv = parallax_coord();

	/* Green holds a terrain shading term, the normal's y is rebuilt from x and z. */
	nmap = textureGrad(u_nmap, uv, dFdx(interp_texcoord), dFdy(interp_texcoord));
	len = nmap.y;
	n = nmap.rgb * 2.0 - 1.0;
	n.y = sqrt(max(1.0 - n.z * n.z - n.x * n.x, 0.0));
	n = normalize(interp_modelview * tilt_to_ground(n));
	up = normalize(interp_modelview[1]);

	albedo = to_linear(textureGrad(u_tex, uv, dFdx(interp_texcoord), dFdy(interp_texcoord)).rgb);
	ndl = max(dot(n, light), 0.0) * sun_shadow(interp_viewer, n, 0.0);

	/* Sky light falls off on surfaces facing away from the sky. */
	ambient = sky_light(u_ambient.rgb) * (0.65 + 0.35 * dot(n, up));

	lit = albedo * (ambient + to_linear(u_diffuse) * ndl * len * sun_boost());

	s = textureGrad(u_smap, uv, dFdx(interp_texcoord), dFdy(interp_texcoord));
	power = s.g * 128.0 + 0.125;
	if (ndl > 0.0)
		lit += to_linear(u_specular_color) * s.r * len * ndl *
		       specular(n, interp_viewer, light, power) * fresnel(n, interp_viewer, 0.04) * 4.0;

	f_fragcolor = vec4(finish_color(lit, u_fog_color, u_fog_density), u_ambient.a);
}
