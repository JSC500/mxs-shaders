#pragma once

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;

#define SHADOW_UNIT 8	/* and the next one, kept bound during a race */

/* What post_run needs from the frame, and the game state it has to put back. */
typedef struct {
	GLuint scene_fbo;
	int w, h;
	const float *projection;
	const float *sun_view;
	const float *view_up;		/* world up in view space */
	const float *fog_color;		/* the game's, gamma encoded */
	const float *shadow_matrix;	/* view space to wide shadow map, NULL without one */
	float camera_height;		/* above the ground around the rider */
	float fog_amount;		/* the fog setting, 1 is the pack's default */
	float dof, saturation, contrast, split_tone;	/* the same for these */
	float sharpen, sun_disc, lens_flare;
	int goggles;			/* the setting is on and the camera is first person */
	float sun_moved;		/* 1 when the player moved the sun */
	const float *world_from_view;	/* this frame's camera, for weather fixed to the track */
	float time;			/* seconds, for weather that moves */
	int weather;			/* as common.glsl's WEATHER_ constants */
	float weather_amount, wet;
	float night;			/* the time of day setting, 1 has nothing from the sun */
	float motion;			/* blur per frame of movement */
	float blur_near;		/* what's closer moves with the camera */
	float reproject[16];		/* this frame's view space to last frame's screen */
	GLuint program, vao;
	GLenum active_unit, blend_src, blend_dst;
	GLint viewport[4];
} post_state;

void post_init(PROC (WINAPI *get_proc)(LPCSTR));
void post_run(const post_state *s);
void post_reload(void);
BOOL post_ready(void);
void post_context_lost(void);
