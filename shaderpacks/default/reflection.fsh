#version 150

/* Environment reflection, added over the lit model by the game. */

in vec3 interp_normal, interp_tangent, interp_viewer;
in vec2 interp_texcoord;

uniform mat3 u_envmat;
uniform sampler2D u_tex, u_nmap;
uniform vec3 u_specular_color;

out vec4 f_fragcolor;

#include "common.glsl"

/* Painted plastic reflects weakly head on and strongly at grazing angles. */
const float BASE_REFLECTION = 0.45;

/* The environment texture is polar: the center looks up, the edge straight down. */
vec2 environment_coord(vec3 dir)
{
	return 0.5 + normalize(dir.xz) * acos(dir.y) / (2.0 * PI);
}

void main()
{
	vec3 n, t, b, bumps;
	vec3 mirrored;

	n = normalize(interp_normal);
	t = normalize(interp_tangent - n * dot(n, interp_tangent));
	b = cross(n, t);
	bumps = texture(u_nmap, interp_texcoord).rgb * 2.0 - 1.0;
	n = normalize(t * bumps.x + b * bumps.y + n * bumps.z);

	mirrored = normalize(u_envmat * reflect(interp_viewer, n));
	f_fragcolor = texture(u_tex, environment_coord(mirrored)) * vec4(u_specular_color, 1.0);
	if (u_hdr != 0)
		f_fragcolor.rgb = to_linear(f_fragcolor.rgb);
	/* The environment is a daylight picture, it dims with the sky's light. */
	f_fragcolor.rgb *= fresnel(n, interp_viewer, BASE_REFLECTION) * (1.0 - u_ambient_loss);
}
