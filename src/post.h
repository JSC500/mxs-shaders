#pragma once

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;

#define SHADOW_UNIT 8	/* and the next one, kept bound during a race */

enum { CLOUDS_OFF, CLOUDS_LOW, CLOUDS_MEDIUM, CLOUDS_HIGH };	/* the Cloud Quality setting */

/* What post_run needs from the frame, and the game state it has to put back. */
typedef struct {
	GLuint scene_fbo;
	int w, h;
	const float *projection;
	const float *sun_view;		/* the light the scene is shaded with, the moon at night */
	const float *sky_sun_view;	/* where the sun really is, below the horizon too */
	const float *sun_world;		/* the same in world space, and the moon's */
	const float *moon_world;
	const float *direct_loss;	/* how much of the game's sun and sky light the time of day takes away */
	const float *ambient_loss;
	float cloud_cover;		/* the setting, 1 at 100 */
	int cloud_quality;		/* CLOUDS_ */
	const float *cloud_light;	/* world space, toward what lights the clouds: the sun, or the moon at night */
	int cloud_moon;
	const float *view_up;		/* world up in view space */
	const float *fog_color;		/* the game's, gamma encoded */
	const float *shadow_matrix;	/* view space to wide shadow map, NULL without one */
	const float *shadow_near_matrix;	/* and to the sharp map around the rider */
	float camera_height;		/* above the ground around the rider */
	float fog_amount;		/* the fog setting, 1 is the pack's default */
	float dof, saturation, contrast, split_tone;	/* the same for these */
	float sharpen, bloom, lens_flare;
	int goggles;			/* the setting is on and the camera is first person */
	float sun_moved;		/* 1 when the player moved the sun */
	const float *world_from_view;	/* this frame's camera, for weather fixed to the track */
	float time;			/* seconds, for weather that moves */
	int weather;			/* as common.glsl's WEATHER_ constants */
	float weather_amount, wet;
	float night;			/* 0 by day, 1 once twilight has gone */
	float motion;			/* blur per frame of movement */
	float blur_near;		/* what's closer moves with the camera */
	float reproject[16];		/* this frame's view space to last frame's screen */
	GLuint sky_mask;		/* the skybox's skyline around the horizon, 0 until it's known */
	int sky_mask_view;		/* debugging: tint what the mask counts as sky */
	GLuint program, vao;
	GLenum active_unit, blend_src, blend_dst;
	GLint viewport[4];
} post_state;

void post_init(PROC (WINAPI *get_proc)(LPCSTR));
void post_run(const post_state *s);
void post_reload(void);
BOOL post_ready(void);
GLuint post_sky_texture(void);	/* the clear sky in every direction, 0 until there is one */
/* The last clouds drawn, 0 if there are none, with the view they were drawn from: world to view, and x and y scale. */
GLuint post_cloud_texture(float *view_from_world, float *scale);
void post_context_lost(void);
