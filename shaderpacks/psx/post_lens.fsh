#version 150

/*
 The last pass: the default pack's lens at a low resolution, so the settings still
 work, then the console's 15-bit dithered color.
*/

in vec2 v_uv;
out vec4 f_color;

#include "common.glsl"
#include "lens.glsl"

const float LINES = 240.0;	/* match SNAP_LINES in psx_vertex.glsl */
const float LEVELS = 31.0;	/* per channel */

float bayer(ivec2 p)
{
	const float matrix[16] = float[16](
		0.0, 8.0, 2.0, 10.0,
		12.0, 4.0, 14.0, 6.0,
		3.0, 11.0, 1.0, 9.0,
		15.0, 7.0, 13.0, 5.0);

	return matrix[(p.y & 3) * 4 + (p.x & 3)] / 16.0 - 0.5 + 1.0 / 32.0;
}

void main()
{
	vec2 size = 1.0 / u_texel;
	float scale = size.y / LINES;
	vec2 pixel = floor(v_uv * size / scale);
	vec3 c = lens_finish((pixel + 0.5) * scale * u_texel);

	c += bayer(ivec2(pixel)) / LEVELS;
	f_color = vec4(floor(clamp(c, 0.0, 1.0) * LEVELS + 0.5) / LEVELS, 1.0);
}
