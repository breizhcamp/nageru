#define NO_SDL_GLEXT 1

#include "flow.h"

#include "embedded_files.h"
#include "gpu_timers.h"
#include "shared/read_file.h"
#include "util.h"

#include <algorithm>
#include <assert.h>
#include <deque>
#include <dlfcn.h>
#include <epoxy/gl.h>
#include <map>
#include <memory>
#include <stack>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

// Weighting constants for the different parts of the variational refinement.
// These don't correspond 1:1 to the values given in the DIS paper,
// since we have different normalizations and ranges in some cases.
// These are found through a simple grid search on some MPI-Sintel data,
// although the error (EPE) seems to be fairly insensitive to the precise values.
// Only the relative values matter, so we fix alpha (the smoothness constant)
// at unity and tweak the others.
//
// TODO: Maybe this should not be global.
float vr_alpha = 1.0f, vr_delta = 0.25f, vr_gamma = 0.25f;

// Some global OpenGL objects.
// TODO: These should really be part of DISComputeFlow.
GLuint nearest_sampler, linear_sampler, zero_border_sampler;
GLuint vertex_vbo;

int find_num_levels(int width, int height)
{
	int levels = 1;
	for (int w = width, h = height; w > 1 || h > 1;) {
		w >>= 1;
		h >>= 1;
		++levels;
	}
	return levels;
}

GLuint compile_shader(const string &shader_src, GLenum type)
{
	GLuint obj = glCreateShader(type);
	const GLchar *source[] = { shader_src.data() };
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

GLuint link_program(GLuint vs_obj, GLuint fs_obj)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, vs_obj);
	glAttachShader(program, fs_obj);
	glLinkProgram(program);
	GLint success;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (success == GL_FALSE) {
		GLchar error_log[1024] = { 0 };
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
	gray_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	gray_fs_obj = compile_shader(read_file("gray.frag", _binary_gray_frag_data, _binary_gray_frag_size), GL_FRAGMENT_SHADER);
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
	sobel_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	sobel_fs_obj = compile_shader(read_file("sobel.frag", _binary_sobel_frag_data, _binary_sobel_frag_size), GL_FRAGMENT_SHADER);
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
	motion_vs_obj = compile_shader(read_file("motion_search.vert", _binary_motion_search_vert_data, _binary_motion_search_vert_size), GL_VERTEX_SHADER);
	motion_fs_obj = compile_shader(read_file("motion_search.frag", _binary_motion_search_frag_data, _binary_motion_search_frag_size), GL_FRAGMENT_SHADER);
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
	densify_vs_obj = compile_shader(read_file("densify.vert", _binary_densify_vert_data, _binary_densify_vert_size), GL_VERTEX_SHADER);
	densify_fs_obj = compile_shader(read_file("densify.frag", _binary_densify_frag_data, _binary_densify_frag_size), GL_FRAGMENT_SHADER);
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
	prewarp_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	prewarp_fs_obj = compile_shader(read_file("prewarp.frag", _binary_prewarp_frag_data, _binary_prewarp_frag_size), GL_FRAGMENT_SHADER);
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
	derivatives_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	derivatives_fs_obj = compile_shader(read_file("derivatives.frag", _binary_derivatives_frag_data, _binary_derivatives_frag_size), GL_FRAGMENT_SHADER);
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
	diffusivity_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	diffusivity_fs_obj = compile_shader(read_file("diffusivity.frag", _binary_diffusivity_frag_data, _binary_diffusivity_frag_size), GL_FRAGMENT_SHADER);
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
	equations_vs_obj = compile_shader(read_file("equations.vert", _binary_equations_vert_data, _binary_equations_vert_size), GL_VERTEX_SHADER);
	equations_fs_obj = compile_shader(read_file("equations.frag", _binary_equations_frag_data, _binary_equations_frag_size), GL_FRAGMENT_SHADER);
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
	sor_vs_obj = compile_shader(read_file("sor.vert", _binary_sor_vert_data, _binary_sor_vert_size), GL_VERTEX_SHADER);
	sor_fs_obj = compile_shader(read_file("sor.frag", _binary_sor_frag_data, _binary_sor_frag_size), GL_FRAGMENT_SHADER);
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
	add_flow_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	add_flow_fs_obj = compile_shader(read_file("add_base_flow.frag", _binary_add_base_flow_frag_data, _binary_add_base_flow_frag_size), GL_FRAGMENT_SHADER);
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
	resize_flow_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	resize_flow_fs_obj = compile_shader(read_file("resize_flow.frag", _binary_resize_flow_frag_data, _binary_resize_flow_frag_size), GL_FRAGMENT_SHADER);
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
	glDisable(GL_DITHER);

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

		// TODO: If we don't have variational refinement, we don't need I and I_t,
		// so computing them is a waste.
		if (op.variational_refinement) {
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
			// The output is in base_flow_tex; we don't need to make a new texture.
			{
				ScopedTimer timer("Add differential flow", &varref_timer);
				add_base_flow.exec(base_flow_tex, diff_flow_tex, level_width, level_height, num_layers);
			}
			pool.release_texture(diff_flow_tex);
		}

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
	splat_vs_obj = compile_shader(read_file("splat.vert", _binary_splat_vert_data, _binary_splat_vert_size), GL_VERTEX_SHADER);
	splat_fs_obj = compile_shader(read_file("splat.frag", _binary_splat_frag_data, _binary_splat_frag_size), GL_FRAGMENT_SHADER);
	splat_program = link_program(splat_vs_obj, splat_fs_obj);

	uniform_splat_size = glGetUniformLocation(splat_program, "splat_size");
	uniform_alpha = glGetUniformLocation(splat_program, "alpha");
	uniform_gray_tex = glGetUniformLocation(splat_program, "gray_tex");
	uniform_flow_tex = glGetUniformLocation(splat_program, "flow_tex");
	uniform_inv_flow_size = glGetUniformLocation(splat_program, "inv_flow_size");
}

void Splat::exec(GLuint gray_tex, GLuint bidirectional_flow_tex, GLuint flow_tex, GLuint depth_rb, int width, int height, float alpha)
{
	glUseProgram(splat_program);

	bind_sampler(splat_program, uniform_gray_tex, 0, gray_tex, linear_sampler);
	bind_sampler(splat_program, uniform_flow_tex, 1, bidirectional_flow_tex, nearest_sampler);

	glProgramUniform2f(splat_program, uniform_splat_size, op.splat_size / width, op.splat_size / height);
	glProgramUniform1f(splat_program, uniform_alpha, alpha);
	glProgramUniform2f(splat_program, uniform_inv_flow_size, 1.0f / width, 1.0f / height);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
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
	fill_vs_obj = compile_shader(read_file("hole_fill.vert", _binary_hole_fill_vert_data, _binary_hole_fill_vert_size), GL_VERTEX_SHADER);
	fill_fs_obj = compile_shader(read_file("hole_fill.frag", _binary_hole_fill_frag_data, _binary_hole_fill_frag_size), GL_FRAGMENT_SHADER);
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
	blend_vs_obj = compile_shader(read_file("hole_fill.vert", _binary_hole_fill_vert_data, _binary_hole_fill_vert_size), GL_VERTEX_SHADER);  // Reuse the vertex shader from the fill.
	blend_fs_obj = compile_shader(read_file("hole_blend.frag", _binary_hole_blend_frag_data, _binary_hole_blend_frag_size), GL_FRAGMENT_SHADER);
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

Blend::Blend(bool split_ycbcr_output)
	: split_ycbcr_output(split_ycbcr_output)
{
	string frag_shader = read_file("blend.frag", _binary_blend_frag_data, _binary_blend_frag_size);
	if (split_ycbcr_output) {
		// Insert after the first #version line.
		size_t offset = frag_shader.find('\n');
		assert(offset != string::npos);
		frag_shader = frag_shader.substr(0, offset + 1) + "#define SPLIT_YCBCR_OUTPUT 1\n" + frag_shader.substr(offset + 1);
	}

	blend_vs_obj = compile_shader(read_file("vs.vert", _binary_vs_vert_data, _binary_vs_vert_size), GL_VERTEX_SHADER);
	blend_fs_obj = compile_shader(frag_shader, GL_FRAGMENT_SHADER);
	blend_program = link_program(blend_vs_obj, blend_fs_obj);

	uniform_image_tex = glGetUniformLocation(blend_program, "image_tex");
	uniform_flow_tex = glGetUniformLocation(blend_program, "flow_tex");
	uniform_alpha = glGetUniformLocation(blend_program, "alpha");
	uniform_flow_consistency_tolerance = glGetUniformLocation(blend_program, "flow_consistency_tolerance");
}

void Blend::exec(GLuint image_tex, GLuint flow_tex, GLuint output_tex, GLuint output2_tex, int level_width, int level_height, float alpha)
{
	glUseProgram(blend_program);
	bind_sampler(blend_program, uniform_image_tex, 0, image_tex, linear_sampler);
	bind_sampler(blend_program, uniform_flow_tex, 1, flow_tex, linear_sampler);  // May be upsampled.
	glProgramUniform1f(blend_program, uniform_alpha, alpha);

	glViewport(0, 0, level_width, level_height);
	if (split_ycbcr_output) {
		fbos_split.render_to(output_tex, output2_tex);
	} else {
		fbos.render_to(output_tex);
	}
	glDisable(GL_BLEND);  // A bit ironic, perhaps.
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

Interpolate::Interpolate(const OperatingPoint &op, bool split_ycbcr_output)
	: flow_level(op.finest_level),
	  split_ycbcr_output(split_ycbcr_output),
	  splat(op),
	  blend(split_ycbcr_output)
{
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

pair<GLuint, GLuint> Interpolate::exec(GLuint image_tex, GLuint gray_tex, GLuint bidirectional_flow_tex, GLuint width, GLuint height, float alpha)
{
	GPUTimers timers;

	ScopedTimer total_timer("Interpolate", &timers);

	glBindVertexArray(vao);
	glDisable(GL_DITHER);

	// Pick out the right level to test splatting results on.
	GLuint tex_view;
	glGenTextures(1, &tex_view);
	glTextureView(tex_view, GL_TEXTURE_2D_ARRAY, gray_tex, GL_R8, flow_level, 1, 0, 2);

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

	GLuint output_tex, output2_tex = 0;
	if (split_ycbcr_output) {
		output_tex = pool.get_texture(GL_R8, width, height);
		output2_tex = pool.get_texture(GL_RG8, width, height);
		{
			ScopedTimer timer("Blend", &total_timer);
			blend.exec(image_tex, flow_tex, output_tex, output2_tex, width, height, alpha);
		}
	} else {
		output_tex = pool.get_texture(GL_RGBA8, width, height);
		{
			ScopedTimer timer("Blend", &total_timer);
			blend.exec(image_tex, flow_tex, output_tex, 0, width, height, alpha);
		}
	}
	pool.release_texture(flow_tex);
	total_timer.end();
	if (!in_warmup) {
		timers.print();
	}

	return make_pair(output_tex, output2_tex);
}

GLuint TexturePool::get_texture(GLenum format, GLuint width, GLuint height, GLuint num_layers)
{
	{
		lock_guard<mutex> lock(mu);
		for (Texture &tex : textures) {
			if (!tex.in_use && !tex.is_renderbuffer && tex.format == format &&
			    tex.width == width && tex.height == height && tex.num_layers == num_layers) {
				tex.in_use = true;
				return tex.tex_num;
			}
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
	{
		lock_guard<mutex> lock(mu);
		textures.push_back(tex);
	}
	return tex.tex_num;
}

GLuint TexturePool::get_renderbuffer(GLenum format, GLuint width, GLuint height)
{
	{
		lock_guard<mutex> lock(mu);
		for (Texture &tex : textures) {
			if (!tex.in_use && tex.is_renderbuffer && tex.format == format &&
			    tex.width == width && tex.height == height) {
				tex.in_use = true;
				return tex.tex_num;
			}
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
	{
		lock_guard<mutex> lock(mu);
		textures.push_back(tex);
	}
	return tex.tex_num;
}

void TexturePool::release_texture(GLuint tex_num)
{
	lock_guard<mutex> lock(mu);
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
	lock_guard<mutex> lock(mu);
	for (Texture &tex : textures) {
		if (tex.is_renderbuffer && tex.tex_num == tex_num) {
			assert(tex.in_use);
			tex.in_use = false;
			return;
		}
	}
	//assert(false);
}
