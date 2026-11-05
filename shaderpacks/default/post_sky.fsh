#version 150

/*
 The clear sky in every direction, drawn into a small texture each frame for
 post_final.fsh. Linear light, at the scene's brightness.
*/

in vec2 v_uv;
out vec4 f_color;

#include "common.glsl"
#include "sky.glsl"

void main()
{
	f_color = vec4(sky_radiance(sky_lut_dir(v_uv), normalize(u_sky_sun)), 1.0);
}
