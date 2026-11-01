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


#define SHADOW_DEPTH_RANGE 400.0f

enum { ROLE_NONE, ROLE_TERRAIN, ROLE_MODEL };

/* Cascade 0 covers the distance, cascade 1 is a sharper map around the rider. */
#define SHADOW_CASCADES 2
#define SHADOW_NEAR_SIZE 4096	/* thin parts like levers blink at coarser sizes */
#define SHADOW_NEAR_RANGE 20.0f
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
static BOOL g_particle_phase;
static GLint g_loc_sprite_mode[MAX_IDS], g_loc_sun_view[MAX_IDS], g_loc_screen[MAX_IDS], g_loc_hdr[MAX_IDS], g_loc_parallax[MAX_IDS];
static GLuint g_reflection_prog;
static int g_hdr_on;

static int g_cfg_shadows, g_cfg_shadow_size, g_cfg_shadow_range;
static BOOL g_shadow_enabled;
static GLuint g_shadow_fbo[2][SHADOW_CASCADES], g_shadow_tex[2][SHADOW_CASCADES];
static int g_shadow_write;
static BOOL g_shadow_frame;	/* this frame's map has been started */
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

static float g_light_view[3];
static BOOL g_have_light;
static float g_light_matrix[SHADOW_CASCADES][16];	/* world to shadow map */
static float g_shadow_from_view[SHADOW_CASCADES][16];
static float g_shadow_from_view_dyn[SHADOW_CASCADES][16];	/* moved on by the camera's step */
static float g_view_up[3], g_camera_y, g_ground_y;
static float g_view_step[16];	/* this frame's view space to last frame's */
static float g_frame_seconds;
static float g_shadow_receiver[16];	/* this frame's view space to the wide map */
static BOOL g_shadow_receiver_valid;
static float g_prev_inv_view[16];
static BOOL g_have_prev_cam;
static GLint g_vp[4];
static GLboolean g_depth_mask = 1;
static GLenum g_active_unit = GL_TEXTURE0;

typedef struct {
	int w, h, internal, min_filter;
	BOOL aniso;
} tex_info;

static tex_info g_tex[MAX_IDS];
static GLuint g_bound_tex;

/* --- settings and redirected framebuffer --- */

static int g_cfg_gl_major, g_cfg_gl_minor, g_cfg_msaa, g_cfg_aniso, g_cfg_gl_debug, g_cfg_debug;
static float g_aniso, g_aniso_setting;
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
static int g_cfg_hide_hud, g_cfg_hide_cursor, g_cfg_time_of_day, g_cfg_ui_key, g_cfg_enabled;

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
static void program_refresh(GLuint program);
static void sprite_set_mode(int mode);
static void hdr_set(int on);

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

	g_frame++;
	g_draws = g_clears = g_binds = g_programs = g_dynamic_draws = 0;
	g_shadow_ready = g_shadow_frame && !g_rec_overflow;
	g_shadow_frame = FALSE;
	g_post_done = g_particle_phase = g_hud_phase = FALSE;
	g_hud_boxes_last = g_hud_boxes;
	g_hud_boxes = 0;
	cursor_update();

	/*
	 A frame after a 3D frame opens with the sky, which already belongs to the
	 linear scene. Menus don't, so a frame that wasn't 3D turns it off again.
	*/
	sprite_set_mode(g_shadow_ready ? SPRITE_SKY : SPRITE_NORMAL);
	if (!g_shadow_ready)
		hdr_set(0);

	/* This frame's recording is what the next frame's shadows are drawn from. */
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

	if (g_reload_requested)
		hot_reload();
	g_reload_requested = FALSE;

	ui_key = focused && (GetAsyncKeyState(g_cfg_ui_key) & 0x8000) != 0;
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
static void shadow_replay(void);

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

static void shadow_create(void)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const unsigned char white_pixel[4] = { 255, 255, 255, 255 };
	GLenum status;
	int i, c;

	if (!g_cfg_shadows || !my_glProgramUniformMatrix4fv || !my_glProgramUniform1i)
		return;

	g_shadow_enabled = TRUE;
	my_glGenFramebuffers(2 * SHADOW_CASCADES, g_shadow_fbo[0]);
	my_glGenTextures(2 * SHADOW_CASCADES, g_shadow_tex[0]);
	for (i = 0; i < 2 * SHADOW_CASCADES; i++) {
		int size = i % SHADOW_CASCADES ? SHADOW_NEAR_SIZE : g_cfg_shadow_size;

		c = i / SHADOW_CASCADES;
		real_glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT + i % SHADOW_CASCADES);
		real_glBindTexture(GL_TEXTURE_2D, g_shadow_tex[c][i % SHADOW_CASCADES]);
		real_glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, size, size,
				  0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		real_glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, white);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

		real_glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[c][i % SHADOW_CASCADES]);
		real_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
					    g_shadow_tex[c][i % SHADOW_CASCADES], 0);
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

#define FULL_NIGHT 100	/* shadows have faded out here, so the maps aren't drawn */

/* Sent every frame, the setting changes live and a shader reload forgets it. */
static void time_of_day_apply(void)
{
	GLuint prog[4];
	int i;

	prog[0] = g_terrain_prog;
	prog[1] = g_model_prog;
	prog[2] = g_reflection_prog;
	prog[3] = g_sprite_prog;
	for (i = 0; i < 4; i++) {
		if (!prog[i])
			continue;
		if (g_loc_night[prog[i]] >= 0)
			my_glProgramUniform1f(prog[i], g_loc_night[prog[i]], g_cfg_time_of_day / 100.0f);
		if (g_cfg_time_of_day >= FULL_NIGHT && g_loc_shadow_on[prog[i]] >= 0)
			my_glProgramUniform1i(prog[i], g_loc_shadow_on[prog[i]], 0);
	}
}

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
	float inv_view[16], shifted[16], receiver[SHADOW_CASCADES][16];
	float motion[3] = { 0.0f, 0.0f, 0.0f };
	int c;

	/* Shadow frames carry every other effect, so without one the game draws as it would alone. */
	if (!g_shadow_enabled || !g_have_light || !g_cfg_enabled)
		return;
	hdr_set(g_cfg_post && g_fbo && post_ready());
	sprite_set_mode(SPRITE_NORMAL);
	time_of_day_apply();
	mat_inv_rigid(inv_view, view);

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
	 The maps are drawn from last frame's recording, now that this frame's camera
	 is known. The chase camera moves with the rider, so its real movement says
	 where they are, including frames where physics ticked twice.
	*/
	if (g_shadow_ready && g_have_prev_cam && !g_replay_block && g_cfg_time_of_day < FULL_NIGHT) {
		for (c = 0; c < 3; c++)
			motion[c] = inv_view[12 + c] - g_prev_inv_view[12 + c];
		if (v_dot(motion, motion) > SHADOW_MAX_MOTION * SHADOW_MAX_MOTION)
			motion[0] = motion[1] = motion[2] = 0.0f;

		memcpy(shifted, g_prev_inv_view, sizeof shifted);
		for (c = 0; c < 3; c++)
			shifted[12 + c] += motion[c];
		for (c = 0; c < SHADOW_CASCADES; c++) {
			mat_mul(g_shadow_from_view[c], g_light_matrix[c], g_prev_inv_view);
			mat_mul(g_shadow_from_view_dyn[c], g_light_matrix[c], shifted);
		}

		g_shadow_write ^= 1;
		shadow_replay();

		for (c = 0; c < SHADOW_CASCADES; c++)
			mat_mul(receiver[c], g_light_matrix[c], inv_view);
		memcpy(g_shadow_receiver, receiver[0], sizeof g_shadow_receiver);
		g_shadow_receiver_valid = TRUE;
		shadow_set_receiver(g_terrain_prog, receiver[0], receiver[1]);
		shadow_set_receiver(g_model_prog, receiver[0], receiver[1]);
		shadow_set_receiver(g_sprite_prog, receiver[0], receiver[1]);
		for (c = 0; c < SHADOW_CASCADES; c++) {
			real_glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT + c);
			real_glBindTexture(GL_TEXTURE_2D, g_shadow_tex[g_shadow_write][c]);
		}
		real_glActiveTexture(g_active_unit);
	}

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
static uni_entry *model_uniform(const char *name)
{
	size_t n = strlen(name);
	int i;

	for (i = 0; g_model_prog && i < g_uni_count[g_model_prog]; i++) {
		uni_entry *u = &g_uni[g_model_prog][i];

		if (!strncmp(u->name, name, n) && (!u->name[n] || u->name[n] == '['))
			return u;
	}
	return NULL;
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
	post_state s;

	g_post_done = TRUE;
	if (!g_cfg_post || !g_fbo || !g_terrain_prog)
		return;
	s.scene_fbo = g_fbo;
	s.w = g_fbo_w;
	s.h = g_fbo_h;
	s.projection = g_proj_value[g_terrain_prog];
	s.sun_view = g_light_view;
	s.view_up = g_view_up;
	s.fog_color = fog && fog->data ? fog->data : default_fog;
	s.shadow_matrix = g_shadow_receiver_valid ? g_shadow_receiver : NULL;
	s.camera_height = g_camera_y - g_ground_y;
	s.fog_amount = g_cfg_fog / 100.0f;
	s.dof = g_cfg_dof / 100.0f;
	s.saturation = g_cfg_saturation / 100.0f;
	s.contrast = g_cfg_contrast / 100.0f;
	s.split_tone = g_cfg_split_tone / 100.0f;
	s.night = g_cfg_time_of_day / 100.0f;
	s.motion = g_frame_seconds > 0.0f ? g_cfg_motion_blur / 100.0f * BLUR_SHUTTER / g_frame_seconds : 0.0f;
	s.blur_near = g_frame_trimmed ? BLUR_NEAR_FIRST : BLUR_NEAR_CHASE;
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
static const int g_msaa_levels[] = { 0, 2, 4, 8 };
static const int g_aniso_levels[] = { 1, 2, 4, 8, 16 };

#define SLIDER(label, key, var, lo, hi, step) { label, key, UI_SLIDER, &var, lo, hi, step, NULL, 0, FALSE }
#define CHOICE(label, key, var, list) { label, key, UI_CHOICE, &var, 0, 0, 0, list, sizeof list / sizeof list[0], TRUE }

static const ui_item g_ui_items[] = {
	SLIDER("Day to Night", L"time_of_day", g_cfg_time_of_day, 0, 100, 5),
	SLIDER("Haze", L"fog", g_cfg_fog, 0, 300, 5),
	SLIDER("Dirt Depth", L"parallax", g_cfg_parallax, 0, 200, 5),
	SLIDER("Motion Blur", L"motion_blur", g_cfg_motion_blur, 0, 200, 5),
	SLIDER("Depth of Field", L"dof", g_cfg_dof, 0, 200, 5),
	SLIDER("Saturation", L"saturation", g_cfg_saturation, 0, 200, 5),
	SLIDER("Contrast", L"contrast", g_cfg_contrast, 0, 100, 5),
	SLIDER("Split Toning", L"split_tone", g_cfg_split_tone, 0, 100, 5),
	SLIDER("Shadow Distance", L"shadow_range", g_cfg_shadow_range, 20, 500, 10),
	{ "Hide Game HUD", L"hide_hud", UI_TOGGLE, &g_cfg_hide_hud, 0, 0, 0, NULL, 0, FALSE },
	{ "Hide Cursor in Race", L"hide_cursor", UI_TOGGLE, &g_cfg_hide_cursor, 0, 0, 0, NULL, 0, FALSE },
	CHOICE("Shadow Quality", L"shadow_size", g_cfg_shadow_size, g_shadow_sizes),
	CHOICE("Antialiasing", L"msaa", g_cfg_msaa, g_msaa_levels),
	CHOICE("Texture Filtering", L"anisotropy", g_cfg_aniso, g_aniso_levels),
	{ "Borderless Window", L"borderless", UI_TOGGLE, &g_cfg_borderless, 0, 0, 0, NULL, 0, TRUE },
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
void ui_setting_changed(const ui_item *item)
{
	wchar_t path[MAX_PATH], text[16], section[80] = L"mxs-shaders";

	if (item->kind == UI_SLIDER)
		pack_section(section, 80);
	log_path(path, L"mxs-shaders.ini");
	swprintf_s(text, 16, L"%d", *item->value);
	WritePrivateProfileStringW(section, item->key, text, path);
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

/* The game never set these itself, so its own look has them at 1. */
static void aniso_apply(void)
{
	GLuint unit = g_active_unit - GL_TEXTURE0, t;

	g_aniso = g_cfg_enabled ? g_aniso_setting : 1.0f;
	if (g_aniso_setting <= 1.0f)
		return;
	for (t = 1; t < MAX_IDS; t++) {
		if (!g_tex[t].aniso || !my_glIsTexture(t))
			continue;
		real_glBindTexture(GL_TEXTURE_2D, t);
		real_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, g_aniso);
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
	aniso_apply();
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

		if (slot)
			skinned_draw_seen(slot, count, type, indices);
		else if (kind == DRAW_STATIC && g_cur_prog == g_model_prog) {
			rigid_draw_seen(count, type, indices);
			if (g_rider_run == RIDER_DRAWING)
				g_rider_run = RIDER_DONE;
		}
		rec(REC_DRAWELEMENTS, mode, count, type, slot, kind, (size_t)indices, NULL, 0);
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
		real_glShaderSource(shader, 1, &repl, NULL);
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

/* Everything hot reload can pick up without rebuilding GL objects. */
static void load_live_settings(void)
{
	g_cfg_time_of_day = pack_int(L"time_of_day", 0);
	g_cfg_fog = pack_int(L"fog", 10);
	g_cfg_parallax = pack_int(L"parallax", 200);
	g_cfg_motion_blur = pack_int(L"motion_blur", 10);
	g_cfg_dof = pack_int(L"dof", 15);
	g_cfg_saturation = pack_int(L"saturation", 110);
	g_cfg_contrast = pack_int(L"contrast", 20);
	g_cfg_split_tone = pack_int(L"split_tone", 20);
	g_cfg_hide_hud = ini_int(L"hide_hud", 0);
	g_cfg_hide_cursor = ini_int(L"hide_cursor", 1);
	g_cfg_ui_key = ini_int(L"ui_key", VK_F8);
	g_cfg_shadow_range = max(pack_int(L"shadow_range", 500), (int)SHADOW_NEAR_RANGE);
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

		if (arrays[i] < MAX_IDS)
			g_mesh_bytes[arrays[i]] = 0;
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

	location = uni_set(location, REC_UNIFORM3FV, count, 0, 0, value, count * 12);
	rec(REC_UNIFORM3FV, location, count, 0, 0, 0, 0, value, count * 12);
	real_glUniform3fv(location, count, value);
	if (p < MAX_IDS && g_prog_role[p] && location >= 0 && location == g_loc_light[p]) {
		memcpy(g_light_view, value, sizeof g_light_view);
		g_have_light = TRUE;
	}
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

static void replay_stream(const unsigned char *stream, size_t used, GLuint prog, GLuint vao, int cascade)
{
	BOOL state_only = cascade == REPLAY_STATE_ONLY;
	int dynamic_now = 0;
	size_t at = 0;

	while (at < used) {
		const rec_cmd *c = (const rec_cmd *)(stream + at);
		const float *f = (const float *)(c + 1);

		at += sizeof *c + ((c->size + 7) & ~7);

		if ((c->type == REC_DRAWELEMENTS || c->type == REC_DRAWRANGE) && state_only)
			continue;

		/* Riders move on before the map is used, scenery doesn't. */
		if ((c->type == REC_DRAWELEMENTS || c->type == REC_DRAWRANGE) && prog == g_model_prog &&
		    c->e != DRAW_NO_SHADOW && (c->e == DRAW_DYNAMIC) != dynamic_now) {
			dynamic_now = c->e == DRAW_DYNAMIC;
			my_glProgramUniformMatrix4fv(prog, g_loc_proj[prog], 1, 0, dynamic_now ?
						     g_shadow_from_view_dyn[cascade] : g_shadow_from_view[cascade]);
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
	my_glProgramUniformMatrix4fv(g_model_prog, g_loc_proj[g_model_prog], 1, 0, g_shadow_from_view_dyn[cascade]);
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

static void replay_cascade(int cascade)
{
	int size = cascade ? SHADOW_NEAR_SIZE : g_cfg_shadow_size;

	real_glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[g_shadow_write][cascade]);
	real_glViewport(0, 0, size, size);
	real_glClear(GL_DEPTH_BUFFER_BIT);

	shadow_pass_uniforms(g_terrain_prog, g_shadow_from_view[cascade], 1);
	shadow_pass_uniforms(g_model_prog, g_shadow_from_view[cascade], 1);

	real_glUseProgram(g_rec_prev_prog);
	real_glBindVertexArray(g_rec_prev_vao);
	replay_stream(g_rec_prev, g_rec_prev_used, g_rec_prev_prog, g_rec_prev_vao, cascade);
	if (g_rigid_needed)
		draw_rigid(cascade);
}

/*
 Runs partway into a frame, from last frame's recording. Afterwards this frame's
 own recording is applied again, so the game finds GL as it left it moments ago.
*/
static void shadow_replay(void)
{
	GLboolean depth = my_glIsEnabled(GL_DEPTH_TEST), cull = my_glIsEnabled(GL_CULL_FACE);
	int c;

	real_glEnable(GL_DEPTH_TEST);
	real_glDisable(GL_CULL_FACE);
	real_glDepthMask(1);
	for (c = 0; c < SHADOW_CASCADES; c++)
		replay_cascade(c);

	if (g_terrain_prog)
		shadow_pass_uniforms(g_terrain_prog, g_proj_value[g_terrain_prog], 0);
	if (g_model_prog)
		shadow_pass_uniforms(g_model_prog, g_proj_value[g_model_prog], 0);

	if (!depth)
		real_glDisable(GL_DEPTH_TEST);
	if (cull)
		real_glEnable(GL_CULL_FACE);
	real_glDepthMask(g_depth_mask);
	real_glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
	bind_current_fb();

	real_glUseProgram(g_rec_start_prog);
	real_glBindVertexArray(g_rec_start_vao);
	replay_stream(g_rec, g_rec_used, g_rec_start_prog, g_rec_start_vao, REPLAY_STATE_ONLY);
	real_glUseProgram(g_cur_prog);
	real_glBindVertexArray(g_cur_vao);
	real_glActiveTexture(g_active_unit);
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
	log_msg("glFramebufferTexture2D fb=%u attachment=%x textarget=%x tex=%u level=%d", g_cur_fb, attachment, textarget, tex, level);
	real_glFramebufferTexture2D(target, attachment, textarget, tex, level);
}

HOOK(void, glDrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices))
{
	count_draw();
	cap("drawrangeelements mode=%x start=%u end=%u count=%d type=%x fb=%u prog=%u", mode, start, end, count, type, g_cur_fb, g_cur_prog);
	rec(REC_DRAWRANGE, mode, count, type, start, shadow_draw_kind(), (size_t)indices, &end, sizeof end);
	if (!hud_draw_hidden())
		real_glDrawRangeElements(mode, start, end, count, type, indices);
}

typedef struct {
	const char *name;
	PROC hook;
	PROC *real;
} dyn_hook;

#define DYN(n) { #n, (PROC)hook_##n, (PROC *)&real_##n }

static const dyn_hook g_dyn[] = {
	DYN(wglCreateContextAttribsARB),
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

/* --- init --- */

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
	STATIC(wglCreateContext);
	STATIC(wglDeleteContext);
	STATIC(wglMakeCurrent);
	STATIC(wglSwapBuffers);
	STATIC(wglSwapLayerBuffers);
	STATIC(wglGetProcAddress);
}
