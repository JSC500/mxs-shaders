#version 150

/* The last pass: blur, exposure, tone mapping and grading, all in lens.glsl. */

in vec2 v_uv;
out vec4 f_color;

#include "common.glsl"
#include "lens.glsl"

void main()
{
	f_color = vec4(lens_finish(v_uv), 1.0);
}
