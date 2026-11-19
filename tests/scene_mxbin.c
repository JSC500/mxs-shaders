/*
 Usage: mxbin

 A stand-in race frame for the shadow pass, which only the game's own terrain and rider
 draws reach. It draws a terrain block whose vertex shader takes its height from the
 height texture, the way the game's does, and a rider: first in chase view, drawing the
 full set, then in first person, drawing the trimmed one. Every frame ends with another
 texture bound on the height texture's unit, as the game's 2D overlay does.

 What it reports is the state the shadow pass leaves behind. The pass runs from this
 frame's terrain uniforms, so right after them the height texture must still be bound
 where the game put it; restoring last frame's recording instead (the `61e98b3` fallback)
 left the overlay's texture there. First person's shadow has to draw the full rider in
 place of the trimmed set, which the mod says in its log.

 It exits on its own after the last frame, so the test needs no driver.
*/
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
/* No GL headers on the build machine, so the few declarations needed are here. */
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef unsigned int GLuint, GLenum, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;
typedef struct __GLXcontextRec *GLXContext;

#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_DEPTH_SIZE 12

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 4
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

/* Resolved to the mod's hooks: they're the symbols the preloaded library exports. */
XVisualInfo *glXChooseVisual(Display *, int, int *);
GLXContext glXCreateContext(Display *, XVisualInfo *, GLXContext, Bool);
Bool glXMakeCurrent(Display *, XID, GLXContext);
void glXDestroyContext(Display *, GLXContext);
void glXSwapBuffers(Display *, XID);
void (*glXGetProcAddress(const GLubyte *))(void);

void glClearColor(float, float, float, float);
void glClear(GLbitfield);
void glBindTexture(GLenum, GLuint);
void glGenTextures(GLsizei, GLuint *);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void glTexParameteri(GLenum, GLenum, GLint);
void glDrawElements(GLenum, GLsizei, GLenum, const void *);

#define GPA(T, n) ((T)glXGetProcAddress((const GLubyte *)n))

#define HEIGHT_SIZE 64
#define MESHES 3
#define CHASE_FRAMES 10
#define FRAMES 22	/* 12 in first person, so the fallback replays a first-person frame */

static const GLsizei full_counts[MESHES] = { 300, 240, 120 };
static const GLsizei trimmed_counts[MESHES] = { 300, 120, 60 };	/* shares two sizes with the full set */

static const char *const g_terrain_vs =
	"#version 150\n"
	"in vec3 attr_pos;\n"
	"uniform mat4 u_projection, u_modelview;\n"
	"uniform sampler2D u_heights;\n"
	"uniform vec2 u_meshoffset;\n"
	"uniform float u_meshscale, u_spacing, u_heights_dim_inv;\n"
	"out vec2 interp_texcoord;\n"
	"out vec2 interp_hcoord;\n"
	"out vec3 interp_viewer;\n"
	"out mat3 interp_modelview;\n"
	"void main()\n"
	"{\n"
	"\tvec2 hcoord = u_meshoffset + attr_pos.xz * (u_meshscale / u_spacing);\n"
	"\tfloat h = texture(u_heights, (hcoord + 0.5) * u_heights_dim_inv).r;\n"
	"\tvec4 v = u_modelview * vec4(attr_pos.x * u_meshscale + u_meshoffset.x, h,\n"
	"\t\t\t\t    attr_pos.z * u_meshscale + u_meshoffset.y, 1.0);\n"
	"\tinterp_hcoord = hcoord;\n"
	"\tinterp_texcoord = hcoord * 0.01;\n"
	"\tinterp_viewer = v.xyz;\n"
	"\tinterp_modelview = mat3(u_modelview);\n"
	"\tgl_Position = u_projection * v;\n"
	"}\n";

/* Says "terrain" to the mod itself; the pack's own terrain.fsh takes over once it links. */
static const char *const g_terrain_fs =
	"#version 150\n"
	"in vec2 interp_hcoord;\n"
	"uniform sampler2D u_heights;\n"
	"uniform vec3 u_light_position;\n"
	"out vec4 f_fragcolor;\n"
	"void main()\n"
	"{\n"
	"\tf_fragcolor = vec4(vec3(texture(u_heights, interp_hcoord * 0.01).r) + u_light_position * 0.0, 1.0);\n"
	"}\n";

static const char *const g_model_vs =
	"#version 150\n"
	"in vec3 attr_pos;\n"
	"uniform mat4 u_projection, u_modelview;\n"
	"out vec3 interp_normal, interp_tangent, interp_viewer;\n"
	"out vec2 interp_texcoord;\n"
	"void main()\n"
	"{\n"
	"\tvec4 v = u_modelview * vec4(attr_pos, 1.0);\n"
	"\tinterp_normal = mat3(u_modelview) * vec3(0.0, 1.0, 0.0);\n"
	"\tinterp_tangent = mat3(u_modelview) * vec3(1.0, 0.0, 0.0);\n"
	"\tinterp_viewer = v.xyz;\n"
	"\tinterp_texcoord = attr_pos.xz;\n"
	"\tgl_Position = u_projection * v;\n"
	"}\n";

/* u_shininess is how the mod tells the model shader; u_nbones marks the draws skinned. */
static const char *const g_model_fs =
	"#version 150\n"
	"in vec3 interp_normal, interp_tangent, interp_viewer;\n"
	"in vec2 interp_texcoord;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_shininess;\n"
	"uniform int u_nbones;\n"
	"out vec4 f_fragcolor;\n"
	"void main()\n"
	"{\n"
	"\tf_fragcolor = vec4(vec3(texture(u_tex, interp_texcoord).r) * 0.0 + u_shininess * 0.0 +\n"
	"\t\t\t  float(u_nbones) * 0.0 + 1.0);\n"
	"}\n";

/* Everything the mod hooks is fetched the way the game fetches it, so the hooks see it. */
static GLuint (*create_shader)(GLenum);
static void (*shader_source)(GLuint, GLsizei, const char *const *, const GLint *);
static void (*compile_shader)(GLuint);
static void (*get_shaderiv)(GLuint, GLenum, GLint *);
static GLuint (*create_program)(void);
static void (*attach_shader)(GLuint, GLuint);
static void (*link_program)(GLuint);
static void (*get_programiv)(GLuint, GLenum, GLint *);
static void (*use_program)(GLuint);
static GLint (*get_uniform)(GLuint, const char *);
static GLint (*get_attrib)(GLuint, const char *);
static void (*active_texture)(GLenum);
static void (*uniform1i)(GLint, GLint);
static void (*uniform1f)(GLint, GLfloat);
static void (*uniform2fv)(GLint, GLsizei, const GLfloat *);
static void (*uniform3fv)(GLint, GLsizei, const GLfloat *);
static void (*uniform_matrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
static void (*gen_vao)(GLsizei, GLuint *);
static void (*bind_vao)(GLuint);
static void (*gen_buffer)(GLsizei, GLuint *);
static void (*bind_buffer)(GLenum, GLuint);
static void (*buffer_data)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*attrib_pointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*enable_attrib)(GLuint);
static void (*get_integerv)(GLenum, GLint *);

static void load_procs(void)
{
	*(void **)&create_shader = (void *)GPA(void *, "glCreateShader");
	*(void **)&shader_source = (void *)GPA(void *, "glShaderSource");
	*(void **)&compile_shader = (void *)GPA(void *, "glCompileShader");
	*(void **)&get_shaderiv = (void *)GPA(void *, "glGetShaderiv");
	*(void **)&create_program = (void *)GPA(void *, "glCreateProgram");
	*(void **)&attach_shader = (void *)GPA(void *, "glAttachShader");
	*(void **)&link_program = (void *)GPA(void *, "glLinkProgram");
	*(void **)&get_programiv = (void *)GPA(void *, "glGetProgramiv");
	*(void **)&use_program = (void *)GPA(void *, "glUseProgram");
	*(void **)&get_uniform = (void *)GPA(void *, "glGetUniformLocation");
	*(void **)&get_attrib = (void *)GPA(void *, "glGetAttribLocation");
	*(void **)&active_texture = (void *)GPA(void *, "glActiveTexture");
	*(void **)&uniform1i = (void *)GPA(void *, "glUniform1i");
	*(void **)&uniform1f = (void *)GPA(void *, "glUniform1f");
	*(void **)&uniform2fv = (void *)GPA(void *, "glUniform2fv");
	*(void **)&uniform3fv = (void *)GPA(void *, "glUniform3fv");
	*(void **)&uniform_matrix4fv = (void *)GPA(void *, "glUniformMatrix4fv");
	*(void **)&gen_vao = (void *)GPA(void *, "glGenVertexArrays");
	*(void **)&bind_vao = (void *)GPA(void *, "glBindVertexArray");
	*(void **)&gen_buffer = (void *)GPA(void *, "glGenBuffers");
	*(void **)&bind_buffer = (void *)GPA(void *, "glBindBuffer");
	*(void **)&buffer_data = (void *)GPA(void *, "glBufferData");
	*(void **)&attrib_pointer = (void *)GPA(void *, "glVertexAttribPointer");
	*(void **)&enable_attrib = (void *)GPA(void *, "glEnableVertexAttribArray");
	*(void **)&get_integerv = (void *)GPA(void *, "glGetIntegerv");
}

static GLuint mesh_program(const char *vs, const char *fs, const char *name)
{
	GLuint v = create_shader(GL_VERTEX_SHADER), f = create_shader(GL_FRAGMENT_SHADER), p;
	GLint ok = 0;

	shader_source(v, 1, &vs, NULL);
	compile_shader(v);
	get_shaderiv(v, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		printf("scene: %s vertex shader failed to compile\n", name);
		exit(2);
	}
	shader_source(f, 1, &fs, NULL);
	compile_shader(f);
	get_shaderiv(f, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		printf("scene: %s fragment shader failed to compile\n", name);
		exit(2);
	}
	p = create_program();
	attach_shader(p, v);
	attach_shader(p, f);
	link_program(p);
	get_programiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		printf("scene: %s program failed to link\n", name);
		exit(2);
	}
	return p;
}

/* One mesh: a triangle and its indices, as the game uploads a rider's parts. */
static void mesh(GLuint attr, GLsizei count, GLuint *vao)
{
	static const GLfloat verts[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
	unsigned short indices[512];
	GLuint vbo, ibo;
	int i;

	for (i = 0; i < count && i < 512; i++)
		indices[i] = (unsigned short)(i % 3);
	gen_vao(1, vao);
	bind_vao(*vao);
	gen_buffer(1, &vbo);
	bind_buffer(GL_ARRAY_BUFFER, vbo);
	buffer_data(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
	enable_attrib((GLuint)attr);
	attrib_pointer((GLuint)attr, 3, GL_FLOAT, 0, 0, NULL);
	gen_buffer(1, &ibo);
	bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	/* This is where the mod learns how long each mesh is, used to find the full rider. */
	buffer_data(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * 2, indices, GL_STATIC_DRAW);
}

static GLuint texture(int size, int value)
{
	unsigned char pixels[HEIGHT_SIZE * HEIGHT_SIZE];
	GLuint t;
	int i;

	for (i = 0; i < size * size; i++)
		pixels[i] = (unsigned char)value;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	return t;
}

int main(void)
{
	int attribs[] = { GLX_DOUBLEBUFFER, GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
			  GLX_DEPTH_SIZE, 16, None };
	static const GLfloat projection[16] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
					      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -10.0f, 1.0f };
	static const GLfloat view[16] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
					 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 10.0f, 1.0f };
	static const GLfloat sun[3] = { 0.45f, 0.8f, 0.3f };
	static const GLfloat origin[2] = { 0.0f, 0.0f };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes a;
	Window top, child;
	GLXContext ctx;
	GLuint terrain, model, terrain_attr, model_attr, height_tex, overlay_tex, terrain_vao;
	GLuint full_vao[MESHES], trimmed_vao[MESHES];
	unsigned kept = 0, wrong = 0, frame;
	int size;

	if (!d)
		return 2;
	vi = glXChooseVisual(d, DefaultScreen(d), attribs);
	if (!vi)
		return 3;
	memset(&a, 0, sizeof a);
	a.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	a.event_mask = StructureNotifyMask;
	top = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 640, 400, 0, vi->depth, InputOutput, vi->visual,
			    CWColormap | CWEventMask, &a);
	child = XCreateWindow(d, top, 0, 0, 640, 400, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask,
			      &a);
	XStoreName(d, top, "MX Simulator");
	XMapWindow(d, child);
	XMapWindow(d, top);
	XFlush(d);
	usleep(300000);	/* a window manager would frame it before the context is made current */
	ctx = glXCreateContext(d, vi, NULL, True);
	if (!ctx || !glXMakeCurrent(d, child, ctx))
		return 4;
	load_procs();

	terrain = mesh_program(g_terrain_vs, g_terrain_fs, "terrain");
	model = mesh_program(g_model_vs, g_model_fs, "model");
	terrain_attr = get_attrib(terrain, "attr_pos");
	model_attr = get_attrib(model, "attr_pos");

	/* The heightmap is the one texture the game binds and the mod must leave alone. */
	active_texture(GL_TEXTURE0);
	height_tex = texture(HEIGHT_SIZE, 96);
	overlay_tex = texture(4, 8);
	mesh(terrain_attr, 6, &terrain_vao);
	for (size = 0; size < MESHES; size++)
		mesh(model_attr, full_counts[size], &full_vao[size]);
	for (size = 0; size < MESHES; size++)
		mesh(model_attr, trimmed_counts[size], &trimmed_vao[size]);

	for (frame = 0; frame < FRAMES; frame++) {
		int first_person = frame >= CHASE_FRAMES, i, binding = 0;

		/* Bound before the draw that reads it, once a frame, as the game does. */
		active_texture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, height_tex);

		use_program(terrain);
		uniform1i(get_uniform(terrain, "u_heights"), 0);
		uniform1i(get_uniform(terrain, "u_shadow_pass"), 0);
		uniform1f(get_uniform(terrain, "u_spacing"), 1.0f);
		uniform2fv(get_uniform(terrain, "u_meshoffset"), 1, origin);
		uniform1f(get_uniform(terrain, "u_meshscale"), 4.0f);
		uniform1f(get_uniform(terrain, "u_heights_dim_inv"), 1.0f / (GLfloat)HEIGHT_SIZE);
		uniform3fv(get_uniform(terrain, "u_light_position"), 1, sun);
		uniform_matrix4fv(get_uniform(terrain, "u_projection"), 1, 0, projection);
		/* Terrain has no model transform: the mod takes this as the frame's camera and
		   draws the shadow map here. */
		uniform_matrix4fv(get_uniform(terrain, "u_modelview"), 1, 0, view);
		bind_vao(terrain_vao);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, NULL);

		get_integerv(GL_TEXTURE_BINDING_2D, &binding);
		if (binding == (int)height_tex) {
			kept++;
		} else if (!wrong++) {
			printf("scene: the shadow pass left texture %d on the height texture's unit at frame %u\n",
			       binding, frame);
		}

		use_program(model);
		uniform1i(get_uniform(model, "u_nbones"), 9);
		uniform1i(get_uniform(model, "u_tex"), 1);
		uniform3fv(get_uniform(model, "u_light_position"), 1, sun);
		uniform_matrix4fv(get_uniform(model, "u_projection"), 1, 0, projection);
		uniform_matrix4fv(get_uniform(model, "u_modelview"), 1, 0, view);
		for (i = 0; i < MESHES; i++) {
			bind_vao(first_person ? trimmed_vao[i] : full_vao[i]);
			glDrawElements(GL_TRIANGLES, first_person ? trimmed_counts[i] : full_counts[i], GL_UNSIGNED_SHORT,
				       NULL);
		}

		/* What the game's overlay does last in a frame: this is what the replay of last
		   frame's recording ends up holding. */
		active_texture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, overlay_tex);
		glXSwapBuffers(d, child);
	}

	printf("scene: height texture kept on %s (%u of %u frames)\n", wrong ? "some frames" : "all frames", kept, FRAMES);
	printf("scene: %u of the last %u frames drew the trimmed rider\n", FRAMES - CHASE_FRAMES, FRAMES);
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
