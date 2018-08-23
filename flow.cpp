#define NO_SDL_GLEXT 1

#include <epoxy/gl.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_video.h>

#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>

#include "flow.h"
#include "gpu_timers.h"
#include "util.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <map>
#include <stack>
#include <vector>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

SDL_Window *window;

// Weighting constants for the different parts of the variational refinement.
// These don't correspond 1:1 to the values given in the DIS paper,
// since we have different normalizations and ranges in some cases.
// These are found through a simple grid search on some MPI-Sintel data,
// although the error (EPE) seems to be fairly insensitive to the precise values.
// Only the relative values matter, so we fix alpha (the smoothness constant)
// at unity and tweak the others.
static float vr_alpha = 1.0f, vr_delta = 0.25f, vr_gamma = 0.25f;

bool enable_timing = true;
bool detailed_timing = false;
bool enable_warmup = false;
bool in_warmup = false;
bool enable_variational_refinement = true;  // Just for debugging.
bool enable_interpolation = false;

// Some global OpenGL objects.
// TODO: These should really be part of DISComputeFlow.
GLuint nearest_sampler, linear_sampler, zero_border_sampler;
GLuint vertex_vbo;

// Structures for asynchronous readback. We assume everything is the same size (and GL_RG16F).
struct ReadInProgress {
	GLuint pbo;
	string filename0, filename1;
	string flow_filename, ppm_filename;  // Either may be empty for no write.
};
stack<GLuint> spare_pbos;
deque<ReadInProgress> reads_in_progress;

int find_num_levels(int width, int height)
{
	int levels = 1;
	for (int w = width, h = height; w > 1 || h > 1; ) {
		w >>= 1;
		h >>= 1;
		++levels;
	}
	return levels;
}

string read_file(const string &filename)
{
	FILE *fp = fopen(filename.c_str(), "r");
	if (fp == nullptr) {
		perror(filename.c_str());
		exit(1);
	}

	int ret = fseek(fp, 0, SEEK_END);
	if (ret == -1) {
		perror("fseek(SEEK_END)");
		exit(1);
	}

	int size = ftell(fp);

	ret = fseek(fp, 0, SEEK_SET);
	if (ret == -1) {
		perror("fseek(SEEK_SET)");
		exit(1);
	}

	string str;
	str.resize(size);
	ret = fread(&str[0], size, 1, fp);
	if (ret == -1) {
		perror("fread");
		exit(1);
	}
	if (ret == 0) {
		fprintf(stderr, "Short read when trying to read %d bytes from %s\n",
				size, filename.c_str());
		exit(1);
	}
	fclose(fp);

	return str;
}


GLuint compile_shader(const string &shader_src, GLenum type)
{
	GLuint obj = glCreateShader(type);
	const GLchar* source[] = { shader_src.data() };
	const GLint length[] = { (GLint)shader_src.size() };
	glShaderSource(obj, 1, source, length);
	glCompileShader(obj);

	GLchar info_log[4096];
	GLsizei log_length = sizeof(info_log) - 1;
	glGetShaderInfoLog(obj, log_length, &log_length, info_log);
	info_log[log_length] = 0;
	if (strlen(info_log) > 0) {
		fprintf(stderr, "Shader compile log: %s\n", info_log);
	}

	GLint status;
	glGetShaderiv(obj, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		// Add some line numbers to easier identify compile errors.
		string src_with_lines = "/*   1 */ ";
		size_t lineno = 1;
		for (char ch : shader_src) {
			src_with_lines.push_back(ch);
			if (ch == '\n') {
				char buf[32];
				snprintf(buf, sizeof(buf), "/* %3zu */ ", ++lineno);
				src_with_lines += buf;
			}
		}

		fprintf(stderr, "Failed to compile shader:\n%s\n", src_with_lines.c_str());
		exit(1);
	}

	return obj;
}

enum MipmapPolicy {
	WITHOUT_MIPMAPS,
	WITH_MIPMAPS
};

GLuint load_texture(const char *filename, unsigned *width_ret, unsigned *height_ret, MipmapPolicy mipmaps)
{
	SDL_Surface *surf = IMG_Load(filename);
	if (surf == nullptr) {
		fprintf(stderr, "IMG_Load(%s): %s\n", filename, IMG_GetError());
		exit(1);
	}

	// For whatever reason, SDL doesn't support converting to YUV surfaces
	// nor grayscale, so we'll do it ourselves.
	SDL_Surface *rgb_surf = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, /*flags=*/0);
	if (rgb_surf == nullptr) {
		fprintf(stderr, "SDL_ConvertSurfaceFormat(%s): %s\n", filename, SDL_GetError());
		exit(1);
	}

	SDL_FreeSurface(surf);

	unsigned width = rgb_surf->w, height = rgb_surf->h;
	const uint8_t *sptr = (uint8_t *)rgb_surf->pixels;
	unique_ptr<uint8_t[]> pix(new uint8_t[width * height * 4]);

	// Extract the Y component, and convert to bottom-left origin.
	for (unsigned y = 0; y < height; ++y) {
		unsigned y2 = height - 1 - y;
		memcpy(pix.get() + y * width * 4, sptr + y2 * rgb_surf->pitch, width * 4);
	}
	SDL_FreeSurface(rgb_surf);

	int num_levels = (mipmaps == WITH_MIPMAPS) ? find_num_levels(width, height) : 1;

	GLuint tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex);
	glTextureStorage2D(tex, num_levels, GL_RGBA8, width, height);
	glTextureSubImage2D(tex, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pix.get());

	if (mipmaps == WITH_MIPMAPS) {
		glGenerateTextureMipmap(tex);
	}

	*width_ret = width;
	*height_ret = height;

	return tex;
}

GLuint link_program(GLuint vs_obj, GLuint fs_obj)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, vs_obj);
	glAttachShader(program, fs_obj);
	glLinkProgram(program);
	GLint success;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (success == GL_FALSE) {
		GLchar error_log[1024] = {0};
		glGetProgramInfoLog(program, 1024, nullptr, error_log);
		fprintf(stderr, "Error linking program: %s\n", error_log);
		exit(1);
	}
	return program;
}

void bind_sampler(GLuint program, GLint location, GLuint texture_unit, GLuint tex, GLuint sampler)
{
	if (location == -1) {
		return;
	}

	glBindTextureUnit(texture_unit, tex);
	glBindSampler(texture_unit, sampler);
	glProgramUniform1i(program, location, texture_unit);
}

template<size_t num_elements>
void PersistentFBOSet<num_elements>::render_to(const array<GLuint, num_elements> &textures)
{
	auto it = fbos.find(textures);
	if (it != fbos.end()) {
		glBindFramebuffer(GL_FRAMEBUFFER, it->second);
		return;
	}

	GLuint fbo;
	glCreateFramebuffers(1, &fbo);
	GLenum bufs[num_elements];
	for (size_t i = 0; i < num_elements; ++i) {
		glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i, textures[i], 0);
		bufs[i] = GL_COLOR_ATTACHMENT0 + i;
	}
	glNamedFramebufferDrawBuffers(fbo, num_elements, bufs);

	fbos[textures] = fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

template<size_t num_elements>
void PersistentFBOSetWithDepth<num_elements>::render_to(GLuint depth_rb, const array<GLuint, num_elements> &textures)
{
	auto key = make_pair(depth_rb, textures);

	auto it = fbos.find(key);
	if (it != fbos.end()) {
		glBindFramebuffer(GL_FRAMEBUFFER, it->second);
		return;
	}

	GLuint fbo;
	glCreateFramebuffers(1, &fbo);
	GLenum bufs[num_elements];
	glNamedFramebufferRenderbuffer(fbo, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
	for (size_t i = 0; i < num_elements; ++i) {
		glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i, textures[i], 0);
		bufs[i] = GL_COLOR_ATTACHMENT0 + i;
	}
	glNamedFramebufferDrawBuffers(fbo, num_elements, bufs);

	fbos[key] = fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

GrayscaleConversion::GrayscaleConversion()
{
	gray_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	gray_fs_obj = compile_shader(read_file("gray.frag"), GL_FRAGMENT_SHADER);
	gray_program = link_program(gray_vs_obj, gray_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &gray_vao);
	glBindVertexArray(gray_vao);

	GLint position_attrib = glGetAttribLocation(gray_program, "position");
	glEnableVertexArrayAttrib(gray_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_tex = glGetUniformLocation(gray_program, "tex");
}

void GrayscaleConversion::exec(GLint tex, GLint gray_tex, int width, int height, int num_layers)
{
	glUseProgram(gray_program);
	bind_sampler(gray_program, uniform_tex, 0, tex, nearest_sampler);

	glViewport(0, 0, width, height);
	fbos.render_to(gray_tex);
	glBindVertexArray(gray_vao);
	glDisable(GL_BLEND);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

Sobel::Sobel()
{
	sobel_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	sobel_fs_obj = compile_shader(read_file("sobel.frag"), GL_FRAGMENT_SHADER);
	sobel_program = link_program(sobel_vs_obj, sobel_fs_obj);

	uniform_tex = glGetUniformLocation(sobel_program, "tex");
}

void Sobel::exec(GLint tex_view, GLint grad_tex, int level_width, int level_height, int num_layers)
{
	glUseProgram(sobel_program);
	bind_sampler(sobel_program, uniform_tex, 0, tex_view, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	fbos.render_to(grad_tex);
	glDisable(GL_BLEND);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

MotionSearch::MotionSearch(const OperatingPoint &op)
	: op(op)
{
	motion_vs_obj = compile_shader(read_file("motion_search.vert"), GL_VERTEX_SHADER);
	motion_fs_obj = compile_shader(read_file("motion_search.frag"), GL_FRAGMENT_SHADER);
	motion_search_program = link_program(motion_vs_obj, motion_fs_obj);

	uniform_inv_image_size = glGetUniformLocation(motion_search_program, "inv_image_size");
	uniform_inv_prev_level_size = glGetUniformLocation(motion_search_program, "inv_prev_level_size");
	uniform_out_flow_size = glGetUniformLocation(motion_search_program, "out_flow_size");
	uniform_image_tex = glGetUniformLocation(motion_search_program, "image_tex");
	uniform_grad_tex = glGetUniformLocation(motion_search_program, "grad_tex");
	uniform_flow_tex = glGetUniformLocation(motion_search_program, "flow_tex");
	uniform_patch_size = glGetUniformLocation(motion_search_program, "patch_size");
	uniform_num_iterations = glGetUniformLocation(motion_search_program, "num_iterations");
}

void MotionSearch::exec(GLuint tex_view, GLuint grad_tex, GLuint flow_tex, GLuint flow_out_tex, int level_width, int level_height, int prev_level_width, int prev_level_height, int width_patches, int height_patches, int num_layers)
{
	glUseProgram(motion_search_program);

	bind_sampler(motion_search_program, uniform_image_tex, 0, tex_view, linear_sampler);
	bind_sampler(motion_search_program, uniform_grad_tex, 1, grad_tex, nearest_sampler);
	bind_sampler(motion_search_program, uniform_flow_tex, 2, flow_tex, linear_sampler);

	glProgramUniform2f(motion_search_program, uniform_inv_image_size, 1.0f / level_width, 1.0f / level_height);
	glProgramUniform2f(motion_search_program, uniform_inv_prev_level_size, 1.0f / prev_level_width, 1.0f / prev_level_height);
	glProgramUniform2f(motion_search_program, uniform_out_flow_size, width_patches, height_patches);
	glProgramUniform1ui(motion_search_program, uniform_patch_size, op.patch_size_pixels);
	glProgramUniform1ui(motion_search_program, uniform_num_iterations, op.search_iterations);

	glViewport(0, 0, width_patches, height_patches);
	fbos.render_to(flow_out_tex);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

Densify::Densify(const OperatingPoint &op)
	: op(op)
{
	densify_vs_obj = compile_shader(read_file("densify.vert"), GL_VERTEX_SHADER);
	densify_fs_obj = compile_shader(read_file("densify.frag"), GL_FRAGMENT_SHADER);
	densify_program = link_program(densify_vs_obj, densify_fs_obj);

	uniform_patch_size = glGetUniformLocation(densify_program, "patch_size");
	uniform_image_tex = glGetUniformLocation(densify_program, "image_tex");
	uniform_flow_tex = glGetUniformLocation(densify_program, "flow_tex");
}

void Densify::exec(GLuint tex_view, GLuint flow_tex, GLuint dense_flow_tex, int level_width, int level_height, int width_patches, int height_patches, int num_layers)
{
	glUseProgram(densify_program);

	bind_sampler(densify_program, uniform_image_tex, 0, tex_view, linear_sampler);
	bind_sampler(densify_program, uniform_flow_tex, 1, flow_tex, nearest_sampler);

	glProgramUniform2f(densify_program, uniform_patch_size,
		float(op.patch_size_pixels) / level_width,
		float(op.patch_size_pixels) / level_height);

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	fbos.render_to(dense_flow_tex);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width_patches * height_patches * num_layers);
}

Prewarp::Prewarp()
{
	prewarp_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	prewarp_fs_obj = compile_shader(read_file("prewarp.frag"), GL_FRAGMENT_SHADER);
	prewarp_program = link_program(prewarp_vs_obj, prewarp_fs_obj);

	uniform_image_tex = glGetUniformLocation(prewarp_program, "image_tex");
	uniform_flow_tex = glGetUniformLocation(prewarp_program, "flow_tex");
}

void Prewarp::exec(GLuint tex_view, GLuint flow_tex, GLuint I_tex, GLuint I_t_tex, GLuint normalized_flow_tex, int level_width, int level_height, int num_layers)
{
	glUseProgram(prewarp_program);

	bind_sampler(prewarp_program, uniform_image_tex, 0, tex_view, linear_sampler);
	bind_sampler(prewarp_program, uniform_flow_tex, 1, flow_tex, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	fbos.render_to(I_tex, I_t_tex, normalized_flow_tex);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

Derivatives::Derivatives()
{
	derivatives_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	derivatives_fs_obj = compile_shader(read_file("derivatives.frag"), GL_FRAGMENT_SHADER);
	derivatives_program = link_program(derivatives_vs_obj, derivatives_fs_obj);

	uniform_tex = glGetUniformLocation(derivatives_program, "tex");
}

void Derivatives::exec(GLuint input_tex, GLuint I_x_y_tex, GLuint beta_0_tex, int level_width, int level_height, int num_layers)
{
	glUseProgram(derivatives_program);

	bind_sampler(derivatives_program, uniform_tex, 0, input_tex, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	fbos.render_to(I_x_y_tex, beta_0_tex);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

ComputeDiffusivity::ComputeDiffusivity()
{
	diffusivity_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	diffusivity_fs_obj = compile_shader(read_file("diffusivity.frag"), GL_FRAGMENT_SHADER);
	diffusivity_program = link_program(diffusivity_vs_obj, diffusivity_fs_obj);

	uniform_flow_tex = glGetUniformLocation(diffusivity_program, "flow_tex");
	uniform_diff_flow_tex = glGetUniformLocation(diffusivity_program, "diff_flow_tex");
	uniform_alpha = glGetUniformLocation(diffusivity_program, "alpha");
	uniform_zero_diff_flow = glGetUniformLocation(diffusivity_program, "zero_diff_flow");
}

void ComputeDiffusivity::exec(GLuint flow_tex, GLuint diff_flow_tex, GLuint diffusivity_tex, int level_width, int level_height, bool zero_diff_flow, int num_layers)
{
	glUseProgram(diffusivity_program);

	bind_sampler(diffusivity_program, uniform_flow_tex, 0, flow_tex, nearest_sampler);
	bind_sampler(diffusivity_program, uniform_diff_flow_tex, 1, diff_flow_tex, nearest_sampler);
	glProgramUniform1f(diffusivity_program, uniform_alpha, vr_alpha);
	glProgramUniform1i(diffusivity_program, uniform_zero_diff_flow, zero_diff_flow);

	glViewport(0, 0, level_width, level_height);

	glDisable(GL_BLEND);
	fbos.render_to(diffusivity_tex);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

SetupEquations::SetupEquations()
{
	equations_vs_obj = compile_shader(read_file("equations.vert"), GL_VERTEX_SHADER);
	equations_fs_obj = compile_shader(read_file("equations.frag"), GL_FRAGMENT_SHADER);
	equations_program = link_program(equations_vs_obj, equations_fs_obj);

	uniform_I_x_y_tex = glGetUniformLocation(equations_program, "I_x_y_tex");
	uniform_I_t_tex = glGetUniformLocation(equations_program, "I_t_tex");
	uniform_diff_flow_tex = glGetUniformLocation(equations_program, "diff_flow_tex");
	uniform_base_flow_tex = glGetUniformLocation(equations_program, "base_flow_tex");
	uniform_beta_0_tex = glGetUniformLocation(equations_program, "beta_0_tex");
	uniform_diffusivity_tex = glGetUniformLocation(equations_program, "diffusivity_tex");
	uniform_gamma = glGetUniformLocation(equations_program, "gamma");
	uniform_delta = glGetUniformLocation(equations_program, "delta");
	uniform_zero_diff_flow = glGetUniformLocation(equations_program, "zero_diff_flow");
}

void SetupEquations::exec(GLuint I_x_y_tex, GLuint I_t_tex, GLuint diff_flow_tex, GLuint base_flow_tex, GLuint beta_0_tex, GLuint diffusivity_tex, GLuint equation_red_tex, GLuint equation_black_tex, int level_width, int level_height, bool zero_diff_flow, int num_layers)
{
	glUseProgram(equations_program);

	bind_sampler(equations_program, uniform_I_x_y_tex, 0, I_x_y_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_I_t_tex, 1, I_t_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_diff_flow_tex, 2, diff_flow_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_base_flow_tex, 3, base_flow_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_beta_0_tex, 4, beta_0_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_diffusivity_tex, 5, diffusivity_tex, zero_border_sampler);
	glProgramUniform1f(equations_program, uniform_delta, vr_delta);
	glProgramUniform1f(equations_program, uniform_gamma, vr_gamma);
	glProgramUniform1i(equations_program, uniform_zero_diff_flow, zero_diff_flow);

	glViewport(0, 0, (level_width + 1) / 2, level_height);
	glDisable(GL_BLEND);
	fbos.render_to(equation_red_tex, equation_black_tex);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

SOR::SOR()
{
	sor_vs_obj = compile_shader(read_file("sor.vert"), GL_VERTEX_SHADER);
	sor_fs_obj = compile_shader(read_file("sor.frag"), GL_FRAGMENT_SHADER);
	sor_program = link_program(sor_vs_obj, sor_fs_obj);

	uniform_diff_flow_tex = glGetUniformLocation(sor_program, "diff_flow_tex");
	uniform_equation_red_tex = glGetUniformLocation(sor_program, "equation_red_tex");
	uniform_equation_black_tex = glGetUniformLocation(sor_program, "equation_black_tex");
	uniform_diffusivity_tex = glGetUniformLocation(sor_program, "diffusivity_tex");
	uniform_phase = glGetUniformLocation(sor_program, "phase");
	uniform_num_nonzero_phases = glGetUniformLocation(sor_program, "num_nonzero_phases");
}

void SOR::exec(GLuint diff_flow_tex, GLuint equation_red_tex, GLuint equation_black_tex, GLuint diffusivity_tex, int level_width, int level_height, int num_iterations, bool zero_diff_flow, int num_layers, ScopedTimer *sor_timer)
{
	glUseProgram(sor_program);

	bind_sampler(sor_program, uniform_diff_flow_tex, 0, diff_flow_tex, nearest_sampler);
	bind_sampler(sor_program, uniform_diffusivity_tex, 1, diffusivity_tex, zero_border_sampler);
	bind_sampler(sor_program, uniform_equation_red_tex, 2, equation_red_tex, nearest_sampler);
	bind_sampler(sor_program, uniform_equation_black_tex, 3, equation_black_tex, nearest_sampler);

	if (!zero_diff_flow) {
		glProgramUniform1i(sor_program, uniform_num_nonzero_phases, 2);
	}

	// NOTE: We bind to the texture we are rendering from, but we never write any value
	// that we read in the same shader pass (we call discard for red values when we compute
	// black, and vice versa), and we have barriers between the passes, so we're fine
	// as per the spec.
	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	fbos.render_to(diff_flow_tex);

	for (int i = 0; i < num_iterations; ++i) {
		{
			ScopedTimer timer("Red pass", sor_timer);
			if (zero_diff_flow && i == 0) {
				glProgramUniform1i(sor_program, uniform_num_nonzero_phases, 0);
			}
			glProgramUniform1i(sor_program, uniform_phase, 0);
			glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
			glTextureBarrier();
		}
		{
			ScopedTimer timer("Black pass", sor_timer);
			if (zero_diff_flow && i == 0) {
				glProgramUniform1i(sor_program, uniform_num_nonzero_phases, 1);
			}
			glProgramUniform1i(sor_program, uniform_phase, 1);
			glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
			if (zero_diff_flow && i == 0) {
				glProgramUniform1i(sor_program, uniform_num_nonzero_phases, 2);
			}
			if (i != num_iterations - 1) {
				glTextureBarrier();
			}
		}
	}
}

AddBaseFlow::AddBaseFlow()
{
	add_flow_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	add_flow_fs_obj = compile_shader(read_file("add_base_flow.frag"), GL_FRAGMENT_SHADER);
	add_flow_program = link_program(add_flow_vs_obj, add_flow_fs_obj);

	uniform_diff_flow_tex = glGetUniformLocation(add_flow_program, "diff_flow_tex");
}

void AddBaseFlow::exec(GLuint base_flow_tex, GLuint diff_flow_tex, int level_width, int level_height, int num_layers)
{
	glUseProgram(add_flow_program);

	bind_sampler(add_flow_program, uniform_diff_flow_tex, 0, diff_flow_tex, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	fbos.render_to(base_flow_tex);

	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

ResizeFlow::ResizeFlow()
{
	resize_flow_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	resize_flow_fs_obj = compile_shader(read_file("resize_flow.frag"), GL_FRAGMENT_SHADER);
	resize_flow_program = link_program(resize_flow_vs_obj, resize_flow_fs_obj);

	uniform_flow_tex = glGetUniformLocation(resize_flow_program, "flow_tex");
	uniform_scale_factor = glGetUniformLocation(resize_flow_program, "scale_factor");
}

void ResizeFlow::exec(GLuint flow_tex, GLuint out_tex, int input_width, int input_height, int output_width, int output_height, int num_layers)
{
	glUseProgram(resize_flow_program);

	bind_sampler(resize_flow_program, uniform_flow_tex, 0, flow_tex, nearest_sampler);

	glProgramUniform2f(resize_flow_program, uniform_scale_factor, float(output_width) / input_width, float(output_height) / input_height);

	glViewport(0, 0, output_width, output_height);
	glDisable(GL_BLEND);
	fbos.render_to(out_tex);

	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, num_layers);
}

DISComputeFlow::DISComputeFlow(int width, int height, const OperatingPoint &op)
	: width(width), height(height), op(op), motion_search(op), densify(op)
{
	// Make some samplers.
	glCreateSamplers(1, &nearest_sampler);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glCreateSamplers(1, &linear_sampler);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// The smoothness is sampled so that once we get to a smoothness involving
	// a value outside the border, the diffusivity between the two becomes zero.
	// Similarly, gradients are zero outside the border, since the edge is taken
	// to be constant.
	glCreateSamplers(1, &zero_border_sampler);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float zero[] = { 0.0f, 0.0f, 0.0f, 0.0f };  // Note that zero alpha means we can also see whether we sampled outside the border or not.
	glSamplerParameterfv(zero_border_sampler, GL_TEXTURE_BORDER_COLOR, zero);

	// Initial flow is zero, 1x1.
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &initial_flow_tex);
	glTextureStorage3D(initial_flow_tex, 1, GL_RG16F, 1, 1, 1);
	glClearTexImage(initial_flow_tex, 0, GL_RG, GL_FLOAT, nullptr);

	// Set up the vertex data that will be shared between all passes.
	float vertices[] = {
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
	};
	glCreateBuffers(1, &vertex_vbo);
	glNamedBufferData(vertex_vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glCreateVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = 0;  // Hard-coded in every vertex shader.
	glEnableVertexArrayAttrib(vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
}

GLuint DISComputeFlow::exec(GLuint tex, FlowDirection flow_direction, ResizeStrategy resize_strategy)
{
	int num_layers = (flow_direction == FORWARD_AND_BACKWARD) ? 2 : 1;
	int prev_level_width = 1, prev_level_height = 1;
	GLuint prev_level_flow_tex = initial_flow_tex;

	GPUTimers timers;

	glBindVertexArray(vao);

	ScopedTimer total_timer("Compute flow", &timers);
	for (int level = op.coarsest_level; level >= int(op.finest_level); --level) {
		char timer_name[256];
		snprintf(timer_name, sizeof(timer_name), "Level %d (%d x %d)", level, width >> level, height >> level);
		ScopedTimer level_timer(timer_name, &total_timer);

		int level_width = width >> level;
		int level_height = height >> level;
		float patch_spacing_pixels = op.patch_size_pixels * (1.0f - op.patch_overlap_ratio);

		// Make sure we have patches at least every Nth pixel, e.g. for width=9
		// and patch_spacing=3 (the default), we put out patch centers in
		// x=0, x=3, x=6, x=9, which is four patches. The fragment shader will
		// lock all the centers to integer coordinates if needed.
		int width_patches = 1 + ceil(level_width / patch_spacing_pixels);
		int height_patches = 1 + ceil(level_height / patch_spacing_pixels);

		// Make sure we always read from the correct level; the chosen
		// mipmapping could otherwise be rather unpredictable, especially
		// during motion search.
		GLuint tex_view;
		glGenTextures(1, &tex_view);
		glTextureView(tex_view, GL_TEXTURE_2D_ARRAY, tex, GL_R8, level, 1, 0, 2);

		// Create a new texture to hold the gradients.
		GLuint grad_tex = pool.get_texture(GL_R32UI, level_width, level_height, num_layers);

		// Find the derivative.
		{
			ScopedTimer timer("Sobel", &level_timer);
			sobel.exec(tex_view, grad_tex, level_width, level_height, num_layers);
		}

		// Motion search to find the initial flow. We use the flow from the previous
		// level (sampled bilinearly; no fancy tricks) as a guide, then search from there.

		// Create an output flow texture.
		GLuint flow_out_tex = pool.get_texture(GL_RGB16F, width_patches, height_patches, num_layers);

		// And draw.
		{
			ScopedTimer timer("Motion search", &level_timer);
			motion_search.exec(tex_view, grad_tex, prev_level_flow_tex, flow_out_tex, level_width, level_height, prev_level_width, prev_level_height, width_patches, height_patches, num_layers);
		}
		pool.release_texture(grad_tex);

		// Densification.

		// Set up an output texture (cleared in Densify).
		GLuint dense_flow_tex = pool.get_texture(GL_RGB16F, level_width, level_height, num_layers);

		// And draw.
		{
			ScopedTimer timer("Densification", &level_timer);
			densify.exec(tex_view, flow_out_tex, dense_flow_tex, level_width, level_height, width_patches, height_patches, num_layers);
		}
		pool.release_texture(flow_out_tex);

		// Everything below here in the loop belongs to variational refinement.
		ScopedTimer varref_timer("Variational refinement", &level_timer);

		// Prewarping; create I and I_t, and a normalized base flow (so we don't
		// have to normalize it over and over again, and also save some bandwidth).
		//
		// During the entire rest of the variational refinement, flow will be measured
		// in pixels, not 0..1 normalized OpenGL texture coordinates.
		// This is because variational refinement depends so heavily on derivatives,
		// which are measured in intensity levels per pixel.
		GLuint I_tex = pool.get_texture(GL_R16F, level_width, level_height, num_layers);
		GLuint I_t_tex = pool.get_texture(GL_R16F, level_width, level_height, num_layers);
		GLuint base_flow_tex = pool.get_texture(GL_RG16F, level_width, level_height, num_layers);
		{
			ScopedTimer timer("Prewarping", &varref_timer);
			prewarp.exec(tex_view, dense_flow_tex, I_tex, I_t_tex, base_flow_tex, level_width, level_height, num_layers);
		}
		pool.release_texture(dense_flow_tex);
		glDeleteTextures(1, &tex_view);

		// Calculate I_x and I_y. We're only calculating first derivatives;
		// the others will be taken on-the-fly in order to sample from fewer
		// textures overall, since sampling from the L1 cache is cheap.
		// (TODO: Verify that this is indeed faster than making separate
		// double-derivative textures.)
		GLuint I_x_y_tex = pool.get_texture(GL_RG16F, level_width, level_height, num_layers);
		GLuint beta_0_tex = pool.get_texture(GL_R16F, level_width, level_height, num_layers);
		{
			ScopedTimer timer("First derivatives", &varref_timer);
			derivatives.exec(I_tex, I_x_y_tex, beta_0_tex, level_width, level_height, num_layers);
		}
		pool.release_texture(I_tex);

		// We need somewhere to store du and dv (the flow increment, relative
		// to the non-refined base flow u0 and v0). It's initially garbage,
		// but not read until we've written something sane to it.
		GLuint diff_flow_tex = pool.get_texture(GL_RG16F, level_width, level_height, num_layers);

		// And for diffusivity.
		GLuint diffusivity_tex = pool.get_texture(GL_R16F, level_width, level_height, num_layers);

		// And finally for the equation set. See SetupEquations for
		// the storage format.
		GLuint equation_red_tex = pool.get_texture(GL_RGBA32UI, (level_width + 1) / 2, level_height, num_layers);
		GLuint equation_black_tex = pool.get_texture(GL_RGBA32UI, (level_width + 1) / 2, level_height, num_layers);

		for (int outer_idx = 0; outer_idx < level + 1; ++outer_idx) {
			// Calculate the diffusivity term for each pixel.
			{
				ScopedTimer timer("Compute diffusivity", &varref_timer);
				compute_diffusivity.exec(base_flow_tex, diff_flow_tex, diffusivity_tex, level_width, level_height, outer_idx == 0, num_layers);
			}

			// Set up the 2x2 equation system for each pixel.
			{
				ScopedTimer timer("Set up equations", &varref_timer);
				setup_equations.exec(I_x_y_tex, I_t_tex, diff_flow_tex, base_flow_tex, beta_0_tex, diffusivity_tex, equation_red_tex, equation_black_tex, level_width, level_height, outer_idx == 0, num_layers);
			}

			// Run a few SOR iterations. Note that these are to/from the same texture.
			{
				ScopedTimer timer("SOR", &varref_timer);
				sor.exec(diff_flow_tex, equation_red_tex, equation_black_tex, diffusivity_tex, level_width, level_height, 5, outer_idx == 0, num_layers, &timer);
			}
		}

		pool.release_texture(I_t_tex);
		pool.release_texture(I_x_y_tex);
		pool.release_texture(beta_0_tex);
		pool.release_texture(diffusivity_tex);
		pool.release_texture(equation_red_tex);
		pool.release_texture(equation_black_tex);

		// Add the differential flow found by the variational refinement to the base flow,
		// giving the final flow estimate for this level.
		// The output is in diff_flow_tex; we don't need to make a new texture.
		//
		// Disabling this doesn't save any time (although we could easily make it so that
		// it is more efficient), but it helps debug the motion search.
		if (op.variational_refinement) {
			ScopedTimer timer("Add differential flow", &varref_timer);
			add_base_flow.exec(base_flow_tex, diff_flow_tex, level_width, level_height, num_layers);
		}
		pool.release_texture(diff_flow_tex);

		if (prev_level_flow_tex != initial_flow_tex) {
			pool.release_texture(prev_level_flow_tex);
		}
		prev_level_flow_tex = base_flow_tex;
		prev_level_width = level_width;
		prev_level_height = level_height;
	}
	total_timer.end();

	if (!in_warmup) {
		timers.print();
	}

	// Scale up the flow to the final size (if needed).
	if (op.finest_level == 0 || resize_strategy == DO_NOT_RESIZE_FLOW) {
		return prev_level_flow_tex;
	} else {
		GLuint final_tex = pool.get_texture(GL_RG16F, width, height, num_layers);
		resize_flow.exec(prev_level_flow_tex, final_tex, prev_level_width, prev_level_height, width, height, num_layers);
		pool.release_texture(prev_level_flow_tex);
		return final_tex;
	}
}

Splat::Splat(const OperatingPoint &op)
	: op(op)
{
	splat_vs_obj = compile_shader(read_file("splat.vert"), GL_VERTEX_SHADER);
	splat_fs_obj = compile_shader(read_file("splat.frag"), GL_FRAGMENT_SHADER);
	splat_program = link_program(splat_vs_obj, splat_fs_obj);

	uniform_splat_size = glGetUniformLocation(splat_program, "splat_size");
	uniform_alpha = glGetUniformLocation(splat_program, "alpha");
	uniform_image_tex = glGetUniformLocation(splat_program, "image_tex");
	uniform_flow_tex = glGetUniformLocation(splat_program, "flow_tex");
	uniform_inv_flow_size = glGetUniformLocation(splat_program, "inv_flow_size");
}

void Splat::exec(GLuint image_tex, GLuint bidirectional_flow_tex, GLuint flow_tex, GLuint depth_rb, int width, int height, float alpha)
{
	glUseProgram(splat_program);

	bind_sampler(splat_program, uniform_image_tex, 0, image_tex, linear_sampler);
	bind_sampler(splat_program, uniform_flow_tex, 1, bidirectional_flow_tex, nearest_sampler);

	glProgramUniform2f(splat_program, uniform_splat_size, op.splat_size / width, op.splat_size / height);
	glProgramUniform1f(splat_program, uniform_alpha, alpha);
	glProgramUniform2f(splat_program, uniform_inv_flow_size, 1.0f / width, 1.0f / height);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);  // We store the difference between I_0 and I_1, where less difference is good. (Default 1.0 is effectively +inf, which always loses.)

	fbos.render_to(depth_rb, flow_tex);

	// Evidently NVIDIA doesn't use fast clears for glClearTexImage, so clear now that
	// we've got it bound.
	glClearColor(1000.0f, 1000.0f, 0.0f, 1.0f);  // Invalid flow.
	glClearDepth(1.0f);  // Effectively infinity.
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width * height * 2);

	glDisable(GL_DEPTH_TEST);
}

HoleFill::HoleFill()
{
	fill_vs_obj = compile_shader(read_file("hole_fill.vert"), GL_VERTEX_SHADER);
	fill_fs_obj = compile_shader(read_file("hole_fill.frag"), GL_FRAGMENT_SHADER);
	fill_program = link_program(fill_vs_obj, fill_fs_obj);

	uniform_tex = glGetUniformLocation(fill_program, "tex");
	uniform_z = glGetUniformLocation(fill_program, "z");
	uniform_sample_offset = glGetUniformLocation(fill_program, "sample_offset");
}

void HoleFill::exec(GLuint flow_tex, GLuint depth_rb, GLuint temp_tex[3], int width, int height)
{
	glUseProgram(fill_program);

	bind_sampler(fill_program, uniform_tex, 0, flow_tex, nearest_sampler);

	glProgramUniform1f(fill_program, uniform_z, 1.0f - 1.0f / 1024.0f);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);  // Only update the values > 0.999f (ie., only invalid pixels).

	fbos.render_to(depth_rb, flow_tex);  // NOTE: Reading and writing to the same texture.

	// Fill holes from the left, by shifting 1, 2, 4, 8, etc. pixels to the right.
	for (int offs = 1; offs < width; offs *= 2) {
		glProgramUniform2f(fill_program, uniform_sample_offset, -offs / float(width), 0.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTextureBarrier();
	}
	glCopyImageSubData(flow_tex, GL_TEXTURE_2D, 0, 0, 0, 0, temp_tex[0], GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);

	// Similar to the right; adjust Z a bit down, so that we re-fill the pixels that
	// were overwritten in the last algorithm.
	glProgramUniform1f(fill_program, uniform_z, 1.0f - 2.0f / 1024.0f);
	for (int offs = 1; offs < width; offs *= 2) {
		glProgramUniform2f(fill_program, uniform_sample_offset, offs / float(width), 0.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTextureBarrier();
	}
	glCopyImageSubData(flow_tex, GL_TEXTURE_2D, 0, 0, 0, 0, temp_tex[1], GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);

	// Up.
	glProgramUniform1f(fill_program, uniform_z, 1.0f - 3.0f / 1024.0f);
	for (int offs = 1; offs < height; offs *= 2) {
		glProgramUniform2f(fill_program, uniform_sample_offset, 0.0f, -offs / float(height));
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTextureBarrier();
	}
	glCopyImageSubData(flow_tex, GL_TEXTURE_2D, 0, 0, 0, 0, temp_tex[2], GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);

	// Down.
	glProgramUniform1f(fill_program, uniform_z, 1.0f - 4.0f / 1024.0f);
	for (int offs = 1; offs < height; offs *= 2) {
		glProgramUniform2f(fill_program, uniform_sample_offset, 0.0f, offs / float(height));
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTextureBarrier();
	}

	glDisable(GL_DEPTH_TEST);
}

HoleBlend::HoleBlend()
{
	blend_vs_obj = compile_shader(read_file("hole_fill.vert"), GL_VERTEX_SHADER);  // Reuse the vertex shader from the fill.
	blend_fs_obj = compile_shader(read_file("hole_blend.frag"), GL_FRAGMENT_SHADER);
	blend_program = link_program(blend_vs_obj, blend_fs_obj);

	uniform_left_tex = glGetUniformLocation(blend_program, "left_tex");
	uniform_right_tex = glGetUniformLocation(blend_program, "right_tex");
	uniform_up_tex = glGetUniformLocation(blend_program, "up_tex");
	uniform_down_tex = glGetUniformLocation(blend_program, "down_tex");
	uniform_z = glGetUniformLocation(blend_program, "z");
	uniform_sample_offset = glGetUniformLocation(blend_program, "sample_offset");
}

void HoleBlend::exec(GLuint flow_tex, GLuint depth_rb, GLuint temp_tex[3], int width, int height)
{
	glUseProgram(blend_program);

	bind_sampler(blend_program, uniform_left_tex, 0, temp_tex[0], nearest_sampler);
	bind_sampler(blend_program, uniform_right_tex, 1, temp_tex[1], nearest_sampler);
	bind_sampler(blend_program, uniform_up_tex, 2, temp_tex[2], nearest_sampler);
	bind_sampler(blend_program, uniform_down_tex, 3, flow_tex, nearest_sampler);

	glProgramUniform1f(blend_program, uniform_z, 1.0f - 4.0f / 1024.0f);
	glProgramUniform2f(blend_program, uniform_sample_offset, 0.0f, 0.0f);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);  // Skip over all of the pixels that were never holes to begin with.

	fbos.render_to(depth_rb, flow_tex);  // NOTE: Reading and writing to the same texture.

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisable(GL_DEPTH_TEST);
}

Blend::Blend()
{
	blend_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	blend_fs_obj = compile_shader(read_file("blend.frag"), GL_FRAGMENT_SHADER);
	blend_program = link_program(blend_vs_obj, blend_fs_obj);

	uniform_image_tex = glGetUniformLocation(blend_program, "image_tex");
	uniform_flow_tex = glGetUniformLocation(blend_program, "flow_tex");
	uniform_alpha = glGetUniformLocation(blend_program, "alpha");
	uniform_flow_consistency_tolerance = glGetUniformLocation(blend_program, "flow_consistency_tolerance");
}

void Blend::exec(GLuint image_tex, GLuint flow_tex, GLuint output_tex, int level_width, int level_height, float alpha)
{
	glUseProgram(blend_program);
	bind_sampler(blend_program, uniform_image_tex, 0, image_tex, linear_sampler);
	bind_sampler(blend_program, uniform_flow_tex, 1, flow_tex, linear_sampler);  // May be upsampled.
	glProgramUniform1f(blend_program, uniform_alpha, alpha);

	glViewport(0, 0, level_width, level_height);
	fbos.render_to(output_tex);
	glDisable(GL_BLEND);  // A bit ironic, perhaps.
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

Interpolate::Interpolate(int width, int height, const OperatingPoint &op)
	: width(width), height(height), flow_level(op.finest_level), op(op), splat(op) {
	// Set up the vertex data that will be shared between all passes.
	float vertices[] = {
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
	};
	glCreateBuffers(1, &vertex_vbo);
	glNamedBufferData(vertex_vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glCreateVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = 0;  // Hard-coded in every vertex shader.
	glEnableVertexArrayAttrib(vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
}

GLuint Interpolate::exec(GLuint image_tex, GLuint bidirectional_flow_tex, GLuint width, GLuint height, float alpha)
{
	GPUTimers timers;

	ScopedTimer total_timer("Interpolate", &timers);

	glBindVertexArray(vao);

	// Pick out the right level to test splatting results on.
	GLuint tex_view;
	glGenTextures(1, &tex_view);
	glTextureView(tex_view, GL_TEXTURE_2D_ARRAY, image_tex, GL_RGBA8, flow_level, 1, 0, 2);

	int flow_width = width >> flow_level;
	int flow_height = height >> flow_level;

	GLuint flow_tex = pool.get_texture(GL_RG16F, flow_width, flow_height);
	GLuint depth_rb = pool.get_renderbuffer(GL_DEPTH_COMPONENT16, flow_width, flow_height);  // Used for ranking flows.

	{
		ScopedTimer timer("Splat", &total_timer);
		splat.exec(tex_view, bidirectional_flow_tex, flow_tex, depth_rb, flow_width, flow_height, alpha);
	}
	glDeleteTextures(1, &tex_view);

	GLuint temp_tex[3];
	temp_tex[0] = pool.get_texture(GL_RG16F, flow_width, flow_height);
	temp_tex[1] = pool.get_texture(GL_RG16F, flow_width, flow_height);
	temp_tex[2] = pool.get_texture(GL_RG16F, flow_width, flow_height);

	{
		ScopedTimer timer("Fill holes", &total_timer);
		hole_fill.exec(flow_tex, depth_rb, temp_tex, flow_width, flow_height);
		hole_blend.exec(flow_tex, depth_rb, temp_tex, flow_width, flow_height);
	}

	pool.release_texture(temp_tex[0]);
	pool.release_texture(temp_tex[1]);
	pool.release_texture(temp_tex[2]);
	pool.release_renderbuffer(depth_rb);

	GLuint output_tex = pool.get_texture(GL_RGBA8, width, height);
	{
		ScopedTimer timer("Blend", &total_timer);
		blend.exec(image_tex, flow_tex, output_tex, width, height, alpha);
	}
	pool.release_texture(flow_tex);
	total_timer.end();
	if (!in_warmup) {
		timers.print();
	}

	return output_tex;
}

GLuint TexturePool::get_texture(GLenum format, GLuint width, GLuint height, GLuint num_layers)
{
	for (Texture &tex : textures) {
		if (!tex.in_use && !tex.is_renderbuffer && tex.format == format &&
		    tex.width == width && tex.height == height && tex.num_layers == num_layers) {
			tex.in_use = true;
			return tex.tex_num;
		}
	}

	Texture tex;
	if (num_layers == 0) {
		glCreateTextures(GL_TEXTURE_2D, 1, &tex.tex_num);
		glTextureStorage2D(tex.tex_num, 1, format, width, height);
	} else {
		glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex.tex_num);
		glTextureStorage3D(tex.tex_num, 1, format, width, height, num_layers);
	}
	tex.format = format;
	tex.width = width;
	tex.height = height;
	tex.num_layers = num_layers;
	tex.in_use = true;
	tex.is_renderbuffer = false;
	textures.push_back(tex);
	return tex.tex_num;
}

GLuint TexturePool::get_renderbuffer(GLenum format, GLuint width, GLuint height)
{
	for (Texture &tex : textures) {
		if (!tex.in_use && tex.is_renderbuffer && tex.format == format &&
		    tex.width == width && tex.height == height) {
			tex.in_use = true;
			return tex.tex_num;
		}
	}

	Texture tex;
	glCreateRenderbuffers(1, &tex.tex_num);
	glNamedRenderbufferStorage(tex.tex_num, format, width, height);

	tex.format = format;
	tex.width = width;
	tex.height = height;
	tex.in_use = true;
	tex.is_renderbuffer = true;
	textures.push_back(tex);
	return tex.tex_num;
}

void TexturePool::release_texture(GLuint tex_num)
{
	for (Texture &tex : textures) {
		if (!tex.is_renderbuffer && tex.tex_num == tex_num) {
			assert(tex.in_use);
			tex.in_use = false;
			return;
		}
	}
	assert(false);
}

void TexturePool::release_renderbuffer(GLuint tex_num)
{
	for (Texture &tex : textures) {
		if (tex.is_renderbuffer && tex.tex_num == tex_num) {
			assert(tex.in_use);
			tex.in_use = false;
			return;
		}
	}
	//assert(false);
}

// OpenGL uses a bottom-left coordinate system, .flo files use a top-left coordinate system.
void flip_coordinate_system(float *dense_flow, unsigned width, unsigned height)
{
	for (unsigned i = 0; i < width * height; ++i) {
		dense_flow[i * 2 + 1] = -dense_flow[i * 2 + 1];
	}
}

// Not relevant for RGB.
void flip_coordinate_system(uint8_t *dense_flow, unsigned width, unsigned height)
{
}

void write_flow(const char *filename, const float *dense_flow, unsigned width, unsigned height)
{
	FILE *flowfp = fopen(filename, "wb");
	fprintf(flowfp, "FEIH");
	fwrite(&width, 4, 1, flowfp);
	fwrite(&height, 4, 1, flowfp);
	for (unsigned y = 0; y < height; ++y) {
		int yy = height - y - 1;
		fwrite(&dense_flow[yy * width * 2], width * 2 * sizeof(float), 1, flowfp);
	}
	fclose(flowfp);
}

// Not relevant for RGB.
void write_flow(const char *filename, const uint8_t *dense_flow, unsigned width, unsigned height)
{
	assert(false);
}

void write_ppm(const char *filename, const float *dense_flow, unsigned width, unsigned height)
{
	FILE *fp = fopen(filename, "wb");
	fprintf(fp, "P6\n%d %d\n255\n", width, height);
	for (unsigned y = 0; y < unsigned(height); ++y) {
		int yy = height - y - 1;
		for (unsigned x = 0; x < unsigned(width); ++x) {
			float du = dense_flow[(yy * width + x) * 2 + 0];
			float dv = dense_flow[(yy * width + x) * 2 + 1];

			uint8_t r, g, b;
			flow2rgb(du, dv, &r, &g, &b);
			putc(r, fp);
			putc(g, fp);
			putc(b, fp);
		}
	}
	fclose(fp);
}

void write_ppm(const char *filename, const uint8_t *rgba, unsigned width, unsigned height)
{
	unique_ptr<uint8_t[]> rgb_line(new uint8_t[width * 3 + 1]);

	FILE *fp = fopen(filename, "wb");
	fprintf(fp, "P6\n%d %d\n255\n", width, height);
	for (unsigned y = 0; y < height; ++y) {
		unsigned y2 = height - 1 - y;
		for (size_t x = 0; x < width; ++x) {
			memcpy(&rgb_line[x * 3], &rgba[(y2 * width + x) * 4], 4);
		}
		fwrite(rgb_line.get(), width * 3, 1, fp);
	}
	fclose(fp);
}

struct FlowType {
	using type = float;
	static constexpr GLenum gl_format = GL_RG;
	static constexpr GLenum gl_type = GL_FLOAT;
	static constexpr int num_channels = 2;
};

struct RGBAType {
	using type = uint8_t;
	static constexpr GLenum gl_format = GL_RGBA;
	static constexpr GLenum gl_type = GL_UNSIGNED_BYTE;
	static constexpr int num_channels = 4;
};

template <class Type>
void finish_one_read(GLuint width, GLuint height)
{
	using T = typename Type::type;
	constexpr int bytes_per_pixel = Type::num_channels * sizeof(T);

	assert(!reads_in_progress.empty());
	ReadInProgress read = reads_in_progress.front();
	reads_in_progress.pop_front();

	unique_ptr<T[]> flow(new typename Type::type[width * height * Type::num_channels]);
	void *buf = glMapNamedBufferRange(read.pbo, 0, width * height * bytes_per_pixel, GL_MAP_READ_BIT);  // Blocks if the read isn't done yet.
	memcpy(flow.get(), buf, width * height * bytes_per_pixel);  // TODO: Unneeded for RGBType, since flip_coordinate_system() does nothing.:
	glUnmapNamedBuffer(read.pbo);
	spare_pbos.push(read.pbo);

	flip_coordinate_system(flow.get(), width, height);
	if (!read.flow_filename.empty()) {
		write_flow(read.flow_filename.c_str(), flow.get(), width, height);
		fprintf(stderr, "%s %s -> %s\n", read.filename0.c_str(), read.filename1.c_str(), read.flow_filename.c_str());
	}
	if (!read.ppm_filename.empty()) {
		write_ppm(read.ppm_filename.c_str(), flow.get(), width, height);
	}
}

template <class Type>
void schedule_read(GLuint tex, GLuint width, GLuint height, const char *filename0, const char *filename1, const char *flow_filename, const char *ppm_filename)
{
	using T = typename Type::type;
	constexpr int bytes_per_pixel = Type::num_channels * sizeof(T);

	if (spare_pbos.empty()) {
		finish_one_read<Type>(width, height);
	}
	assert(!spare_pbos.empty());
	reads_in_progress.emplace_back(ReadInProgress{ spare_pbos.top(), filename0, filename1, flow_filename, ppm_filename });
	glBindBuffer(GL_PIXEL_PACK_BUFFER, spare_pbos.top());
	spare_pbos.pop();
	glGetTextureImage(tex, 0, Type::gl_format, Type::gl_type, width * height * bytes_per_pixel, nullptr);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void compute_flow_only(int argc, char **argv, int optind)
{
	const char *filename0 = argc >= (optind + 1) ? argv[optind] : "test1499.png";
	const char *filename1 = argc >= (optind + 2) ? argv[optind + 1] : "test1500.png";
	const char *flow_filename = argc >= (optind + 3) ? argv[optind + 2] : "flow.flo";

	// Load pictures.
	unsigned width1, height1, width2, height2;
	GLuint tex0 = load_texture(filename0, &width1, &height1, WITHOUT_MIPMAPS);
	GLuint tex1 = load_texture(filename1, &width2, &height2, WITHOUT_MIPMAPS);

	if (width1 != width2 || height1 != height2) {
		fprintf(stderr, "Image dimensions don't match (%dx%d versus %dx%d)\n",
			width1, height1, width2, height2);
		exit(1);
	}

	// Move them into an array texture, since that's how the rest of the code
	// would like them.
	GLuint image_tex;
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &image_tex);
	glTextureStorage3D(image_tex, 1, GL_RGBA8, width1, height1, 2);
	glCopyImageSubData(tex0, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, width1, height1, 1);
	glCopyImageSubData(tex1, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, width1, height1, 1);
	glDeleteTextures(1, &tex0);
	glDeleteTextures(1, &tex1);

	// Set up some PBOs to do asynchronous readback.
	GLuint pbos[5];
	glCreateBuffers(5, pbos);
	for (int i = 0; i < 5; ++i) {
		glNamedBufferData(pbos[i], width1 * height1 * 2 * 2 * sizeof(float), nullptr, GL_STREAM_READ);
		spare_pbos.push(pbos[i]);
	}

	int levels = find_num_levels(width1, height1);

	GLuint tex_gray;
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex_gray);
	glTextureStorage3D(tex_gray, levels, GL_R8, width1, height1, 2);

	GrayscaleConversion gray;
	gray.exec(image_tex, tex_gray, width1, height1, /*num_layers=*/2);
	glGenerateTextureMipmap(tex_gray);

	OperatingPoint op = operating_point3;
	if (!enable_variational_refinement) {
		op.variational_refinement = false;
	}
	DISComputeFlow compute_flow(width1, height1, op);

	if (enable_warmup) {
		in_warmup = true;
		for (int i = 0; i < 10; ++i) {
			GLuint final_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);
			compute_flow.release_texture(final_tex);
		}
		in_warmup = false;
	}

	GLuint final_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);
	//GLuint final_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);

	schedule_read<FlowType>(final_tex, width1, height1, filename0, filename1, flow_filename, "flow.ppm");
	compute_flow.release_texture(final_tex);

	// See if there are more flows on the command line (ie., more than three arguments),
	// and if so, process them.
	int num_flows = (argc - optind) / 3;
	for (int i = 1; i < num_flows; ++i) {
		const char *filename0 = argv[optind + i * 3 + 0];
		const char *filename1 = argv[optind + i * 3 + 1];
		const char *flow_filename = argv[optind + i * 3 + 2];
		GLuint width, height;
		GLuint tex0 = load_texture(filename0, &width, &height, WITHOUT_MIPMAPS);
		if (width != width1 || height != height1) {
			fprintf(stderr, "%s: Image dimensions don't match (%dx%d versus %dx%d)\n",
				filename0, width, height, width1, height1);
			exit(1);
		}
		glCopyImageSubData(tex0, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, width1, height1, 1);
		glDeleteTextures(1, &tex0);

		GLuint tex1 = load_texture(filename1, &width, &height, WITHOUT_MIPMAPS);
		if (width != width1 || height != height1) {
			fprintf(stderr, "%s: Image dimensions don't match (%dx%d versus %dx%d)\n",
				filename1, width, height, width1, height1);
			exit(1);
		}
		glCopyImageSubData(tex1, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, width1, height1, 1);
		glDeleteTextures(1, &tex1);

		gray.exec(image_tex, tex_gray, width1, height1, /*num_layers=*/2);
		glGenerateTextureMipmap(tex_gray);

		GLuint final_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);

		schedule_read<FlowType>(final_tex, width1, height1, filename0, filename1, flow_filename, "");
		compute_flow.release_texture(final_tex);
	}
	glDeleteTextures(1, &tex_gray);

	while (!reads_in_progress.empty()) {
		finish_one_read<FlowType>(width1, height1);
	}
}

// Interpolate images based on
//
//   Herbst, Seitz, Baker: “Occlusion Reasoning for Temporal Interpolation
//   Using Optical Flow”
//
// or at least a reasonable subset thereof. Unfinished.
void interpolate_image(int argc, char **argv, int optind)
{
	const char *filename0 = argc >= (optind + 1) ? argv[optind] : "test1499.png";
	const char *filename1 = argc >= (optind + 2) ? argv[optind + 1] : "test1500.png";
	//const char *out_filename = argc >= (optind + 3) ? argv[optind + 2] : "interpolated.png";

	// Load pictures.
	unsigned width1, height1, width2, height2;
	GLuint tex0 = load_texture(filename0, &width1, &height1, WITH_MIPMAPS);
	GLuint tex1 = load_texture(filename1, &width2, &height2, WITH_MIPMAPS);

	if (width1 != width2 || height1 != height2) {
		fprintf(stderr, "Image dimensions don't match (%dx%d versus %dx%d)\n",
			width1, height1, width2, height2);
		exit(1);
	}

	// Move them into an array texture, since that's how the rest of the code
	// would like them.
	int levels = find_num_levels(width1, height1);
	GLuint image_tex;
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &image_tex);
	glTextureStorage3D(image_tex, levels, GL_RGBA8, width1, height1, 2);
	glCopyImageSubData(tex0, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, width1, height1, 1);
	glCopyImageSubData(tex1, GL_TEXTURE_2D, 0, 0, 0, 0, image_tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, width1, height1, 1);
	glDeleteTextures(1, &tex0);
	glDeleteTextures(1, &tex1);
	glGenerateTextureMipmap(image_tex);

	// Set up some PBOs to do asynchronous readback.
	GLuint pbos[5];
	glCreateBuffers(5, pbos);
	for (int i = 0; i < 5; ++i) {
		glNamedBufferData(pbos[i], width1 * height1 * 4 * sizeof(uint8_t), nullptr, GL_STREAM_READ);
		spare_pbos.push(pbos[i]);
	}

	OperatingPoint op = operating_point3;
	if (!enable_variational_refinement) {
		op.variational_refinement = false;
	}
	DISComputeFlow compute_flow(width1, height1, op);
	GrayscaleConversion gray;
	Interpolate interpolate(width1, height1, op);

	GLuint tex_gray;
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex_gray);
	glTextureStorage3D(tex_gray, levels, GL_R8, width1, height1, 2);
	gray.exec(image_tex, tex_gray, width1, height1, /*num_layers=*/2);
	glGenerateTextureMipmap(tex_gray);

	if (enable_warmup) {
		in_warmup = true;
		for (int i = 0; i < 10; ++i) {
			GLuint bidirectional_flow_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::DO_NOT_RESIZE_FLOW);
			GLuint interpolated_tex = interpolate.exec(image_tex, bidirectional_flow_tex, width1, height1, 0.5f);
			compute_flow.release_texture(bidirectional_flow_tex);
			interpolate.release_texture(interpolated_tex);
		}
		in_warmup = false;
	}

	GLuint bidirectional_flow_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::DO_NOT_RESIZE_FLOW);

	for (int frameno = 1; frameno < 60; ++frameno) {
		char ppm_filename[256];
		snprintf(ppm_filename, sizeof(ppm_filename), "interp%04d.ppm", frameno);

		float alpha = frameno / 60.0f;
		GLuint interpolated_tex = interpolate.exec(image_tex, bidirectional_flow_tex, width1, height1, alpha);

		schedule_read<RGBAType>(interpolated_tex, width1, height1, filename0, filename1, "", ppm_filename);
		interpolate.release_texture(interpolated_tex);
	}

	while (!reads_in_progress.empty()) {
		finish_one_read<RGBAType>(width1, height1);
	}
}

int main(int argc, char **argv)
{
        static const option long_options[] = {
		{ "smoothness-relative-weight", required_argument, 0, 's' },  // alpha.
		{ "intensity-relative-weight", required_argument, 0, 'i' },  // delta.
		{ "gradient-relative-weight", required_argument, 0, 'g' },  // gamma.
		{ "disable-timing", no_argument, 0, 1000 },
		{ "detailed-timing", no_argument, 0, 1003 },
		{ "ignore-variational-refinement", no_argument, 0, 1001 },  // Still calculates it, just doesn't apply it.
		{ "interpolate", no_argument, 0, 1002 },
		{ "warmup", no_argument, 0, 1004 }
	};

	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "s:i:g:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 's':
			vr_alpha = atof(optarg);
			break;
		case 'i':
			vr_delta = atof(optarg);
			break;
		case 'g':
			vr_gamma = atof(optarg);
			break;
		case 1000:
			enable_timing = false;
			break;
		case 1001:
			enable_variational_refinement = false;
			break;
		case 1002:
			enable_interpolation = true;
			break;
		case 1003:
			detailed_timing = true;
			break;
		case 1004:
			enable_warmup = true;
			break;
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			exit(1);
		};
	}

	if (SDL_Init(SDL_INIT_EVERYTHING) == -1) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
	// SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
	window = SDL_CreateWindow("OpenGL window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		64, 64,
		SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	SDL_GLContext context = SDL_GL_CreateContext(window);
	assert(context != nullptr);

	glDisable(GL_DITHER);

	// FIXME: Should be part of DISComputeFlow (but needs to be initialized
	// before all the render passes).
	float vertices[] = {
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
	};
	glCreateBuffers(1, &vertex_vbo);
	glNamedBufferData(vertex_vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	if (enable_interpolation) {
		interpolate_image(argc, argv, optind);
	} else {
		compute_flow_only(argc, argv, optind);
	}
}
