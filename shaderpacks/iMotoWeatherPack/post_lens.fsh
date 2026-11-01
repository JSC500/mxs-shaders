#version 150

/*
 The last pass: the default pack's lens, then the ReShade effects translated in
 reshade.glsl. Everything else comes from the default pack.
*/

in vec2 v_uv;
out vec4 f_color;

#include "common.glsl"
#include "lens.glsl"
#include "reshade.glsl"

void main()
{
	f_color = vec4(reshade_finish(v_uv), 1.0);
}
