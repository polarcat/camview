/* Copyright (C) 2022 Aliaksei Katovich. All rights reserved.
 *
 * This source code is licensed under the BSD Zero Clause License found in
 * the 0BSD file in the root directory of this source tree.
 */

#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <linux/videodev2.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include "camera.h"
#include "log.h"

#ifndef WIN_WIDTH
#define WIN_WIDTH 960
#endif

#ifndef WIN_HEIGHT
#define WIN_HEIGHT 540
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define unused_arg(a) __attribute__((unused)) a

namespace {

static const char *vsrc_ =
	"#version 330\n"
	"in vec2 a_pos;\n"
	"out vec2 v_uv;\n"
	"void main(){\n"
		"float x=float(((uint(gl_VertexID)+2u)/3u)%2u);\n"
		"float y=float(((uint(gl_VertexID)+1u)/3u)%2u);\n"
		"gl_Position=vec4(a_pos,0.,1.);\n"
		"v_uv=vec2(x,y);\n"
	"}\n";

static const char *fsrc_ =
	"#version 330\n"
	"uniform sampler2D u_tex;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"void main(){\n"
		"frag=texture2D(u_tex,v_uv);\n"
	"}\n";

static const float verts_[] = {
	-1., 1.,
	1., 1.,
	1., -1.,
	1., -1.,
	-1., -1.,
	-1., 1.,
};

struct context {
	GLuint prog;
	GLuint vbo;
	GLuint vao;
	GLuint tex;
	GLint u_tex;
	GLint a_pos;
	float ratio;
	uint64_t sec;
	uint64_t nsec;
	float fps;
	bool print_fps;
	const char *dev;
	camera::params cam;
	camera::stream_ptr stream;
};

struct buffer {
	stbi_uc *data;
	size_t size;
	int w;
	int h;
};

static int fit_w_;
static int fit_h_;
static float ratio_ = 1.;
static float rratio_ = 1.;

static void print_fps(struct context *ctx, camera::image *img)
{
	uint64_t ms1 = ctx->sec * 1000 + ctx->nsec * .000001;
	uint64_t ms2 = img->sec * 1000 + img->nsec * .000001;
	uint32_t diff = ms2 - ms1;
	ctx->fps = 1. / (diff * .001);
	ctx->sec = img->sec;
	ctx->nsec = img->nsec;
	printf("\033[?25l\033[Gfps \033[1;33m%u\033[0m diff %d ms\033[K",
	 (uint8_t) ctx->fps, diff);
}

static const char *format2str(uint32_t format)
{
	if (format == V4L2_PIX_FMT_RGB24)
		return "RGB8";
	else if (format == V4L2_PIX_FMT_MJPEG)
		return "JPEG";
	else
		return "<nil>";
}

static constexpr uint8_t RGB_PLANES = 3;

static bool decompress_image(struct buffer *buf)
{
	int n = 0;
	buf->data = stbi_load_from_memory(buf->data, buf->size, &buf->w,
	 &buf->h, &n, RGB_PLANES);

	if (n != RGB_PLANES) {
		ee("only RGB color scheme is supported, n=%d\n", n);
		return false;
	}

	return true;
}

static void key_cb(GLFWwindow *win, int key, unused_arg(int code), int action,
 unused_arg(int mods))
{
	if (action != GLFW_PRESS)
		return;
	else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q)
		glfwSetWindowShouldClose(win, GLFW_TRUE);
	else if (key == GLFW_KEY_F)
		glfwSetWindowSize(win, fit_w_, fit_h_);
}

static void error_cb(int err, const char *str)
{
	fprintf(stderr, "%s, err=%d\n", str, err);
}

static inline bool gl_error(const char *msg)
{
	bool ret = false;
	for (GLint err = glGetError(); err; err = glGetError()) {
		ee("%s error '0x%x'\n", msg, err);
		ret = true;
	}
	return ret;
}

static GLuint make_shader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);

	if (!shader) {
		gl_error("create shader");
		return 0;
	}

	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

	if (!compiled) {
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

		if (!len)
			return 0;

		char *buf = (char *) malloc(len);
		if (!buf)
			return 0;

		glGetShaderInfoLog(shader, len, NULL, buf);
		ee("could not compile %s shader: %s",
		 type == GL_VERTEX_SHADER ? "vertex" : "fragment\n", buf);
		free(buf);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static bool make_prog(struct context *ctx)
{
	ctx->prog = glCreateProgram();
	if (!ctx->prog) {
		gl_error("create program");
		return false;
	}

	GLuint vsh = make_shader(GL_VERTEX_SHADER, vsrc_);
	if (!vsh)
		return false;

	GLuint fsh = make_shader(GL_FRAGMENT_SHADER, fsrc_);
	if (!fsh)
		return false;

	glAttachShader(ctx->prog, vsh);
	glAttachShader(ctx->prog, fsh);
	glLinkProgram(ctx->prog);

	GLint status = GL_FALSE;
	glGetProgramiv(ctx->prog, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		GLint len = 0;
		glGetProgramiv(ctx->prog, GL_INFO_LOG_LENGTH, &len);

		if (len) {
			char *buf = (char *) malloc(len);

			if (buf) {
				glGetProgramInfoLog(ctx->prog, len, NULL, buf);
				ee("%s", buf);
				free(buf);
			}
		}

		glDeleteProgram(ctx->prog);
		ee("failed to link program %u\n", ctx->prog);
		ctx->prog = 0;
		return false;
	}

	ctx->u_tex = glGetUniformLocation(ctx->prog, "u_tex");
	ctx->a_pos = glGetAttribLocation(ctx->prog, "a_pos");

	glGenBuffers(1, &ctx->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2 * 6, verts_,
	 GL_STATIC_DRAW);
	/* keep VBO bound for VAO */

	glGenVertexArrays(1, &ctx->vao);
	glBindVertexArray(ctx->vao);
	glEnableVertexAttribArray(ctx->a_pos);
	glVertexAttribPointer(ctx->a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glGenTextures(1, &ctx->tex);
	glBindTexture(GL_TEXTURE_2D, ctx->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

	return true;
}

static void draw_image(struct context *ctx)
{
	struct buffer buf;
	camera::image img;

	glUseProgram(ctx->prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ctx->tex);

	ctx->stream->get_frame(img);
	ctx->ratio = (float) img.w / img.h;

	buf.data = img.data;
	buf.size = img.bytes;
	buf.w = img.w;
	buf.h = img.h;

	if (ctx->cam.fmt == V4L2_PIX_FMT_MJPEG && !decompress_image(&buf)) {
		goto out;
	} else if (!buf.data || buf.w == 0 || buf.h == 0) {
		goto out;
	}

	ratio_ = buf.w / (float) buf.h;
	rratio_ = buf.h / (float) buf.w;

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, buf.w, buf.h, 0, GL_RGB,
	 GL_UNSIGNED_BYTE, buf.data);

	if (ctx->print_fps)
		print_fps(ctx, &img);

	glBindVertexArray(ctx->vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

	if (ctx->cam.fmt == V4L2_PIX_FMT_MJPEG)
		free(buf.data);
out:
	ctx->stream->put_frame();
}

static void help(const char *name)
{
	printf("Usage: %s <options>\n"
	 "Options:\n"
	 "\033[2m"
	 " -d, --dev <str>     video device, e.g. /dev/video0\n"
	 " -p, --params <str>  stream hints (WxH@fps), e.g. 1920x1080@30\n"
	 " -j, --jpeg          request jpeg compressed stream\n"
	 " -f, --fps           print fps\n"
	 "\033[0m"
	 "Example: %s -d /dev/video0 -p 1920x1080@30\n",
	 name, name);
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static void init_context(int argc, const char *argv[], struct context *ctx)
{
	const char *geom_w;
	const char *geom_h;
	const char *fps;
	const char *arg;

	ctx->cam.fmt = V4L2_PIX_FMT_RGB24;
	ctx->fps = 30;
	ctx->dev = NULL;

	for (uint8_t i = 0; i < argc; ++i) {
		arg = argv[i];
		if (opt(arg, "-d", "--dev")) {
			i++;
			ctx->dev = argv[i];
		} else if (opt(arg, "-p", "--params")) {
			i++;
			if (!(geom_w = argv[i])) {
				ee("malformed width, geom string e.g. 1920x1080\n");
				exit(1);
			} else if (!(geom_h = strchr(geom_w, 'x'))) {
				ee("malformed height, geom string e.g. 1920x1080\n");
				exit(1);
			} else if ((fps = strchr(geom_h, '@'))) {
				ctx->cam.fps = atoi(++fps);
			}

			ctx->cam.w = atoi(geom_w);
			ctx->cam.h = atoi(++geom_h);
		} else if (opt(arg, "-j", "--jpeg")) {
			ctx->cam.fmt = V4L2_PIX_FMT_MJPEG;
		} else if (opt(arg, "-f", "--fps")) {
			ctx->print_fps = true;
		} else if (opt(arg, "-h", "--help")) {
			help(argv[0]);
			exit(1);
		}
	}

	if (!ctx->dev) {
		help(argv[0]);
		exit(1);
	}

	ii("open camera %s; hinted params %s; format %s\n", ctx->dev, geom_w,
	 format2str(ctx->cam.fmt));

	ctx->stream = camera::create_stream(ctx->dev, &ctx->cam);
	if (!ctx->stream.get()) {
		ee("failed to create %s stream\n", format2str(ctx->cam.fmt));
		exit(1);
	} else if (!ctx->stream->start()) {
		exit(1);
	}
}

} /* namespace */

int main(int argc, const char *argv[])
{
	int w;
	int h;
	struct context ctx;
	GLFWwindow *win;

	init_context(argc, argv, &ctx);

	glfwSetErrorCallback(error_cb);
	if (!glfwInit())
		exit(1);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	if (!(win = glfwCreateWindow(ctx.cam.w, ctx.cam.h, ctx.dev, NULL,
	 NULL))) {
		glfwTerminate();
		exit(1);
	}

	glfwSetKeyCallback(win, key_cb);
	glfwMakeContextCurrent(win);
	gladLoadGL(glfwGetProcAddress);
	glfwSwapInterval(1);
	glClearColor(0, 0, 0, 1);

	if (!make_prog(&ctx))
		exit(1);

	while (!glfwWindowShouldClose(win)) {
		/* lame way of tracking window resize */
		glfwGetFramebufferSize(win, &w, &h);

		/* try to maintain original aspect ratio */
		fit_w_ = h * ratio_;
		if (fit_w_ <= w) {
			fit_h_ = h;
		} else {
			fit_w_ = w;
			fit_h_ = w * rratio_;
		}

		glViewport(0, 0, fit_w_, fit_h_);
		glClear(GL_COLOR_BUFFER_BIT);
		draw_image(&ctx);
		glfwSwapBuffers(win);
		glfwPollEvents();
	}

	glDeleteProgram(ctx.prog);
	glfwDestroyWindow(win);
	glfwTerminate();
	/* restore cursor */
	printf("\033[?25h\n");

	return 0;
}
