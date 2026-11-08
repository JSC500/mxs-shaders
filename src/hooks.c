/*
 Records how the game renders (context type, shader sources, framebuffers and
 draw order) and applies the settings in mxs-shaders.ini: a newer GL context,
 anisotropic filtering and MSAA through a redirected default framebuffer.

 Static hooks are exports of opengl32 (listed in src/static_hooks.txt).
 Dynamic hooks are functions the game fetches with wglGetProcAddress.
*/
#include "common.h"
#include "post.h"
#include "ui.h"
#ifndef _WIN32
#include "platform.h"
#endif
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_MAX_SAMPLES 0x8D57
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_NEAREST 0x2600
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_DEBUG_OUTPUT 0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#define GL_DEBUG_TYPE_ERROR 0x824C
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_TRIANGLES 0x0004
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNSIGNED_INT 0x1405
#define GL_RGBA 0x1908
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_CLAMP 0x864F
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_FLOAT 0x1406
#define GL_NONE 0
#define GL_LINEAR 0x2601
#define GL_LEQUAL 0x0203
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_TEXTURE0 0x84C0
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092

#define HOOK(ret, name, params) \
	typedef ret (WINAPI *name##_t) params; \
	static name##_t real_##name; \
	ret WINAPI hook_##name params

static void cursor_update(void);
static void view_state_log(void);

/* --- capture state --- */

#define MAX_IDS 4096

static FILE *g_cap;
static BOOL g_cap_next, g_cap_shot;
static BOOL g_ui_key_down, g_f9_down;
static HWND g_hwnd;
static BOOL g_saw_swap;

static unsigned g_frame;
static unsigned g_draws, g_clears, g_binds, g_programs;
static unsigned g_cur_fb, g_cur_prog;

static GLenum g_shader_type[MAX_IDS];
static unsigned long long g_shader_hash[MAX_IDS];
static unsigned long long g_prog_vs[MAX_IDS], g_prog_fs[MAX_IDS];

/*
 The game gets stable uniform numbers of our own instead of GL's locations,
 because a relink for hot reload can move the real ones while the game keeps
 using what it looked up. Each entry also keeps the last value set, since a
 relink resets uniforms and some are only ever set once.
*/
#define MAX_UNIFORMS 128

typedef struct {
	char name[48];
	GLint real;
	int kind, count, transpose, size;
	GLint value;
	void *data;
} uni_entry;

static uni_entry *g_uni[MAX_IDS];
static int g_uni_count[MAX_IDS];

/* Original sources, kept so a pack shader that fails can fall back to them. */
static char *g_shader_orig[MAX_IDS];
static BOOL g_shader_replaced[MAX_IDS];
static GLuint g_prog_vs_id[MAX_IDS], g_prog_fs_id[MAX_IDS];
static const char *g_shader_role[MAX_IDS];

/* --- shadow state --- */


#define SHADOW_DEPTH_RANGE 800.0f

enum { ROLE_NONE, ROLE_TERRAIN, ROLE_MODEL };

/* Cascade 0 covers the distance, cascade 1 is a sharper map around the rider. */
#define SHADOW_CASCADES 2
#define SHADOW_NEAR_SIZE 4096	/* thin parts like levers blink at coarser sizes */
#define SHADOW_NEAR_RANGE 20.0f
#define SHADOW_RANGE 500	/* the wide map's reach in front of the camera, see SHADOW_DEPTH_RANGE */
#define SHADOW_MAX_MOTION 2.0f	/* per frame, more than this is a camera cut */
#define GROUND_FOLLOW 0.002f	/* per frame, slow enough to ride out jumps */
#define GROUND_MAX_CLIMB 60.0f	/* further than this is a new track or a teleport */
#define HUD_MENU_BOXES 8	/* the HUD draws one or two, the smallest menu seen has dozens */
#define BLUR_SHUTTER (1.0f / 60.0f)	/* seconds at motion_blur=100, the same at any frame rate */
#define BLUR_NEAR_FIRST 3.0f	/* in first person the bike ends about here */
#define BLUR_NEAR_CHASE 14.0f

enum { SPRITE_NORMAL, SPRITE_SKY, SPRITE_PARTICLE };

static GLuint g_sprite_prog;
static int g_sprite_mode = -1, g_sprite_phase;
static int g_sky_draws;	/* sky faces drawn so far this frame */
static BOOL g_particle_phase;
static GLint g_loc_sprite_mode[MAX_IDS], g_loc_sun_view[MAX_IDS], g_loc_screen[MAX_IDS], g_loc_hdr[MAX_IDS], g_loc_parallax[MAX_IDS];
static GLuint g_reflection_prog;
static int g_hdr_on;

static int g_cfg_shadows, g_cfg_shadow_size, g_cfg_shadow_range;
static BOOL g_shadow_enabled;
static GLuint g_shadow_fbo[SHADOW_CASCADES], g_shadow_tex[SHADOW_CASCADES];
static BOOL g_shadow_frame;	/* this frame draws the 3D scene, so its map can be drawn */
static BOOL g_shadow_ready;	/* a finished map exists to sample */
static unsigned char *g_rec, *g_rec_prev;
static size_t g_rec_used, g_rec_prev_used;
static GLuint g_rec_prev_prog, g_rec_prev_vao;	/* what was bound when that recording began */
static GLuint g_rec_start_prog, g_rec_start_vao;	/* and for the one being recorded */
static int g_skinned_index, g_replay_block;
static GLuint g_unit_tex[32];
static GLuint g_white_tex;	/* the helmet may never have been drawn, so its own texture isn't known */
static unsigned g_frames_without_riders;
static void full_meshes_forget(void);
static void rigid_pose_commit(void);
static BOOL g_rec_overflow;

static int g_prog_role[MAX_IDS];
static GLint g_loc_proj[MAX_IDS], g_loc_mv[MAX_IDS], g_loc_light[MAX_IDS];
static GLint g_loc_shadow_matrix[MAX_IDS], g_loc_shadow_map[MAX_IDS], g_loc_shadow_on[MAX_IDS], g_loc_night[MAX_IDS];
static GLint g_loc_direct_loss[MAX_IDS], g_loc_ambient_loss[MAX_IDS];
static GLint g_loc_world_from_view[MAX_IDS], g_loc_time[MAX_IDS], g_loc_weather[MAX_IDS], g_loc_weather_amount[MAX_IDS], g_loc_wet[MAX_IDS];
static GLint g_loc_weather_noise[MAX_IDS];
static GLint g_loc_shadow_pass[MAX_IDS], g_loc_view_up[MAX_IDS];
static GLint g_loc_shadow_near_matrix[MAX_IDS], g_loc_shadow_near_map[MAX_IDS], g_loc_nbones[MAX_IDS];
static GLint g_cur_nbones;
static unsigned g_dynamic_draws;

/*
 The HUD shares the scene's framebuffer. After the last model the game turns
 culling off for particles and back on for the HUD. Effects run at the start of
 that phase: particles write no depth, so SSAO would darken the dust.
*/
static int g_cfg_post;
static BOOL g_post_done;
static GLuint g_cur_vao, g_cur_vbo, g_cur_ibo;
static GLenum g_blend_src = 1, g_blend_dst;
static float g_proj_value[MAX_IDS][16];
static GLuint g_terrain_prog, g_model_prog;

static float g_light_view[3];	/* the sun everything uses, after the player's changes */
static float g_light_game[3];	/* as the game sent it */
static BOOL g_have_light;
static float g_light_matrix[SHADOW_CASCADES][16];	/* world to shadow map */
static float g_shadow_from_view[SHADOW_CASCADES][16];	/* this frame's view space to each map */
static float g_view_up[3], g_camera_y, g_ground_y;
static float g_view_step[16];	/* this frame's view space to last frame's */
static float g_frame_seconds;
static float g_shadow_receiver[16];	/* this frame's view space to the wide map */
static float g_shadow_receiver_near[16];
static BOOL g_shadow_receiver_valid;
static float g_prev_inv_view[16];
static BOOL g_have_prev_cam;
static GLint g_vp[4];
static GLboolean g_depth_mask = 1;
static GLenum g_active_unit = GL_TEXTURE0;

typedef struct {
	int w, h, internal, min_filter;
	BOOL aniso;
	unsigned uploads;	/* level 0 uploads, so a reused name counts as a new texture */
} tex_info;

static tex_info g_tex[MAX_IDS];
static GLuint g_bound_tex;

/* --- settings and redirected framebuffer --- */

static int g_cfg_gl_major, g_cfg_gl_minor, g_cfg_msaa, g_cfg_aniso, g_cfg_gl_debug, g_cfg_debug;
static float g_aniso, g_aniso_setting, g_lod_bias;
static unsigned g_aniso_count;
static HGLRC g_main_rc;
static GLuint g_fbo, g_fbo_color, g_fbo_depth, g_present_fbo, g_present_color;
static int g_fbo_w, g_fbo_h, g_fbo_samples;
static int g_cfg_borderless;
static wchar_t g_cfg_pack[64];
static BOOL g_hud_phase;
static int g_hud_boxes, g_hud_boxes_last;
static BOOL g_cursor_hidden;
static BOOL g_reload_requested, g_enabled_changed;	/* from the panel, done at the frame boundary */
static int g_cfg_fog, g_cfg_parallax, g_cfg_motion_blur, g_cfg_dof, g_cfg_saturation, g_cfg_contrast, g_cfg_split_tone;
static int g_cfg_hide_hud, g_cfg_hide_cursor, g_cfg_clock, g_cfg_ui_key, g_cfg_enabled;
static int g_cfg_sharpen, g_cfg_texture_sharpness, g_cfg_bloom, g_cfg_lens_flare, g_cfg_goggles, g_cfg_sun_turn;
static int g_cfg_weather, g_cfg_weather_amount, g_cfg_wet, g_cfg_cloud_cover, g_cfg_cloud_quality;
static GLint g_loc_cloud_cover[MAX_IDS];
static float g_weather_time;	/* seconds since the first frame, for drifting clouds and falling rain */

typedef void (WINAPI *glGenObjects_t)(GLsizei, GLuint *);
typedef void (WINAPI *glDeleteObjects_t)(GLsizei, const GLuint *);
typedef void (WINAPI *glBindFramebuffer_t)(GLenum, GLuint);
typedef void (WINAPI *glBindRenderbuffer_t)(GLenum, GLuint);
typedef void (WINAPI *glRenderbufferStorageMultisample_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (WINAPI *glFramebufferRenderbuffer_t)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (WINAPI *glCheckFramebufferStatus_t)(GLenum);
typedef void (WINAPI *glBlitFramebuffer_t)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (WINAPI *glGetIntegerv_t)(GLenum, GLint *);
typedef void (WINAPI *glGetFloatv_t)(GLenum, float *);
typedef GLboolean (WINAPI *glIsEnabled_t)(GLenum);
typedef GLenum (WINAPI *glGetError_t)(void);

typedef void (WINAPI *glEnable_t)(GLenum);
typedef void (WINAPI *glDisable_t)(GLenum);
typedef void (WINAPI *glTexParameterf_t)(GLenum, GLenum, float);
typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
typedef void (WINAPI *glReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

typedef void (WINAPI *glClear_t)(GLbitfield);
typedef void (WINAPI *glViewport_t)(GLint, GLint, GLsizei, GLsizei);
typedef void (WINAPI *glDepthMask_t)(GLboolean);
typedef void (WINAPI *glBindTexture_t)(GLenum, GLuint);
typedef void (WINAPI *glTexParameteri_t)(GLenum, GLenum, GLint);
typedef void (WINAPI *glTexParameterfv_t)(GLenum, GLenum, const float *);
typedef void (WINAPI *glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (WINAPI *glDrawBuffer_t)(GLenum);
typedef void (WINAPI *glReadBuffer_t)(GLenum);
typedef void (WINAPI *glActiveTexture_t)(GLenum);
typedef void (WINAPI *glUniformMatrix4fv_t)(GLint, GLsizei, GLboolean, const float *);
typedef void (WINAPI *glUniform3fv_t)(GLint, GLsizei, const float *);
typedef GLint (WINAPI *glGetUniformLocation_t)(GLuint, const GLchar *);
typedef void (WINAPI *glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (WINAPI *glProgramUniformMatrix4fv_t)(GLuint, GLint, GLsizei, GLboolean, const float *);
typedef void (WINAPI *glProgramUniform1i_t)(GLuint, GLint, GLint);
typedef void (WINAPI *glProgramUniform3fv_t)(GLuint, GLint, GLsizei, const float *);
typedef void (WINAPI *glProgramUniform1f_t)(GLuint, GLint, float);

static glClear_t real_glClear;
static glViewport_t real_glViewport;
static glDepthMask_t real_glDepthMask;
static glBindTexture_t real_glBindTexture;
static glTexParameteri_t real_glTexParameteri;
static glTexParameterfv_t real_glTexParameterfv;
static glTexImage2D_t real_glTexImage2D;
static glActiveTexture_t real_glActiveTexture;
static glUniformMatrix4fv_t real_glUniformMatrix4fv;
static glUniform3fv_t real_glUniform3fv;
static glGetUniformLocation_t real_glGetUniformLocation;
static glFramebufferTexture2D_t real_glFramebufferTexture2D;
typedef void (WINAPI *glGenTextures_t)(GLsizei, GLuint *);
static glGenTextures_t my_glGenTextures;
static glDrawBuffer_t my_glDrawBuffer;
static glReadBuffer_t my_glReadBuffer;
static glProgramUniformMatrix4fv_t my_glProgramUniformMatrix4fv;
static glProgramUniform1i_t my_glProgramUniform1i;
static glProgramUniform3fv_t my_glProgramUniform3fv;
static glProgramUniform1f_t my_glProgramUniform1f;

typedef void (WINAPI *glGetAttachedShaders_t)(GLuint, GLsizei, GLsizei *, GLuint *);
typedef void (WINAPI *glDetachShader_t)(GLuint, GLuint);
typedef void (WINAPI *glDeleteShader_t)(GLuint);
typedef GLuint (WINAPI *glCreateProgram_t)(void);
typedef void (WINAPI *glDeleteProgram_t)(GLuint);
typedef GLboolean (WINAPI *glIsProgram_t)(GLuint);
typedef GLboolean (WINAPI *glIsTexture_t)(GLuint);

typedef void (WINAPI *glUniform1i_t)(GLint, GLint);
typedef void (WINAPI *glUniform1f_t)(GLint, float);
typedef void (WINAPI *glUniform2fv_t)(GLint, GLsizei, const float *);
typedef void (WINAPI *glUniform4fv_t)(GLint, GLsizei, const float *);
typedef void (WINAPI *glUniformMatrix3fv_t)(GLint, GLsizei, GLboolean, const float *);

static glUniform1i_t real_glUniform1i;
static glUniform1f_t real_glUniform1f;
static glUniform2fv_t real_glUniform2fv;
static glUniform4fv_t real_glUniform4fv;
static glUniformMatrix3fv_t real_glUniformMatrix3fv;
static glGetAttachedShaders_t my_glGetAttachedShaders;
static glDetachShader_t my_glDetachShader;
static glDeleteShader_t my_glDeleteShader;
static glCreateProgram_t my_glCreateProgram;
static glDeleteProgram_t my_glDeleteProgram;
static glIsProgram_t my_glIsProgram;
static glIsTexture_t my_glIsTexture;

static glReadPixels_t real_glReadPixels;
typedef void (WINAPI *glGetTexImage_t)(GLenum, GLint, GLenum, GLenum, void *);
static glGetTexImage_t real_glGetTexImage;
static glBindFramebuffer_t real_glBindFramebuffer;
static glEnable_t real_glEnable;
static glDisable_t real_glDisable;
static glTexParameterf_t real_glTexParameterf;
static wglGetProcAddress_t real_wglGetProcAddress;
static glGenObjects_t my_glGenFramebuffers, my_glGenRenderbuffers;
static glDeleteObjects_t my_glDeleteFramebuffers, my_glDeleteRenderbuffers;
static glBindRenderbuffer_t my_glBindRenderbuffer;
static glRenderbufferStorageMultisample_t my_glRenderbufferStorageMultisample;
static glFramebufferRenderbuffer_t my_glFramebufferRenderbuffer;
static glCheckFramebufferStatus_t my_glCheckFramebufferStatus;
static glBlitFramebuffer_t my_glBlitFramebuffer;
static glGetIntegerv_t my_glGetIntegerv;
static glGetFloatv_t my_glGetFloatv;
static glIsEnabled_t my_glIsEnabled;
static glGetError_t my_glGetError;

typedef void (WINAPI *debug_proc_t)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef void (WINAPI *glDebugMessageCallback_t)(debug_proc_t, const void *);
typedef void (WINAPI *glGetObjectiv_t)(GLuint, GLenum, GLint *);
typedef void (WINAPI *glGetInfoLog_t)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (WINAPI *glShaderSource_t)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (WINAPI *glCompileShader_t)(GLuint);
typedef void (WINAPI *glBeginQuery_t)(GLenum, GLuint);
typedef void (WINAPI *glEndQuery_t)(GLenum);
typedef void (WINAPI *glGetQueryObjectiv_t)(GLuint, GLenum, GLint *);
typedef void (WINAPI *glGetQueryObjectui64v_t)(GLuint, GLenum, unsigned long long *);

static glShaderSource_t real_glShaderSource;
static glCompileShader_t real_glCompileShader;
static glDebugMessageCallback_t my_glDebugMessageCallback;
static glGetObjectiv_t my_glGetShaderiv, my_glGetProgramiv;
static glGetInfoLog_t my_glGetShaderInfoLog, my_glGetProgramInfoLog;
static glGenObjects_t my_glGenQueries;
static glBeginQuery_t my_glBeginQuery;
static glEndQuery_t my_glEndQuery;
static glGetQueryObjectiv_t my_glGetQueryObjectiv;
static glGetQueryObjectui64v_t my_glGetQueryObjectui64v;

#define TIMER_QUERIES 4

static GLuint g_timer[TIMER_QUERIES];
static BOOL g_timer_used[TIMER_QUERIES], g_timer_active;
static unsigned g_timer_idx;
static unsigned long long g_gpu_ns, g_gpu_samples;

static void cap(const char *fmt, ...)
{
	va_list ap;

	if (!g_cap)
		return;
	va_start(ap, fmt);
	vfprintf(g_cap, fmt, ap);
	va_end(ap);
	fputc('\n', g_cap);
}

static unsigned long long fnv1a(unsigned long long h, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void log_frame_stats(const char *source)
{
	static LARGE_INTEGER last;
	LARGE_INTEGER now, freq;
	double fps = 0.0;

	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (last.QuadPart)
		fps = 600.0 * freq.QuadPart / (double)(now.QuadPart - last.QuadPart);
	last = now;

	log_msg("frame %u: %.1f fps, gpu %.2f ms, %u draws (%u dynamic), %u clears, %u fb binds, %u program binds, aniso on %u textures (via %s)",
		g_frame, fps, g_gpu_samples ? g_gpu_ns / 1e6 / g_gpu_samples : 0.0,
		g_draws, g_dynamic_draws, g_clears, g_binds, g_programs, g_aniso_count, source);
	g_gpu_ns = g_gpu_samples = 0;
}

static void timer_begin(void)
{
	unsigned next = (g_timer_idx + 1) % TIMER_QUERIES;
	GLint ready = 0;

	if (!g_timer[0])
		return;

	/* The oldest query is the one about to be reused. */
	if (g_timer_used[next]) {
		my_glGetQueryObjectiv(g_timer[next], GL_QUERY_RESULT_AVAILABLE, &ready);
		if (ready) {
			unsigned long long ns = 0;

			my_glGetQueryObjectui64v(g_timer[next], GL_QUERY_RESULT, &ns);
			g_gpu_ns += ns;
			g_gpu_samples++;
		}
	}
	g_timer_idx = next;
	my_glBeginQuery(GL_TIME_ELAPSED, g_timer[next]);
	g_timer_used[next] = TRUE;
	g_timer_active = TRUE;
}

static void timer_end(void)
{
	if (g_timer_active)
		my_glEndQuery(GL_TIME_ELAPSED);
	g_timer_active = FALSE;
}

static void WINAPI gl_debug_message(GLenum source, GLenum type, GLuint id, GLenum severity,
				    GLsizei length, const GLchar *message, const void *user)
{
	static unsigned logged;

	(void)source; (void)severity; (void)length; (void)user;
	if (type == GL_DEBUG_TYPE_ERROR && (++logged <= 20 || logged % 1000 == 0))
		log_msg("GL error #%u %u at frame %u, prog=%u fb=%u: %s", logged, id, g_frame, g_cur_prog, g_cur_fb, message);
}

static void bind_current_fb(void);
static void hot_reload(void);
static void enabled_apply(void);
static void texture_filter_apply(void);
static float lod_bias_wanted(void);
static void program_refresh(GLuint program);
static void sprite_set_mode(int mode);
static void hdr_set(int on);
static void time_of_day_apply(BOOL race);
static void terrain_frame_end(void);

/* Saves the frame from GL, since desktop capture can't be trusted to see the game. */
static void save_frame(int w, int h)
{
	BITMAPFILEHEADER fh = { 0 };
	BITMAPINFOHEADER ih = { 0 };
	unsigned size = (unsigned)w * h * 4;
	unsigned char *pixels = malloc(size);
	char name[64];
	FILE *f;

	if (!pixels)
		return;
	real_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	real_glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	bind_current_fb();

	sprintf_s(name, sizeof name, "frame_%06u.bmp", g_frame);
	f = log_open(L"frames", name);
	if (f) {
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof fh + sizeof ih;
		fh.bfSize = fh.bfOffBits + size;
		ih.biSize = sizeof ih;
		ih.biWidth = w;
		ih.biHeight = h; /* positive means bottom-up, matching glReadPixels */
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		fwrite(&fh, sizeof fh, 1, f);
		fwrite(&ih, sizeof ih, 1, f);
		fwrite(pixels, 1, size, f);
		fclose(f);
	}
	free(pixels);
}

static void frame_boundary(const char *source)
{
	BOOL ui_key, f9, focused;

	if (g_cap) {
		cap("--- end of frame %u: %u draws, %u clears, %u fb binds, %u program binds (via %s)",
		    g_frame, g_draws, g_clears, g_binds, g_programs, source);
		fclose(g_cap);
		g_cap = NULL;
	}

	if (g_frame % 600 == 0)
		log_frame_stats(source);
	view_state_log();

	g_frame++;
	g_draws = g_clears = g_binds = g_programs = g_dynamic_draws = 0;
	g_shadow_ready = g_shadow_frame && !g_rec_overflow;
	g_shadow_frame = FALSE;
	g_post_done = g_particle_phase = g_hud_phase = FALSE;
	g_hud_boxes_last = g_hud_boxes;
	g_hud_boxes = 0;
	g_sky_draws = 0;
	cursor_update();

	/*
	 A frame after a 3D frame opens with the sky, which already belongs to the
	 linear scene. Menus don't, so a frame that wasn't 3D turns it off again.
	*/
	sprite_set_mode(g_shadow_ready ? SPRITE_SKY : SPRITE_NORMAL);
	if (!g_shadow_ready) {
		hdr_set(0);
		time_of_day_apply(FALSE);
	}

	/* This frame's recording becomes this frame's shadow map, and the last one its fallback. */
	{
		unsigned char *spare = g_rec_prev;

		g_rec_prev = g_rec;
		g_rec_prev_used = g_rec_used;
		g_rec_prev_prog = g_rec_start_prog;
		g_rec_prev_vao = g_rec_start_vao;
		g_rec_start_prog = g_cur_prog;
		g_rec_start_vao = g_cur_vao;
		g_rec = spare;
	}
	g_rec_used = 0;
	terrain_frame_end();
	if (g_replay_block)
		g_replay_block--;

	rigid_pose_commit();
	if (g_skinned_index)
		g_frames_without_riders = 0;
	else if (++g_frames_without_riders == 120)
		full_meshes_forget();
	g_skinned_index = 0;
	g_rec_overflow = FALSE;

	/* Keys only count while the game has focus, typing elsewhere shouldn't trigger them. */
	focused = g_hwnd && GetForegroundWindow() == g_hwnd;
	f9 = g_cfg_debug && focused && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
	if (f9 && !g_f9_down)
		g_cap_next = TRUE;
	g_f9_down = f9;

	if (g_enabled_changed)
		enabled_apply();
	g_enabled_changed = FALSE;
	if (g_lod_bias != lod_bias_wanted())
		texture_filter_apply();

	if (g_reload_requested)
		hot_reload();
	g_reload_requested = FALSE;

	ui_key = focused && (GetAsyncKeyState(g_cfg_ui_key) & 0x8000) != 0;
#ifndef _WIN32
	/* Linux: Escape closes the panel. preload.c keeps it from the game, which would open its menu.
	 While the panel waits for a new panel key, Escape cancels that instead (ui.c). */
	if (plat_escape_pressed() && ui_is_open() && !ui_is_reading_key())
		ui_toggle(g_hwnd);
#endif
	if ((ui_key && !g_ui_key_down && !ui_is_reading_key()) || (ui_is_open() && !focused))
		ui_toggle(g_hwnd);
	g_ui_key_down = ui_key;

	if (g_cap_next || (g_cfg_debug && (g_frame == 300 || g_frame % 1800 == 0))) {
		char name[64];

		g_cap_next = FALSE;
		sprintf_s(name, sizeof name, "frame_%06u.txt", g_frame);
		g_cap = log_open(L"frames", name);
		log_msg("capturing %s", name);
		cap("start fb=%u prog=%u", g_cur_fb, g_cur_prog);
		g_cap_shot = g_frame != 300;
	}
}

static void count_draw(void)
{
	/* Timing starts at the first draw so the frame limiter's wait isn't counted. */
	if (!g_timer_active)
		timer_begin();
	g_draws++;
}

static void bind_current_fb(void)
{
	real_glBindFramebuffer(GL_FRAMEBUFFER, g_cur_fb == 0 && g_fbo ? g_fbo : g_cur_fb);
}

#define REC_ARENA (16 * 1024 * 1024)

typedef void (WINAPI *glUseProgram_t)(GLuint);
typedef void (WINAPI *glDrawRangeElements_t)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);

typedef void (WINAPI *glDeleteVertexArrays_t)(GLsizei, const GLuint *);

static glDeleteVertexArrays_t real_glDeleteVertexArrays;
static glUseProgram_t real_glUseProgram;
static glDrawRangeElements_t real_glDrawRangeElements;
static void shadow_replay(const unsigned char *stream, size_t used, GLuint prog, GLuint vao);
static void shadow_frame_end(void);
static void shadow_forward(const float *inv_view);

/* --- shadows ---

 Every terrain and model draw is repeated into a depth map with u_projection
 swapped for a sun matrix, which needs no vertex shader of our own and so works
 with any game version's shaders, bones included. The repeat comes from the
 previous frame's recording, right before this frame's terrain: scenery hasn't
 moved, and the camera's real step tells where the rider has gone.
*/

static void mat_mul(float *out, const float *a, const float *b)
{
	int c, r;

	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
					 a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

/* The game's view matrix is a rotation and translation. */
static void mat_inv_rigid(float *o, const float *m)
{
	o[0] = m[0]; o[1] = m[4]; o[2] = m[8]; o[3] = 0.0f;
	o[4] = m[1]; o[5] = m[5]; o[6] = m[9]; o[7] = 0.0f;
	o[8] = m[2]; o[9] = m[6]; o[10] = m[10]; o[11] = 0.0f;
	o[12] = -(m[12] * o[0] + m[13] * o[4] + m[14] * o[8]);
	o[13] = -(m[12] * o[1] + m[13] * o[5] + m[14] * o[9]);
	o[14] = -(m[12] * o[2] + m[13] * o[6] + m[14] * o[10]);
	o[15] = 1.0f;
}

static float v_dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v_cross(float *o, const float *a, const float *b)
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static BOOL v_normalize(float *v)
{
	float l = (float)sqrt(v_dot(v, v));

	if (l < 1e-6f)
		return FALSE;
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return TRUE;
}

#define SUN_MIN_HEIGHT 8	/* degrees, lower and the shadows would cover the map */
#define RADIANS (3.14159265f / 180.0f)

/*
 --- time of day ---

 The Time of Day setting is 0 for the track's own light, else 1 + quarter hours past
 midnight. The sun runs a late spring day at 45 degrees north: up around five, 60
 degrees high at noon, where the track's own sun points at Sun Turn 0 so its intended
 light stays. The moon is full, opposite the sun, up all night. A little after sunset
 the game's light becomes the moon's, so the moon casts the shadows. The colour and
 strength of both lights, and of the sky's, go to the shaders as how much of the
 game's light is taken away, so 0 leaves a shader that doesn't know them as it was.
*/
#define CLOCK_TRACK 0
#define CLOCK_STEPS 96
#define LATITUDE 45.0f
#define DECLINATION 15.0f
#define MOON_TAKES_OVER -3.0f	/* the sun's height when the light becomes the moon's */
#define NIGHT_DEPTH 12.0f	/* degrees below the horizon where twilight has gone */

static const float g_zenith_depth[3] = { 0.10f, 0.17f, 0.33f };	/* of the air straight up, haze included */
/* Against the noon sun, blue to dark adapted eyes. The moon outshines the night sky, so it casts clear shadows. */
static const float g_moonlight[3] = { 0.07f, 0.085f, 0.12f };
static const float g_night_sky_light[3] = { 0.03f, 0.038f, 0.06f };
static const float g_twilight_sky_light[3] = { 0.72f, 0.84f, 1.15f };	/* the sky's light without the sun in it */

static float g_sun_world[3] = { 0.0f, 1.0f, 0.0f };	/* toward the sun, below the horizon too */
static float g_moon_world[3] = { 0.0f, -1.0f, 0.0f };
static float g_night;	/* 0 by day, 1 when twilight has gone */
static BOOL g_light_is_moon;	/* the game's light has become the moon's */
static float g_direct_loss[3], g_ambient_loss[3];

static BOOL clock_set(void)
{
	return g_cfg_enabled && g_cfg_clock > CLOCK_TRACK;
}

static BOOL sun_moved(void)
{
	return clock_set() || (g_cfg_enabled && g_cfg_sun_turn % 360 != 0);
}

static float smooth_step(float a, float b, float x)
{
	float t = (x - a) / (b - a);

	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

/* How much air the sun shines through at a height in degrees, 1 straight up (Kasten and Young). */
static float air_mass(float height)
{
	height = max(height, 0.0f);
	return 1.0f / ((float)sin(height * RADIANS) + 0.50572f * (float)pow(height + 6.07995f, -1.6364f));
}

/*
 The light's colour and strength by the sun's and moon's height in degrees, against the
 noon sun the tracks are lit for.
*/
static void daylight(float sun, float moon, float *direct, float *ambient)
{
	float level, blue, sun_up = smooth_step(-1.0f, 2.0f, sun);
	float moon_up = smooth_step(0.0f, 6.0f, moon) * smooth_step(MOON_TAKES_OVER, MOON_TAKES_OVER - 5.0f, sun);
	int i;

	/* Bright from the sky by day, a little left in twilight, then only what the moon gives. */
	level = sun >= 0.0f ? 0.3f + 0.7f * smooth_step(0.0f, 30.0f, sun) : 0.3f * smooth_step(-NIGHT_DEPTH, 0.0f, sun);
	blue = 1.0f - smooth_step(0.0f, 10.0f, sun);
	for (i = 0; i < 3; i++) {
		if (sun > MOON_TAKES_OVER)
			direct[i] = min((float)exp(-g_zenith_depth[i] * (air_mass(sun) - air_mass(60.0f))), 1.0f) * sun_up;
		else
			direct[i] = g_moonlight[i] * moon_up;
		ambient[i] = min(level * (1.0f + (g_twilight_sky_light[i] - 1.0f) * blue) + g_night_sky_light[i], 1.0f);
	}
}

/* The sun at the set time, world space, south being where the track's sun points. */
static void clock_sun(float *sun, const float *south)
{
	float hours = (g_cfg_clock - 1) * 0.25f, h = (hours - 12.0f) * 15.0f * RADIANS;
	float lat = LATITUDE * RADIANS, dec = DECLINATION * RADIANS;
	float up = (float)(sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(h));
	float east = (float)(-cos(dec) * sin(h));
	float north = (float)(cos(lat) * sin(dec) - sin(lat) * cos(dec) * cos(h));

	/* With y up, east is up x south. */
	sun[0] = south[2] * east - south[0] * north;
	sun[1] = up;
	sun[2] = -south[0] * east - south[2] * north;
}

/* A direction raised to at least a height in degrees, keeping where it points around the horizon. */
static void raise_to(float *d, float height)
{
	float flat = (float)sqrt(d[0] * d[0] + d[2] * d[2]), h;

	if ((float)asin(min(max(d[1], -1.0f), 1.0f)) >= height * RADIANS)
		return;
	if (flat < 1e-4f) {
		d[0] = flat = 1.0f;
		d[2] = 0.0f;
	}
	h = height * RADIANS;
	d[0] *= (float)cos(h) / flat;
	d[2] *= (float)cos(h) / flat;
	d[1] = (float)sin(h);
}

/*
 The game's sun in view space, turned by Sun Turn and replaced by the sun or moon at
 the set time. They act on its world direction, so the camera turning doesn't move it.
 Also works out the sky's sun and moon and the light's colour for this frame.
*/
static void sun_effective(float *out, const float *in, const float *inv_view)
{
	float w[3], light[3], south[3], len = (float)sqrt(v_dot(in, in)), turn = g_cfg_sun_turn * RADIANS;
	float x, flat, side, sun_height;
	int i;

	if (len < 1e-6f) {
		memcpy(out, in, 3 * sizeof *out);
		return;
	}
	for (i = 0; i < 3; i++)
		w[i] = (inv_view[0 + i] * in[0] + inv_view[4 + i] * in[1] + inv_view[8 + i] * in[2]) / len;
	/* Whether the game's vector runs toward the sun or away, the result stays on that side. */
	side = w[1] < 0.0f ? -1.0f : 1.0f;
	for (i = 0; i < 3; i++)
		w[i] *= side;
	if (g_cfg_enabled) {
		x = w[0] * (float)cos(turn) - w[2] * (float)sin(turn);
		w[2] = w[0] * (float)sin(turn) + w[2] * (float)cos(turn);
		w[0] = x;
	}

	if (!clock_set()) {
		memcpy(g_sun_world, w, sizeof g_sun_world);
		for (i = 0; i < 3; i++) {
			g_moon_world[i] = -w[i];
			g_direct_loss[i] = g_ambient_loss[i] = 0.0f;
		}
		g_night = 0.0f;
		g_light_is_moon = FALSE;
		if (!sun_moved()) {
			memcpy(out, in, 3 * sizeof *out);
			return;
		}
		memcpy(light, w, sizeof light);
	} else {
		float direct[3], ambient[3];

		flat = (float)sqrt(w[0] * w[0] + w[2] * w[2]);
		south[0] = flat > 1e-4f ? w[0] / flat : 0.0f;
		south[1] = 0.0f;
		south[2] = flat > 1e-4f ? w[2] / flat : 1.0f;
		clock_sun(g_sun_world, south);
		for (i = 0; i < 3; i++)
			g_moon_world[i] = -g_sun_world[i];
		sun_height = (float)asin(min(max(g_sun_world[1], -1.0f), 1.0f)) / RADIANS;
		g_night = 1.0f - smooth_step(-NIGHT_DEPTH, 0.0f, sun_height);
		daylight(sun_height, -sun_height, direct, ambient);
		for (i = 0; i < 3; i++) {
			g_direct_loss[i] = 1.0f - direct[i];
			g_ambient_loss[i] = 1.0f - ambient[i];
		}
		g_light_is_moon = sun_height <= MOON_TAKES_OVER;
		memcpy(light, g_light_is_moon ? g_moon_world : g_sun_world, sizeof light);
		raise_to(light, SUN_MIN_HEIGHT);
	}

	/* Back to view space with the transposed rotation. */
	for (i = 0; i < 3; i++)
		out[i] = (inv_view[i * 4 + 0] * light[0] + inv_view[i * 4 + 1] * light[1] + inv_view[i * 4 + 2] * light[2]) * side * len;
}

/* A world direction in view space. */
static void world_to_view(float *out, const float *w, const float *inv_view)
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = inv_view[i * 4 + 0] * w[0] + inv_view[i * 4 + 1] * w[1] + inv_view[i * 4 + 2] * w[2];
}

/* Orthographic sun view around the area in front of the camera. */
static BOOL build_light_matrix(float *m, const float *inv_view, float range, int size)
{
	static const float world_up[3] = { 0.0f, 1.0f, 0.0f };
	float z[3], x[3], y[3], center[3];
	float texel = 2.0f * range / size;
	float cx, cy, cz;
	int i;

	for (i = 0; i < 3; i++) {
		z[i] = inv_view[0 + i] * g_light_view[0] + inv_view[4 + i] * g_light_view[1] +
		       inv_view[8 + i] * g_light_view[2];
		center[i] = inv_view[12 + i] - inv_view[8 + i] * range * 0.5f;
	}
	if (!v_normalize(z))
		return FALSE;
	v_cross(x, world_up, z);
	if (!v_normalize(x))
		return FALSE;
	v_cross(y, z, x);

	/* Moving the map in whole texels stops shadow edges crawling as the camera moves. */
	cx = (float)floor(v_dot(x, center) / texel) * texel;
	cy = (float)floor(v_dot(y, center) / texel) * texel;
	cz = v_dot(z, center);

	m[0] = x[0] / range; m[4] = x[1] / range; m[8] = x[2] / range; m[12] = -cx / range;
	m[1] = y[0] / range; m[5] = y[1] / range; m[9] = y[2] / range; m[13] = -cy / range;
	m[2] = -z[0] / SHADOW_DEPTH_RANGE; m[6] = -z[1] / SHADOW_DEPTH_RANGE;
	m[10] = -z[2] / SHADOW_DEPTH_RANGE; m[14] = cz / SHADOW_DEPTH_RANGE;
	m[3] = m[7] = m[11] = 0.0f;
	m[15] = 1.0f;
	return TRUE;
}

#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_REPEAT 0x2901
#define GL_TEXTURE_MAX_LEVEL 0x813D

/*
 Tiling noise for the weather, so a cloud, puddle or snow lookup is one texture
 read instead of sixteen hashes in every pixel. Its unit is above the post passes'
 10-16 and the panel's font at 17.
*/
#define WEATHER_NOISE_UNIT 18
#define WEATHER_NOISE_SIZE 256
#define WEATHER_NOISE_CELLS 8	/* lowest octave's cells across, match NOISE_CELLS in common.glsl */
#define WEATHER_NOISE_LEVELS 7	/* down to 4x4, below that rows would need unpack padding */

static GLuint g_weather_noise;

static float noise_corner(int x, int y, int cells, unsigned seed)
{
	unsigned h = (unsigned)(x % cells) * 374761393u + (unsigned)(y % cells) * 668265263u + seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

/* Value noise on a lattice that wraps every cells, so the texture tiles. */
static float noise_value(float x, float y, int cells, unsigned seed)
{
	int x0 = (int)x, y0 = (int)y;
	float fx = x - x0, fy = y - y0, a, b;

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	a = noise_corner(x0, y0, cells, seed) + (noise_corner(x0 + 1, y0, cells, seed) - noise_corner(x0, y0, cells, seed)) * fx;
	b = noise_corner(x0, y0 + 1, cells, seed) + (noise_corner(x0 + 1, y0 + 1, cells, seed) - noise_corner(x0, y0 + 1, cells, seed)) * fx;
	return a + (b - a) * fy;
}

static void weather_noise_create(void)
{
	static unsigned char level[WEATHER_NOISE_SIZE * WEATHER_NOISE_SIZE];
	int x, y, o, size = WEATHER_NOISE_SIZE;

	/* Four octaves, the same sum common.glsl used to make itself. */
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			float sum = 0.0f, amp = 0.5f;

			for (o = 0; o < 4; o++) {
				int cells = WEATHER_NOISE_CELLS << o;
				float s = (float)cells / size;

				sum += noise_value(x * s, y * s, cells, o) * amp;
				amp *= 0.5f;
			}
			level[y * size + x] = (unsigned char)(sum / 0.9375f * 255.0f + 0.5f);
		}
	}

	my_glGenTextures(1, &g_weather_noise);
	real_glActiveTexture(GL_TEXTURE0 + WEATHER_NOISE_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, g_weather_noise);
	for (o = 0; o < WEATHER_NOISE_LEVELS; o++) {
		real_glTexImage2D(GL_TEXTURE_2D, o, GL_R8, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, level);
		/* Each level is the average of the four texels under it, done in place. */
		for (y = 0; y < size / 2; y++)
			for (x = 0; x < size / 2; x++)
				level[y * (size / 2) + x] = (unsigned char)((level[2 * y * size + 2 * x] + level[2 * y * size + 2 * x + 1] +
					level[(2 * y + 1) * size + 2 * x] + level[(2 * y + 1) * size + 2 * x + 1] + 2) / 4);
		size /= 2;
	}
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, WEATHER_NOISE_LEVELS - 1);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	real_glActiveTexture(g_active_unit);
}

static void shadow_create(void)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const unsigned char white_pixel[4] = { 255, 255, 255, 255 };
	GLenum status;
	int i;

	weather_noise_create();
	if (!g_cfg_shadows || !my_glProgramUniformMatrix4fv || !my_glProgramUniform1i)
		return;

	g_shadow_enabled = TRUE;
	my_glGenFramebuffers(SHADOW_CASCADES, g_shadow_fbo);
	my_glGenTextures(SHADOW_CASCADES, g_shadow_tex);
	for (i = 0; i < SHADOW_CASCADES; i++) {
		int size = i ? SHADOW_NEAR_SIZE : g_cfg_shadow_size;

		real_glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT + i);
		real_glBindTexture(GL_TEXTURE_2D, g_shadow_tex[i]);
		real_glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, size, size,
				  0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		real_glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, white);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

		real_glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[i]);
		real_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
					    g_shadow_tex[i], 0);
		my_glDrawBuffer(GL_NONE);
		my_glReadBuffer(GL_NONE);
		status = my_glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			log_msg("shadow framebuffer incomplete (%x), shadows off", status);
			g_shadow_enabled = FALSE;
			break;
		}
		real_glBindTexture(GL_TEXTURE_2D, 0);
	}
	real_glActiveTexture(GL_TEXTURE0);
	my_glGenTextures(1, &g_white_tex);
	real_glBindTexture(GL_TEXTURE_2D, g_white_tex);
	real_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	real_glBindTexture(GL_TEXTURE_2D, g_unit_tex[0]);
	bind_current_fb();
	if (g_shadow_enabled) {
		log_msg("shadow maps %d range %d, %d range %.0f", g_cfg_shadow_size, g_cfg_shadow_range,
			SHADOW_NEAR_SIZE, SHADOW_NEAR_RANGE);
		if (!g_rec) {
			g_rec = malloc(REC_ARENA);
			g_rec_prev = malloc(REC_ARENA);
		}
		if (!g_rec || !g_rec_prev)
			g_shadow_enabled = FALSE;
	}
}

/*
 In a race the scene is linear light and post-processing finishes it. Everything
 else, like the menus, never reaches post, so shaders must finish it themselves.
*/
static void hdr_set(int on)
{
	GLuint prog[3];
	int i;

	if (on == g_hdr_on || !my_glProgramUniform1i)
		return;
	g_hdr_on = on;
	prog[0] = g_terrain_prog;
	prog[1] = g_model_prog;
	prog[2] = g_reflection_prog;
	for (i = 0; i < 3; i++)
		if (prog[i] && g_loc_hdr[prog[i]] >= 0)
			my_glProgramUniform1i(prog[i], g_loc_hdr[prog[i]], on);
	if (g_sprite_prog && g_loc_hdr[g_sprite_prog] >= 0)
		my_glProgramUniform1i(g_sprite_prog, g_loc_hdr[g_sprite_prog], on);
}

/*
 The game also draws terrain tiles into its own framebuffer with the sprite
 shader, so sky and particle effects must only apply on the way to the screen or
 they get baked into the ground textures.
*/
static void sprite_set_mode(int mode)
{
	g_sprite_phase = mode;
	if (g_cur_fb != 0)
		mode = SPRITE_NORMAL;
	if (mode == g_sprite_mode || !g_sprite_prog || g_loc_sprite_mode[g_sprite_prog] < 0 ||
	    !my_glProgramUniform1i)
		return;
	g_sprite_mode = mode;
	my_glProgramUniform1i(g_sprite_prog, g_loc_sprite_mode[g_sprite_prog], mode);
}

/*
 Sent every race frame, the setting changes live and a shader reload forgets it. Menus
 get the game's own light back, or a night race would leave the bike preview dark.
*/
static void time_of_day_apply(BOOL race)
{
	static const float none[3] = { 0.0f, 0.0f, 0.0f };
	static BOOL sent;
	GLuint prog[4];
	int i;

	if (!race && !sent)
		return;
	sent = race;
	prog[0] = g_terrain_prog;
	prog[1] = g_model_prog;
	prog[2] = g_reflection_prog;
	prog[3] = g_sprite_prog;
	for (i = 0; i < 4; i++) {
		if (!prog[i])
			continue;
		if (g_loc_night[prog[i]] >= 0)
			my_glProgramUniform1f(prog[i], g_loc_night[prog[i]], race ? g_night : 0.0f);
		if (g_loc_direct_loss[prog[i]] >= 0)
			my_glProgramUniform3fv(prog[i], g_loc_direct_loss[prog[i]], 1, race ? g_direct_loss : none);
		if (g_loc_ambient_loss[prog[i]] >= 0)
			my_glProgramUniform3fv(prog[i], g_loc_ambient_loss[prog[i]], 1, race ? g_ambient_loss : none);
	}
}

enum { WEATHER_CLEAR, WEATHER_CLOUDY, WEATHER_RAIN, WEATHER_STORM, WEATHER_SNOW };

/* Rain soaks the ground by itself, Wet Ground adds puddles to any weather but snow. */
static float weather_wet(void)
{
	float wet = g_cfg_wet / 100.0f;

	if (g_cfg_weather == WEATHER_SNOW)
		return 0.0f;
	if (g_cfg_weather == WEATHER_RAIN || g_cfg_weather == WEATHER_STORM)
		wet = max(wet, g_cfg_weather_amount / 100.0f);
	return min(wet, 1.0f);
}

/* Sent every frame with the camera, so everything agrees where the clouds and puddles are. */
static void weather_apply(const float *inv_view)
{
	static LARGE_INTEGER start;
	LARGE_INTEGER now, rate;
	GLuint prog[4];
	int i;

	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&rate);
	if (!start.QuadPart)
		start = now;
	g_weather_time = (float)((double)(now.QuadPart - start.QuadPart) / rate.QuadPart);

	real_glActiveTexture(GL_TEXTURE0 + WEATHER_NOISE_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, g_weather_noise);
	real_glActiveTexture(g_active_unit);

	prog[0] = g_terrain_prog;
	prog[1] = g_model_prog;
	prog[2] = g_reflection_prog;
	prog[3] = g_sprite_prog;
	for (i = 0; i < 4; i++) {
		GLuint p = prog[i];

		if (!p)
			continue;
		if (g_loc_world_from_view[p] >= 0)
			my_glProgramUniformMatrix4fv(p, g_loc_world_from_view[p], 1, 0, inv_view);
		if (g_loc_time[p] >= 0)
			my_glProgramUniform1f(p, g_loc_time[p], g_weather_time);
		if (g_loc_weather[p] >= 0)
			my_glProgramUniform1i(p, g_loc_weather[p], g_cfg_weather);
		if (g_loc_weather_amount[p] >= 0)
			my_glProgramUniform1f(p, g_loc_weather_amount[p], g_cfg_weather_amount / 100.0f);
		if (g_loc_cloud_cover[p] >= 0)
			my_glProgramUniform1f(p, g_loc_cloud_cover[p], g_cfg_cloud_cover / 100.0f);
		if (g_loc_wet[p] >= 0)
			my_glProgramUniform1f(p, g_loc_wet[p], weather_wet());
		if (g_loc_weather_noise[p] >= 0)
			my_glProgramUniform1i(p, g_loc_weather_noise[p], WEATHER_NOISE_UNIT);
	}
}

/* The game's shaders can take the map too, for when there is no post pass to shade with. */
static void shadow_set_receiver(GLuint prog, const float *far_matrix, const float *near_matrix)
{
	if (!prog || g_loc_shadow_matrix[prog] < 0)
		return;
	my_glProgramUniformMatrix4fv(prog, g_loc_shadow_matrix[prog], 1, 0, far_matrix);
	if (g_loc_shadow_near_matrix[prog] >= 0)
		my_glProgramUniformMatrix4fv(prog, g_loc_shadow_near_matrix[prog], 1, 0, near_matrix);
	if (g_loc_shadow_on[prog] >= 0)
		my_glProgramUniform1i(prog, g_loc_shadow_on[prog], 1);
}

/* A camera cut has no movement to blur, so it counts as standing still. */
static void frame_step_update(const float *inv_view)
{
	static LARGE_INTEGER last;
	LARGE_INTEGER now, rate;
	float prev_view[16], d[3];
	int i;

	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&rate);
	g_frame_seconds = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / rate.QuadPart : 0.0f;
	last = now;

	for (i = 0; i < 3; i++)
		d[i] = inv_view[12 + i] - g_prev_inv_view[12 + i];
	if (g_have_prev_cam && v_dot(d, d) <= SHADOW_MAX_MOTION * SHADOW_MAX_MOTION) {
		mat_inv_rigid(prev_view, g_prev_inv_view);
		mat_mul(g_view_step, prev_view, inv_view);
	} else {
		memset(g_view_step, 0, sizeof g_view_step);
		g_view_step[0] = g_view_step[5] = g_view_step[10] = g_view_step[15] = 1.0f;
	}
}

/* Called with this frame's view matrix, before the first terrain draw. */
static void shadow_frame_start(const float *view)
{
	float inv_view[16];
	int c;

	/* Shadow frames carry every other effect, so without one the game draws as it would alone. */
	if (!g_shadow_enabled || !g_have_light || !g_cfg_enabled)
		return;
	hdr_set(g_cfg_post && g_fbo && post_ready());
	sprite_set_mode(SPRITE_NORMAL);
	mat_inv_rigid(inv_view, view);
	sun_effective(g_light_view, g_light_game, inv_view);
	time_of_day_apply(TRUE);
	weather_apply(inv_view);

	/* The rider stays near the ground, so slowly following them finds it on any track. */
	memcpy(g_view_up, view + 4, sizeof g_view_up);
	g_camera_y = inv_view[13];
	if (!g_have_prev_cam || fabs(g_camera_y - g_ground_y) > GROUND_MAX_CLIMB)
		g_ground_y = g_camera_y;
	g_ground_y += (g_camera_y - g_ground_y) * GROUND_FOLLOW;
	g_shadow_receiver_valid = FALSE;
	frame_step_update(inv_view);
	if (g_terrain_prog && g_loc_parallax[g_terrain_prog] >= 0)
		my_glProgramUniform1i(g_terrain_prog, g_loc_parallax[g_terrain_prog], g_cfg_parallax);

	if (g_model_prog && g_loc_view_up[g_model_prog] >= 0)
		my_glProgramUniform3fv(g_model_prog, g_loc_view_up[g_model_prog], 1, view + 4);
	if (g_sprite_prog && g_loc_sun_view[g_sprite_prog] >= 0)
		my_glProgramUniform3fv(g_sprite_prog, g_loc_sun_view[g_sprite_prog], 1, g_light_view);
	if (g_sprite_prog && g_loc_screen[g_sprite_prog] >= 0) {
		float screen[3];

		screen[0] = (float)g_vp[2];
		screen[1] = (float)g_vp[3];
		screen[2] = 0.0f;
		my_glProgramUniform3fv(g_sprite_prog, g_loc_screen[g_sprite_prog], 1, screen);
	}

	if (!build_light_matrix(g_light_matrix[0], inv_view, (float)g_cfg_shadow_range, g_cfg_shadow_size) ||
	    !build_light_matrix(g_light_matrix[1], inv_view, SHADOW_NEAR_RANGE, SHADOW_NEAR_SIZE))
		return;

	/*
	 With post there is a pass at the end of the frame that reads a map drawn from this
	 frame's own draw calls (shadow_frame_end), so nothing here has to guess anything.
	 Without one (msaa under 2 draws straight to the window) the map can only be last
	 frame's, and it goes into the game's shaders instead: a frame behind, never sliding.
	*/
	for (c = 0; c < SHADOW_CASCADES; c++)
		mat_mul(g_shadow_from_view[c], g_light_matrix[c], inv_view);
	g_shadow_receiver_valid = FALSE;
	if (!g_cfg_post || !g_fbo || !post_ready())
		shadow_forward(inv_view);

	memcpy(g_prev_inv_view, inv_view, sizeof g_prev_inv_view);
	g_have_prev_cam = TRUE;
	g_shadow_frame = TRUE;
}

/*
 Switching framebuffers around every draw costs far more GPU time than the draws
 themselves, so the frame's state changes and draws are recorded and the shadow
 draws replayed in one batch. Replaying every state change, not just the shadow
 draws, leaves GL in the state the game left it in.
*/
enum {
	REC_USEPROGRAM, REC_ACTIVETEXTURE, REC_BINDTEXTURE, REC_BINDVAO, REC_BINDBUFFER,
	REC_ATTRIBPOINTER, REC_ENABLEATTRIB, REC_DISABLEATTRIB,
	REC_UNIFORM1I, REC_UNIFORM1F, REC_UNIFORM2FV, REC_UNIFORM3FV, REC_UNIFORM4FV,
	REC_UNIFORMMAT3, REC_UNIFORMMAT4, REC_DRAWELEMENTS, REC_DRAWRANGE
};

typedef struct {
	int type, a, b, c, d, e, size;
	size_t ptr;
} rec_cmd;

static void rec(int type, int a, int b, int c, int d, int e, size_t ptr, const void *data, int size)
{
	rec_cmd *cmd;

	if (!g_shadow_enabled || !g_cfg_enabled || g_rec_overflow)
		return;
	if (g_rec_used + sizeof *cmd + size + 8 > REC_ARENA) {
		g_rec_overflow = TRUE;
		log_msg("shadow record buffer full at frame %u", g_frame);
		return;
	}
	cmd = (rec_cmd *)(g_rec + g_rec_used);
	cmd->type = type;
	cmd->a = a;
	cmd->b = b;
	cmd->c = c;
	cmd->d = d;
	cmd->e = e;
	cmd->size = size;
	cmd->ptr = ptr;
	if (size)
		memcpy(cmd + 1, data, size);
	g_rec_used += sizeof *cmd + ((size + 7) & ~7);
}

/* Turns the game's uniform number into the real location and remembers the value. */
static GLint uni_set(GLint number, int kind, int count, int transpose, GLint value, const void *data, int size)
{
	uni_entry *u;

	if (number < 0 || g_cur_prog >= MAX_IDS || number >= g_uni_count[g_cur_prog])
		return -1;
	u = &g_uni[g_cur_prog][number];
	if (size > u->size)
		u->data = realloc(u->data, size);
	if (size)
		memcpy(u->data, data, size);
	u->kind = kind;
	u->count = count;
	u->transpose = transpose;
	u->value = value;
	u->size = max(u->size, size);
	return u->real;
}

/*
 In first person the game swaps the rider for a mesh with the hidden parts cut
 out, which would leave only the arms in the shadow. The game keeps both meshes,
 so the fullest one known for each skinned draw is remembered and used in the
 shadow pass only. It shares the bones of its trimmed version, so it follows the
 same pose.
*/
#define MAX_FULL_MESHES 64

typedef struct {
	GLuint vao;
	GLsizei count;
	GLenum type;
	const void *indices;
} full_mesh;

static full_mesh g_full[MAX_FULL_MESHES];
static size_t g_mesh_bytes[MAX_IDS];	/* index data of each VAO, drawn or not */

static int full_mesh_slot(GLsizei count, GLenum type, const void *indices)
{
	int i = g_skinned_index++;
	full_mesh *f;

	if (i >= MAX_FULL_MESHES)
		return 0;
	f = &g_full[i];
	if (count > f->count) {
		log_msg("full mesh %d: vao %u, %d indices (was vao %u, %d)", i, g_cur_vao, count, f->vao, f->count);
		f->vao = g_cur_vao;
		f->count = count;
		f->type = type;
		f->indices = indices;
	}
	return i + 1;
}

/*
 First person also skips rigid parts of the bike and rider: the frame, the rear
 wheel and the helmet. They aren't skinned, so nothing says where they are in that
 view. In chase view they are the draws right before and after the rider, and each
 is stored relative to every bone. The bone a group moves with is the one that
 relation varies least for, and that bone's live pose then places it in the shadow
 pass. The position is an average, so a suspended part sits at its usual height
 and not wherever the last chase view frame caught it.
*/
#define RIGID_MAX_PARTS 64
#define RIGID_MAX_GROUPS 8
#define POSE_MAX_BONES 32
#define RIGID_RADIUS 6.0f	/* game units from the nearest bone, further is unrelated scenery */
#define RIGID_BEFORE 48		/* draws kept from before the rider */
#define RIGID_AFTER 16
#define RIGID_VAO_GAP 6		/* a bike's parts are close together in the upload order */
#define RIGID_AVERAGE_FRAMES 3000	/* how far back a part's average position reaches */

typedef struct {
	GLuint vao;
	GLsizei count;
	GLenum type;
	const void *indices;
	float ambient[4];
	int group;
	float relative[POSE_MAX_BONES][16];
} rigid_part;

typedef struct {
	GLuint vao;
	GLsizei count;
	GLenum type;
	const void *indices;
	float ambient[4], mv[16];
} rigid_draw;

static rigid_part g_rigid[RIGID_MAX_PARTS];
static int g_rigid_parts, g_rigid_frames;	/* frames of the relation being watched */
static float g_rigid_score[RIGID_MAX_GROUPS][POSE_MAX_BONES];
static int g_rigid_bone[RIGID_MAX_GROUPS];
static rigid_draw g_rigid_before[RIGID_BEFORE], g_rigid_after[RIGID_AFTER];
static int g_rigid_before_n, g_rigid_after_n;
static BOOL g_rider_seen, g_frame_trimmed, g_rigid_needed, g_view_settled;
static unsigned g_vao_frame[MAX_IDS];	/* when each VAO was last drawn as a rigid model */

static float g_pose_mv[16], g_pose_r[POSE_MAX_BONES][9], g_pose_t[POSE_MAX_BONES][3], g_pose_c[POSE_MAX_BONES][3];
static int g_pose_bones;
static float g_replay_pose[POSE_MAX_BONES][16];
static int g_replay_bones;
static uni_entry *prog_uniform(GLuint prog, const char *name)
{
	size_t n = strlen(name);
	int i;

	for (i = 0; prog && i < g_uni_count[prog]; i++) {
		uni_entry *u = &g_uni[prog][i];

		if (!strncmp(u->name, name, n) && (!u->name[n] || u->name[n] == '['))
			return u;
	}
	return NULL;
}

static uni_entry *model_uniform(const char *name)
{
	return prog_uniform(g_model_prog, name);
}

/* Bones turn around a center: p' = r * (p - c) + t. */
static void bone_matrix(float *m, int b)
{
	const float *r = g_pose_r[b], *c = g_pose_c[b], *t = g_pose_t[b];
	int i;

	for (i = 0; i < 3; i++) {
		m[i] = r[i];
		m[4 + i] = r[3 + i];
		m[8 + i] = r[6 + i];
		m[12 + i] = t[i] - (r[i] * c[0] + r[3 + i] * c[1] + r[6 + i] * c[2]);
	}
	m[3] = m[7] = m[11] = 0.0f;
	m[15] = 1.0f;
}

static void pose_capture(void)
{
	uni_entry *mv = model_uniform("u_modelview"), *r = model_uniform("u_bone_r");
	uni_entry *t = model_uniform("u_bone_t"), *c = model_uniform("u_bone_c");

	g_pose_bones = 0;
	if (!mv || !r || !t || !c || !mv->data || !r->data || !t->data || !c->data || g_cur_nbones <= 0)
		return;
	g_pose_bones = min(g_cur_nbones, min(POSE_MAX_BONES, min(r->count, min(t->count, c->count))));
	memcpy(g_pose_mv, mv->data, sizeof g_pose_mv);
	memcpy(g_pose_r, r->data, g_pose_bones * sizeof g_pose_r[0]);
	memcpy(g_pose_t, t->data, g_pose_bones * sizeof g_pose_t[0]);
	memcpy(g_pose_c, c->data, g_pose_bones * sizeof g_pose_c[0]);
}

static void rigid_draw_seen(GLsizei count, GLenum type, const void *indices)
{
	static GLuint prog;
	static uni_entry *mv, *ambient;
	rigid_draw *d;

	if (prog != g_model_prog) {
		prog = g_model_prog;
		mv = model_uniform("u_modelview");
		ambient = model_uniform("u_ambient");
	}
	if (g_cur_vao < MAX_IDS)
		g_vao_frame[g_cur_vao] = g_frame;
	if (!mv || !mv->data)
		return;
	if (!g_rider_seen)
		d = &g_rigid_before[g_rigid_before_n++ % RIGID_BEFORE];
	else if (g_rigid_after_n < RIGID_AFTER)
		d = &g_rigid_after[g_rigid_after_n++];
	else
		return;
	d->vao = g_cur_vao;
	d->count = count;
	d->type = type;
	d->indices = indices;
	memcpy(d->mv, mv->data, sizeof d->mv);
	if (ambient && ambient->data)
		memcpy(d->ambient, ambient->data, sizeof d->ambient);
}

/* The bones hold the rider's place, so the modelview alone says nothing about it. */
static BOOL rigid_is_near(const float *mv)
{
	float nearest = 1e30f, d[3];
	int b, i;

	for (b = 0; b < g_pose_bones; b++) {
		const float *t = g_pose_t[b];

		for (i = 0; i < 3; i++)
			d[i] = mv[12 + i] - (g_pose_mv[i] * t[0] + g_pose_mv[4 + i] * t[1] + g_pose_mv[8 + i] * t[2] + g_pose_mv[12 + i]);
		nearest = min(nearest, v_dot(d, d));
	}
	return nearest <= RIGID_RADIUS * RIGID_RADIUS;
}

/* Called at the end of a chase view frame with everything drawn around the rider. */
static void rigid_update(void)
{
	static const rigid_draw *list[RIGID_MAX_PARTS];
	float to_bone[POSE_MAX_BONES][16];
	int have = min(g_rigid_before_n, RIGID_BEFORE);
	int n = 0, group = -1, was = g_rigid_parts, i, b;

	/* Back from the rider through the bike, stopping at the first thing that isn't part of it. */
	for (i = 0; i < have && n < RIGID_MAX_PARTS - RIGID_AFTER; i++) {
		const rigid_draw *d = &g_rigid_before[(g_rigid_before_n - 1 - i) % RIGID_BEFORE];

		if (!rigid_is_near(d->mv))
			break;
		if (n && (d->vao >= list[n - 1]->vao || list[n - 1]->vao - d->vao > RIGID_VAO_GAP))
			break;
		list[n++] = d;
	}
	for (i = 0; i < n / 2; i++) {
		const rigid_draw *t = list[i];

		list[i] = list[n - 1 - i];
		list[n - 1 - i] = t;
	}
	for (i = 0; i < g_rigid_after_n && rigid_is_near(g_rigid_after[i].mv); i++)
		list[n++] = &g_rigid_after[i];

	/* A stray draw near the rider must not replace what a full chase view taught. */
	if (n < g_rigid_parts)
		return;

	for (b = 0; b < g_pose_bones; b++) {
		float bone[16], world[16];

		bone_matrix(bone, b);
		mat_mul(world, g_pose_mv, bone);
		mat_inv_rigid(to_bone[b], world);
	}

	g_rigid_parts = 0;
	for (i = 0; i < n; i++) {
		const rigid_draw *d = list[i];
		rigid_part *p = &g_rigid[i];
		BOOL seen_before = p->vao == d->vao && p->count == d->count;
		BOOL first = !i || d->mv[12] != list[i - 1]->mv[12] || d->mv[13] != list[i - 1]->mv[13] ||
			     d->mv[14] != list[i - 1]->mv[14];

		if (first && group + 1 == RIGID_MAX_GROUPS)
			break;
		if (first && !seen_before)
			memset(g_rigid_score[group + 1], 0, sizeof g_rigid_score[0]);
		group += first;

		for (b = 0; b < g_pose_bones; b++) {
			float rel[16];

			mat_mul(rel, to_bone[b], d->mv);

			/* A group's bone stays close to the average relation, others wander. A saved part has no average yet. */
			if (first && seen_before && g_rigid_frames) {
				float drift = 0.0f;
				int c;

				for (c = 12; c < 15; c++)
					drift += (float)fabs(rel[c] - p->relative[b][c]);
				g_rigid_score[group][b] += drift + (float)sqrt(rel[12] * rel[12] + rel[13] * rel[13] + rel[14] * rel[14]) * 0.0005f;
			}
			if (seen_before) {
				float weight = 1.0f / min(g_rigid_frames + 1, RIGID_AVERAGE_FRAMES);
				int c;

				for (c = 12; c < 15; c++)
					rel[c] = p->relative[b][c] + (rel[c] - p->relative[b][c]) * weight;
			}
			memcpy(p->relative[b], rel, sizeof rel);
		}
		p->vao = d->vao;
		p->count = d->count;
		p->type = d->type;
		p->indices = d->indices;
		p->group = group;
		memcpy(p->ambient, d->ambient, sizeof p->ambient);
		g_rigid_parts++;
	}
	g_rigid_frames++;
	if (g_rigid_parts != was)
		log_msg("rigid parts: %d in %d groups", g_rigid_parts, group + 1);
}

/*
 Chase view draws the helmet right after the rider and first person doesn't. Trimmed
 meshes alone can't say which view this is, not all gear has them.
*/
static BOOL rigid_chase_view(void)
{
	return g_rider_seen && !g_frame_trimmed && g_pose_bones > 0 && g_rigid_after_n > 0 &&
	       rigid_is_near(g_rigid_after[0].mv);
}

static void rigid_pick_bones(void)
{
	int g, b;

	for (g = 0; g < RIGID_MAX_GROUPS; g++) {
		g_rigid_bone[g] = 0;
		for (b = 1; b < g_pose_bones; b++)
			if (g_rigid_score[g][b] < g_rigid_score[g][g_rigid_bone[g]])
				g_rigid_bone[g] = b;
	}
}
static unsigned long long g_view_sig, g_view_last_sig;

/* Out of the race the next bike may differ, so nothing carries over. */
static void full_meshes_forget(void)
{
	memset(g_full, 0, sizeof g_full);
	memset(g_rigid_score, 0, sizeof g_rigid_score);
	memset(g_rigid, 0, sizeof g_rigid);
	g_rigid_parts = g_rigid_frames = 0;
	g_view_last_sig = 0;
}

/* This frame's skinned draws. Their signature changes with the view and the rider. */
static GLsizei g_view_count[MAX_FULL_MESHES];
static GLuint g_view_vao[MAX_FULL_MESHES];
static GLenum g_view_type[MAX_FULL_MESHES];
static const void *g_view_indices[MAX_FULL_MESHES];
static BOOL g_rigid_needed_was;

/*
 The game uploads a rider's meshes in fixed groups, so a trimmed group has its
 full version a little earlier in the VAO order: the same parts, some longer.
 That finds the full rider without waiting for chase view to show it.
*/
#define FULL_SEARCH 64

static void full_group_search(int first, int n)
{
	GLuint vao = g_view_vao[first];
	int d, i;

	for (d = n; d <= FULL_SEARCH && (GLuint)d < vao; d++) {
		int longer = 0, same = 0;

		for (i = 0; i < n; i++) {
			size_t size = g_view_type[first + i] == GL_UNSIGNED_INT ? 4 : 2;
			size_t need = g_view_count[first + i] * size, have = g_mesh_bytes[vao + i - d];

			if (have < need)
				break;
			if (have > need)
				longer++;
			else
				same++;
		}
		if (i < n || !longer || !same)
			continue;

		log_msg("full rider %u..%u found %d VAOs before the trimmed one", vao, vao + n - 1, d);
		for (i = 0; i < n; i++) {
			size_t size = g_view_type[first + i] == GL_UNSIGNED_INT ? 4 : 2;
			GLsizei count = (GLsizei)(g_mesh_bytes[vao + i - d] / size);
			full_mesh *f = &g_full[first + i];

			if (count > f->count) {
				f->vao = vao + i - d;
				f->count = count;
				f->type = g_view_type[first + i];
				f->indices = g_view_indices[first + i];
			}
		}
		return;
	}
}

static void full_groups_search(void)
{
	int n = min(g_skinned_index, MAX_FULL_MESHES), first = 0, i;

	for (i = 1; i <= n; i++) {
		if (i < n && g_view_vao[i] == g_view_vao[i - 1] + 1 && g_view_vao[i] < MAX_IDS)
			continue;
		full_group_search(first, i - first);
		first = i;
	}
}

/*
 Joining in first person has nothing to place these parts with, so what chase view
 taught is kept per rider, for the next time. Parts are stored as offsets, the VAO
 numbers change with the track.
*/
#define RIGID_MIN_FRAMES 60
#define RIGID_FILE_VERSION 3

typedef struct {
	int offset, group, bone;
	GLsizei count;
	GLenum type;
	size_t indices;
	float ambient[4], relative[16];
} rigid_saved;

static void rigid_file_name(wchar_t *path, unsigned long long sig)
{
	wchar_t name[64];

	swprintf_s(name, 64, L"riders\\rigid_%016llx.bin", sig);
	log_path(path, name);
}

static void rigid_save(void)
{
	int header[2] = { RIGID_FILE_VERSION, g_rigid_parts }, i;
	wchar_t path[MAX_PATH];
	FILE *f;

	if (g_rigid_frames < RIGID_MIN_FRAMES)
		return;
	rigid_file_name(path, g_view_last_sig);
	if (_wfopen_s(&f, path, L"wb"))
		return;
	fwrite(header, sizeof header, 1, f);
	fwrite(g_rigid_score, sizeof g_rigid_score, 1, f);
	for (i = 0; i < g_rigid_parts; i++) {
		const rigid_part *p = &g_rigid[i];
		rigid_saved s = { 0 };

		s.offset = (int)p->vao - (int)g_view_vao[0];
		s.group = p->group;
		s.bone = g_rigid_bone[p->group];
		s.count = p->count;
		s.type = p->type;
		s.indices = (size_t)p->indices;
		memcpy(s.ambient, p->ambient, sizeof s.ambient);
		memcpy(s.relative, p->relative[s.bone], sizeof s.relative);
		fwrite(&s, sizeof s, 1, f);
	}
	fclose(f);
	log_msg("rigid parts: saved %d", g_rigid_parts);
}

static void rigid_load(void)
{
	static rigid_part parts[RIGID_MAX_PARTS];
	static float score[RIGID_MAX_GROUPS][POSE_MAX_BONES];
	int header[2], i;
	wchar_t path[MAX_PATH];
	FILE *f;

	rigid_file_name(path, g_view_sig);
	if (_wfopen_s(&f, path, L"rb"))
		return;
	memset(parts, 0, sizeof parts);
	if (fread(header, sizeof header, 1, f) != 1 || header[0] != RIGID_FILE_VERSION ||
	    header[1] <= 0 || header[1] > RIGID_MAX_PARTS || fread(score, sizeof score, 1, f) != 1) {
		fclose(f);
		return;
	}
	for (i = 0; i < header[1]; i++) {
		rigid_saved s;
		int vao;

		if (fread(&s, sizeof s, 1, f) != 1 || s.group < 0 || s.group >= RIGID_MAX_GROUPS ||
		    s.bone < 0 || s.bone >= POSE_MAX_BONES)
			break;
		vao = (int)g_view_vao[0] + s.offset;
		/* A rider with the same trimmed meshes can wear another helmet. */
		if (vao <= 0 || vao >= MAX_IDS || (size_t)s.count * (s.type == GL_UNSIGNED_INT ? 4 : 2) != g_mesh_bytes[vao])
			break;
		parts[i].vao = vao;
		parts[i].count = s.count;
		parts[i].type = s.type;
		parts[i].indices = (const void *)s.indices;
		parts[i].group = s.group;
		memcpy(parts[i].ambient, s.ambient, sizeof s.ambient);
		memcpy(parts[i].relative[s.bone], s.relative, sizeof s.relative);
	}
	fclose(f);
	if (i < header[1]) {
		log_msg("rigid parts: saved file doesn't match this rider");
		return;
	}
	memcpy(g_rigid, parts, sizeof parts);
	memcpy(g_rigid_score, score, sizeof score);
	g_rigid_parts = header[1];
	log_msg("rigid parts: loaded %d", g_rigid_parts);
}
static void view_update(void)
{
	g_view_settled = g_view_sig && g_view_sig == g_view_last_sig;
	if (g_view_sig && g_view_sig != g_view_last_sig) {
		full_groups_search();
		if (!g_rigid_parts)
			rigid_load();
		g_view_last_sig = g_view_sig;
	}
	g_view_sig = 0;
}

/* The pose for next frame's shadows, from this frame's rider. */
enum { RIDER_NOT_STARTED, RIDER_DRAWING, RIDER_DONE };

static int g_rider_run;

/*
 What the draw-order checks decided this frame, logged once it changes and holds for 30 frames.
 Hide HUD and Hide Cursor need a race frame whose 2D overlay has fewer than HUD_MENU_BOXES
 1x1 boxes, and goggles need the rider trimmed for first person.
*/
static void view_state_log(void)
{
	static int current = -1, logged = -1, flips;
	static unsigned since;
	int state = (g_shadow_frame ? 1 : 0) | (g_hud_phase ? 2 : 0) | (g_hud_boxes >= HUD_MENU_BOXES ? 4 : 0) |
		    (g_frame_trimmed ? 8 : 0);

	if (state != current) {
		current = state;
		since = g_frame;
		flips++;
		return;
	}
	if (current == logged || g_frame - since < 30)
		return;
	if (g_hud_phase)
		log_msg("view at frame %u: race frame %s, 2D overlay with %d boxes (%s), first person %s, %d changes since the last view line",
			g_frame, g_shadow_frame ? "yes" : "no", g_hud_boxes,
			g_hud_boxes >= HUD_MENU_BOXES ? "a menu" : "the race HUD", g_frame_trimmed ? "yes" : "no", flips - 1);
	else
		log_msg("view at frame %u: race frame %s, 2D overlay not reached, first person %s, %d changes since the last view line",
			g_frame, g_shadow_frame ? "yes" : "no", g_frame_trimmed ? "yes" : "no", flips - 1);
	logged = current;
	flips = 1;
}

static void rigid_pose_commit(void)
{
	BOOL chase = rigid_chase_view();

	view_update();
	if (chase && g_view_settled)
		rigid_update();
	g_rigid_needed = g_rider_seen && !chase && g_rigid_parts > 0 && g_pose_bones > 0;
	g_frame_trimmed = g_rider_seen = FALSE;
	g_rider_run = RIDER_NOT_STARTED;
	g_rigid_before_n = g_rigid_after_n = 0;
	if (g_rigid_needed) {
		int b;

		g_replay_bones = g_pose_bones;
		for (b = 0; b < g_pose_bones; b++) {
			float bone[16];

			bone_matrix(bone, b);
			mat_mul(g_replay_pose[b], g_pose_mv, bone);
		}
		rigid_pick_bones();
	}
	if (g_rigid_needed && !g_rigid_needed_was)
		rigid_save();
	g_rigid_needed_was = g_rigid_needed;
}

/*
 Tracks can have animated objects, which are skinned too and come and go with the
 camera. The rider is the first unbroken run of skinned draws with one bone count.
*/
static GLint g_rider_bones;


static BOOL rider_draw(void)
{
	if (g_rider_run == RIDER_NOT_STARTED) {
		g_rider_run = RIDER_DRAWING;
		g_rider_bones = g_cur_nbones;
	}
	if (g_cur_nbones != g_rider_bones)
		g_rider_run = RIDER_DONE;
	return g_rider_run == RIDER_DRAWING;
}

static void skinned_draw_seen(int slot, GLsizei count, GLenum type, const void *indices)
{
	if (slot == 1)
		pose_capture();
	if (slot && slot <= MAX_FULL_MESHES) {
		g_view_count[slot - 1] = count;
		g_view_vao[slot - 1] = g_cur_vao;
		g_view_type[slot - 1] = type;
		g_view_indices[slot - 1] = indices;
		g_view_sig = g_view_sig * 1000003 + count;
	}
	if (slot && slot <= MAX_FULL_MESHES && count < g_full[slot - 1].count)
		g_frame_trimmed = TRUE;
	g_rider_seen = TRUE;
}
enum { DRAW_NO_SHADOW, DRAW_STATIC, DRAW_DYNAMIC };

static int shadow_draw_kind(void)
{
	if (!g_shadow_frame || g_cur_fb != 0 || g_cur_prog >= MAX_IDS || g_prog_role[g_cur_prog] == ROLE_NONE)
		return DRAW_NO_SHADOW;
	if (g_cur_prog == g_model_prog && g_cur_nbones != 0) {
		g_dynamic_draws++;
		return DRAW_DYNAMIC;
	}
	return DRAW_STATIC;
}

/*
 The game skips terrain away from the view, but a hill out of view still shades
 what's on screen, and its shadow would go with it one block at a time. All terrain
 comes from one height texture, so the blocks the game skipped are added to the
 shadow maps here, using its widest mesh and its grid of blocks.
*/
#define TERRAIN_CELLS 256
#define MAX_TERRAIN_FILL 4096
#define TERRAIN_HEIGHT_MARGIN 200.0f	/* a block's height isn't known here */

typedef struct {
	float x, z, scale;
} terrain_block;

static GLuint g_terrain_vao, g_terrain_heights;
static GLsizei g_terrain_count;
static GLenum g_terrain_type;
static const void *g_terrain_indices;
static float g_terrain_spacing, g_terrain_cell, g_terrain_min_scale, g_terrain_max_scale;
static int g_terrain_rows;
static unsigned char g_terrain_seen[TERRAIN_CELLS][TERRAIN_CELLS];
static terrain_block g_terrain_fill[MAX_TERRAIN_FILL];
static int g_terrain_fill_count;

static void terrain_draw_seen(GLsizei count, GLenum type, const void *indices)
{
	uni_entry *offset = prog_uniform(g_terrain_prog, "u_meshoffset"), *scale_u = prog_uniform(g_terrain_prog, "u_meshscale");
	uni_entry *spacing_u = prog_uniform(g_terrain_prog, "u_spacing"), *heights = prog_uniform(g_terrain_prog, "u_heights");
	const float *o;
	float scale, spacing;
	GLuint tex;
	int x, z, x0, z0, n;

	if (!offset || !offset->data || !scale_u || !scale_u->data || !spacing_u || !spacing_u->data ||
	    !heights || heights->value < 0 || heights->value >= 32)
		return;
	tex = g_unit_tex[heights->value];
	if (!tex || tex >= MAX_IDS || g_tex[tex].w <= 0)
		return;
	o = (const float *)offset->data;
	scale = *(const float *)scale_u->data;
	spacing = *(const float *)spacing_u->data;

	/* A new track. */
	if (tex != g_terrain_heights || spacing != g_terrain_spacing) {
		g_terrain_heights = tex;
		g_terrain_spacing = spacing;
		g_terrain_cell = g_tex[tex].w * spacing / TERRAIN_CELLS;
		g_terrain_rows = min(TERRAIN_CELLS, (int)(g_tex[tex].h * spacing / g_terrain_cell + 0.5f));
		g_terrain_min_scale = g_terrain_max_scale = scale;
		g_terrain_count = 0;
		memset(g_terrain_seen, 0, sizeof g_terrain_seen);
	}
	g_terrain_min_scale = min(g_terrain_min_scale, scale);
	g_terrain_max_scale = max(g_terrain_max_scale, scale);

	/* Blocks next to a coarser one have their edge cut, the full mesh has the most indices. */
	if (count > g_terrain_count) {
		g_terrain_vao = g_cur_vao;
		g_terrain_count = count;
		g_terrain_type = type;
		g_terrain_indices = indices;
	}

	x0 = (int)floor(o[0] / g_terrain_cell + 0.5f);
	z0 = (int)floor(o[1] / g_terrain_cell + 0.5f);
	n = (int)(scale / g_terrain_cell + 0.5f);
	for (z = max(z0, 0); z < z0 + n && z < TERRAIN_CELLS; z++)
		for (x = max(x0, 0); x < x0 + n && x < TERRAIN_CELLS; x++)
			g_terrain_seen[z][x] = 1;
}

static void terrain_fill_visit(int x, int z, int n)
{
	int cx, cz, cells = 0, seen = 0;

	for (cz = z; cz < z + n && cz < g_terrain_rows; cz++)
		for (cx = x; cx < x + n && cx < TERRAIN_CELLS; cx++) {
			cells++;
			seen += g_terrain_seen[cz][cx];
		}
	if (seen == cells)
		return;
	if (!seen) {
		if (g_terrain_fill_count < MAX_TERRAIN_FILL) {
			terrain_block *b = &g_terrain_fill[g_terrain_fill_count++];

			b->x = x * g_terrain_cell;
			b->z = z * g_terrain_cell;
			b->scale = n * g_terrain_cell;
		}
		return;
	}
	if (n > 1 && n / 2 * g_terrain_cell > g_terrain_min_scale * 0.75f) {
		n /= 2;
		terrain_fill_visit(x, z, n);
		terrain_fill_visit(x + n, z, n);
		terrain_fill_visit(x, z + n, n);
		terrain_fill_visit(x + n, z + n, n);
	}
}

/* Goes with the recording: what this frame skipped is drawn with this frame's shadows. */
static void terrain_frame_end(void)
{
	int x, z, n;

	g_terrain_fill_count = 0;
	if (!g_terrain_count)
		return;
	n = (int)(g_terrain_max_scale / g_terrain_cell + 0.5f);
	for (z = 0; z < g_terrain_rows; z += n)
		for (x = 0; x < TERRAIN_CELLS; x += n)
			terrain_fill_visit(x, z, n);
	memset(g_terrain_seen, 0, sizeof g_terrain_seen);
}

/*
 Each model is a VAO that u_modelview places, often hundreds of times a frame, so
 a sphere around its vertices says where a draw lands on a shadow map. They're read
 back once per mesh. A mesh that can't be measured has no sphere and is always drawn.
*/
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_BUFFER_SIZE 0x8764
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F

typedef struct {
	float x, y, z, radius;
} mesh_sphere;

enum { SPHERE_UNKNOWN, SPHERE_KNOWN, SPHERE_NONE };

typedef GLint (WINAPI *glGetAttribLocation_t)(GLuint, const char *);
typedef void (WINAPI *glGetVertexAttribiv_t)(GLuint, GLenum, GLint *);
typedef void (WINAPI *glGetVertexAttribPointerv_t)(GLuint, GLenum, void **);
typedef void (WINAPI *glGetBufferParameteriv_t)(GLenum, GLenum, GLint *);
typedef void (WINAPI *glGetBufferSubData_t)(GLenum, ptrdiff_t, ptrdiff_t, void *);
typedef void (WINAPI *glBindBuffer_t)(GLenum, GLuint);

static glGetAttribLocation_t my_glGetAttribLocation;
static glGetVertexAttribiv_t my_glGetVertexAttribiv;
static glGetVertexAttribPointerv_t my_glGetVertexAttribPointerv;
static glGetBufferParameteriv_t my_glGetBufferParameteriv;
static glGetBufferSubData_t my_glGetBufferSubData;
static glBindBuffer_t my_glBindBuffer;

static mesh_sphere g_mesh_sphere[MAX_IDS];
static unsigned char g_mesh_sphere_state[MAX_IDS];

static BOOL mesh_sphere_measure(mesh_sphere *s)
{
	GLint attr, size = 0, type = 0, stride = 0, buffer = 0, bytes = 0;
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	void *pointer = NULL;
	unsigned char *data;
	size_t at, end;
	int i;

	if (!my_glGetAttribLocation || !my_glGetVertexAttribiv || !my_glGetVertexAttribPointerv ||
	    !my_glGetBufferParameteriv || !my_glGetBufferSubData || !my_glBindBuffer)
		return FALSE;
	attr = my_glGetAttribLocation(g_model_prog, "vattr_position");
	if (attr < 0)
		return FALSE;
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
	my_glGetVertexAttribPointerv(attr, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
	if (size < 3 || type != GL_FLOAT || !buffer)
		return FALSE;
	if (!stride)
		stride = size * (int)sizeof(float);

	my_glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	my_glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &bytes);
	data = bytes > 0 ? malloc(bytes) : NULL;
	if (data)
		my_glGetBufferSubData(GL_COPY_READ_BUFFER, 0, bytes, data);
	my_glBindBuffer(GL_COPY_READ_BUFFER, 0);
	if (!data)
		return FALSE;

	end = bytes;
	for (at = (size_t)pointer; at + 3 * sizeof(float) <= end; at += stride) {
		float p[3];

		memcpy(p, data + at, sizeof p);
		for (i = 0; i < 3; i++) {
			lo[i] = min(lo[i], p[i]);
			hi[i] = max(hi[i], p[i]);
		}
	}
	free(data);
	if (lo[0] > hi[0])
		return FALSE;

	s->x = (lo[0] + hi[0]) * 0.5f;
	s->y = (lo[1] + hi[1]) * 0.5f;
	s->z = (lo[2] + hi[2]) * 0.5f;
	s->radius = 0.5f * (float)sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
				       (hi[2] - lo[2]) * (hi[2] - lo[2]));
	return TRUE;
}

/* For the model about to be drawn with the current VAO. */
static const mesh_sphere *mesh_sphere_current(void)
{
	GLuint vao = g_cur_vao;

	if (vao >= MAX_IDS)
		return NULL;
	if (g_mesh_sphere_state[vao] == SPHERE_UNKNOWN)
		g_mesh_sphere_state[vao] = mesh_sphere_measure(&g_mesh_sphere[vao]) ? SPHERE_KNOWN : SPHERE_NONE;
	return g_mesh_sphere_state[vao] == SPHERE_KNOWN ? &g_mesh_sphere[vao] : NULL;
}

/*
 --- the sky ---

 The game's sky is a cube of RGB textures drawn first in a frame, sides then top, and
 some tracks paint mountains or trees into the lower part of the sides. Both end up at
 depth 1, so post can't tell them apart on its own. Each side face is read back once,
 and every column is searched from the top for where the painting turns from sky to
 ground. The result is the skyline's height in each direction around the horizon.

 sprite.fsh then draws the atmosphere above the skyline in the game's own sky draws,
 so every multisample behind a leaf already holds it. Replacing it in post left the
 painted sky in pixels shared with foliage. Each face's corners give the direction
 of every texel, in step with the painting and without waiting for the frame's camera.
*/
#define SKY_MASK_UNIT 19	/* above the weather noise at 18 */
#define SKY_LUT_UNIT 20		/* post.c's clear sky, kept from the last frame */
#define SKY_MASK_BINS 2048	/* directions around the horizon, finer than a face's columns */
#define SKY_MAX_DRAWS 6	/* a cube's faces, the game draws five */
#define SKY_GROUND_RUN 6	/* rows of ground in a row, so one dark texel isn't the skyline */
#define SKY_UNSEEN -2.0f	/* below any skyline, for a direction no side face covered */
#define SKY_TWO_PI 6.28318531f
#define GL_R32F 0x822E
#define GL_CLAMP_TO_EDGE 0x812F

typedef struct {
	GLuint tex;
	unsigned uploads;
	BOOL side, placed;
	/* Columns are the corners at the texture's low and high u and v: (lo, lo), (hi, lo), (lo, hi), (hi, hi). Their w holds lo u, lo v, hi u, hi v. */
	float place[16];
} sky_face;

static sky_face g_sky_face[SKY_MAX_DRAWS];
static float g_sky_line[SKY_MASK_BINS];	/* sine of the skyline's elevation */
static GLuint g_sky_mask;
static BOOL g_sky_mask_valid;
static int g_cfg_sky_mask_view;
static int g_cfg_dynamic_sky;	/* 0 keeps the game's painted skybox */
static GLint g_loc_sky_face_on = -1, g_loc_sky_face = -1, g_loc_sky_sun = -1, g_loc_cloud_view = -1, g_loc_cloud_scale = -1;
#define CLOUDS_UNIT 21	/* post.c's latest clouds */

/* Reads one float attribute of the sprite program's vertices start to start + 3. */
static BOOL sky_attr_read(const char *name, GLuint start, float out[4][4])
{
	GLint attr, size = 0, type = 0, stride = 0, buffer = 0;
	void *pointer = NULL;
	int i;

	attr = my_glGetAttribLocation(g_sprite_prog, name);
	if (attr < 0)
		return FALSE;
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
	my_glGetVertexAttribiv(attr, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
	my_glGetVertexAttribPointerv(attr, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
	if (size < 2 || size > 4 || type != GL_FLOAT || !buffer)
		return FALSE;
	if (!stride)
		stride = size * (int)sizeof(float);
	memset(out, 0, 4 * sizeof out[0]);
	my_glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	for (i = 0; i < 4; i++)
		my_glGetBufferSubData(GL_COPY_READ_BUFFER, (ptrdiff_t)pointer + (ptrdiff_t)(start + i) * stride,
				      size * sizeof(float), out[i]);
	my_glBindBuffer(GL_COPY_READ_BUFFER, 0);
	return TRUE;
}

/* Blue or neutral reads as sky. Green or brown, darkness that isn't deep blue, or fine detail reads as ground. */
static BOOL sky_texel_ground(const unsigned char *p, const unsigned char *below)
{
	float b = p[0], g = p[1], r = p[2];
	float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	float score = 0.0f;

	if (max(g, r) - b > 12.0f)
		score += 1.0f;
	if (lum < 110.0f && b - max(r, g) < 25.0f)
		score += 1.0f;
	if (below && abs(below[1] - p[1]) > 40 && lum < 170.0f)
		score += 0.5f;
	return score >= 1.0f;
}

static void sky_corner(const float c[2][2][3], float s, float t, float out[3])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = (c[0][0][i] * (1 - s) + c[1][0][i] * s) * (1 - t) + (c[0][1][i] * (1 - s) + c[1][1][i] * s) * t;
}

/* The face's corners by their place in the texture, whatever order the quad lists them in. */
static BOOL sky_face_place(sky_face *f, GLuint start)
{
	float pos[4][4], uv[4][4], lo[2], hi[2];
	int have = 0, i, k;

	if (!sky_attr_read("vattr_position", start, pos) || !sky_attr_read("vattr_texcoord", start, uv))
		return FALSE;
	lo[0] = lo[1] = 1e30f;
	hi[0] = hi[1] = -1e30f;
	for (i = 0; i < 4; i++) {
		lo[0] = min(lo[0], uv[i][0]);
		hi[0] = max(hi[0], uv[i][0]);
		lo[1] = min(lo[1], uv[i][1]);
		hi[1] = max(hi[1], uv[i][1]);
	}
	if (hi[0] - lo[0] < 0.5f || hi[1] - lo[1] < 0.5f)
		return FALSE;
	for (i = 0; i < 4; i++) {
		int a = uv[i][0] > (lo[0] + hi[0]) * 0.5f, b = uv[i][1] > (lo[1] + hi[1]) * 0.5f;

		for (k = 0; k < 3; k++)
			f->place[(b * 2 + a) * 4 + k] = pos[i][k];
		have |= 1 << (b * 2 + a);
	}
	f->place[3] = lo[0];
	f->place[7] = lo[1];
	f->place[11] = hi[0];
	f->place[15] = hi[1];
	return have == 15;
}

/* Adds one side face's skyline to the bins. FALSE if it can't be read. */
static BOOL sky_face_read(const sky_face *f, GLuint tex)
{
	float corner[2][2][3], lo[2], hi[2], top[3], bottom[3];
	int w = g_tex[tex].w, h = g_tex[tex].h, i, a, b, x, row0, row1, step;
	unsigned char *pixels;

	for (a = 0; a < 2; a++)
		for (b = 0; b < 2; b++)
			memcpy(corner[a][b], &f->place[(b * 2 + a) * 4], sizeof corner[a][b]);
	lo[0] = f->place[3];
	lo[1] = f->place[7];
	hi[0] = f->place[11];
	hi[1] = f->place[15];

	/* Texture rows run down the face from whichever end is higher. */
	sky_corner(corner, 0.5f, 0.0f, top);
	sky_corner(corner, 0.5f, 1.0f, bottom);
	row0 = (int)ceilf(lo[1] * h - 0.5f);
	row1 = (int)floorf(hi[1] * h - 0.5f);
	if (top[1] < bottom[1]) {
		i = row0;
		row0 = row1;
		row1 = i;
	}
	step = row1 > row0 ? 1 : -1;
	if (row0 < 0 || row0 >= h || row1 < 0 || row1 >= h)
		return FALSE;

	pixels = malloc((size_t)w * h * 4);
	if (!pixels)
		return FALSE;
	real_glActiveTexture(GL_TEXTURE0 + SKY_MASK_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, tex);
	real_glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	real_glBindTexture(GL_TEXTURE_2D, g_sky_mask);
	real_glActiveTexture(g_active_unit);

	for (x = 0; x < w; x++) {
		float u = (x + 0.5f) / w, s = (u - lo[0]) / (hi[0] - lo[0]), t, dir[3], len, az;
		int y, run = 0, line = row1, bin;

		if (s < 0.0f || s > 1.0f)
			continue;
		for (y = row0; y != row1 + step; y += step) {
			const unsigned char *p = pixels + ((size_t)y * w + x) * 4;
			int next = y + 2 * step;
			const unsigned char *below = next >= 0 && next < h ? pixels + ((size_t)next * w + x) * 4 : NULL;

			if (!sky_texel_ground(p, below)) {
				run = 0;
			} else if (++run == SKY_GROUND_RUN) {
				line = y - (SKY_GROUND_RUN - 1) * step;
				break;
			}
		}
		t = ((line + 0.5f) / h - lo[1]) / (hi[1] - lo[1]);
		sky_corner(corner, s, t, dir);
		len = (float)sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (len <= 0.0f)
			continue;
		az = (float)atan2(dir[2], dir[0]);
		bin = (int)((az / SKY_TWO_PI + 0.5f) * SKY_MASK_BINS) % SKY_MASK_BINS;
		g_sky_line[bin] = max(g_sky_line[bin], dir[1] / len);
	}
	free(pixels);
	return TRUE;
}

static void sky_mask_upload(void)
{
	static float filled[SKY_MASK_BINS];
	float last = SKY_UNSEEN;
	int i;

	/* A face narrower than the bins leaves some empty: they take the last one filled. */
	for (i = 0; i < SKY_MASK_BINS; i++)
		if (g_sky_line[i] > SKY_UNSEEN)
			last = g_sky_line[i];
	for (i = 0; i < SKY_MASK_BINS; i++) {
		if (g_sky_line[i] > SKY_UNSEEN)
			last = g_sky_line[i];
		filled[i] = last;
	}

	if (!g_sky_mask)
		my_glGenTextures(1, &g_sky_mask);
	real_glActiveTexture(GL_TEXTURE0 + SKY_MASK_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, g_sky_mask);
	real_glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, SKY_MASK_BINS, 1, 0, GL_RED, GL_FLOAT, filled);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	real_glActiveTexture(g_active_unit);
}

static void sky_context_lost(void)
{
	memset(g_sky_face, 0, sizeof g_sky_face);
	g_sky_mask = 0;
	g_sky_mask_valid = FALSE;
}

/* Tells sprite.fsh where the face about to be drawn sits, or that it isn't one to draw over. */
static void sky_face_send(const sky_face *f)
{
	GLuint lut = post_sky_texture(), clouds;
	BOOL on = f && f->placed && g_sky_mask_valid && lut && g_cfg_enabled && g_cfg_dynamic_sky;
	float cloud_view[16], cloud_scale[3] = { 0.0f, 0.0f, 0.0f };

	if (!my_glProgramUniform1i || g_loc_sky_face_on < 0)
		return;
	my_glProgramUniform1i(g_sprite_prog, g_loc_sky_face_on, on);
	if (!on)
		return;
	my_glProgramUniformMatrix4fv(g_sprite_prog, g_loc_sky_face, 1, 0, f->place);
	my_glProgramUniform3fv(g_sprite_prog, g_loc_sky_sun, 1, g_sun_world);

	/* Last frame's clouds, found by direction. */
	clouds = post_cloud_texture(cloud_view, cloud_scale);
	cloud_scale[2] = clouds ? 1.0f : 0.0f;
	my_glProgramUniform3fv(g_sprite_prog, g_loc_cloud_scale, 1, cloud_scale);
	if (clouds) {
		my_glProgramUniformMatrix4fv(g_sprite_prog, g_loc_cloud_view, 1, 0, cloud_view);
		real_glActiveTexture(GL_TEXTURE0 + CLOUDS_UNIT);
		real_glBindTexture(GL_TEXTURE_2D, clouds);
	}
	real_glActiveTexture(GL_TEXTURE0 + SKY_LUT_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, lut);
	real_glActiveTexture(GL_TEXTURE0 + SKY_MASK_UNIT);
	real_glBindTexture(GL_TEXTURE_2D, g_sky_mask);
	real_glActiveTexture(g_active_unit);
}

/* Called before each sky draw. A face whose texture changed starts the whole mask again. */
static void sky_face_drawn(GLuint start, GLuint end)
{
	int slot = g_sky_draws++, i, sides = 0;
	GLuint tex = g_unit_tex[0];
	sky_face *f;
	float *p;

	if (slot >= SKY_MAX_DRAWS || end != start + 3 || tex >= MAX_IDS || !real_glGetTexImage ||
	    !my_glGetAttribLocation || !my_glGetBufferSubData || g_tex[tex].w <= 0 || g_tex[tex].h <= 0) {
		sky_face_send(NULL);
		return;
	}
	f = &g_sky_face[slot];
	if (f->tex == tex && f->uploads == g_tex[tex].uploads) {
		sky_face_send(f);
		return;
	}

	/* A face that was read before has changed: a new sky, so every face is read again. */
	if (f->tex) {
		memset(g_sky_face, 0, sizeof g_sky_face);
		g_sky_mask_valid = FALSE;
	}
	for (i = 0; i < SKY_MAX_DRAWS && !g_sky_face[i].tex; i++)
		;
	if (i == SKY_MAX_DRAWS)
		for (i = 0; i < SKY_MASK_BINS; i++)
			g_sky_line[i] = SKY_UNSEEN;
	f->tex = tex;
	f->uploads = g_tex[tex].uploads;

	/* A side face's centre is level with the camera, the top's is straight up. */
	f->placed = sky_face_place(f, start);
	p = f->place;
	f->side = f->placed && fabsf(p[1] + p[5] + p[9] + p[13]) <
		  0.5f * (float)sqrt((p[0] + p[4] + p[8] + p[12]) * (p[0] + p[4] + p[8] + p[12]) +
				     (p[2] + p[6] + p[10] + p[14]) * (p[2] + p[6] + p[10] + p[14]));
	if (f->side && !sky_face_read(f, tex)) {
		f->side = FALSE;
		log_msg("sky: face %d (texture %u) couldn't be read", slot, tex);
	}

	for (i = 0; i < SKY_MAX_DRAWS; i++)
		sides += g_sky_face[i].tex && g_sky_face[i].side;
	if (f->side && sides == 4) {
		sky_mask_upload();
		g_sky_mask_valid = TRUE;
		log_msg("sky: mask made from 4 side faces at frame %u", g_frame);
	}
	sky_face_send(f);
}

static void fbo_destroy(void)
{
	if (!g_fbo)
		return;
	my_glDeleteFramebuffers(1, &g_fbo);
	my_glDeleteRenderbuffers(1, &g_fbo_color);
	my_glDeleteRenderbuffers(1, &g_fbo_depth);
	my_glDeleteFramebuffers(1, &g_present_fbo);
	my_glDeleteRenderbuffers(1, &g_present_color);
	g_fbo = g_present_fbo = 0;
}

static void fbo_create(int w, int h)
{
	GLuint status;

	my_glGenFramebuffers(1, &g_fbo);
	my_glGenRenderbuffers(1, &g_fbo_color);
	my_glGenRenderbuffers(1, &g_fbo_depth);

	my_glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_color);
	my_glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_fbo_samples, g_cfg_post ? GL_RGBA16F : GL_RGBA8, w, h);
	my_glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
	my_glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_fbo_samples, GL_DEPTH_COMPONENT24, w, h);
	my_glBindRenderbuffer(GL_RENDERBUFFER, 0);

	real_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_fbo_color);
	my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
	status = my_glCheckFramebufferStatus(GL_FRAMEBUFFER);

	/* A multisampled float buffer can't resolve straight to the window's format. */
	if (status == GL_FRAMEBUFFER_COMPLETE && g_cfg_post) {
		my_glGenFramebuffers(1, &g_present_fbo);
		my_glGenRenderbuffers(1, &g_present_color);
		my_glBindRenderbuffer(GL_RENDERBUFFER, g_present_color);
		my_glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA16F, w, h);
		my_glBindRenderbuffer(GL_RENDERBUFFER, 0);
		real_glBindFramebuffer(GL_FRAMEBUFFER, g_present_fbo);
		my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_present_color);
		status = my_glCheckFramebufferStatus(GL_FRAMEBUFFER);
	}

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		log_msg("redirect framebuffer incomplete (%x), falling back to direct rendering", status);
		fbo_destroy();
	} else {
		g_fbo_w = w;
		g_fbo_h = h;
		log_msg("redirect framebuffer %ux%u, %d samples", w, h, g_fbo_samples);
	}
	bind_current_fb();
}

/* Cut-out edges like foliage have no geometry for MSAA to smooth, alpha can drive it. */
static void alpha_to_coverage_update(void)
{
	static BOOL on;
	BOOL want = g_fbo && g_cur_fb == 0 && g_cur_prog && g_cur_prog == g_model_prog;

	if (want == on)
		return;
	on = want;
	if (want)
		real_glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	else
		real_glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
}

static void client_size(HDC dc, int *w, int *h)
{
	RECT r = { 0 };

	GetClientRect(WindowFromDC(dc), &r);
	*w = r.right;
	*h = r.bottom;
}

static void resolve_to_window(void)
{
	GLboolean scissor = my_glIsEnabled(GL_SCISSOR_TEST);

	if (scissor)
		real_glDisable(GL_SCISSOR_TEST);
	real_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
	if (g_present_fbo) {
		real_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_present_fbo);
		my_glBlitFramebuffer(0, 0, g_fbo_w, g_fbo_h, 0, 0, g_fbo_w, g_fbo_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		real_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_present_fbo);
	}
	real_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	my_glBlitFramebuffer(0, 0, g_fbo_w, g_fbo_h, 0, 0, g_fbo_w, g_fbo_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	if (scissor)
		real_glEnable(GL_SCISSOR_TEST);
}

static void run_post(void)
{
	static const float default_fog[3] = { 0.8f, 0.85f, 0.9f };
	uni_entry *fog = model_uniform("u_fog_color");
	float sky_sun_view[3];
	post_state s;

	g_post_done = TRUE;
	if (!g_cfg_post || !g_fbo || !g_terrain_prog)
		return;
	/* The map is drawn here, at the end of the frame, from what the frame drew. */
	shadow_frame_end();
	s.scene_fbo = g_fbo;
	s.w = g_fbo_w;
	s.h = g_fbo_h;
	s.projection = g_proj_value[g_terrain_prog];
	s.sun_view = g_light_view;
	world_to_view(sky_sun_view, g_sun_world, g_prev_inv_view);
	s.sky_sun_view = sky_sun_view;
	s.sun_world = g_sun_world;
	s.moon_world = g_moon_world;
	s.direct_loss = g_direct_loss;
	s.ambient_loss = g_ambient_loss;
	s.cloud_cover = g_cfg_cloud_cover / 100.0f;
	s.cloud_quality = g_cfg_cloud_quality;
	s.cloud_light = g_light_is_moon ? g_moon_world : g_sun_world;
	s.cloud_moon = g_light_is_moon;
	s.view_up = g_view_up;
	s.fog_color = fog && fog->data ? fog->data : default_fog;
	s.shadow_matrix = g_shadow_receiver_valid ? g_shadow_receiver : NULL;
	s.shadow_near_matrix = g_shadow_receiver_valid ? g_shadow_receiver_near : NULL;
	s.camera_height = g_camera_y - g_ground_y;
	s.fog_amount = g_cfg_fog / 100.0f;
	s.dof = g_cfg_dof / 100.0f;
	s.saturation = g_cfg_saturation / 100.0f;
	s.contrast = g_cfg_contrast / 100.0f;
	s.split_tone = g_cfg_split_tone / 100.0f;
	s.sharpen = g_cfg_sharpen / 100.0f;
	s.bloom = g_cfg_bloom / 100.0f;
	s.lens_flare = g_cfg_lens_flare / 100.0f;
	s.goggles = g_cfg_goggles && g_frame_trimmed;
	s.sun_moved = sun_moved() ? 1.0f : 0.0f;
	s.world_from_view = g_prev_inv_view;	/* already this frame's, set when the terrain started */
	s.time = g_weather_time;
	s.weather = g_cfg_weather;
	s.weather_amount = g_cfg_weather_amount / 100.0f;
	s.wet = weather_wet();
	s.night = g_night;
	s.motion = g_frame_seconds > 0.0f ? g_cfg_motion_blur / 100.0f * BLUR_SHUTTER / g_frame_seconds : 0.0f;
	s.blur_near = g_frame_trimmed ? BLUR_NEAR_FIRST : BLUR_NEAR_CHASE;
	/* Without the dynamic sky the post passes treat the painted skybox as they did before it. */
	s.sky_mask = g_sky_mask_valid && g_cfg_dynamic_sky ? g_sky_mask : 0;
	s.sky_mask_view = g_cfg_sky_mask_view;
	mat_mul(s.reproject, s.projection, g_view_step);
	s.program = g_cur_prog;
	s.vao = g_cur_vao;
	s.active_unit = g_active_unit;
	s.blend_src = g_blend_src;
	s.blend_dst = g_blend_dst;
	memcpy(s.viewport, g_vp, sizeof s.viewport);
	post_run(&s);
}

/*
 The game's fullscreen changes the display mode, which blanks the monitor and
 drops Windows out of HDR. Its window mode doesn't, so a window that covers the
 monitor looks the same without either. An exact fit still counts as fullscreen
 to the driver, so the window hangs one row off the bottom. Only framed windows
 are touched, which leaves the game's real fullscreen alone.
*/
static void borderless_apply(HWND hwnd)
{
#ifdef _WIN32
	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	MONITORINFO info = { sizeof info };
	const RECT *r = &info.rcMonitor;

	if (!g_cfg_borderless || !(style & (WS_CAPTION | WS_THICKFRAME)))
		return;
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
		return;
	SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP);
	SetWindowPos(hwnd, HWND_TOP, r->left, r->top, r->right - r->left, r->bottom - r->top + 1,
		     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	log_msg("borderless window %ldx%ld", r->right - r->left, r->bottom - r->top + 1);
#else
	/* On Linux the game asks the window manager for fullscreen (_NET_WM_STATE_FULLSCREEN), which
	 changes no display mode, so there is no black screen to avoid. */
	(void)hwnd;
#endif
}

static void present(HDC dc)
{
	int w, h;

	g_hwnd = WindowFromDC(dc);
	borderless_apply(g_hwnd);

	/* Should the HUD marker ever be missed, the effects still run, over the HUD. */
	if (g_shadow_frame && !g_post_done)
		run_post();

	if (g_fbo) {
		resolve_to_window();
		bind_current_fb();
		client_size(dc, &w, &h);
		if (w > 0 && h > 0 && (w != g_fbo_w || h != g_fbo_h)) {
			fbo_destroy();
			fbo_create(w, h);
		}
	}
	timer_end();
	if (g_cap_shot) {
		g_cap_shot = FALSE;
		if (g_fbo) {
			w = g_fbo_w;
			h = g_fbo_h;
		} else {
			client_size(dc, &w, &h);
		}
		save_frame(w, h);
	}
	if (ui_is_open()) {
		ui_state s;

		s.program = g_cur_prog;
		s.vao = g_cur_vao;
		s.array_buffer = g_cur_vbo;
		s.active_unit = g_active_unit;
		s.blend_src = g_blend_src;
		s.blend_dst = g_blend_dst;
		memcpy(s.viewport, g_vp, sizeof s.viewport);
		s.key = g_cfg_ui_key;
		s.enabled = g_cfg_enabled;
		s.debug = g_cfg_debug != 0;
		client_size(dc, &w, &h);
		ui_draw(g_hwnd, w, h, g_cfg_pack, &s);
		bind_current_fb();
	}
	if (g_frame % 600 == 0) {
		GLenum err = my_glGetError();

		if (err)
			log_msg("glGetError %x at frame %u", err, g_frame);
	}
}

/* --- the settings panel --- */

static const int g_shadow_sizes[] = { 1024, 2048, 4096, 8192 };
static const char *const g_shadow_names[] = { "Low", "Medium", "High", "Ultra" };
static const int g_msaa_levels[] = { 0, 2, 4, 8 };
static const int g_aniso_levels[] = { 1, 2, 4, 8, 16 };
/* Cloudy became Cloud Cover, its number stays unused so old ini files read the same. */
static const int g_weather_kinds[] = { WEATHER_CLEAR, WEATHER_RAIN, WEATHER_STORM, WEATHER_SNOW };
static const char *const g_weather_names[] = { "None", "Rain", "Storm", "Snow" };
static const int g_cloud_qualities[] = { CLOUDS_OFF, CLOUDS_LOW, CLOUDS_MEDIUM, CLOUDS_HIGH };
static const char *const g_cloud_quality_names[] = { "Off", "Low", "Medium", "High" };

#define SLIDER(tab, label, key, var, lo, hi, step) { label, key, UI_SLIDER, &var, lo, hi, step, NULL, 0, FALSE, NULL, tab }
#define CHOICE(tab, label, key, var, list) { label, key, UI_CHOICE, &var, 0, 0, 0, list, sizeof list / sizeof list[0], TRUE, NULL, tab }
#define NAMED_CHOICE(tab, label, key, var, list, names, restart) { label, key, UI_CHOICE, &var, 0, 0, 0, list, sizeof list / sizeof list[0], restart, names, tab }
#define TOGGLE(tab, label, key, var, restart) { label, key, UI_TOGGLE, &var, 0, 0, 0, NULL, 0, restart, NULL, tab }
#define CLOCK_SLIDER(tab, label, key, var, lo, hi) { label, key, UI_SLIDER, &var, lo, hi, 1, NULL, 0, FALSE, NULL, tab, UI_SHOW_CLOCK }

/*
 Presets set only what costs frame time, the look stays the player's. Changing one of
 these by hand shows Custom. The order follows g_preset_kinds.
*/
enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA, PRESET_CUSTOM };

typedef struct {
	int shadow_size, msaa, aniso, clouds;
} preset;

static const preset g_presets[] = {
	{ 1024, 2, 4, CLOUDS_LOW },
	{ 2048, 4, 8, CLOUDS_LOW },
	{ 4096, 4, 16, CLOUDS_MEDIUM },
	{ 8192, 8, 16, CLOUDS_HIGH },
};
static const int g_preset_kinds[] = { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA, PRESET_CUSTOM };
static const char *const g_preset_names[] = { "Low", "Medium", "High", "Ultra", "Custom" };
static int g_cfg_preset = PRESET_CUSTOM;	/* worked out from the settings, not saved */

static const ui_item g_ui_items[] = {
	NAMED_CHOICE(UI_TAB_TOP, "Preset", L"preset", g_cfg_preset, g_preset_kinds, g_preset_names, TRUE),

	SLIDER(UI_TAB_LOOK, "Haze", L"fog", g_cfg_fog, 0, 300, 5),
	SLIDER(UI_TAB_LOOK, "Dirt Depth", L"parallax", g_cfg_parallax, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Motion Blur", L"motion_blur", g_cfg_motion_blur, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Depth of Field", L"dof", g_cfg_dof, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Saturation", L"saturation", g_cfg_saturation, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Contrast", L"contrast", g_cfg_contrast, 0, 100, 5),
	SLIDER(UI_TAB_LOOK, "Split Toning", L"split_tone", g_cfg_split_tone, 0, 100, 5),
	SLIDER(UI_TAB_LOOK, "Sharpen", L"sharpen", g_cfg_sharpen, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Texture Sharpness", L"texture_sharpness", g_cfg_texture_sharpness, 0, 100, 5),
	SLIDER(UI_TAB_LOOK, "Bloom", L"bloom", g_cfg_bloom, 0, 200, 5),
	SLIDER(UI_TAB_LOOK, "Lens Flare", L"lens_flare", g_cfg_lens_flare, 0, 200, 5),
	TOGGLE(UI_TAB_LOOK, "Goggles in First Person", L"goggles", g_cfg_goggles, FALSE),

	TOGGLE(UI_TAB_WEATHER, "Dynamic Sky", L"dynamic_sky", g_cfg_dynamic_sky, FALSE),
	CLOCK_SLIDER(UI_TAB_WEATHER, "Time of Day", L"clock", g_cfg_clock, CLOCK_TRACK, CLOCK_STEPS),
	NAMED_CHOICE(UI_TAB_WEATHER, "Weather", L"weather", g_cfg_weather, g_weather_kinds, g_weather_names, FALSE),
	SLIDER(UI_TAB_WEATHER, "Weather Strength", L"weather_amount", g_cfg_weather_amount, 0, 100, 5),
	SLIDER(UI_TAB_WEATHER, "Cloud Cover", L"cloud_cover", g_cfg_cloud_cover, 0, 100, 5),
	SLIDER(UI_TAB_WEATHER, "Wet Ground", L"wet", g_cfg_wet, 0, 100, 5),
	SLIDER(UI_TAB_WEATHER, "Sun Turn", L"sun_turn", g_cfg_sun_turn, 0, 355, 5),

	TOGGLE(UI_TAB_DISPLAY, "Hide Game HUD", L"hide_hud", g_cfg_hide_hud, FALSE),
	TOGGLE(UI_TAB_DISPLAY, "Hide Cursor in Race", L"hide_cursor", g_cfg_hide_cursor, FALSE),
	NAMED_CHOICE(UI_TAB_DISPLAY, "Shadow Quality", L"shadow_size", g_cfg_shadow_size, g_shadow_sizes, g_shadow_names, TRUE),
	NAMED_CHOICE(UI_TAB_DISPLAY, "Cloud Quality", L"cloud_quality", g_cfg_cloud_quality, g_cloud_qualities, g_cloud_quality_names, FALSE),
	CHOICE(UI_TAB_DISPLAY, "Antialiasing", L"msaa", g_cfg_msaa, g_msaa_levels),
	CHOICE(UI_TAB_DISPLAY, "Texture Filtering", L"anisotropy", g_cfg_aniso, g_aniso_levels),
#ifdef _WIN32
	TOGGLE(UI_TAB_DISPLAY, "Borderless Window", L"borderless", g_cfg_borderless, TRUE),
#endif
};

/* Sliders belong to the pack. The default pack's stay in the main section, where they always were. */
static BOOL pack_is_default(void)
{
	return !g_cfg_pack[0] || !_wcsicmp(g_cfg_pack, L"default");
}

static void pack_section(wchar_t *section, size_t size)
{
	if (pack_is_default())
		wcscpy_s(section, size, L"mxs-shaders");
	else
		swprintf_s(section, size, L"pack %s", g_cfg_pack);
}

/* WritePrivateProfileString only touches its own line, so the file's comments stay. */
static void setting_write(const ui_item *item)
{
	wchar_t path[MAX_PATH], text[16], section[80] = L"mxs-shaders";

	if (item->kind == UI_SLIDER)
		pack_section(section, 80);
	log_path(path, L"mxs-shaders.ini");
	swprintf_s(text, 16, L"%d", *item->value);
	WritePrivateProfileStringW(section, item->key, text, path);
}

/* Which preset the settings match, else Custom. */
static void preset_detect(void)
{
	int i;

	g_cfg_preset = PRESET_CUSTOM;
	for (i = 0; i < PRESET_CUSTOM; i++) {
		const preset *p = &g_presets[i];

		if (g_cfg_shadow_size == p->shadow_size && g_cfg_msaa == p->msaa && g_cfg_aniso == p->aniso &&
		    g_cfg_cloud_quality == p->clouds)
			g_cfg_preset = i;
	}
}

/* Sets the chosen preset's settings and saves them where their own rows would. Custom moves on to Low. */
static void preset_apply(void)
{
	const preset *p;
	size_t i;

	if (g_cfg_preset == PRESET_CUSTOM)
		g_cfg_preset = PRESET_LOW;
	p = &g_presets[g_cfg_preset];
	g_cfg_shadow_size = p->shadow_size;
	g_cfg_msaa = p->msaa;
	g_cfg_aniso = p->aniso;
	g_cfg_cloud_quality = p->clouds;
	for (i = 0; i < sizeof g_ui_items / sizeof g_ui_items[0]; i++) {
		const int *v = g_ui_items[i].value;

		if (v == &g_cfg_shadow_size || v == &g_cfg_msaa || v == &g_cfg_aniso || v == &g_cfg_cloud_quality)
			setting_write(&g_ui_items[i]);
	}
	log_msg("preset %s", g_preset_names[g_cfg_preset]);
}

void ui_setting_changed(const ui_item *item)
{
	if (item->value == &g_cfg_preset) {
		preset_apply();
		return;
	}
	setting_write(item);
	preset_detect();
}

void ui_pack_changed(const wchar_t *name)
{
	wchar_t path[MAX_PATH];

	wcscpy_s(g_cfg_pack, 64, name);
	log_path(path, L"mxs-shaders.ini");
	WritePrivateProfileStringW(L"mxs-shaders", L"pack", name, path);
	pack_init(name);
	g_reload_requested = TRUE;
}

void ui_key_changed(int key)
{
	wchar_t path[MAX_PATH], text[16];

	g_cfg_ui_key = key;
	g_ui_key_down = TRUE;	/* it's still held, and mustn't close the panel */
	log_path(path, L"mxs-shaders.ini");
	swprintf_s(text, 16, L"%d", key);
	WritePrivateProfileStringW(L"mxs-shaders", L"ui_key", text, path);
}

/* The switch itself waits for the frame boundary, nothing is mid-draw there. */
void ui_enabled_changed(int enabled)
{
	wchar_t path[MAX_PATH];

	g_cfg_enabled = enabled;
	g_enabled_changed = TRUE;
	log_path(path, L"mxs-shaders.ini");
	WritePrivateProfileStringW(L"mxs-shaders", L"enabled", enabled ? L"1" : L"0", path);
}

/* A negative bias picks a sharper mip level, from the Texture Sharpness setting. */
static float lod_bias_wanted(void)
{
	return g_cfg_enabled ? -g_cfg_texture_sharpness / 100.0f : 0.0f;
}

/* The game never set these itself, so its own look has anisotropy at 1 and no bias. */
static void texture_filter_apply(void)
{
	static BOOL bias_touched;
	GLuint unit = g_active_unit - GL_TEXTURE0, t;
	BOOL bias;

	g_aniso = g_cfg_enabled ? g_aniso_setting : 1.0f;
	g_lod_bias = lod_bias_wanted();
	bias = g_lod_bias != 0.0f || bias_touched;
	bias_touched = g_lod_bias != 0.0f;
	if (g_aniso_setting <= 1.0f && !bias)
		return;
	for (t = 1; t < MAX_IDS; t++) {
		if (!g_tex[t].aniso || !my_glIsTexture(t))
			continue;
		real_glBindTexture(GL_TEXTURE_2D, t);
		if (g_aniso_setting > 1.0f)
			real_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, g_aniso);
		if (bias)
			real_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, g_lod_bias);
	}
	real_glBindTexture(GL_TEXTURE_2D, unit < 32 ? g_unit_tex[unit] : g_bound_tex);
}

/* Without the redirect the game draws to the window, the same path msaa=0 takes. */
static void enabled_apply(void)
{
	RECT r = { 0 };

	log_msg("mod %s at frame %u", g_cfg_enabled ? "on" : "off", g_frame);
	GetClientRect(g_hwnd, &r);
	fbo_destroy();
	if (g_cfg_enabled && g_fbo_samples > 1 && r.right > 0 && r.bottom > 0)
		fbo_create(r.right, r.bottom);
	bind_current_fb();
	alpha_to_coverage_update();
	texture_filter_apply();
	g_reload_requested = TRUE;
}

void ui_reload_requested(void)
{
	g_reload_requested = TRUE;
}

#define MY(n) my_##n = (n##_t)real_wglGetProcAddress(#n)

static void context_ready(HDC dc)
{
	float max_aniso = 1.0f;
	GLint max_samples = 0;
	int w, h;

	my_glGenFramebuffers = (glGenObjects_t)real_wglGetProcAddress("glGenFramebuffers");
	my_glGenRenderbuffers = (glGenObjects_t)real_wglGetProcAddress("glGenRenderbuffers");
	my_glDeleteFramebuffers = (glDeleteObjects_t)real_wglGetProcAddress("glDeleteFramebuffers");
	my_glDeleteRenderbuffers = (glDeleteObjects_t)real_wglGetProcAddress("glDeleteRenderbuffers");
	MY(glBindRenderbuffer);
	MY(glRenderbufferStorageMultisample);
	MY(glFramebufferRenderbuffer);
	MY(glCheckFramebufferStatus);
	MY(glBlitFramebuffer);
	real_glBindFramebuffer = (glBindFramebuffer_t)real_wglGetProcAddress("glBindFramebuffer");
	real_glShaderSource = (glShaderSource_t)real_wglGetProcAddress("glShaderSource");
	real_glCompileShader = (glCompileShader_t)real_wglGetProcAddress("glCompileShader");
	my_glGetShaderiv = (glGetObjectiv_t)real_wglGetProcAddress("glGetShaderiv");
	my_glGetProgramiv = (glGetObjectiv_t)real_wglGetProcAddress("glGetProgramiv");
	my_glGetShaderInfoLog = (glGetInfoLog_t)real_wglGetProcAddress("glGetShaderInfoLog");
	my_glGetProgramInfoLog = (glGetInfoLog_t)real_wglGetProcAddress("glGetProgramInfoLog");
	my_glGenQueries = (glGenObjects_t)real_wglGetProcAddress("glGenQueries");
	MY(glBeginQuery);
	MY(glEndQuery);
	MY(glGetQueryObjectiv);
	MY(glGetQueryObjectui64v);
	MY(glGetAttribLocation);
	MY(glGetVertexAttribiv);
	MY(glGetVertexAttribPointerv);
	MY(glGetBufferParameteriv);
	MY(glGetBufferSubData);
	MY(glBindBuffer);
	MY(glDebugMessageCallback);
	MY(glProgramUniformMatrix4fv);
	MY(glProgramUniform1i);
	MY(glProgramUniform3fv);
	MY(glProgramUniform1f);
	MY(glGetAttachedShaders);
	MY(glDetachShader);
	MY(glDeleteShader);
	MY(glCreateProgram);
	MY(glDeleteProgram);
	MY(glIsProgram);
	real_glActiveTexture = (glActiveTexture_t)real_wglGetProcAddress("glActiveTexture");
	real_glUniformMatrix4fv = (glUniformMatrix4fv_t)real_wglGetProcAddress("glUniformMatrix4fv");
	real_glUniform3fv = (glUniform3fv_t)real_wglGetProcAddress("glUniform3fv");
	real_glGetUniformLocation = (glGetUniformLocation_t)real_wglGetProcAddress("glGetUniformLocation");
	real_glFramebufferTexture2D = (glFramebufferTexture2D_t)real_wglGetProcAddress("glFramebufferTexture2D");

	if (g_cfg_gl_debug && my_glDebugMessageCallback) {
		real_glEnable(GL_DEBUG_OUTPUT);
		real_glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		my_glDebugMessageCallback(gl_debug_message, NULL);
		log_msg("GL debug output enabled");
	}
	if (my_glGenQueries && my_glBeginQuery && my_glGetQueryObjectui64v)
		my_glGenQueries(TIMER_QUERIES, g_timer);

	my_glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_aniso);
	g_aniso_setting = g_cfg_aniso > 1 ? min((float)g_cfg_aniso, max_aniso) : 1.0f;
	g_aniso = g_cfg_enabled ? g_aniso_setting : 1.0f;
	log_msg("anisotropy %.0f (driver max %.0f)", g_aniso_setting, max_aniso);
	my_glGetError();

	my_glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
	g_fbo_samples = min(g_cfg_msaa, max_samples);
	client_size(dc, &w, &h);
	if (g_cfg_enabled && g_fbo_samples > 1 && w > 0 && h > 0)
		fbo_create(w, h);
	shadow_create();
	/* Not tied to g_fbo: starting with the mod off has none yet, and it can be turned on later. */
	if (g_cfg_post && g_fbo_samples > 1)
		post_init(real_wglGetProcAddress);
	ui_init(real_wglGetProcAddress, g_ui_items, sizeof g_ui_items / sizeof g_ui_items[0]);
	/* Setting up binds textures on units of the mod's own. The game binds its own wherever it left the unit. */
	real_glActiveTexture(g_active_unit);
}

static void apply_aniso(GLenum target, GLint filter)
{
	if (g_bound_tex < MAX_IDS)
		g_tex[g_bound_tex].min_filter = filter;
	if (target == GL_TEXTURE_2D && filter >= GL_NEAREST_MIPMAP_NEAREST && filter <= GL_LINEAR_MIPMAP_LINEAR) {
		/* Marked even when off, turning the mod on later has to find it. */
		if (g_bound_tex < MAX_IDS)
			g_tex[g_bound_tex].aniso = TRUE;
		if (g_aniso > 1.0f) {
			real_glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, g_aniso);
			g_aniso_count++;
		}
		if (g_lod_bias != 0.0f)
			real_glTexParameterf(target, GL_TEXTURE_LOD_BIAS, g_lod_bias);
	}
}

/* --- static hooks --- */

HOOK(void, glClear, (GLbitfield mask))
{
	/* Fallback in case SwapBuffers never reaches our wglSwapBuffers. */
	if (!g_saw_swap && g_cur_fb == 0 && g_draws > 0)
		frame_boundary("glClear on fb 0");
	g_clears++;
	cap("clear mask=%x fb=%u", mask, g_cur_fb);
	real_glClear(mask);
}

/* Only while riding: anything else may need the mouse. ShowCursor counts, so it stays balanced. */
static void cursor_update(void)
{
	BOOL hide = g_cfg_hide_cursor && g_shadow_ready && g_hud_boxes_last < HUD_MENU_BOXES && !ui_is_open();

	if (hide != g_cursor_hidden) {
		ShowCursor(!hide);
		g_cursor_hidden = hide;
	}
}

/*
 In a race the 2D overlay comes last. Menus share its font, but each of their rows
 is built from plain 1x1 boxes and the HUD has almost none, which tells them apart.
*/
static BOOL hud_draw_hidden(void)
{
	if (!g_hud_phase || g_cur_fb != 0)
		return FALSE;
	if (g_bound_tex < MAX_IDS && g_tex[g_bound_tex].w == 1)
		g_hud_boxes++;
	return g_cfg_hide_hud && g_hud_boxes_last < HUD_MENU_BOXES;
}

HOOK(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))
{
	count_draw();
	cap("drawarrays mode=%x first=%d count=%d fb=%u prog=%u", mode, first, count, g_cur_fb, g_cur_prog);
	if (!hud_draw_hidden())
		real_glDrawArrays(mode, first, count);
}

HOOK(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices))
{
	count_draw();
	cap("drawelements mode=%x count=%d type=%x fb=%u prog=%u offset=%zu nbones=%d vao=%u vbo=%u ibo=%u", mode, count, type,
	    g_cur_fb, g_cur_prog, (size_t)indices, g_cur_prog == g_model_prog ? g_cur_nbones : 0, g_cur_vao, g_cur_vbo, g_cur_ibo);
	{
		int kind = shadow_draw_kind();
		int slot = kind == DRAW_DYNAMIC && rider_draw() ? full_mesh_slot(count, type, indices) : 0;
		const mesh_sphere *sphere = kind == DRAW_STATIC && g_cur_prog == g_model_prog ? mesh_sphere_current() : NULL;

		if (slot)
			skinned_draw_seen(slot, count, type, indices);
		else if (kind == DRAW_STATIC && g_cur_prog == g_model_prog) {
			rigid_draw_seen(count, type, indices);
			if (g_rider_run == RIDER_DRAWING)
				g_rider_run = RIDER_DONE;
		} else if (kind == DRAW_STATIC && g_cur_prog == g_terrain_prog) {
			terrain_draw_seen(count, type, indices);
		}
		rec(REC_DRAWELEMENTS, mode, count, type, slot, kind, (size_t)indices, sphere, sphere ? sizeof *sphere : 0);
	}
	if (!hud_draw_hidden())
		real_glDrawElements(mode, count, type, indices);
}

HOOK(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))
{
	cap("viewport %d %d %d %d", x, y, w, h);
	g_vp[0] = x;
	g_vp[1] = y;
	g_vp[2] = w;
	g_vp[3] = h;
	real_glViewport(x, y, w, h);
}

HOOK(void, glEnable, (GLenum cap_))
{
	cap("enable %x", cap_);
	if (cap_ == GL_CULL_FACE && g_particle_phase) {
		g_particle_phase = FALSE;
		g_hud_phase = TRUE;
		sprite_set_mode(SPRITE_NORMAL);
	}
	real_glEnable(cap_);
}

HOOK(void, glDisable, (GLenum cap_))
{
	cap("disable %x", cap_);
	if (cap_ == GL_CULL_FACE && g_shadow_frame && !g_post_done && g_cur_fb == 0) {
		run_post();
		g_particle_phase = TRUE;
		sprite_set_mode(SPRITE_PARTICLE);
	}
	real_glDisable(cap_);
}

HOOK(void, glBlendFunc, (GLenum s, GLenum d))
{
	cap("blendfunc %x %x", s, d);
	g_blend_src = s;
	g_blend_dst = d;
	real_glBlendFunc(s, d);
}

HOOK(void, glDepthMask, (GLboolean flag))
{
	cap("depthmask %d", flag);
	g_depth_mask = flag;
	real_glDepthMask(flag);
}

HOOK(void, glBindTexture, (GLenum target, GLuint tex))
{
	g_bound_tex = tex;
	if (g_active_unit - GL_TEXTURE0 < 32)
		g_unit_tex[g_active_unit - GL_TEXTURE0] = tex;
	rec(REC_BINDTEXTURE, target, tex, 0, 0, 0, 0, NULL, 0);
	if (g_cap && tex < MAX_IDS)
		cap("bindtexture %x %u prog=%u %dx%d internal=%x minfilter=%x aniso=%d", target, tex, g_cur_prog,
		    g_tex[tex].w, g_tex[tex].h, g_tex[tex].internal, g_tex[tex].min_filter, g_tex[tex].aniso);
	real_glBindTexture(target, tex);
}

HOOK(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			  GLint border, GLenum format, GLenum type, const void *pixels))
{
	cap("teximage2d target=%x level=%d internal=%x %dx%d format=%x type=%x", target, level, internal, w, h, format, type);
	if (level == 0 && g_bound_tex < MAX_IDS) {
		g_tex[g_bound_tex].w = w;
		g_tex[g_bound_tex].h = h;
		g_tex[g_bound_tex].internal = internal;
		g_tex[g_bound_tex].uploads++;
	}
	real_glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}

HOOK(void, glTexParameteri, (GLenum target, GLenum pname, GLint value))
{
	if (pname == GL_TEXTURE_MAX_ANISOTROPY)
		log_msg("game sets anisotropy %d", value);
	real_glTexParameteri(target, pname, value);
	if (pname == GL_TEXTURE_MIN_FILTER)
		apply_aniso(target, value);
}

HOOK(void, glTexParameterf, (GLenum target, GLenum pname, float value))
{
	if (pname == GL_TEXTURE_MAX_ANISOTROPY) {
		log_msg("game sets anisotropy %.1f", value);
		if (g_aniso > value)
			value = g_aniso;
	}
	real_glTexParameterf(target, pname, value);
	if (pname == GL_TEXTURE_MIN_FILTER)
		apply_aniso(target, (GLint)value);
}

HOOK(void, glTexParameterfv, (GLenum target, GLenum pname, const float *value))
{
	real_glTexParameterfv(target, pname, value);
	if (pname == GL_TEXTURE_MIN_FILTER)
		apply_aniso(target, (GLint)value[0]);
}

HOOK(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels))
{
	cap("readpixels %d %d %d %d format=%x type=%x fb=%u", x, y, w, h, format, type, g_cur_fb);
	/* A multisampled framebuffer can't be read, so copy to the window first. */
	if (g_fbo && g_cur_fb == 0) {
		resolve_to_window();
		real_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		real_glReadPixels(x, y, w, h, format, type, pixels);
		bind_current_fb();
		return;
	}
	real_glReadPixels(x, y, w, h, format, type, pixels);
}

HOOK(void, glFinish, (void))
{
	cap("finish");
	real_glFinish();
}

#ifdef _WIN32
HOOK(HGLRC, wglCreateContext, (HDC dc))
{
	HGLRC rc = real_wglCreateContext(dc);

	log_msg("wglCreateContext -> %p", rc);
	return rc;
}

HOOK(BOOL, wglDeleteContext, (HGLRC rc))
{
	log_msg("wglDeleteContext %p", rc);
	if (rc == g_main_rc) {
		/* The GL objects die with the context. */
		g_main_rc = NULL;
		g_fbo = 0;
		g_aniso = 1.0f;
		sky_context_lost();
		post_context_lost();
		ui_context_lost();
	}
	return real_wglDeleteContext(rc);
}

HOOK(BOOL, wglMakeCurrent, (HDC dc, HGLRC rc))
{
	typedef const char *(WINAPI *glGetString_t)(GLenum);
	BOOL ok = real_wglMakeCurrent(dc, rc);

	log_msg("wglMakeCurrent %p -> %d", rc, ok);
	if (ok && rc) {
		glGetString_t gs = (glGetString_t)GetProcAddress(g_real_gl, "glGetString");

		log_msg("  GL_VENDOR   %s", gs(GL_VENDOR));
		log_msg("  GL_RENDERER %s", gs(GL_RENDERER));
		log_msg("  GL_VERSION  %s", gs(GL_VERSION));
		log_msg("  GLSL        %s", gs(GL_SHADING_LANGUAGE_VERSION));
		if (rc == g_main_rc && !real_glBindFramebuffer)
			context_ready(dc);
	}
	return ok;
}

HOOK(BOOL, wglSwapBuffers, (HDC dc))
{
	if (!g_saw_swap)
		log_msg("wglSwapBuffers reached the proxy");
	g_saw_swap = TRUE;
	present(dc);
	frame_boundary("wglSwapBuffers");
	return real_wglSwapBuffers(dc);
}

HOOK(BOOL, wglSwapLayerBuffers, (HDC dc, UINT planes))
{
	if (!g_saw_swap)
		log_msg("wglSwapLayerBuffers reached the proxy");
	g_saw_swap = TRUE;
	present(dc);
	frame_boundary("wglSwapLayerBuffers");
	return real_wglSwapLayerBuffers(dc, planes);
}
#endif

/* --- dynamic hooks --- */

/*
 Shaders are identified by the names they use, not by hash, so that a game
 version with a slightly different shader still gets the same role.
*/
static const char *shader_role(GLenum type, const char *src)
{
	if (type == GL_VERTEX_SHADER) {
		if (strstr(src, "interp_tilecoord"))
			return "tilemap";
		if (strstr(src, "interp_hcoord"))
			return "terrain";
		if (strstr(src, "vattr_bones"))
			return "skinned";
		if (strstr(src, "interp_tc"))
			return "decal";
		if (strstr(src, "interp_color"))
			return "sprite";
	} else if (type == GL_FRAGMENT_SHADER) {
		if (strstr(src, "u_tilemap"))
			return "tilemap";
		if (strstr(src, "interp_hcoord"))
			return "terrain";
		if (strstr(src, "u_shadows"))
			return "decal";
		if (strstr(src, "u_envmat"))
			return "reflection";
		if (strstr(src, "u_shininess"))
			return "model";
		if (strstr(src, "interp_color"))
			return "sprite";
	}
	return NULL;
}

/*
 A pack's source for a role, or NULL for the game's own. A vertex shader can also be
 wrapped: a .vwrap file goes after the game's source, which is why it can use the
 game's variables, and calls game_main() in place of the game's main(). Packs can
 change where vertices land this way without holding a copy of the game's shader.
*/
static char *pack_shader(const char *role, GLenum type, const char *game_src)
{
	static const char game_main[] = "void game_main";
	char file[64], *repl, *wrap, *out;
	const char *at;
	size_t before, size;

	sprintf_s(file, sizeof file, "%s.%s", role, type == GL_VERTEX_SHADER ? "vsh" : "fsh");
	repl = pack_load(file);
	if (repl || type != GL_VERTEX_SHADER)
		return repl;

	sprintf_s(file, sizeof file, "%s.vwrap", role);
	wrap = pack_load(file);
	if (!wrap)
		return NULL;
	at = strstr(game_src, "void main");
	if (!at) {
		log_msg("  %s: the game's shader has no main to wrap", file);
		free(wrap);
		return NULL;
	}
	before = at - game_src;
	size = strlen(game_src) + strlen(wrap) + sizeof game_main + 2;
	out = malloc(size);
	memcpy(out, game_src, before);
	sprintf_s(out + before, size - before, "%s%s\n%s", game_main, at + strlen("void main"), wrap);
	free(wrap);
	return out;
}

#ifdef _WIN32
HOOK(HGLRC, wglCreateContextAttribsARB, (HDC dc, HGLRC share, const int *attribs))
{
	int patched[32];
	HGLRC rc = NULL;
	int i = 0;

	if (g_cfg_gl_major > 0 && attribs) {
		for (; attribs[i] && i < 30; i += 2) {
			log_msg("context attrib %x = %x", attribs[i], attribs[i + 1]);
			patched[i] = attribs[i];
			patched[i + 1] = attribs[i + 1];
			if (attribs[i] == WGL_CONTEXT_MAJOR_VERSION_ARB)
				patched[i + 1] = g_cfg_gl_major;
			else if (attribs[i] == WGL_CONTEXT_MINOR_VERSION_ARB)
				patched[i + 1] = g_cfg_gl_minor;
		}
		patched[i] = 0;
		rc = real_wglCreateContextAttribsARB(dc, share, patched);
		log_msg("requested GL %d.%d -> %p", g_cfg_gl_major, g_cfg_gl_minor, rc);
	}
	if (!rc)
		rc = real_wglCreateContextAttribsARB(dc, share, attribs);
	log_msg("wglCreateContextAttribsARB -> %p", rc);
	g_main_rc = rc;
	return rc;
}
#endif

HOOK(GLuint, glCreateShader, (GLenum type))
{
	GLuint s = real_glCreateShader(type);

	if (s < MAX_IDS)
		g_shader_type[s] = type;
	return s;
}

HOOK(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))
{
	unsigned long long h = 0xcbf29ce484222325ULL;
	const char *kind = "unknown", *role;
	char name[64];
	char *src, *repl = NULL;
	size_t total = 0;
	FILE *f;
	GLsizei i;

	for (i = 0; i < count; i++)
		total += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
	src = malloc(total + 1);
	total = 0;
	for (i = 0; i < count; i++) {
		size_t n = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);

		memcpy(src + total, string[i], n);
		total += n;
	}
	src[total] = 0;
	h = fnv1a(h, src, total);

	if (shader < MAX_IDS) {
		g_shader_hash[shader] = h;
		if (g_shader_type[shader] == GL_VERTEX_SHADER)
			kind = "vs";
		else if (g_shader_type[shader] == GL_FRAGMENT_SHADER)
			kind = "fs";
	}

	sprintf_s(name, sizeof name, "%s_%016llx.glsl", kind, h);
	log_msg("glShaderSource shader=%u %s", shader, name);

	f = g_cfg_debug ? log_open(L"shaders", name) : NULL;
	if (f) {
		fwrite(src, 1, total, f);
		fclose(f);
	}

	role = shader_role(shader < MAX_IDS ? g_shader_type[shader] : 0, src);
	if (shader < MAX_IDS)
		g_shader_role[shader] = role;
	if (role && shader < MAX_IDS && g_cfg_enabled) {
		repl = pack_shader(role, g_shader_type[shader], src);
		log_msg("  role %s.%s, %s", role, g_shader_type[shader] == GL_VERTEX_SHADER ? "vsh" : "fsh",
			repl ? "using pack shader" : "using game shader");
	} else {
		log_msg("  unknown shader role, using game shader");
	}

	if (shader < MAX_IDS) {
		free(g_shader_orig[shader]);
		g_shader_orig[shader] = src;
		g_shader_replaced[shader] = repl != NULL;
	}
	if (repl) {
		real_glShaderSource(shader, 1, (const GLchar *const *)&repl, NULL);
		free(repl);
	} else {
		real_glShaderSource(shader, count, string, length);
	}
	if (shader >= MAX_IDS)
		free(src);
}

static void restore_game_shader(GLuint shader)
{
	const char *src = g_shader_orig[shader];

	real_glShaderSource(shader, 1, &src, NULL);
	real_glCompileShader(shader);
	g_shader_replaced[shader] = FALSE;
}

HOOK(void, glCompileShader, (GLuint shader))
{
	char info[2048] = "";
	GLint ok = 1;

	real_glCompileShader(shader);
	if (shader >= MAX_IDS || !g_shader_replaced[shader])
		return;

	my_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		my_glGetShaderInfoLog(shader, sizeof info, NULL, info);
		log_msg("pack shader failed to compile, using game shader:\n%s", info);
		restore_game_shader(shader);
	}
}

HOOK(void, glAttachShader, (GLuint program, GLuint shader))
{
	if (program < MAX_IDS && shader < MAX_IDS) {
		if (g_shader_type[shader] == GL_VERTEX_SHADER) {
			g_prog_vs[program] = g_shader_hash[shader];
			g_prog_vs_id[program] = shader;
		} else {
			g_prog_fs[program] = g_shader_hash[shader];
			g_prog_fs_id[program] = shader;
		}
	}
	real_glAttachShader(program, shader);
}

HOOK(void, glLinkProgram, (GLuint program))
{
	if (program < MAX_IDS)
		log_msg("glLinkProgram program=%u vs=%016llx fs=%016llx", program, g_prog_vs[program], g_prog_fs[program]);
	real_glLinkProgram(program);

	if (program < MAX_IDS && (g_shader_replaced[g_prog_vs_id[program]] || g_shader_replaced[g_prog_fs_id[program]])) {
		char info[2048] = "";
		GLint ok = 1;

		my_glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok) {
			my_glGetProgramInfoLog(program, sizeof info, NULL, info);
			log_msg("pack shader failed to link, using game shaders:\n%s", info);
			if (g_shader_replaced[g_prog_vs_id[program]])
				restore_game_shader(g_prog_vs_id[program]);
			if (g_shader_replaced[g_prog_fs_id[program]])
				restore_game_shader(g_prog_fs_id[program]);
			real_glLinkProgram(program);
		}
	}
	program_refresh(program);
}

static void program_refresh(GLuint program)
{
	if (program < MAX_IDS) {
		const char *role = g_shader_role[g_prog_fs_id[program]];

		g_loc_proj[program] = real_glGetUniformLocation(program, "u_projection");
		g_loc_mv[program] = real_glGetUniformLocation(program, "u_modelview");
		g_loc_light[program] = real_glGetUniformLocation(program, "u_light_position");
		g_loc_shadow_matrix[program] = real_glGetUniformLocation(program, "u_shadow_matrix");
		g_loc_shadow_map[program] = real_glGetUniformLocation(program, "u_shadow_map");
		g_loc_shadow_on[program] = real_glGetUniformLocation(program, "u_shadow_on");
		g_loc_night[program] = real_glGetUniformLocation(program, "u_night");
		g_loc_direct_loss[program] = real_glGetUniformLocation(program, "u_direct_loss");
		g_loc_ambient_loss[program] = real_glGetUniformLocation(program, "u_ambient_loss");
		g_loc_shadow_pass[program] = real_glGetUniformLocation(program, "u_shadow_pass");
		g_loc_view_up[program] = real_glGetUniformLocation(program, "u_view_up");
		g_loc_shadow_near_matrix[program] = real_glGetUniformLocation(program, "u_shadow_near_matrix");
		g_loc_shadow_near_map[program] = real_glGetUniformLocation(program, "u_shadow_near_map");
		g_loc_nbones[program] = real_glGetUniformLocation(program, "u_nbones");
		g_loc_sprite_mode[program] = real_glGetUniformLocation(program, "u_sprite_mode");
		g_loc_sun_view[program] = real_glGetUniformLocation(program, "u_sun_view");
		g_loc_screen[program] = real_glGetUniformLocation(program, "u_screen");
		g_loc_hdr[program] = real_glGetUniformLocation(program, "u_hdr");
		g_loc_parallax[program] = real_glGetUniformLocation(program, "u_parallax");
		g_loc_world_from_view[program] = real_glGetUniformLocation(program, "u_world_from_view");
		g_loc_time[program] = real_glGetUniformLocation(program, "u_time");
		g_loc_weather[program] = real_glGetUniformLocation(program, "u_weather");
		g_loc_weather_amount[program] = real_glGetUniformLocation(program, "u_weather_amount");
		g_loc_cloud_cover[program] = real_glGetUniformLocation(program, "u_cloud_cover");
		g_loc_wet[program] = real_glGetUniformLocation(program, "u_wet");
		g_loc_weather_noise[program] = real_glGetUniformLocation(program, "u_weather_noise");

		g_prog_role[program] = ROLE_NONE;
		if (role && !strcmp(role, "terrain")) {
			g_prog_role[program] = ROLE_TERRAIN;
			g_terrain_prog = program;
		} else if (role && !strcmp(role, "model")) {
			g_prog_role[program] = ROLE_MODEL;
			g_model_prog = program;
		} else if (role && !strcmp(role, "sprite")) {
			g_sprite_prog = program;
			g_sprite_mode = -1;
			g_loc_sky_face_on = real_glGetUniformLocation(program, "u_sky_face_on");
			g_loc_sky_face = real_glGetUniformLocation(program, "u_sky_face");
			g_loc_sky_sun = real_glGetUniformLocation(program, "u_sky_sun");
			g_loc_cloud_view = real_glGetUniformLocation(program, "u_cloud_view");
			g_loc_cloud_scale = real_glGetUniformLocation(program, "u_cloud_scale");
			if (my_glProgramUniform1i) {
				my_glProgramUniform1i(program, real_glGetUniformLocation(program, "u_clouds"), CLOUDS_UNIT);
				my_glProgramUniform1i(program, real_glGetUniformLocation(program, "u_sky"), SKY_LUT_UNIT);
				my_glProgramUniform1i(program, real_glGetUniformLocation(program, "u_sky_mask"), SKY_MASK_UNIT);
			}
		} else if (role && !strcmp(role, "reflection")) {
			g_reflection_prog = program;
		}
		if (g_loc_hdr[program] >= 0 && my_glProgramUniform1i)
			my_glProgramUniform1i(program, g_loc_hdr[program], g_hdr_on);
		if (g_loc_shadow_map[program] >= 0 && my_glProgramUniform1i)
			my_glProgramUniform1i(program, g_loc_shadow_map[program], SHADOW_UNIT);
		if (g_loc_shadow_near_map[program] >= 0 && my_glProgramUniform1i)
			my_glProgramUniform1i(program, g_loc_shadow_near_map[program], SHADOW_UNIT + 1);
	}
}

/* --- hot reload --- */

/*
 Rebuilds a shader from the pack, or the game's source if the pack has no such
 file or the mod is off. Returns 0 and logs if it doesn't compile.
*/
static GLuint reload_shader(GLuint old_id, GLenum type)
{
	char info[2048] = "";
	const char *role = g_shader_role[old_id];
	const char *src = g_shader_orig[old_id];
	char *repl = NULL;
	GLuint s;
	GLint ok = 0;

	if (role && g_cfg_enabled) {
		repl = pack_shader(role, type, src);
		if (repl)
			src = repl;
	}
	s = real_glCreateShader(type);
	real_glShaderSource(s, 1, &src, NULL);
	real_glCompileShader(s);
	my_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		my_glGetShaderInfoLog(s, sizeof info, NULL, info);
		log_msg("hot reload: %s shader failed to compile, keeping the current shader:\n%s", repl ? role : "game", info);
		my_glDeleteShader(s);
		s = 0;
	}
	free(repl);
	return s;
}

static void restore_uniforms(GLuint p)
{
	int i;

	real_glUseProgram(p);
	for (i = 0; i < g_uni_count[p]; i++) {
		uni_entry *u = &g_uni[p][i];

		u->real = real_glGetUniformLocation(p, u->name);
		if (u->real < 0 || !u->count)
			continue;
		switch (u->kind) {
		case REC_UNIFORM1I: real_glUniform1i(u->real, u->value); break;
		case REC_UNIFORM1F: real_glUniform1f(u->real, *(const float *)u->data); break;
		case REC_UNIFORM2FV: real_glUniform2fv(u->real, u->count, u->data); break;
		case REC_UNIFORM3FV: real_glUniform3fv(u->real, u->count, u->data); break;
		case REC_UNIFORM4FV: real_glUniform4fv(u->real, u->count, u->data); break;
		case REC_UNIFORMMAT3: real_glUniformMatrix3fv(u->real, u->count, (GLboolean)u->transpose, u->data); break;
		case REC_UNIFORMMAT4: real_glUniformMatrix4fv(u->real, u->count, (GLboolean)u->transpose, u->data); break;
		}
	}
}

static BOOL reload_program(GLuint p)
{
	GLuint vs, fs, test, old[8];
	GLsizei n = 0;
	GLint ok = 0;
	char info[2048] = "";
	GLsizei i;

	vs = reload_shader(g_prog_vs_id[p], GL_VERTEX_SHADER);
	fs = reload_shader(g_prog_fs_id[p], GL_FRAGMENT_SHADER);
	if (!vs || !fs) {
		if (vs)
			my_glDeleteShader(vs);
		if (fs)
			my_glDeleteShader(fs);
		return FALSE;
	}

	/* A failed link would break the running program, so prove it on a spare one. */
	test = my_glCreateProgram();
	real_glAttachShader(test, vs);
	real_glAttachShader(test, fs);
	real_glLinkProgram(test);
	my_glGetProgramiv(test, GL_LINK_STATUS, &ok);
	if (!ok) {
		my_glGetProgramInfoLog(test, sizeof info, NULL, info);
		log_msg("hot reload: program %u failed to link, keeping the current shaders:\n%s", p, info);
	}
	my_glDeleteProgram(test);

	if (ok) {
		my_glGetAttachedShaders(p, 8, &n, old);
		for (i = 0; i < n; i++)
			my_glDetachShader(p, old[i]);
		real_glAttachShader(p, vs);
		real_glAttachShader(p, fs);
		real_glLinkProgram(p);
		my_glDetachShader(p, vs);
		my_glDetachShader(p, fs);
		program_refresh(p);
		restore_uniforms(p);
	}
	my_glDeleteShader(vs);
	my_glDeleteShader(fs);
	return ok != 0;
}

/* New keys are added to an existing ini so they stay discoverable. */
static int ini_int(const wchar_t *key, int fallback)
{
	wchar_t path[MAX_PATH], text[16];
	int value;

	log_path(path, L"mxs-shaders.ini");
	value = GetPrivateProfileIntW(L"mxs-shaders", key, -1, path);
	if (value >= 0)
		return value;
	swprintf_s(text, 16, L"%d", fallback);
	WritePrivateProfileStringW(L"mxs-shaders", key, text, path);
	return fallback;
}

/* What the player chose for this pack, else what the pack's settings.ini suggests. */
static int pack_int(const wchar_t *key, int fallback)
{
	wchar_t path[MAX_PATH], section[80], file[MAX_PATH];
	int value;

	if (pack_is_default())
		return ini_int(key, fallback);
	pack_section(section, 80);
	log_path(path, L"mxs-shaders.ini");
	value = GetPrivateProfileIntW(section, key, -1, path);
	if (value >= 0)
		return value;
	swprintf_s(file, MAX_PATH, L"shaderpacks\\%s\\settings.ini", g_cfg_pack);
	log_path(path, file);
	value = GetPrivateProfileIntW(L"settings", key, -1, path);
	return value >= 0 ? value : fallback;
}

/*
 Time of Day replaced Day to Night and Sun Height. Without a setting of its own yet, a
 night becomes midnight and a raised sun the morning hour that puts it that high.
*/
static int clock_from_old_settings(void)
{
	wchar_t path[MAX_PATH], section[80];
	int night, height, q;
	float noon = 90.0f - LATITUDE + DECLINATION;

	/* Read without writing, so the old keys aren't added to new ini files. */
	pack_section(section, 80);
	log_path(path, L"mxs-shaders.ini");
	night = GetPrivateProfileIntW(section, L"time_of_day", 0, path);
	height = GetPrivateProfileIntW(section, L"sun_height", 0, path);

	if (night >= 50)
		return 1;
	if (height <= 0)
		return CLOCK_TRACK;
	if (height >= noon)
		return 1 + 12 * 4;
	for (q = 12 * 4; q > 0; q--) {
		float h = (q * 0.25f - 12.0f) * 15.0f * RADIANS, lat = LATITUDE * RADIANS, dec = DECLINATION * RADIANS;

		if ((float)asin(sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(h)) < height * RADIANS)
			return 1 + q + 1;
	}
	return CLOCK_TRACK;
}

/* Everything hot reload can pick up without rebuilding GL objects. */
static void load_live_settings(void)
{
	g_cfg_clock = min(pack_int(L"clock", clock_from_old_settings()), CLOCK_STEPS);
	g_cfg_fog = pack_int(L"fog", 10);
	g_cfg_parallax = pack_int(L"parallax", 200);
	g_cfg_motion_blur = pack_int(L"motion_blur", 10);
	g_cfg_dof = pack_int(L"dof", 15);
	g_cfg_saturation = pack_int(L"saturation", 110);
	g_cfg_contrast = pack_int(L"contrast", 20);
	g_cfg_split_tone = pack_int(L"split_tone", 20);
	g_cfg_sharpen = pack_int(L"sharpen", 30);
	g_cfg_texture_sharpness = pack_int(L"texture_sharpness", 0);
	g_cfg_bloom = pack_int(L"bloom", 100);
	g_cfg_lens_flare = pack_int(L"lens_flare", 50);
	g_cfg_sun_turn = pack_int(L"sun_turn", 0);
	g_cfg_goggles = ini_int(L"goggles", 0);
	g_cfg_weather = min(max(ini_int(L"weather", WEATHER_CLEAR), WEATHER_CLEAR), WEATHER_SNOW);
	g_cfg_weather_amount = pack_int(L"weather_amount", 60);
	/* Cloudy is Cloud Cover now: as much cloud as Cloudy gave, and no rain. */
	g_cfg_cloud_cover = min(pack_int(L"cloud_cover", g_cfg_weather == WEATHER_CLOUDY ? 25 + g_cfg_weather_amount * 45 / 100 : 20), 100);
	if (g_cfg_weather == WEATHER_CLOUDY) {
		wchar_t path[MAX_PATH];

		g_cfg_weather = WEATHER_CLEAR;
		log_path(path, L"mxs-shaders.ini");
		WritePrivateProfileStringW(L"mxs-shaders", L"weather", L"0", path);
	}
	g_cfg_cloud_quality = min(max(ini_int(L"cloud_quality", CLOUDS_MEDIUM), CLOUDS_OFF), CLOUDS_HIGH);
	g_cfg_dynamic_sky = ini_int(L"dynamic_sky", 1) != 0;
	g_cfg_wet = pack_int(L"wet", 0);
	g_cfg_hide_hud = ini_int(L"hide_hud", 0);
	g_cfg_hide_cursor = ini_int(L"hide_cursor", 1);
	g_cfg_ui_key = ini_int(L"ui_key", VK_F8);
	/*
	 Fixed: a shorter reach saved little frame time and made shadows pop in close by,
	 Shadow Quality and the rest are what to lower. Old shadow_range lines are ignored.
	*/
	g_cfg_shadow_range = SHADOW_RANGE;
	preset_detect();
}

static void hot_reload(void)
{
	int reloaded = 0, failed = 0;
	GLuint p;

	load_live_settings();

	/* Relinking moves uniforms, and the last recording holds their old locations. */
	g_replay_block = max(g_replay_block, 1);
	for (p = 1; p < MAX_IDS; p++) {
		if (!g_prog_fs_id[p] || !my_glIsProgram(p))
			continue;
		if (reload_program(p))
			reloaded++;
		else
			failed++;
	}
	real_glUseProgram(g_cur_prog);
	post_reload();
	log_msg("hot reload at frame %u: %d programs reloaded, %d kept", g_frame, reloaded, failed);
}

HOOK(void, glActiveTexture, (GLenum unit))
{
	g_active_unit = unit;
	rec(REC_ACTIVETEXTURE, unit, 0, 0, 0, 0, 0, NULL, 0);
	real_glActiveTexture(unit);
}

HOOK(void, glBindVertexArray, (GLuint vao))
{
	rec(REC_BINDVAO, vao, 0, 0, 0, 0, 0, NULL, 0);
	g_cur_vao = vao;
	real_glBindVertexArray(vao);
}

HOOK(void, glBindBuffer, (GLenum target, GLuint buffer))
{
	rec(REC_BINDBUFFER, target, buffer, 0, 0, 0, 0, NULL, 0);
	if (target == GL_ARRAY_BUFFER)
		g_cur_vbo = buffer;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		g_cur_ibo = buffer;
	real_glBindBuffer(target, buffer);
}

HOOK(void, glBufferData, (GLenum target, ptrdiff_t size, const void *data, GLenum usage))
{
	if (target == GL_ELEMENT_ARRAY_BUFFER && g_cur_vao < MAX_IDS)
		g_mesh_bytes[g_cur_vao] = size;
	if (g_cur_vao < MAX_IDS)
		g_mesh_sphere_state[g_cur_vao] = SPHERE_UNKNOWN;
	real_glBufferData(target, size, data, usage);
}

HOOK(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))
{
	rec(REC_ATTRIBPOINTER, index, size, type, normalized, stride, (size_t)pointer, NULL, 0);
	real_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

HOOK(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays))
{
	GLsizei i;

	for (i = 0; i < n; i++) {
		int j;

		if (arrays[i] == g_terrain_vao) {
			g_terrain_count = 0;
			g_terrain_heights = 0;
		}
		if (arrays[i] < MAX_IDS) {
			g_mesh_bytes[arrays[i]] = 0;
			g_mesh_sphere_state[arrays[i]] = SPHERE_UNKNOWN;
		}
		for (j = 0; j < g_rigid_parts; j++)
			if (g_rigid[j].vao == arrays[i])
				g_rigid_parts = 0;
		for (j = 0; j < MAX_FULL_MESHES; j++)
			if (g_full[j].vao == arrays[i])
				memset(&g_full[j], 0, sizeof g_full[j]);
	}
	real_glDeleteVertexArrays(n, arrays);

	/* The recordings may still use them, so they can't be replayed for a moment. */
	g_replay_block = 2;
}

HOOK(void, glEnableVertexAttribArray, (GLuint index))
{
	rec(REC_ENABLEATTRIB, index, 0, 0, 0, 0, 0, NULL, 0);
	real_glEnableVertexAttribArray(index);
}

HOOK(void, glDisableVertexAttribArray, (GLuint index))
{
	rec(REC_DISABLEATTRIB, index, 0, 0, 0, 0, 0, NULL, 0);
	real_glDisableVertexAttribArray(index);
}

HOOK(void, glUniform1i, (GLint location, GLint value))
{
	location = uni_set(location, REC_UNIFORM1I, 1, 0, value, NULL, 0);
	rec(REC_UNIFORM1I, location, value, 0, 0, 0, 0, NULL, 0);
	if (g_cur_prog == g_model_prog && location >= 0 && location == g_loc_nbones[g_cur_prog])
		g_cur_nbones = value;
	real_glUniform1i(location, value);
}

HOOK(void, glUniform1f, (GLint location, float value))
{
	location = uni_set(location, REC_UNIFORM1F, 1, 0, 0, &value, sizeof value);
	rec(REC_UNIFORM1F, location, 0, 0, 0, 0, 0, &value, sizeof value);
	real_glUniform1f(location, value);
}

HOOK(void, glUniform2fv, (GLint location, GLsizei count, const float *value))
{
	location = uni_set(location, REC_UNIFORM2FV, count, 0, 0, value, count * 8);
	rec(REC_UNIFORM2FV, location, count, 0, 0, 0, 0, value, count * 8);
	real_glUniform2fv(location, count, value);
}

HOOK(void, glUniform4fv, (GLint location, GLsizei count, const float *value))
{
	location = uni_set(location, REC_UNIFORM4FV, count, 0, 0, value, count * 16);
	rec(REC_UNIFORM4FV, location, count, 0, 0, 0, 0, value, count * 16);
	real_glUniform4fv(location, count, value);
}

HOOK(void, glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const float *value))
{
	location = uni_set(location, REC_UNIFORMMAT3, count, transpose, 0, value, count * 36);
	rec(REC_UNIFORMMAT3, location, count, transpose, 0, 0, 0, value, count * 36);
	real_glUniformMatrix3fv(location, count, transpose, value);
}

HOOK(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const float *value))
{
	GLuint p = g_cur_prog;

	location = uni_set(location, REC_UNIFORMMAT4, count, transpose, 0, value, count * 64);
	rec(REC_UNIFORMMAT4, location, count, transpose, 0, 0, 0, value, count * 64);
	real_glUniformMatrix4fv(location, count, transpose, value);
	if (p >= MAX_IDS || location < 0 || !g_prog_role[p])
		return;
	if (location == g_loc_proj[p]) {
		memcpy(g_proj_value[p], value, sizeof g_proj_value[p]);
	} else if (location == g_loc_mv[p] && g_prog_role[p] == ROLE_TERRAIN &&
		   !g_shadow_frame && g_cur_fb == 0) {
		/* Terrain has no model transform, so its modelview is the camera. */
		shadow_frame_start(value);
	}
}

HOOK(void, glUniform3fv, (GLint location, GLsizei count, const float *value))
{
	GLuint p = g_cur_prog;
	GLint real = p < MAX_IDS && location >= 0 && location < g_uni_count[p] ? g_uni[p][location].real : -1;

	/* The game's own shading follows the moved sun. Its camera is last frame's until the terrain starts. */
	if (real >= 0 && g_prog_role[p] && real == g_loc_light[p] && count == 1) {
		memcpy(g_light_game, value, sizeof g_light_game);
		if (g_have_prev_cam)
			sun_effective(g_light_view, value, g_prev_inv_view);
		else
			memcpy(g_light_view, value, sizeof g_light_view);
		value = g_light_view;
		g_have_light = TRUE;
	}
	location = uni_set(location, REC_UNIFORM3FV, count, 0, 0, value, count * 12);
	rec(REC_UNIFORM3FV, location, count, 0, 0, 0, 0, value, count * 12);
	real_glUniform3fv(location, count, value);
}

static void shadow_pass_uniforms(GLuint prog, const float *projection, int pass)
{
	if (!prog || g_loc_proj[prog] < 0)
		return;
	my_glProgramUniformMatrix4fv(prog, g_loc_proj[prog], 1, 0, projection);
	if (g_loc_shadow_pass[prog] >= 0)
		my_glProgramUniform1i(prog, g_loc_shadow_pass[prog], pass);
}

#define REPLAY_STATE_ONLY -1

/* Sideways on the map only: a block any way along the sun's line can still shade what's on it. */
static BOOL terrain_block_on_map(int cascade, float x, float z, float scale)
{
	const float *m = g_light_matrix[cascade];
	float range = cascade ? SHADOW_NEAR_RANGE : (float)g_cfg_shadow_range;
	float cx = x + scale * 0.5f, cz = z + scale * 0.5f;
	float reach = 1.0f + (scale * 0.71f + TERRAIN_HEIGHT_MARGIN) / range;

	return fabs(m[0] * cx + m[4] * g_camera_y + m[8] * cz + m[12]) <= reach &&
	       fabs(m[1] * cx + m[5] * g_camera_y + m[9] * cz + m[13]) <= reach;
}

/* Sideways only as well. The small extra keeps a caster at the edge for the shadow's soft samples. */
static BOOL sphere_on_map(int cascade, const float *mv, const mesh_sphere *s)
{
	const float *m = g_shadow_from_view[cascade];
	float x = mv[0] * s->x + mv[4] * s->y + mv[8] * s->z + mv[12];
	float y = mv[1] * s->x + mv[5] * s->y + mv[9] * s->z + mv[13];
	float z = mv[2] * s->x + mv[6] * s->y + mv[10] * s->z + mv[14];
	float grow = (float)sqrt(max(max(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2],
					 mv[4] * mv[4] + mv[5] * mv[5] + mv[6] * mv[6]),
				     mv[8] * mv[8] + mv[9] * mv[9] + mv[10] * mv[10]));
	float r = s->radius * grow;

	return fabs(m[0] * x + m[4] * y + m[8] * z + m[12]) <= 1.02f + r * (float)sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]) &&
	       fabs(m[1] * x + m[5] * y + m[9] * z + m[13]) <= 1.02f + r * (float)sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
}

static void replay_stream(const unsigned char *stream, size_t used, GLuint prog, GLuint vao, int cascade)
{
	BOOL state_only = cascade == REPLAY_STATE_ONLY;
	uni_entry *offset = prog_uniform(g_terrain_prog, "u_meshoffset"), *scale = prog_uniform(g_terrain_prog, "u_meshscale");
	GLint offset_real = offset ? offset->real : -1, scale_real = scale ? scale->real : -1;
	const float *block_offset = NULL, *model_view = NULL;
	float block_scale = 0.0f;
	size_t at = 0;

	while (at < used) {
		const rec_cmd *c = (const rec_cmd *)(stream + at);
		const float *f = (const float *)(c + 1);

		at += sizeof *c + ((c->size + 7) & ~7);

		if ((c->type == REC_DRAWELEMENTS || c->type == REC_DRAWRANGE) && state_only)
			continue;

		/* Scenery is drawn for the view too, and little of it is near the near map. */
		if (prog == g_model_prog) {
			if (c->type == REC_UNIFORMMAT4 && c->a == g_loc_mv[prog] && c->b == 1 && !c->c)
				model_view = f;
			else if (c->type == REC_DRAWELEMENTS && c->e == DRAW_STATIC && c->size && model_view &&
				 !sphere_on_map(cascade, model_view, (const mesh_sphere *)f))
				continue;
		}

		/* The game draws terrain for its view, much of it off the shadow map's sides. */
		if (prog == g_terrain_prog) {
			if (c->type == REC_UNIFORM2FV && c->a == offset_real)
				block_offset = f;
			else if (c->type == REC_UNIFORM1F && c->a == scale_real)
				block_scale = f[0];
			else if ((c->type == REC_DRAWELEMENTS || c->type == REC_DRAWRANGE) && block_offset && block_scale > 0.0f &&
				 !terrain_block_on_map(cascade, block_offset[0], block_offset[1], block_scale))
				continue;
		}

		switch (c->type) {
		case REC_USEPROGRAM: prog = c->a; real_glUseProgram(prog); break;
		case REC_ACTIVETEXTURE: real_glActiveTexture(c->a); break;
		case REC_BINDTEXTURE: real_glBindTexture(c->a, c->b); break;
		case REC_BINDVAO: vao = c->a; real_glBindVertexArray(vao); break;
		case REC_BINDBUFFER: real_glBindBuffer(c->a, c->b); break;
		case REC_ATTRIBPOINTER:
			real_glVertexAttribPointer(c->a, c->b, c->c, (GLboolean)c->d, c->e, (const void *)c->ptr);
			break;
		case REC_ENABLEATTRIB: real_glEnableVertexAttribArray(c->a); break;
		case REC_DISABLEATTRIB: real_glDisableVertexAttribArray(c->a); break;
		case REC_UNIFORM1I: real_glUniform1i(c->a, c->b); break;
		case REC_UNIFORM1F: real_glUniform1f(c->a, f[0]); break;
		case REC_UNIFORM2FV: real_glUniform2fv(c->a, c->b, f); break;
		case REC_UNIFORM3FV: real_glUniform3fv(c->a, c->b, f); break;
		case REC_UNIFORM4FV: real_glUniform4fv(c->a, c->b, f); break;
		case REC_UNIFORMMAT3: real_glUniformMatrix3fv(c->a, c->b, (GLboolean)c->c, f); break;
		case REC_UNIFORMMAT4:
			/* The sun matrix stands in for the game's projection during this pass. */
			if (state_only || prog >= MAX_IDS || !g_prog_role[prog] || c->a != g_loc_proj[prog])
				real_glUniformMatrix4fv(c->a, c->b, (GLboolean)c->c, f);
			break;
		case REC_DRAWELEMENTS:
			if (c->d && g_full[c->d - 1].count > c->b) {
				const full_mesh *m = &g_full[c->d - 1];

				real_glBindVertexArray(m->vao);
				real_glDrawElements(c->a, m->count, m->type, m->indices);
				real_glBindVertexArray(vao);
			} else if (c->e) {
				real_glDrawElements(c->a, c->b, c->c, (const void *)c->ptr);
			}
			break;
		case REC_DRAWRANGE:
			if (c->e)
				real_glDrawRangeElements(c->a, c->d, *(const GLuint *)f, c->b, c->c, (const void *)c->ptr);
			break;
		}
	}
}

static void draw_rigid(int cascade)
{
	uni_entry *mv = model_uniform("u_modelview"), *nb = model_uniform("u_nbones"), *tex = model_uniform("u_tex");
	uni_entry *ambient = model_uniform("u_ambient");
	int unit = tex ? tex->value : 0;
	int i;

	if (!mv || !mv->data || !nb || mv->real < 0)
		return;

	real_glUseProgram(g_model_prog);
	if (nb->real >= 0)
		real_glUniform1i(nb->real, 0);
	my_glProgramUniformMatrix4fv(g_model_prog, g_loc_proj[g_model_prog], 1, 0, g_shadow_from_view[cascade]);
	real_glActiveTexture(GL_TEXTURE0 + unit);
	real_glBindTexture(GL_TEXTURE_2D, g_white_tex);

	for (i = 0; i < g_rigid_parts; i++) {
		const rigid_part *p = &g_rigid[i];
		int bone = g_rigid_bone[p->group];
		float placed[16];

		/* The front of the bike is still drawn, it would only double up. */
		if (g_vao_frame[p->vao] && g_frame - g_vao_frame[p->vao] <= 2)
			continue;
		mat_mul(placed, g_replay_pose[bone], p->relative[bone]);
		real_glUniformMatrix4fv(mv->real, 1, 0, placed);
		/* By now it holds the frame's last material, whose alpha could discard the part. */
		if (ambient && ambient->real >= 0)
			real_glUniform4fv(ambient->real, 1, p->ambient);
		real_glBindVertexArray(p->vao);
		real_glDrawElements(GL_TRIANGLES, p->count, p->type, p->indices);
	}

	if (nb->real >= 0)
		real_glUniform1i(nb->real, nb->value);
	real_glUniformMatrix4fv(mv->real, 1, 0, mv->data);
	if (ambient && ambient->real >= 0 && ambient->data)
		real_glUniform4fv(ambient->real, 1, ambient->data);
	real_glBindTexture(GL_TEXTURE_2D, unit >= 0 && unit < 32 ? g_unit_tex[unit] : 0);
}

static void draw_terrain_fill(int cascade)
{
	uni_entry *offset = prog_uniform(g_terrain_prog, "u_meshoffset"), *scale = prog_uniform(g_terrain_prog, "u_meshscale");
	uni_entry *heights = prog_uniform(g_terrain_prog, "u_heights");
	int i;

	if (!g_terrain_fill_count || !offset || !scale || !heights || offset->real < 0 || scale->real < 0)
		return;

	real_glUseProgram(g_terrain_prog);
	real_glBindVertexArray(g_terrain_vao);
	real_glActiveTexture(GL_TEXTURE0 + heights->value);
	real_glBindTexture(GL_TEXTURE_2D, g_terrain_heights);

	for (i = 0; i < g_terrain_fill_count; i++) {
		const terrain_block *b = &g_terrain_fill[i];
		float block_offset[2];

		if (!terrain_block_on_map(cascade, b->x, b->z, b->scale))
			continue;
		block_offset[0] = b->x;
		block_offset[1] = b->z;
		real_glUniform2fv(offset->real, 1, block_offset);
		real_glUniform1f(scale->real, b->scale);
		real_glDrawElements(GL_TRIANGLES, g_terrain_count, g_terrain_type, g_terrain_indices);
	}

	if (offset->data)
		real_glUniform2fv(offset->real, 1, offset->data);
	if (scale->data)
		real_glUniform1f(scale->real, *(const float *)scale->data);
	real_glBindTexture(GL_TEXTURE_2D, g_unit_tex[heights->value]);
}

static void replay_cascade(int cascade, const unsigned char *stream, size_t used, GLuint prog, GLuint vao)
{
	int size = cascade ? SHADOW_NEAR_SIZE : g_cfg_shadow_size;

	real_glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[cascade]);
	real_glViewport(0, 0, size, size);
	real_glClear(GL_DEPTH_BUFFER_BIT);

	shadow_pass_uniforms(g_terrain_prog, g_shadow_from_view[cascade], 1);
	shadow_pass_uniforms(g_model_prog, g_shadow_from_view[cascade], 1);

	real_glUseProgram(prog);
	real_glBindVertexArray(vao);
	replay_stream(stream, used, prog, vao, cascade);
	draw_terrain_fill(cascade);
	if (g_rigid_needed)
		draw_rigid(cascade);
}

/*
 Draws the map from one recording, then puts the game's state back: the same stream's
 state changes are replayed without its draws, so GL ends where the game left it.
*/
static void shadow_replay(const unsigned char *stream, size_t used, GLuint prog, GLuint vao)
{
	GLboolean depth = my_glIsEnabled(GL_DEPTH_TEST), cull = my_glIsEnabled(GL_CULL_FACE);
	GLboolean clamp = my_glIsEnabled(GL_DEPTH_CLAMP);
	int c;

	real_glEnable(GL_DEPTH_TEST);
	real_glDisable(GL_CULL_FACE);
	/* Casters beyond the map's sunward end still shadow everything, so they're flattened onto it. */
	real_glEnable(GL_DEPTH_CLAMP);
	real_glDepthMask(1);
	for (c = 0; c < SHADOW_CASCADES; c++)
		replay_cascade(c, stream, used, prog, vao);

	if (g_terrain_prog)
		shadow_pass_uniforms(g_terrain_prog, g_proj_value[g_terrain_prog], 0);
	if (g_model_prog)
		shadow_pass_uniforms(g_model_prog, g_proj_value[g_model_prog], 0);

	if (!depth)
		real_glDisable(GL_DEPTH_TEST);
	if (cull)
		real_glEnable(GL_CULL_FACE);
	if (!clamp)
		real_glDisable(GL_DEPTH_CLAMP);
	real_glDepthMask(g_depth_mask);
	real_glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
	bind_current_fb();

	real_glUseProgram(prog);
	real_glBindVertexArray(vao);
	replay_stream(stream, used, prog, vao, REPLAY_STATE_ONLY);
	real_glUseProgram(g_cur_prog);
	real_glBindVertexArray(g_cur_vao);
	real_glActiveTexture(g_active_unit);
}

/*
 No post pass to shade the finished frame, so the map goes into the game's shaders: last
 frame's recording, drawn and sampled through one matrix, so it sits a frame behind the
 bike instead of sliding against it.
*/
static void shadow_forward(const float *inv_view)
{
	static BOOL logged;
	int c;

	if (!g_shadow_ready || !g_have_prev_cam || g_replay_block)
		return;
	for (c = 0; c < SHADOW_CASCADES; c++)
		mat_mul(g_shadow_from_view[c], g_light_matrix[c], g_prev_inv_view);
	shadow_replay(g_rec_prev, g_rec_prev_used, g_rec_prev_prog, g_rec_prev_vao);
	memcpy(g_shadow_receiver, g_shadow_from_view[0], sizeof g_shadow_receiver);
	memcpy(g_shadow_receiver_near, g_shadow_from_view[1], sizeof g_shadow_receiver_near);
	g_shadow_receiver_valid = TRUE;
	shadow_set_receiver(g_terrain_prog, g_shadow_receiver, g_shadow_receiver_near);
	shadow_set_receiver(g_model_prog, g_shadow_receiver, g_shadow_receiver_near);
	shadow_set_receiver(g_sprite_prog, g_shadow_receiver, g_shadow_receiver_near);
	for (c = 0; c < SHADOW_CASCADES; c++) {
		real_glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT + c);
		real_glBindTexture(GL_TEXTURE_2D, g_shadow_tex[c]);
	}
	real_glActiveTexture(g_active_unit);
	if (!logged) {
		logged = TRUE;
		log_msg("shadows: no post pass, so the game's shaders get last frame's map");
	}
}

/*
 The frame's shadow map, drawn from the frame's own recording. It runs at scene end,
 after every caster has been drawn, so the map and the picture are the same frame:
 there is no motion to predict and nothing left to stutter. The post pass reads it.
*/
static void shadow_frame_end(void)
{
	static BOOL logged;
	int c;

	if (!g_shadow_enabled || !g_have_light || !g_cfg_enabled || !g_shadow_frame || !post_ready())
		return;
	if (!g_rec_used || g_rec_overflow)
		return;
	for (c = 0; c < SHADOW_CASCADES; c++) {
		float m[16];

		mat_mul(m, g_light_matrix[c], g_prev_inv_view);
		memcpy(c ? g_shadow_receiver_near : g_shadow_receiver, m, sizeof m);
	}
	shadow_replay(g_rec, g_rec_used, g_rec_start_prog, g_rec_start_vao);
	g_shadow_receiver_valid = TRUE;
	/* Where the post pass reads them from. */
	for (c = 0; c < SHADOW_CASCADES; c++) {
		real_glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT + c);
		real_glBindTexture(GL_TEXTURE_2D, g_shadow_tex[c]);
	}
	real_glActiveTexture(g_active_unit);
	if (!logged) {
		logged = TRUE;
		log_msg("shadows: the map is drawn at the end of the frame, from this frame's own draws");
	}
}

HOOK(void, glUseProgram, (GLuint program))
{
	g_programs++;
	g_cur_prog = program;
	alpha_to_coverage_update();
	rec(REC_USEPROGRAM, program, 0, 0, 0, 0, 0, NULL, 0);
	cap("useprogram %u", program);
	real_glUseProgram(program);
}

HOOK(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))
{
	GLint loc = real_glGetUniformLocation(program, name);
	uni_entry *u;
	int i;

	log_msg("uniform program=%u %s = %d", program, name, loc);
	if (program >= MAX_IDS || strlen(name) >= sizeof u->name)
		return -1;
	if (!g_uni[program])
		g_uni[program] = calloc(MAX_UNIFORMS, sizeof **g_uni);

	for (i = 0; i < g_uni_count[program]; i++)
		if (!strcmp(g_uni[program][i].name, name))
			return i;
	if (i == MAX_UNIFORMS) {
		log_msg("too many uniforms in program %u, ignoring %s", program, name);
		return -1;
	}

	/* A number even if GL has no such uniform, a reloaded shader might use it. */
	u = &g_uni[program][g_uni_count[program]++];
	strcpy_s(u->name, sizeof u->name, name);
	u->real = loc;
	return i;
}

HOOK(void, glBindFramebuffer, (GLenum target, GLuint fb))
{
	g_binds++;
	if (g_fbo && fb == g_fbo)
		fb = 0;
	g_cur_fb = fb;
	cap("bindframebuffer %x %u", target, fb);
	sprite_set_mode(g_sprite_phase);
	alpha_to_coverage_update();
	real_glBindFramebuffer(target, fb == 0 && g_fbo ? g_fbo : fb);
}

HOOK(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level))
{
	static unsigned logged;

	/* The Linux game attaches a texture about once a frame, which would grow the log without end. */
	if (logged < 20)
		log_msg("glFramebufferTexture2D fb=%u attachment=%x textarget=%x tex=%u level=%d", g_cur_fb, attachment, textarget, tex, level);
	else if (logged == 20)
		log_msg("glFramebufferTexture2D: further calls aren't logged");
	if (logged <= 20)
		logged++;
	real_glFramebufferTexture2D(target, attachment, textarget, tex, level);
}

HOOK(void, glDrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices))
{
	BOOL sky = g_sprite_mode == SPRITE_SKY && g_cur_fb == 0 && g_cur_prog == g_sprite_prog && count == 6;

	count_draw();
	cap("drawrangeelements mode=%x start=%u end=%u count=%d type=%x fb=%u prog=%u", mode, start, end, count, type, g_cur_fb, g_cur_prog);
	if (sky)
		sky_face_drawn(start, end);
	rec(REC_DRAWRANGE, mode, count, type, start, shadow_draw_kind(), (size_t)indices, &end, sizeof end);
	if (!hud_draw_hidden())
		real_glDrawRangeElements(mode, start, end, count, type, indices);
	/* Only the faces themselves are drawn over. */
	if (sky)
		sky_face_send(NULL);
}

typedef struct {
	const char *name;
	PROC hook;
	PROC *real;
} dyn_hook;

#define DYN(n) { #n, (PROC)hook_##n, (PROC *)&real_##n }

static const dyn_hook g_dyn[] = {
#ifdef _WIN32
	DYN(wglCreateContextAttribsARB),
#endif
	DYN(glCreateShader),
	DYN(glShaderSource),
	DYN(glCompileShader),
	DYN(glAttachShader),
	DYN(glLinkProgram),
	DYN(glUseProgram),
	DYN(glGetUniformLocation),
	DYN(glBindFramebuffer),
	DYN(glFramebufferTexture2D),
	DYN(glDrawRangeElements),
	DYN(glActiveTexture),
	DYN(glUniformMatrix4fv),
	DYN(glUniform3fv),
	DYN(glBindVertexArray),
	DYN(glDeleteVertexArrays),
	DYN(glBindBuffer),
	DYN(glBufferData),
	DYN(glVertexAttribPointer),
	DYN(glEnableVertexAttribArray),
	DYN(glDisableVertexAttribArray),
	DYN(glUniform1i),
	DYN(glUniform1f),
	DYN(glUniform2fv),
	DYN(glUniform4fv),
	DYN(glUniformMatrix3fv),
};

#ifdef _WIN32
HOOK(PROC, wglGetProcAddress, (LPCSTR name))
{
	PROC p = real_wglGetProcAddress(name);
	int i;

	log_msg("wglGetProcAddress %s -> %p", name, p);
	if (!p)
		return NULL;

	/* Pointers can differ per context, so always keep the latest one. */
	for (i = 0; i < sizeof g_dyn / sizeof g_dyn[0]; i++) {
		if (!strcmp(name, g_dyn[i].name)) {
			*g_dyn[i].real = p;
			return g_dyn[i].hook;
		}
	}
	return p;
}
#endif

/* --- init --- */

#ifndef _WIN32
/* What wglGetProcAddress is to the rest of this file. glXGetProcAddress also finds GL 1.1 functions. */
static PROC linux_get_proc(LPCSTR name)
{
	return GetProcAddress(g_real_gl, name);
}
#endif

#define STATIC(n) real_##n = (n##_t)GetProcAddress(g_real_gl, #n)
#define STATIC_MY(n) my_##n = (n##_t)GetProcAddress(g_real_gl, #n)

static void load_config(void)
{
	wchar_t path[MAX_PATH];
	FILE *f;

	log_path(path, L"mxs-shaders.ini");
	f = _wfopen(path, L"r");
	if (f) {
		fclose(f);
	} else if ((f = _wfopen(path, L"w")) != NULL) {
		fputs("[mxs-shaders]\n"
		      "; 0 keeps the game's own GL version\n"
		      "gl_major=4\n"
		      "gl_minor=6\n"
		      "; 0 or 1 disables anisotropic filtering\n"
		      "anisotropy=16\n"
		      "; 0 or 1 disables MSAA and the redirected framebuffer\n"
		      "msaa=8\n", f);
		fclose(f);
	}

	g_cfg_gl_major = GetPrivateProfileIntW(L"mxs-shaders", L"gl_major", 4, path);
	g_cfg_gl_minor = GetPrivateProfileIntW(L"mxs-shaders", L"gl_minor", 6, path);
	g_cfg_aniso = GetPrivateProfileIntW(L"mxs-shaders", L"anisotropy", 16, path);
	g_cfg_msaa = GetPrivateProfileIntW(L"mxs-shaders", L"msaa", 8, path);
	g_cfg_enabled = ini_int(L"enabled", 1);
	g_cfg_debug = ini_int(L"debug", 0);
	g_cfg_gl_debug = GetPrivateProfileIntW(L"mxs-shaders", L"gl_debug", g_cfg_debug, path);
	g_cfg_sky_mask_view = GetPrivateProfileIntW(L"mxs-shaders", L"sky_mask_view", 0, path);
	g_cfg_shadows = GetPrivateProfileIntW(L"mxs-shaders", L"shadows", 1, path);
	g_cfg_shadow_size = ini_int(L"shadow_size", 8192);
	g_cfg_post = GetPrivateProfileIntW(L"mxs-shaders", L"post", 1, path);
	g_cfg_borderless = GetPrivateProfileIntW(L"mxs-shaders", L"borderless", -1, path);

	if (g_cfg_borderless < 0) {
		g_cfg_borderless = 1;
		WritePrivateProfileStringW(L"mxs-shaders", L"borderless", L"1", path);
	}
	GetPrivateProfileStringW(L"mxs-shaders", L"pack", L"?", g_cfg_pack, 64, path);
	if (!wcscmp(g_cfg_pack, L"?")) {
		wcscpy_s(g_cfg_pack, 64, L"default");
		WritePrivateProfileStringW(L"mxs-shaders", L"pack", g_cfg_pack, path);
	}
	pack_init(g_cfg_pack);
	load_live_settings();

	log_msg("config: gl %d.%d, anisotropy %d, msaa %d, gl_debug %d", g_cfg_gl_major, g_cfg_gl_minor, g_cfg_aniso, g_cfg_msaa, g_cfg_gl_debug);
}

void hooks_init(void)
{
	load_config();
	STATIC_MY(glGetIntegerv);
	STATIC_MY(glIsTexture);
	STATIC_MY(glGetFloatv);
	STATIC_MY(glIsEnabled);
	STATIC_MY(glGetError);
	STATIC_MY(glGenTextures);
	STATIC_MY(glDrawBuffer);
	STATIC_MY(glReadBuffer);
	STATIC(glTexParameteri);
	STATIC(glTexParameterf);
	STATIC(glTexParameterfv);
	STATIC(glClear);
	STATIC(glDrawArrays);
	STATIC(glDrawElements);
	STATIC(glViewport);
	STATIC(glEnable);
	STATIC(glDisable);
	STATIC(glBlendFunc);
	STATIC(glDepthMask);
	STATIC(glBindTexture);
	STATIC(glTexImage2D);
	STATIC(glReadPixels);
	STATIC(glFinish);
	STATIC(glGetTexImage);
#ifdef _WIN32
	STATIC(wglCreateContext);
	STATIC(wglDeleteContext);
	STATIC(wglMakeCurrent);
	STATIC(wglSwapBuffers);
	STATIC(wglSwapLayerBuffers);
	STATIC(wglGetProcAddress);
#else
	real_wglGetProcAddress = linux_get_proc;
#endif
}

#ifndef _WIN32
/*
 --- Linux: what the wgl hooks above do, called by preload.c's GLX and EGL hooks ---

 The game opens its display one of two ways: a legacy glXCreateContext context, or, when the
 X server has no GLX, EGL. Either way it makes one context and never asks for a version, so
 gl_major and gl_minor have nothing to change. The main context is the first one made current
 on a window, which preload.c also uses to pick the swaps that end a frame; on the EGL path
 the window is the one the EGL surface was made on.
*/
static void context_made_current(HDC dc, HGLRC rc, const char *call)
{
	typedef const char *(WINAPI *glGetString_t)(GLenum);
	glGetString_t gs = (glGetString_t)GetProcAddress(g_real_gl, "glGetString");

	log_msg("%s %p on window 0x%lx", call, rc, dc);
	if (!g_main_rc) {
		g_main_rc = rc;
		if (g_cfg_gl_major > 0)
			log_msg("legacy GL context: the driver picks its version, gl_major and gl_minor aren't used");
	}
	if (gs) {
		log_msg("  GL_VENDOR   %s", gs(GL_VENDOR));
		log_msg("  GL_RENDERER %s", gs(GL_RENDERER));
		log_msg("  GL_VERSION  %s", gs(GL_VERSION));
		log_msg("  GLSL        %s", gs(GL_SHADING_LANGUAGE_VERSION));
	}
	if (rc == g_main_rc && !real_glBindFramebuffer)
		context_ready(dc);
}

static void context_destroyed(HGLRC rc, const char *call)
{
	log_msg("%s %p", call, rc);
	if (rc == g_main_rc) {
		g_main_rc = NULL;
		g_fbo = 0;
		g_aniso = 1.0f;
		sky_context_lost();
		post_context_lost();
		ui_context_lost();
	}
}

static void frame_swapped(HDC dc, const char *call)
{
	if (!g_saw_swap)
		log_msg("%s reached the library", call);
	g_saw_swap = TRUE;
	present(dc);
	frame_boundary(call);
}

/* What to hand the game for a get-proc-address name, like the wglGetProcAddress hook. */
static void *proc_wrapped(const char *name, void *p, const char *call)
{
	size_t i;

	log_msg("%s %s -> %p", call, name, p);
	if (!p)
		return NULL;
	for (i = 0; i < sizeof g_dyn / sizeof g_dyn[0]; i++) {
		if (!strcmp(name, g_dyn[i].name)) {
			*g_dyn[i].real = (PROC)p;
			return (void *)g_dyn[i].hook;
		}
	}
	return p;
}

void hooks_glx_context_created(HGLRC rc)
{
	log_msg("glXCreateContext -> %p", rc);
}

void hooks_egl_context_created(HGLRC rc)
{
	log_msg("eglCreateContext -> %p", rc);
}

void hooks_glx_context_destroyed(HGLRC rc)
{
	context_destroyed(rc, "glXDestroyContext");
}

void hooks_egl_context_destroyed(HGLRC rc)
{
	context_destroyed(rc, "eglDestroyContext");
}

void hooks_glx_made_current(HDC dc, HGLRC rc)
{
	context_made_current(dc, rc, "glXMakeCurrent");
}

void hooks_egl_made_current(HDC dc, HGLRC rc)
{
	context_made_current(dc, rc, "eglMakeCurrent");
}

void hooks_glx_swap(HDC dc)
{
	frame_swapped(dc, "glXSwapBuffers");
}

void hooks_egl_swap(HDC dc)
{
	frame_swapped(dc, "eglSwapBuffers");
}

void *hooks_glx_proc(const char *name, void *p)
{
	return proc_wrapped(name, p, "glXGetProcAddress");
}

void *hooks_egl_proc(const char *name, void *p)
{
	return proc_wrapped(name, p, "eglGetProcAddress");
}
#endif
