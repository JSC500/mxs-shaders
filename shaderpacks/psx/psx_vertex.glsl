/*
 Goes after the game's vertex shader. The console had no sub-pixel precision, so
 vertices snap to the low resolution grid, and that's the wobble. The extra coordinate
 skips perspective correction, which the fragment shaders use to warp textures.
*/
noperspective out vec2 interp_affine;

uniform int u_shadow_pass;

const float SNAP_LINES = 240.0;	/* match LINES in post_lens.fsh */

void psx_finish()
{
	interp_affine = interp_texcoord;

	/* The shadow map has to stay steady, or shadows wobble on their own. */
	if (u_shadow_pass == 0 && gl_Position.w > 0.0) {
		float half_lines = SNAP_LINES * 0.5;
		vec2 grid = vec2(half_lines * u_projection[1][1] / u_projection[0][0], half_lines);

		gl_Position.xy = floor(gl_Position.xy / gl_Position.w * grid + 0.5) / grid * gl_Position.w;
	}
}
