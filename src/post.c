/*
 Post-processing, run between the 3D scene and the HUD. The multisampled scene is
 resolved to textures, the effects read those, and the result is drawn back into
 the scene framebuffer so the HUD lands on top unchanged.

 Every stage is a fragment shader from the shader pack. If one is missing or
 broken the whole stage is switched off and the game looks as it did.
*/
#include "common.h"
#include "post.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_2D 0x0DE1
#define GL_TRIANGLES 0x0004
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_RED 0x1903
#define GL_RGBA16F 0x881A
#define GL_RG16F 0x822F
#define GL_R16F 0x822D
#define GL_RG8 0x822B
#define GL_RG 0x8227
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_ONE 1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

#define BLOOM_LEVELS 4

/* Texture units above anything the game touches. */
#define UNIT_COLOR 10
#define UNIT_DEPTH 11
#define UNIT_AO 12
#define UNIT_BLOOM 13
#define UNIT_SRC 14
#define UNIT_EXPOSURE 15
#define UNIT_SHAFTS 16
#define UNIT_WEATHER_NOISE 18	/* hooks.c keeps the weather noise bound there */
#define UNIT_SKY_MASK 19
#define UNIT_SKY 20
#define UNIT_CLOUDS 21		/* the latest clouds, for post_final.fsh and next frame's sprite.fsh */
#define UNIT_CLOUD_NOISE 22

/*
 The clouds are drawn for a view this much wider than the screen, so next frame's sky
 can still find them by direction after the camera turns.
*/
#define CLOUD_MARGIN 1.15f
/*
 Last frame's clouds are reused for the camera turning, not moving. A jump further than
 this in one frame is a cut to another camera, and the clouds sit elsewhere from there.
*/
#define CLOUD_CUT 50.0f
#define CLOUD_NOISE_SIZE 64
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_R8 0x8229

#define SKY_LUT_W 256	/* match SKY_LUT_SIZE in sky.glsl */
#define SKY_LUT_H 128
#define GL_REPEAT 0x2901

typedef void (WINAPI *cap_fn)(GLenum);
typedef GLboolean (WINAPI *isenabled_fn)(GLenum);
typedef void (WINAPI *blendfunc_fn)(GLenum, GLenum);
typedef void (WINAPI *viewport_fn)(GLint, GLint, GLsizei, GLsizei);
typedef void (WINAPI *bindtexture_fn)(GLenum, GLuint);
typedef void (WINAPI *gentextures_fn)(GLsizei, GLuint *);
typedef void (WINAPI *deletetextures_fn)(GLsizei, const GLuint *);
typedef void (WINAPI *teximage2d_fn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (WINAPI *texparameteri_fn)(GLenum, GLenum, GLint);
typedef void (WINAPI *drawarrays_fn)(GLenum, GLint, GLsizei);

typedef void (WINAPI *activetexture_fn)(GLenum);
typedef GLuint (WINAPI *createshader_fn)(GLenum);
typedef void (WINAPI *shadersource_fn)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (WINAPI *compileshader_fn)(GLuint);
typedef void (WINAPI *getobjectiv_fn)(GLuint, GLenum, GLint *);
typedef void (WINAPI *getinfolog_fn)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (WINAPI *createprogram_fn)(void);
typedef void (WINAPI *attachshader_fn)(GLuint, GLuint);
typedef void (WINAPI *linkprogram_fn)(GLuint);
typedef void (WINAPI *deleteshader_fn)(GLuint);
typedef void (WINAPI *useprogram_fn)(GLuint);
typedef GLint (WINAPI *getuniformlocation_fn)(GLuint, const GLchar *);
typedef void (WINAPI *uniform1i_fn)(GLint, GLint);
typedef void (WINAPI *uniform1f_fn)(GLint, float);
typedef void (WINAPI *uniform2f_fn)(GLint, float, float);
typedef void (WINAPI *genobjects_fn)(GLsizei, GLuint *);
typedef void (WINAPI *deleteobjects_fn)(GLsizei, const GLuint *);
typedef void (WINAPI *bindobject_fn)(GLuint);
typedef void (WINAPI *bindframebuffer_fn)(GLenum, GLuint);
typedef void (WINAPI *framebuffertexture2d_fn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (WINAPI *checkframebufferstatus_fn)(GLenum);
typedef void (WINAPI *blitframebuffer_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static cap_fn gl_Enable, gl_Disable;
static isenabled_fn gl_IsEnabled;
static blendfunc_fn gl_BlendFunc;
static viewport_fn gl_Viewport;
static bindtexture_fn gl_BindTexture;
static gentextures_fn gl_GenTextures;
static deletetextures_fn gl_DeleteTextures;
static teximage2d_fn gl_TexImage2D;
static texparameteri_fn gl_TexParameteri;
static drawarrays_fn gl_DrawArrays;

static activetexture_fn gl_ActiveTexture;
static createshader_fn gl_CreateShader;
static shadersource_fn gl_ShaderSource;
static compileshader_fn gl_CompileShader;
static getobjectiv_fn gl_GetShaderiv, gl_GetProgramiv;
static getinfolog_fn gl_GetShaderInfoLog, gl_GetProgramInfoLog;
static createprogram_fn gl_CreateProgram;
static attachshader_fn gl_AttachShader;
static linkprogram_fn gl_LinkProgram;
static deleteshader_fn gl_DeleteShader;
static useprogram_fn gl_UseProgram;
static getuniformlocation_fn gl_GetUniformLocation;
static uniform1i_fn gl_Uniform1i;
static uniform1f_fn gl_Uniform1f;
static uniform2f_fn gl_Uniform2f;
static void (WINAPI *gl_Uniform3f)(GLint, float, float, float);
static void (WINAPI *gl_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const float *);
static genobjects_fn gl_GenVertexArrays, gl_GenFramebuffers;
static deleteobjects_fn gl_DeleteFramebuffers;
static deleteshader_fn gl_DeleteProgram;
static bindobject_fn gl_BindVertexArray;
static bindframebuffer_fn gl_BindFramebuffer;
static framebuffertexture2d_fn gl_FramebufferTexture2D;
static checkframebufferstatus_fn gl_CheckFramebufferStatus;
static blitframebuffer_fn gl_BlitFramebuffer;

static BOOL g_ready;
static GLuint g_vao;
static GLuint g_prog_ao, g_prog_blur, g_prog_down, g_prog_up, g_prog_final;
static GLint g_blur_dir, g_blur_texel, g_down_first, g_down_texel, g_up_texel;
static GLint g_ao_texel, g_ao_a, g_ao_b, g_ao_fx, g_ao_fy, g_ao_sun, g_ao_night;

static int g_w, g_h;
static GLuint g_tex_color, g_tex_depth, g_fbo_resolve;
static GLuint g_tex_ao[2], g_fbo_ao[2];
static GLuint g_tex_bloom[BLOOM_LEVELS], g_fbo_bloom[BLOOM_LEVELS];
static GLuint g_tex_exposure[2], g_fbo_exposure[2];
static int g_exposure_write;
static BOOL g_exposure_first;
static GLuint g_prog_exposure;
static GLint g_exposure_first_loc;
static GLuint g_tex_shafts[2], g_fbo_shafts[2];
static GLuint g_tex_comp, g_fbo_comp;	/* the finished scene, still linear */
static GLuint g_prog_lens;
static GLint g_lens_a, g_lens_b, g_lens_fx, g_lens_fy, g_lens_texel, g_lens_reproject, g_lens_motion, g_lens_near, g_lens_dof, g_lens_saturation, g_lens_contrast, g_lens_split_tone, g_lens_night;
static GLint g_lens_flare, g_lens_sun, g_lens_up, g_lens_goggles, g_lens_sun_moved;
static GLint g_exposure_a, g_exposure_b, g_exposure_fx, g_exposure_fy, g_exposure_sun;
static GLuint g_prog_shafts;
static GLint g_shafts_a, g_shafts_b, g_shafts_fx, g_shafts_fy, g_shafts_matrix, g_shafts_on;
static GLint g_final_a, g_final_b, g_final_fx, g_final_fy, g_final_height, g_final_up, g_final_sun, g_final_fog, g_final_amount, g_final_night;
static GLint g_final_sharpen, g_final_bloom, g_final_shadow, g_final_shadow_near, g_final_shadow_on, g_final_sky_mask_on, g_final_sky_mask_view;
static GLuint g_prog_sky, g_tex_sky, g_fbo_sky;	/* the clear sky in every direction */
static BOOL g_sky_drawn;	/* g_tex_sky holds a sky, not nothing */

static void (WINAPI *gl_TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static GLuint g_prog_clouds, g_tex_clouds[2], g_fbo_clouds[2], g_tex_cloud_noise;
static int g_cloud_w, g_cloud_h, g_cloud_write;
static BOOL g_clouds_drawn;	/* g_tex_clouds[g_cloud_write] is a finished frame */
static float g_cloud_view[16], g_cloud_scale[2];	/* what it was drawn from: world to view, x and y scale */
static float g_cloud_camera[3];	/* and where the camera was */
static unsigned g_cloud_frame;
static GLint g_clouds_fx, g_clouds_fy, g_clouds_margin, g_clouds_steps, g_clouds_light_steps, g_clouds_history;
static GLint g_clouds_light, g_clouds_moon, g_clouds_prev_view, g_clouds_frame, g_clouds_night, g_clouds_ambient_loss, g_clouds_sky_sun;
static GLint g_final_clouds_on, g_final_cloud_margin;
static void release_clouds(void);
static GLint g_sky_sun, g_sky_night;
static GLint g_final_sky_sun, g_final_moon, g_final_direct_loss, g_final_ambient_loss, g_final_light;

/* The weather uniforms common.glsl declares, in the post programs that use them. */
typedef struct {
	GLint world, time, weather, amount, wet, cover;
} weather_locs;

static weather_locs g_final_weather, g_lens_weather, g_sky_weather, g_clouds_weather;

static const char *const g_vs =
	"#version 150\n"
	"out vec2 v_uv;\n"
	"void main()\n"
	"{\n"
	"	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
	"	v_uv = p;\n"
	"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static GLuint compile(GLenum type, const char *src, const char *name)
{
	char info[2048] = "";
	GLuint s = gl_CreateShader(type);
	GLint ok = 0;

	gl_ShaderSource(s, 1, &src, NULL);
	gl_CompileShader(s);
	gl_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		gl_GetShaderInfoLog(s, sizeof info, NULL, info);
		log_msg("post: %s failed to compile:\n%s", name, info);
		gl_DeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint program(const char *file)
{
	char *src = pack_load(file);
	char info[2048] = "";
	GLuint vs, fs, p;
	GLint ok = 0;

	if (!src) {
		log_msg("post: pack has no %s", file);
		return 0;
	}
	vs = compile(GL_VERTEX_SHADER, g_vs, "post vertex shader");
	fs = compile(GL_FRAGMENT_SHADER, src, file);
	free(src);
	if (!vs || !fs)
		return 0;

	p = gl_CreateProgram();
	gl_AttachShader(p, vs);
	gl_AttachShader(p, fs);
	gl_LinkProgram(p);
	gl_DeleteShader(vs);
	gl_DeleteShader(fs);
	gl_GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		gl_GetProgramInfoLog(p, sizeof info, NULL, info);
		log_msg("post: %s failed to link:\n%s", file, info);
		return 0;
	}

	gl_UseProgram(p);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_color"), UNIT_COLOR);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_depth"), UNIT_DEPTH);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_ao"), UNIT_AO);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_bloom"), UNIT_BLOOM);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_src"), UNIT_SRC);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_exposure"), UNIT_EXPOSURE);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_shafts"), UNIT_SHAFTS);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_shadow_map"), SHADOW_UNIT);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_shadow_near_map"), SHADOW_UNIT + 1);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_weather_noise"), UNIT_WEATHER_NOISE);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_sky_mask"), UNIT_SKY_MASK);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_sky"), UNIT_SKY);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_clouds"), UNIT_CLOUDS);
	gl_Uniform1i(gl_GetUniformLocation(p, "u_cloud_noise"), UNIT_CLOUD_NOISE);
	return p;
}

static BOOL load_programs(void);

static void weather_find(weather_locs *w, GLuint p)
{
	w->world = gl_GetUniformLocation(p, "u_world_from_view");
	w->time = gl_GetUniformLocation(p, "u_time");
	w->weather = gl_GetUniformLocation(p, "u_weather");
	w->amount = gl_GetUniformLocation(p, "u_weather_amount");
	w->wet = gl_GetUniformLocation(p, "u_wet");
	w->cover = gl_GetUniformLocation(p, "u_cloud_cover");
}

static void weather_send(const weather_locs *w, const post_state *s)
{
	gl_UniformMatrix4fv(w->world, 1, 0, s->world_from_view);
	gl_Uniform1f(w->time, s->time);
	gl_Uniform1i(w->weather, s->weather);
	gl_Uniform1f(w->amount, s->weather_amount);
	gl_Uniform1f(w->wet, s->wet);
	gl_Uniform1f(w->cover, s->cloud_cover);
}

static unsigned cell_hash(int x, int y, int z, unsigned seed)
{
	unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)z * 2147483647u + seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/* Distance from p, in cells, to the nearest of one random point per cell, wrapping every cells. */
static float worley(float px, float py, float pz, int cells, unsigned seed)
{
	int cx = (int)px, cy = (int)py, cz = (int)pz, x, y, z;
	float best = 9.0f;

	for (z = -1; z <= 1; z++)
		for (y = -1; y <= 1; y++)
			for (x = -1; x <= 1; x++) {
				int gx = cx + x, gy = cy + y, gz = cz + z;
				unsigned h = cell_hash((gx + cells) % cells, (gy + cells) % cells, (gz + cells) % cells, seed);
				float dx = gx + (h & 255) / 255.0f - px;
				float dy = gy + ((h >> 8) & 255) / 255.0f - py;
				float dz = gz + ((h >> 16) & 255) / 255.0f - pz;
				float d = dx * dx + dy * dy + dz * dz;

				if (d < best)
					best = d;
			}
	return (float)sqrt(best);
}

/*
 Three octaves of tiling Worley noise, high in the gaps between round cells, so taking
 it away from a cloud's edge leaves puffs. Made once, a march reads it many times a pixel.
*/
static void cloud_noise_create(void)
{
	static unsigned char voxels[CLOUD_NOISE_SIZE * CLOUD_NOISE_SIZE * CLOUD_NOISE_SIZE];
	int x, y, z, n = CLOUD_NOISE_SIZE;

	if (!gl_TexImage3D)
		return;
	for (z = 0; z < n; z++)
		for (y = 0; y < n; y++)
			for (x = 0; x < n; x++) {
				float v = 0.0f, amp = 0.5f;
				int o;

				for (o = 0; o < 3; o++) {
					int cells = 4 << o;
					float s = (float)cells / n;

					v += fminf(worley(x * s, y * s, z * s, cells, o), 1.0f) * amp;
					amp *= 0.5f;
				}
				voxels[(z * n + y) * n + x] = (unsigned char)(fminf(v / 0.875f, 1.0f) * 255.0f + 0.5f);
			}

	gl_GenTextures(1, &g_tex_cloud_noise);
	gl_ActiveTexture(GL_TEXTURE0 + UNIT_CLOUD_NOISE);
	gl_BindTexture(GL_TEXTURE_3D, g_tex_cloud_noise);
	gl_TexImage3D(GL_TEXTURE_3D, 0, GL_R8, n, n, n, 0, GL_RED, GL_UNSIGNED_BYTE, voxels);
	gl_TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	gl_TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	gl_TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	/* Leaves the first unit selected, as the game expects at start. hooks.c puts back whatever the game chose. */
	gl_ActiveTexture(GL_TEXTURE0);
}

#define LOAD(var, name) *(PROC *)&var = get_proc(name)
#define LOAD_GL11(var, name) *(FARPROC *)&var = GetProcAddress(g_real_gl, name)

void post_init(PROC (WINAPI *get_proc)(LPCSTR))
{
	LOAD_GL11(gl_Enable, "glEnable");
	LOAD_GL11(gl_Disable, "glDisable");
	LOAD_GL11(gl_IsEnabled, "glIsEnabled");
	LOAD_GL11(gl_BlendFunc, "glBlendFunc");
	LOAD_GL11(gl_Viewport, "glViewport");
	LOAD_GL11(gl_BindTexture, "glBindTexture");
	LOAD_GL11(gl_GenTextures, "glGenTextures");
	LOAD_GL11(gl_DeleteTextures, "glDeleteTextures");
	LOAD_GL11(gl_TexImage2D, "glTexImage2D");
	LOAD_GL11(gl_TexParameteri, "glTexParameteri");
	LOAD_GL11(gl_DrawArrays, "glDrawArrays");

	LOAD(gl_ActiveTexture, "glActiveTexture");
	LOAD(gl_CreateShader, "glCreateShader");
	LOAD(gl_ShaderSource, "glShaderSource");
	LOAD(gl_CompileShader, "glCompileShader");
	LOAD(gl_GetShaderiv, "glGetShaderiv");
	LOAD(gl_GetProgramiv, "glGetProgramiv");
	LOAD(gl_GetShaderInfoLog, "glGetShaderInfoLog");
	LOAD(gl_GetProgramInfoLog, "glGetProgramInfoLog");
	LOAD(gl_CreateProgram, "glCreateProgram");
	LOAD(gl_AttachShader, "glAttachShader");
	LOAD(gl_LinkProgram, "glLinkProgram");
	LOAD(gl_DeleteShader, "glDeleteShader");
	LOAD(gl_UseProgram, "glUseProgram");
	LOAD(gl_GetUniformLocation, "glGetUniformLocation");
	LOAD(gl_Uniform1i, "glUniform1i");
	LOAD(gl_Uniform1f, "glUniform1f");
	LOAD(gl_Uniform2f, "glUniform2f");
	LOAD(gl_Uniform3f, "glUniform3f");
	LOAD(gl_UniformMatrix4fv, "glUniformMatrix4fv");
	LOAD(gl_GenVertexArrays, "glGenVertexArrays");
	LOAD(gl_GenFramebuffers, "glGenFramebuffers");
	LOAD(gl_DeleteFramebuffers, "glDeleteFramebuffers");
	LOAD(gl_BindVertexArray, "glBindVertexArray");
	LOAD(gl_BindFramebuffer, "glBindFramebuffer");
	LOAD(gl_FramebufferTexture2D, "glFramebufferTexture2D");
	LOAD(gl_CheckFramebufferStatus, "glCheckFramebufferStatus");
	LOAD(gl_BlitFramebuffer, "glBlitFramebuffer");
	LOAD(gl_DeleteProgram, "glDeleteProgram");
	LOAD(gl_TexImage3D, "glTexImage3D");

	if (!load_programs()) {
		log_msg("post-processing off");
		gl_UseProgram(0);
		return;
	}
	gl_UseProgram(0);
	gl_GenVertexArrays(1, &g_vao);
	cloud_noise_create();
	g_ready = TRUE;
	log_msg("post-processing ready");
}

/* All or nothing: a broken stage keeps every current program running. */
static BOOL load_programs(void)
{
	GLuint fresh[10];
	int i;

	fresh[9] = program("post_clouds.fsh");
	fresh[0] = program("post_ao.fsh");
	fresh[1] = program("post_ao_blur.fsh");
	fresh[2] = program("post_bloom_down.fsh");
	fresh[3] = program("post_bloom_up.fsh");
	fresh[4] = program("post_final.fsh");
	fresh[5] = program("post_exposure.fsh");
	fresh[6] = program("post_shafts.fsh");
	fresh[7] = program("post_lens.fsh");
	fresh[8] = program("post_sky.fsh");

	for (i = 0; i < 10 && fresh[i]; i++)
		;
	if (i < 10) {
		for (i = 0; i < 10; i++)
			if (fresh[i])
				gl_DeleteProgram(fresh[i]);
		return FALSE;
	}

	if (g_prog_ao) {
		gl_DeleteProgram(g_prog_ao);
		gl_DeleteProgram(g_prog_blur);
		gl_DeleteProgram(g_prog_down);
		gl_DeleteProgram(g_prog_up);
		gl_DeleteProgram(g_prog_final);
		gl_DeleteProgram(g_prog_exposure);
		gl_DeleteProgram(g_prog_shafts);
		gl_DeleteProgram(g_prog_lens);
		gl_DeleteProgram(g_prog_sky);
		gl_DeleteProgram(g_prog_clouds);
	}
	g_prog_clouds = fresh[9];
	g_clouds_fx = gl_GetUniformLocation(g_prog_clouds, "u_fx");
	g_clouds_fy = gl_GetUniformLocation(g_prog_clouds, "u_fy");
	g_clouds_margin = gl_GetUniformLocation(g_prog_clouds, "u_margin");
	g_clouds_steps = gl_GetUniformLocation(g_prog_clouds, "u_steps");
	g_clouds_light_steps = gl_GetUniformLocation(g_prog_clouds, "u_light_steps");
	g_clouds_history = gl_GetUniformLocation(g_prog_clouds, "u_history");
	g_clouds_light = gl_GetUniformLocation(g_prog_clouds, "u_cloud_light");
	g_clouds_moon = gl_GetUniformLocation(g_prog_clouds, "u_cloud_moon");
	g_clouds_prev_view = gl_GetUniformLocation(g_prog_clouds, "u_prev_view");
	g_clouds_frame = gl_GetUniformLocation(g_prog_clouds, "u_frame");
	g_clouds_night = gl_GetUniformLocation(g_prog_clouds, "u_night");
	g_clouds_ambient_loss = gl_GetUniformLocation(g_prog_clouds, "u_ambient_loss");
	g_clouds_sky_sun = gl_GetUniformLocation(g_prog_clouds, "u_sky_sun");
	weather_find(&g_clouds_weather, g_prog_clouds);
	g_prog_sky = fresh[8];
	g_sky_sun = gl_GetUniformLocation(g_prog_sky, "u_sky_sun");
	g_sky_night = gl_GetUniformLocation(g_prog_sky, "u_night");
	weather_find(&g_sky_weather, g_prog_sky);
	g_prog_lens = fresh[7];
	g_lens_a = gl_GetUniformLocation(g_prog_lens, "u_proj_a");
	g_lens_b = gl_GetUniformLocation(g_prog_lens, "u_proj_b");
	g_lens_fx = gl_GetUniformLocation(g_prog_lens, "u_fx");
	g_lens_fy = gl_GetUniformLocation(g_prog_lens, "u_fy");
	g_lens_texel = gl_GetUniformLocation(g_prog_lens, "u_texel");
	g_lens_reproject = gl_GetUniformLocation(g_prog_lens, "u_reproject");
	g_lens_motion = gl_GetUniformLocation(g_prog_lens, "u_motion");
	g_lens_near = gl_GetUniformLocation(g_prog_lens, "u_blur_near");
	g_lens_dof = gl_GetUniformLocation(g_prog_lens, "u_dof");
	g_lens_saturation = gl_GetUniformLocation(g_prog_lens, "u_saturation");
	g_lens_contrast = gl_GetUniformLocation(g_prog_lens, "u_contrast");
	g_lens_split_tone = gl_GetUniformLocation(g_prog_lens, "u_split_tone");
	g_lens_night = gl_GetUniformLocation(g_prog_lens, "u_night");
	g_lens_flare = gl_GetUniformLocation(g_prog_lens, "u_flare");
	g_lens_sun = gl_GetUniformLocation(g_prog_lens, "u_sun");
	g_lens_up = gl_GetUniformLocation(g_prog_lens, "u_up");
	g_lens_goggles = gl_GetUniformLocation(g_prog_lens, "u_goggles");
	g_lens_sun_moved = gl_GetUniformLocation(g_prog_lens, "u_sun_moved");
	weather_find(&g_lens_weather, g_prog_lens);
	g_prog_shafts = fresh[6];
	g_shafts_a = gl_GetUniformLocation(g_prog_shafts, "u_proj_a");
	g_shafts_b = gl_GetUniformLocation(g_prog_shafts, "u_proj_b");
	g_shafts_fx = gl_GetUniformLocation(g_prog_shafts, "u_fx");
	g_shafts_fy = gl_GetUniformLocation(g_prog_shafts, "u_fy");
	g_shafts_matrix = gl_GetUniformLocation(g_prog_shafts, "u_shadow_matrix");
	g_shafts_on = gl_GetUniformLocation(g_prog_shafts, "u_shadow_on");
	g_prog_exposure = fresh[5];
	g_exposure_first_loc = gl_GetUniformLocation(g_prog_exposure, "u_first");
	g_exposure_a = gl_GetUniformLocation(g_prog_exposure, "u_proj_a");
	g_exposure_b = gl_GetUniformLocation(g_prog_exposure, "u_proj_b");
	g_exposure_fx = gl_GetUniformLocation(g_prog_exposure, "u_fx");
	g_exposure_fy = gl_GetUniformLocation(g_prog_exposure, "u_fy");
	g_exposure_sun = gl_GetUniformLocation(g_prog_exposure, "u_sun");
	g_prog_ao = fresh[0];
	g_prog_blur = fresh[1];
	g_prog_down = fresh[2];
	g_prog_up = fresh[3];
	g_prog_final = fresh[4];
	g_final_a = gl_GetUniformLocation(g_prog_final, "u_proj_a");
	g_final_b = gl_GetUniformLocation(g_prog_final, "u_proj_b");
	g_final_fx = gl_GetUniformLocation(g_prog_final, "u_fx");
	g_final_fy = gl_GetUniformLocation(g_prog_final, "u_fy");
	g_final_height = gl_GetUniformLocation(g_prog_final, "u_camera_height");
	g_final_up = gl_GetUniformLocation(g_prog_final, "u_up");
	g_final_sun = gl_GetUniformLocation(g_prog_final, "u_sun");
	g_final_fog = gl_GetUniformLocation(g_prog_final, "u_fog_color");
	g_final_amount = gl_GetUniformLocation(g_prog_final, "u_fog_amount");
	g_final_night = gl_GetUniformLocation(g_prog_final, "u_night");
	g_final_sharpen = gl_GetUniformLocation(g_prog_final, "u_sharpen");
	g_final_bloom = gl_GetUniformLocation(g_prog_final, "u_bloom_amount");
	g_final_sky_mask_on = gl_GetUniformLocation(g_prog_final, "u_sky_mask_on");
	g_final_sky_mask_view = gl_GetUniformLocation(g_prog_final, "u_sky_mask_view");
	g_final_sky_sun = gl_GetUniformLocation(g_prog_final, "u_sky_sun");
	g_final_moon = gl_GetUniformLocation(g_prog_final, "u_moon");
	g_final_light = gl_GetUniformLocation(g_prog_final, "u_light");
	g_final_clouds_on = gl_GetUniformLocation(g_prog_final, "u_clouds_on");
	g_final_cloud_margin = gl_GetUniformLocation(g_prog_final, "u_cloud_margin");
	g_final_direct_loss = gl_GetUniformLocation(g_prog_final, "u_direct_loss");
	g_final_ambient_loss = gl_GetUniformLocation(g_prog_final, "u_ambient_loss");
	g_final_shadow = gl_GetUniformLocation(g_prog_final, "u_shadow_matrix");
	g_final_shadow_near = gl_GetUniformLocation(g_prog_final, "u_shadow_near_matrix");
	g_final_shadow_on = gl_GetUniformLocation(g_prog_final, "u_shadow_on");
	weather_find(&g_final_weather, g_prog_final);

	g_ao_texel = gl_GetUniformLocation(g_prog_ao, "u_texel");
	g_ao_a = gl_GetUniformLocation(g_prog_ao, "u_proj_a");
	g_ao_b = gl_GetUniformLocation(g_prog_ao, "u_proj_b");
	g_ao_fx = gl_GetUniformLocation(g_prog_ao, "u_fx");
	g_ao_fy = gl_GetUniformLocation(g_prog_ao, "u_fy");
	g_ao_sun = gl_GetUniformLocation(g_prog_ao, "u_sun");
	g_ao_night = gl_GetUniformLocation(g_prog_ao, "u_night");
	g_blur_dir = gl_GetUniformLocation(g_prog_blur, "u_dir");
	g_blur_texel = gl_GetUniformLocation(g_prog_blur, "u_texel");
	g_down_first = gl_GetUniformLocation(g_prog_down, "u_first");
	g_down_texel = gl_GetUniformLocation(g_prog_down, "u_texel");
	g_up_texel = gl_GetUniformLocation(g_prog_up, "u_texel");
	return TRUE;
}

void post_reload(void)
{
	if (!g_ready)
		return;
	if (!load_programs())
		log_msg("hot reload: keeping the current post-processing shaders");
}

static GLuint new_texture(GLint internal, GLenum format, GLenum type, int w, int h, GLint filter)
{
	GLuint t;

	gl_GenTextures(1, &t);
	gl_BindTexture(GL_TEXTURE_2D, t);
	gl_TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static GLuint new_fbo(GLuint color)
{
	GLuint f;

	gl_GenFramebuffers(1, &f);
	gl_BindFramebuffer(GL_FRAMEBUFFER, f);
	gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	return f;
}

static void release_targets(void)
{
	if (!g_fbo_resolve)
		return;
	gl_DeleteFramebuffers(1, &g_fbo_resolve);
	gl_DeleteFramebuffers(2, g_fbo_ao);
	gl_DeleteFramebuffers(BLOOM_LEVELS, g_fbo_bloom);
	gl_DeleteFramebuffers(2, g_fbo_exposure);
	gl_DeleteTextures(2, g_tex_exposure);
	gl_DeleteFramebuffers(2, g_fbo_shafts);
	gl_DeleteTextures(2, g_tex_shafts);
	gl_DeleteFramebuffers(1, &g_fbo_comp);
	gl_DeleteTextures(1, &g_tex_comp);
	gl_DeleteFramebuffers(1, &g_fbo_sky);
	gl_DeleteTextures(1, &g_tex_sky);
	release_clouds();
	gl_DeleteTextures(1, &g_tex_color);
	gl_DeleteTextures(1, &g_tex_depth);
	gl_DeleteTextures(2, g_tex_ao);
	gl_DeleteTextures(BLOOM_LEVELS, g_tex_bloom);
	g_fbo_resolve = 0;
}

static BOOL create_targets(int w, int h)
{
	GLenum status;
	int i;

	release_targets();
	g_w = w;
	g_h = h;
	g_sky_drawn = FALSE;

	gl_ActiveTexture(GL_TEXTURE0 + UNIT_SRC);
	g_tex_color = new_texture(GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_LINEAR);
	g_tex_depth = new_texture(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, w, h, GL_NEAREST);

	g_fbo_resolve = new_fbo(g_tex_color);
	gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_tex_depth, 0);
	status = gl_CheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		log_msg("post: resolve framebuffer incomplete (%x), post-processing off", status);
		g_ready = FALSE;
		return FALSE;
	}

	for (i = 0; i < 2; i++) {
		/* Red holds ambient occlusion, green contact shadows. */
		g_tex_ao[i] = new_texture(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, w / 2, h / 2, GL_LINEAR);
		g_fbo_ao[i] = new_fbo(g_tex_ao[i]);
		g_tex_shafts[i] = new_texture(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, w / 2, h / 2, GL_LINEAR);
		g_fbo_shafts[i] = new_fbo(g_tex_shafts[i]);
	}
	for (i = 0; i < BLOOM_LEVELS; i++) {
		g_tex_bloom[i] = new_texture(GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, w >> (i + 1), h >> (i + 1), GL_LINEAR);
		g_fbo_bloom[i] = new_fbo(g_tex_bloom[i]);
	}
	for (i = 0; i < 2; i++) {
		g_tex_exposure[i] = new_texture(GL_RGBA16F, GL_RGBA, GL_FLOAT, 1, 1, GL_NEAREST);
		g_fbo_exposure[i] = new_fbo(g_tex_exposure[i]);
	}
	g_tex_comp = new_texture(GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_LINEAR);
	g_fbo_comp = new_fbo(g_tex_comp);
	/* Across is the turn around the horizon, which wraps. */
	g_tex_sky = new_texture(GL_RGBA16F, GL_RGBA, GL_FLOAT, SKY_LUT_W, SKY_LUT_H, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	g_fbo_sky = new_fbo(g_tex_sky);
	g_exposure_first = TRUE;
	log_msg("post: targets %dx%d", w, h);
	return TRUE;
}

static void bind_unit(int unit, GLuint tex)
{
	gl_ActiveTexture(GL_TEXTURE0 + unit);
	gl_BindTexture(GL_TEXTURE_2D, tex);
}

static void fullscreen(void)
{
	gl_DrawArrays(GL_TRIANGLES, 0, 3);
}

static void pass_ao(const post_state *s)
{
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_ao[0]);
	gl_Viewport(0, 0, g_w / 2, g_h / 2);
	gl_UseProgram(g_prog_ao);
	gl_Uniform2f(g_ao_texel, 2.0f / g_w, 2.0f / g_h);
	gl_Uniform1f(g_ao_a, s->projection[10]);
	gl_Uniform1f(g_ao_b, s->projection[14]);
	gl_Uniform1f(g_ao_fx, s->projection[0]);
	gl_Uniform1f(g_ao_fy, s->projection[5]);
	gl_Uniform3f(g_ao_sun, s->sun_view[0], s->sun_view[1], s->sun_view[2]);
	gl_Uniform1f(g_ao_night, s->night);
	fullscreen();

	/* Blur across then down, keeping the result in g_tex_ao[0]. */
	gl_UseProgram(g_prog_blur);
	gl_Uniform2f(g_blur_texel, 2.0f / g_w, 2.0f / g_h);

	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_ao[1]);
	bind_unit(UNIT_SRC, g_tex_ao[0]);
	gl_Uniform2f(g_blur_dir, 1.0f, 0.0f);
	fullscreen();

	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_ao[0]);
	bind_unit(UNIT_SRC, g_tex_ao[1]);
	gl_Uniform2f(g_blur_dir, 0.0f, 1.0f);
	fullscreen();
}

static void pass_shafts(const post_state *s)
{
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_shafts[0]);
	gl_Viewport(0, 0, g_w / 2, g_h / 2);
	gl_UseProgram(g_prog_shafts);
	gl_Uniform1f(g_shafts_a, s->projection[10]);
	gl_Uniform1f(g_shafts_b, s->projection[14]);
	gl_Uniform1f(g_shafts_fx, s->projection[0]);
	gl_Uniform1f(g_shafts_fy, s->projection[5]);
	gl_Uniform1i(g_shafts_on, s->shadow_matrix != NULL);
	if (s->shadow_matrix)
		gl_UniformMatrix4fv(g_shafts_matrix, 1, 0, s->shadow_matrix);
	if (s->shadow_matrix)
		gl_UniformMatrix4fv(g_shafts_matrix, 1, 0, s->shadow_matrix);
	fullscreen();

	/* The same depth-aware blur as AO, across then down. */
	gl_UseProgram(g_prog_blur);
	gl_Uniform2f(g_blur_texel, 2.0f / g_w, 2.0f / g_h);

	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_shafts[1]);
	bind_unit(UNIT_SRC, g_tex_shafts[0]);
	gl_Uniform2f(g_blur_dir, 1.0f, 0.0f);
	fullscreen();

	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_shafts[0]);
	bind_unit(UNIT_SRC, g_tex_shafts[1]);
	gl_Uniform2f(g_blur_dir, 0.0f, 1.0f);
	fullscreen();
}

static void pass_bloom(void)
{
	int i;

	gl_UseProgram(g_prog_down);
	for (i = 0; i < BLOOM_LEVELS; i++) {
		int w = g_w >> (i + 1), h = g_h >> (i + 1);

		gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_bloom[i]);
		gl_Viewport(0, 0, w, h);
		bind_unit(UNIT_SRC, i ? g_tex_bloom[i - 1] : g_tex_color);
		gl_Uniform1i(g_down_first, i == 0);
		gl_Uniform2f(g_down_texel, 1.0f / w, 1.0f / h);
		fullscreen();
	}

	/* Back up the chain, each level added onto the next larger one. */
	gl_UseProgram(g_prog_up);
	gl_Enable(GL_BLEND);
	gl_BlendFunc(GL_ONE, GL_ONE);
	for (i = BLOOM_LEVELS - 2; i >= 0; i--) {
		int w = g_w >> (i + 1), h = g_h >> (i + 1);

		gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_bloom[i]);
		gl_Viewport(0, 0, w, h);
		bind_unit(UNIT_SRC, g_tex_bloom[i + 1]);
		gl_Uniform2f(g_up_texel, 1.0f / w, 1.0f / h);
		fullscreen();
	}
	gl_Disable(GL_BLEND);
}

/* Each frame eases last frame's value, so it needs two targets to swap between. */
static void pass_exposure(const post_state *s)
{
	int read = g_exposure_write;

	g_exposure_write ^= 1;
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_exposure[g_exposure_write]);
	gl_Viewport(0, 0, 1, 1);
	gl_UseProgram(g_prog_exposure);
	gl_Uniform1i(g_exposure_first_loc, g_exposure_first);
	gl_Uniform1f(g_exposure_a, s->projection[10]);
	gl_Uniform1f(g_exposure_b, s->projection[14]);
	gl_Uniform1f(g_exposure_fx, s->projection[0]);
	gl_Uniform1f(g_exposure_fy, s->projection[5]);
	gl_Uniform3f(g_exposure_sun, s->sky_sun_view[0], s->sky_sun_view[1], s->sky_sun_view[2]);
	bind_unit(UNIT_SRC, g_tex_exposure[read]);
	fullscreen();
	bind_unit(UNIT_EXPOSURE, g_tex_exposure[g_exposure_write]);
	g_exposure_first = FALSE;
}

/* Only needed where the skybox has been read, the painted sky stays elsewhere. */
static void pass_sky(const post_state *s)
{
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_sky);
	gl_Viewport(0, 0, SKY_LUT_W, SKY_LUT_H);
	gl_UseProgram(g_prog_sky);
	gl_Uniform3f(g_sky_sun, s->sun_world[0], s->sun_world[1], s->sun_world[2]);
	gl_Uniform1f(g_sky_night, s->night);
	weather_send(&g_sky_weather, s);
	fullscreen();
	bind_unit(UNIT_SKY, g_tex_sky);
	g_sky_drawn = TRUE;
}

static void release_clouds(void)
{
	if (!g_fbo_clouds[0])
		return;
	gl_DeleteFramebuffers(2, g_fbo_clouds);
	gl_DeleteTextures(2, g_tex_clouds);
	g_fbo_clouds[0] = g_fbo_clouds[1] = 0;
	g_cloud_w = g_cloud_h = 0;
	g_clouds_drawn = FALSE;
}

/*
 Clouds by Cloud Quality: the share of the screen they're drawn at, the steps through
 the cumulus layer, and the steps toward the light for each.
*/
static void pass_clouds(const post_state *s)
{
	static const int divide[] = { 1, 4, 4, 2 }, steps[] = { 0, 24, 48, 64 }, light_steps[] = { 0, 4, 5, 6 };
	int q = s->cloud_quality, w = g_w / divide[q], h = g_h / divide[q], read, i, j;
	float dx = s->world_from_view[12] - g_cloud_camera[0], dy = s->world_from_view[13] - g_cloud_camera[1];
	float dz = s->world_from_view[14] - g_cloud_camera[2];
	BOOL history = g_clouds_drawn && dx * dx + dy * dy + dz * dz < CLOUD_CUT * CLOUD_CUT;

	if (w != g_cloud_w || h != g_cloud_h) {
		release_clouds();
		gl_ActiveTexture(GL_TEXTURE0 + UNIT_SRC);
		for (i = 0; i < 2; i++) {
			g_tex_clouds[i] = new_texture(GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_LINEAR);
			g_fbo_clouds[i] = new_fbo(g_tex_clouds[i]);
		}
		g_cloud_w = w;
		g_cloud_h = h;
	}

	read = g_cloud_write;
	g_cloud_write ^= 1;
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_clouds[g_cloud_write]);
	gl_Viewport(0, 0, w, h);
	gl_UseProgram(g_prog_clouds);
	gl_Uniform1f(g_clouds_fx, s->projection[0]);
	gl_Uniform1f(g_clouds_fy, s->projection[5]);
	gl_Uniform1f(g_clouds_margin, CLOUD_MARGIN);
	gl_Uniform1i(g_clouds_steps, steps[q]);
	gl_Uniform1i(g_clouds_light_steps, light_steps[q]);
	gl_Uniform1i(g_clouds_history, history);
	gl_Uniform3f(g_clouds_light, s->cloud_light[0], s->cloud_light[1], s->cloud_light[2]);
	gl_Uniform1f(g_clouds_moon, s->cloud_moon ? 1.0f : 0.0f);
	gl_UniformMatrix4fv(g_clouds_prev_view, 1, 0, g_cloud_view);
	gl_Uniform1f(g_clouds_frame, (float)(g_cloud_frame++ % 64));
	gl_Uniform1f(g_clouds_night, s->night);
	gl_Uniform3f(g_clouds_ambient_loss, s->ambient_loss[0], s->ambient_loss[1], s->ambient_loss[2]);
	gl_Uniform3f(g_clouds_sky_sun, s->sun_world[0], s->sun_world[1], s->sun_world[2]);
	weather_send(&g_clouds_weather, s);
	bind_unit(UNIT_CLOUDS, g_tex_clouds[read]);
	fullscreen();
	bind_unit(UNIT_CLOUDS, g_tex_clouds[g_cloud_write]);

	/* The view they were drawn from: world to view is the camera's rotation, transposed. */
	memset(g_cloud_view, 0, sizeof g_cloud_view);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			g_cloud_view[j * 4 + i] = s->world_from_view[i * 4 + j];
	g_cloud_view[15] = 1.0f;
	memcpy(g_cloud_camera, &s->world_from_view[12], sizeof g_cloud_camera);
	g_cloud_scale[0] = s->projection[0] / CLOUD_MARGIN;
	g_cloud_scale[1] = s->projection[5] / CLOUD_MARGIN;
	g_clouds_drawn = TRUE;
}

GLuint post_cloud_texture(float *view_from_world, float *scale)
{
	if (!g_ready || !g_clouds_drawn)
		return 0;
	memcpy(view_from_world, g_cloud_view, sizeof g_cloud_view);
	scale[0] = g_cloud_scale[0];
	scale[1] = g_cloud_scale[1];
	return g_tex_clouds[g_cloud_write];
}

/* For sprite.fsh, which draws the sky at the start of the next frame. */
GLuint post_sky_texture(void)
{
	return g_ready && g_sky_drawn ? g_tex_sky : 0;
}

BOOL post_ready(void)
{
	return g_ready;
}

void post_run(const post_state *s)
{
	GLboolean blend, depth, cull, scissor;

	if (!g_ready)
		return;

	blend = gl_IsEnabled(GL_BLEND);
	depth = gl_IsEnabled(GL_DEPTH_TEST);
	cull = gl_IsEnabled(GL_CULL_FACE);
	scissor = gl_IsEnabled(GL_SCISSOR_TEST);
	gl_Disable(GL_BLEND);
	gl_Disable(GL_DEPTH_TEST);
	gl_Disable(GL_CULL_FACE);
	gl_Disable(GL_SCISSOR_TEST);

	gl_BindVertexArray(g_vao);

	if ((s->w != g_w || s->h != g_h) && !create_targets(s->w, s->h))
		goto restore;

	gl_BindFramebuffer(GL_READ_FRAMEBUFFER, s->scene_fbo);
	gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo_resolve);
	gl_BlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);

	bind_unit(UNIT_COLOR, g_tex_color);
	bind_unit(UNIT_DEPTH, g_tex_depth);

	pass_ao(s);
	pass_shafts(s);
	if (s->bloom > 0.0f)
		pass_bloom();
	pass_exposure(s);
	if (s->sky_mask)
		pass_sky(s);
	/* The clouds are drawn into the atmosphere, so they need the skybox read too. */
	if (s->sky_mask && s->cloud_quality != CLOUDS_OFF)
		pass_clouds(s);
	else
		release_clouds();

	bind_unit(UNIT_AO, g_tex_ao[0]);
	bind_unit(UNIT_BLOOM, g_tex_bloom[0]);
	bind_unit(UNIT_SHAFTS, g_tex_shafts[0]);
	gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_comp);
	gl_Viewport(0, 0, g_w, g_h);
	gl_UseProgram(g_prog_final);
	gl_Uniform1f(g_final_a, s->projection[10]);
	gl_Uniform1f(g_final_b, s->projection[14]);
	gl_Uniform1f(g_final_fx, s->projection[0]);
	gl_Uniform1f(g_final_fy, s->projection[5]);
	gl_Uniform1f(g_final_height, s->camera_height);
	gl_Uniform1f(g_final_amount, s->fog_amount);
	gl_Uniform1f(g_final_night, s->night);
	gl_Uniform3f(g_final_up, s->view_up[0], s->view_up[1], s->view_up[2]);
	gl_Uniform3f(g_final_sun, s->sky_sun_view[0], s->sky_sun_view[1], s->sky_sun_view[2]);
	gl_Uniform3f(g_final_sky_sun, s->sun_world[0], s->sun_world[1], s->sun_world[2]);
	gl_Uniform3f(g_final_moon, s->moon_world[0], s->moon_world[1], s->moon_world[2]);
	gl_Uniform3f(g_final_light, s->sun_view[0], s->sun_view[1], s->sun_view[2]);
	gl_Uniform1i(g_final_clouds_on, g_clouds_drawn);
	gl_Uniform1f(g_final_cloud_margin, CLOUD_MARGIN);
	gl_Uniform3f(g_final_direct_loss, s->direct_loss[0], s->direct_loss[1], s->direct_loss[2]);
	gl_Uniform3f(g_final_ambient_loss, s->ambient_loss[0], s->ambient_loss[1], s->ambient_loss[2]);
	gl_Uniform3f(g_final_fog, s->fog_color[0], s->fog_color[1], s->fog_color[2]);
	gl_Uniform1f(g_final_sharpen, s->sharpen);
	gl_Uniform1f(g_final_bloom, s->bloom);
	gl_Uniform1i(g_final_shadow_on, s->shadow_matrix != NULL ? 1 : 0);
	if (s->shadow_matrix) {
		gl_UniformMatrix4fv(g_final_shadow, 1, 0, s->shadow_matrix);
		gl_UniformMatrix4fv(g_final_shadow_near, 1, 0, s->shadow_near_matrix);
	}
	gl_Uniform1i(g_final_sky_mask_on, s->sky_mask != 0);
	gl_Uniform1i(g_final_sky_mask_view, s->sky_mask_view);
	if (s->sky_mask)
		bind_unit(UNIT_SKY_MASK, s->sky_mask);
	weather_send(&g_final_weather, s);
	fullscreen();

	gl_BindFramebuffer(GL_FRAMEBUFFER, s->scene_fbo);
	bind_unit(UNIT_SRC, g_tex_comp);
	gl_UseProgram(g_prog_lens);
	gl_Uniform1f(g_lens_a, s->projection[10]);
	gl_Uniform1f(g_lens_b, s->projection[14]);
	gl_Uniform1f(g_lens_fx, s->projection[0]);
	gl_Uniform1f(g_lens_fy, s->projection[5]);
	gl_Uniform2f(g_lens_texel, 1.0f / g_w, 1.0f / g_h);
	gl_UniformMatrix4fv(g_lens_reproject, 1, 0, s->reproject);
	gl_Uniform1f(g_lens_motion, s->motion);
	gl_Uniform1f(g_lens_near, s->blur_near);
	gl_Uniform1f(g_lens_dof, s->dof);
	gl_Uniform1f(g_lens_saturation, s->saturation);
	gl_Uniform1f(g_lens_contrast, s->contrast);
	gl_Uniform1f(g_lens_split_tone, s->split_tone);
	gl_Uniform1f(g_lens_night, s->night);
	gl_Uniform1f(g_lens_flare, s->lens_flare);
	gl_Uniform3f(g_lens_sun, s->sky_sun_view[0], s->sky_sun_view[1], s->sky_sun_view[2]);
	gl_Uniform3f(g_lens_up, s->view_up[0], s->view_up[1], s->view_up[2]);
	gl_Uniform1i(g_lens_goggles, s->goggles);
	gl_Uniform1f(g_lens_sun_moved, s->sun_moved);
	weather_send(&g_lens_weather, s);
	fullscreen();

restore:
	gl_BindFramebuffer(GL_FRAMEBUFFER, s->scene_fbo);
	gl_UseProgram(s->program);
	gl_BindVertexArray(s->vao);
	gl_ActiveTexture(s->active_unit);
	gl_Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	gl_BlendFunc(s->blend_src, s->blend_dst);
	if (blend)
		gl_Enable(GL_BLEND);
	if (depth)
		gl_Enable(GL_DEPTH_TEST);
	if (cull)
		gl_Enable(GL_CULL_FACE);
	if (scissor)
		gl_Enable(GL_SCISSOR_TEST);
}

void post_context_lost(void)
{
	g_ready = FALSE;
	g_fbo_resolve = 0;
	g_w = g_h = 0;
	g_sky_drawn = FALSE;
	g_fbo_clouds[0] = g_fbo_clouds[1] = 0;
	g_cloud_w = g_cloud_h = 0;
	g_clouds_drawn = FALSE;
}
