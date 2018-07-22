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
#include <stdio.h>
#include <unistd.h>

#include "util.h"

#include <algorithm>
#include <memory>
#include <vector>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

// Operating point 3 (10 Hz on CPU, excluding preprocessing).
constexpr float patch_overlap_ratio = 0.75f;
constexpr unsigned coarsest_level = 5;
constexpr unsigned finest_level = 1;
constexpr unsigned patch_size_pixels = 12;

// Weighting constants for the different parts of the variational refinement.
// These don't correspond 1:1 to the values given in the DIS paper,
// since we have different normalizations and ranges in some cases.
float vr_gamma = 10.0f, vr_delta = 5.0f, vr_alpha = 10.0f;

// Some global OpenGL objects.
GLuint nearest_sampler, linear_sampler, smoothness_sampler;
GLuint vertex_vbo;

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

GLuint load_texture(const char *filename, unsigned *width_ret, unsigned *height_ret)
{
	SDL_Surface *surf = IMG_Load(filename);
	if (surf == nullptr) {
		fprintf(stderr, "IMG_Load(%s): %s\n", filename, IMG_GetError());
		exit(1);
	}

	// For whatever reason, SDL doesn't support converting to YUV surfaces
	// nor grayscale, so we'll do it (slowly) ourselves.
	SDL_Surface *rgb_surf = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA8888, /*flags=*/0);
	if (rgb_surf == nullptr) {
		fprintf(stderr, "SDL_ConvertSurfaceFormat(%s): %s\n", filename, SDL_GetError());
		exit(1);
	}

	SDL_FreeSurface(surf);

	unsigned width = rgb_surf->w, height = rgb_surf->h;
	const uint8_t *sptr = (uint8_t *)rgb_surf->pixels;
	unique_ptr<uint8_t[]> pix(new uint8_t[width * height]);

	// Extract the Y component, and convert to bottom-left origin.
	for (unsigned y = 0; y < height; ++y) {
		unsigned y2 = height - 1 - y;
		for (unsigned x = 0; x < width; ++x) {
			uint8_t r = sptr[(y2 * width + x) * 4 + 3];
			uint8_t g = sptr[(y2 * width + x) * 4 + 2];
			uint8_t b = sptr[(y2 * width + x) * 4 + 1];

			// Rec. 709.
			pix[y * width + x] = lrintf(r * 0.2126f + g * 0.7152f + b * 0.0722f);
		}
	}
	SDL_FreeSurface(rgb_surf);

	int levels = 1;
	for (int w = width, h = height; w > 1 || h > 1; ) {
		w >>= 1;
		h >>= 1;
		++levels;
	}

	GLuint tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex);
	glTextureStorage2D(tex, levels, GL_R8, width, height);
	glTextureSubImage2D(tex, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, pix.get());
	glGenerateTextureMipmap(tex);

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

GLuint generate_vbo(GLint size, GLsizeiptr data_size, const GLvoid *data)
{
	GLuint vbo;
	glCreateBuffers(1, &vbo);
	glBufferData(GL_ARRAY_BUFFER, data_size, data, GL_STATIC_DRAW);
	glNamedBufferData(vbo, data_size, data, GL_STATIC_DRAW);
	return vbo;
}

GLuint fill_vertex_attribute(GLuint vao, GLuint glsl_program_num, const string &attribute_name, GLint size, GLenum type, GLsizeiptr data_size, const GLvoid *data)
{
	int attrib = glGetAttribLocation(glsl_program_num, attribute_name.c_str());
	if (attrib == -1) {
		return -1;
	}

	GLuint vbo = generate_vbo(size, data_size, data);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexArrayAttrib(vao, attrib);
	glVertexAttribPointer(attrib, size, type, GL_FALSE, 0, BUFFER_OFFSET(0));
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return vbo;
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

// Compute gradients in every point, used for the motion search.
// The DIS paper doesn't actually mention how these are computed,
// but seemingly, a 3x3 Sobel operator is used here (at least in
// later versions of the code), while a [1 -8 0 8 -1] kernel is
// used for all the derivatives in the variational refinement part
// (which borrows code from DeepFlow). This is inconsistent,
// but I guess we're better off with staying with the original
// decisions until we actually know having different ones would be better.
class Sobel {
public:
	Sobel();
	void exec(GLint tex0_view, GLint grad0_tex, int level_width, int level_height);

private:
	GLuint sobel_vs_obj;
	GLuint sobel_fs_obj;
	GLuint sobel_program;
	GLuint sobel_vao;

	GLuint uniform_tex, uniform_image_size;
};

Sobel::Sobel()
{
	sobel_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	sobel_fs_obj = compile_shader(read_file("sobel.frag"), GL_FRAGMENT_SHADER);
	sobel_program = link_program(sobel_vs_obj, sobel_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &sobel_vao);
	glBindVertexArray(sobel_vao);

	GLint position_attrib = glGetAttribLocation(sobel_program, "position");
	glEnableVertexArrayAttrib(sobel_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_tex = glGetUniformLocation(sobel_program, "tex");
}

void Sobel::exec(GLint tex0_view, GLint grad0_tex, int level_width, int level_height)
{
	glUseProgram(sobel_program);
	glBindTextureUnit(0, tex0_view);
	glBindSampler(0, nearest_sampler);
	glProgramUniform1i(sobel_program, uniform_tex, 0);

	GLuint grad0_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &grad0_fbo);
	glNamedFramebufferTexture(grad0_fbo, GL_COLOR_ATTACHMENT0, grad0_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glBindFramebuffer(GL_FRAMEBUFFER, grad0_fbo);
	glBindVertexArray(sobel_vao);
	glUseProgram(sobel_program);
	glDisable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Motion search to find the initial flow. See motion_search.frag for documentation.
class MotionSearch {
public:
	MotionSearch();
	void exec(GLuint tex0_view, GLuint tex1_view, GLuint grad0_tex, GLuint flow_tex, GLuint flow_out_tex, int level_width, int level_height, int prev_level_width, int prev_level_height, int width_patches, int height_patches);

private:
	GLuint motion_vs_obj;
	GLuint motion_fs_obj;
	GLuint motion_search_program;
	GLuint motion_search_vao;

	GLuint uniform_image_size, uniform_inv_image_size, uniform_inv_prev_level_size;
	GLuint uniform_image0_tex, uniform_image1_tex, uniform_grad0_tex, uniform_flow_tex;
};

MotionSearch::MotionSearch()
{
	motion_vs_obj = compile_shader(read_file("motion_search.vert"), GL_VERTEX_SHADER);
	motion_fs_obj = compile_shader(read_file("motion_search.frag"), GL_FRAGMENT_SHADER);
	motion_search_program = link_program(motion_vs_obj, motion_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &motion_search_vao);
	glBindVertexArray(motion_search_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(motion_search_program, "position");
	glEnableVertexArrayAttrib(motion_search_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_image_size = glGetUniformLocation(motion_search_program, "image_size");
	uniform_inv_image_size = glGetUniformLocation(motion_search_program, "inv_image_size");
	uniform_inv_prev_level_size = glGetUniformLocation(motion_search_program, "inv_prev_level_size");
	uniform_image0_tex = glGetUniformLocation(motion_search_program, "image0_tex");
	uniform_image1_tex = glGetUniformLocation(motion_search_program, "image1_tex");
	uniform_grad0_tex = glGetUniformLocation(motion_search_program, "grad0_tex");
	uniform_flow_tex = glGetUniformLocation(motion_search_program, "flow_tex");
}

void MotionSearch::exec(GLuint tex0_view, GLuint tex1_view, GLuint grad0_tex, GLuint flow_tex, GLuint flow_out_tex, int level_width, int level_height, int prev_level_width, int prev_level_height, int width_patches, int height_patches)
{
	glUseProgram(motion_search_program);

	bind_sampler(motion_search_program, uniform_image0_tex, 0, tex0_view, nearest_sampler);
	bind_sampler(motion_search_program, uniform_image1_tex, 1, tex1_view, linear_sampler);
	bind_sampler(motion_search_program, uniform_grad0_tex, 2, grad0_tex, nearest_sampler);
	bind_sampler(motion_search_program, uniform_flow_tex, 3, flow_tex, linear_sampler);

	glProgramUniform2f(motion_search_program, uniform_image_size, level_width, level_height);
	glProgramUniform2f(motion_search_program, uniform_inv_image_size, 1.0f / level_width, 1.0f / level_height);
	glProgramUniform2f(motion_search_program, uniform_inv_prev_level_size, 1.0f / prev_level_width, 1.0f / prev_level_height);

	GLuint flow_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &flow_fbo);
	glNamedFramebufferTexture(flow_fbo, GL_COLOR_ATTACHMENT0, flow_out_tex, 0);

	glViewport(0, 0, width_patches, height_patches);
	glBindFramebuffer(GL_FRAMEBUFFER, flow_fbo);
	glBindVertexArray(motion_search_vao);
	glUseProgram(motion_search_program);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Do “densification”, ie., upsampling of the flow patches to the flow field
// (the same size as the image at this level). We draw one quad per patch
// over its entire covered area (using instancing in the vertex shader),
// and then weight the contributions in the pixel shader by post-warp difference.
// This is equation (3) in the paper.
//
// We accumulate the flow vectors in the R/G channels (for u/v) and the total
// weight in the B channel. Dividing R and G by B gives the normalized values.
class Densify {
public:
	Densify();
	void exec(GLuint tex0_view, GLuint tex1_view, GLuint flow_tex, GLuint dense_flow_tex, int level_width, int level_height, int width_patches, int height_patches);

private:
	GLuint densify_vs_obj;
	GLuint densify_fs_obj;
	GLuint densify_program;
	GLuint densify_vao;

	GLuint uniform_width_patches, uniform_patch_size, uniform_patch_spacing;
	GLuint uniform_image0_tex, uniform_image1_tex, uniform_flow_tex;
};

Densify::Densify()
{
	densify_vs_obj = compile_shader(read_file("densify.vert"), GL_VERTEX_SHADER);
	densify_fs_obj = compile_shader(read_file("densify.frag"), GL_FRAGMENT_SHADER);
	densify_program = link_program(densify_vs_obj, densify_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &densify_vao);
	glBindVertexArray(densify_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(densify_program, "position");
	glEnableVertexArrayAttrib(densify_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_width_patches = glGetUniformLocation(densify_program, "width_patches");
	uniform_patch_size = glGetUniformLocation(densify_program, "patch_size");
	uniform_patch_spacing = glGetUniformLocation(densify_program, "patch_spacing");
	uniform_image0_tex = glGetUniformLocation(densify_program, "image0_tex");
	uniform_image1_tex = glGetUniformLocation(densify_program, "image1_tex");
	uniform_flow_tex = glGetUniformLocation(densify_program, "flow_tex");
}

void Densify::exec(GLuint tex0_view, GLuint tex1_view, GLuint flow_tex, GLuint dense_flow_tex, int level_width, int level_height, int width_patches, int height_patches)
{
	glUseProgram(densify_program);

	bind_sampler(densify_program, uniform_image0_tex, 0, tex0_view, nearest_sampler);
	bind_sampler(densify_program, uniform_image1_tex, 1, tex1_view, linear_sampler);
	bind_sampler(densify_program, uniform_flow_tex, 2, flow_tex, nearest_sampler);

	glProgramUniform1i(densify_program, uniform_width_patches, width_patches);
	glProgramUniform2f(densify_program, uniform_patch_size,
		float(patch_size_pixels) / level_width,
		float(patch_size_pixels) / level_height);

	float patch_spacing_x = float(level_width - patch_size_pixels) / (width_patches - 1);
	float patch_spacing_y = float(level_height - patch_size_pixels) / (height_patches - 1);
	if (width_patches == 1) patch_spacing_x = 0.0f;  // Avoid infinities.
	if (height_patches == 1) patch_spacing_y = 0.0f;
	glProgramUniform2f(densify_program, uniform_patch_spacing,
		patch_spacing_x / level_width,
		patch_spacing_y / level_height);

	GLuint dense_flow_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &dense_flow_fbo);
	glNamedFramebufferTexture(dense_flow_fbo, GL_COLOR_ATTACHMENT0, dense_flow_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(densify_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, dense_flow_fbo);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width_patches * height_patches);
}

// Warp I_1 to I_w, and then compute the mean (I) and difference (I_t) of
// I_0 and I_w. The prewarping is what enables us to solve the variational
// flow for du,dv instead of u,v.
//
// Also calculates the normalized flow, ie. divides by z (this is needed because
// Densify works by additive blending) and multiplies by the image size.
//
// See variational_refinement.txt for more information.
class Prewarp {
public:
	Prewarp();
	void exec(GLuint tex0_view, GLuint tex1_view, GLuint flow_tex, GLuint normalized_flow_tex, GLuint I_tex, GLuint I_t_tex, int level_width, int level_height);

private:
	GLuint prewarp_vs_obj;
	GLuint prewarp_fs_obj;
	GLuint prewarp_program;
	GLuint prewarp_vao;

	GLuint uniform_image0_tex, uniform_image1_tex, uniform_flow_tex;
	GLuint uniform_image_size;
};

Prewarp::Prewarp()
{
	prewarp_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	prewarp_fs_obj = compile_shader(read_file("prewarp.frag"), GL_FRAGMENT_SHADER);
	prewarp_program = link_program(prewarp_vs_obj, prewarp_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &prewarp_vao);
	glBindVertexArray(prewarp_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(prewarp_program, "position");
	glEnableVertexArrayAttrib(prewarp_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_image0_tex = glGetUniformLocation(prewarp_program, "image0_tex");
	uniform_image1_tex = glGetUniformLocation(prewarp_program, "image1_tex");
	uniform_flow_tex = glGetUniformLocation(prewarp_program, "flow_tex");

	uniform_image_size = glGetUniformLocation(prewarp_program, "image_size");
}

void Prewarp::exec(GLuint tex0_view, GLuint tex1_view, GLuint flow_tex, GLuint I_tex, GLuint I_t_tex, GLuint normalized_flow_tex, int level_width, int level_height)
{
	glUseProgram(prewarp_program);

	bind_sampler(prewarp_program, uniform_image0_tex, 0, tex0_view, nearest_sampler);
	bind_sampler(prewarp_program, uniform_image1_tex, 1, tex1_view, linear_sampler);
	bind_sampler(prewarp_program, uniform_flow_tex, 2, flow_tex, nearest_sampler);

	glProgramUniform2f(prewarp_program, uniform_image_size, level_width, level_height);

	GLuint prewarp_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &prewarp_fbo);
	GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	glNamedFramebufferDrawBuffers(prewarp_fbo, 3, bufs);
	glNamedFramebufferTexture(prewarp_fbo, GL_COLOR_ATTACHMENT0, I_tex, 0);
	glNamedFramebufferTexture(prewarp_fbo, GL_COLOR_ATTACHMENT1, I_t_tex, 0);
	glNamedFramebufferTexture(prewarp_fbo, GL_COLOR_ATTACHMENT2, normalized_flow_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(prewarp_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, prewarp_fbo);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// From I, calculate the partial derivatives I_x and I_y. We use a four-tap
// central difference filter, since apparently, that's tradition (I haven't
// measured quality versus a more normal 0.5 (I[x+1] - I[x-1]).)
// The coefficients come from
//
//   https://en.wikipedia.org/wiki/Finite_difference_coefficient
//
// Also computes β_0, since it depends only on I_x and I_y.
class Derivatives {
public:
	Derivatives();
	void exec(GLuint input_tex, GLuint I_x_y_tex, GLuint beta_0_tex, int level_width, int level_height);

private:
	GLuint derivatives_vs_obj;
	GLuint derivatives_fs_obj;
	GLuint derivatives_program;
	GLuint derivatives_vao;

	GLuint uniform_tex;
};

Derivatives::Derivatives()
{
	derivatives_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	derivatives_fs_obj = compile_shader(read_file("derivatives.frag"), GL_FRAGMENT_SHADER);
	derivatives_program = link_program(derivatives_vs_obj, derivatives_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &derivatives_vao);
	glBindVertexArray(derivatives_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(derivatives_program, "position");
	glEnableVertexArrayAttrib(derivatives_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_tex = glGetUniformLocation(derivatives_program, "tex");
}

void Derivatives::exec(GLuint input_tex, GLuint I_x_y_tex, GLuint beta_0_tex, int level_width, int level_height)
{
	glUseProgram(derivatives_program);

	bind_sampler(derivatives_program, uniform_tex, 0, input_tex, nearest_sampler);

	GLuint derivatives_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &derivatives_fbo);
	GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glNamedFramebufferDrawBuffers(derivatives_fbo, 2, bufs);
	glNamedFramebufferTexture(derivatives_fbo, GL_COLOR_ATTACHMENT0, I_x_y_tex, 0);
	glNamedFramebufferTexture(derivatives_fbo, GL_COLOR_ATTACHMENT1, beta_0_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(derivatives_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, derivatives_fbo);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Calculate the smoothness constraints between neighboring pixels;
// s_x(x,y) stores smoothness between pixel (x,y) and (x+1,y),
// and s_y(x,y) stores between (x,y) and (x,y+1). We'll sample with
// border color (0,0) later, so that there's zero diffusion out of
// the border.
//
// See variational_refinement.txt for more information.
class ComputeSmoothness {
public:
	ComputeSmoothness();
	void exec(GLuint flow_tex, GLuint diff_flow_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, int level_width, int level_height);

private:
	GLuint smoothness_vs_obj;
	GLuint smoothness_fs_obj;
	GLuint smoothness_program;
	GLuint smoothness_vao;

	GLuint uniform_flow_tex, uniform_diff_flow_tex;
	GLuint uniform_alpha;
};

ComputeSmoothness::ComputeSmoothness()
{
	smoothness_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	smoothness_fs_obj = compile_shader(read_file("smoothness.frag"), GL_FRAGMENT_SHADER);
	smoothness_program = link_program(smoothness_vs_obj, smoothness_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &smoothness_vao);
	glBindVertexArray(smoothness_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(smoothness_program, "position");
	glEnableVertexArrayAttrib(smoothness_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_flow_tex = glGetUniformLocation(smoothness_program, "flow_tex");
	uniform_diff_flow_tex = glGetUniformLocation(smoothness_program, "diff_flow_tex");
	uniform_alpha = glGetUniformLocation(smoothness_program, "alpha");
}

void ComputeSmoothness::exec(GLuint flow_tex, GLuint diff_flow_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, int level_width, int level_height)
{
	glUseProgram(smoothness_program);

	bind_sampler(smoothness_program, uniform_flow_tex, 0, flow_tex, nearest_sampler);
	bind_sampler(smoothness_program, uniform_diff_flow_tex, 1, diff_flow_tex, nearest_sampler);
	glProgramUniform1f(smoothness_program, uniform_alpha, vr_alpha);

	GLuint smoothness_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &smoothness_fbo);
	GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glNamedFramebufferDrawBuffers(smoothness_fbo, 2, bufs);
	glNamedFramebufferTexture(smoothness_fbo, GL_COLOR_ATTACHMENT0, smoothness_x_tex, 0);
	glNamedFramebufferTexture(smoothness_fbo, GL_COLOR_ATTACHMENT1, smoothness_y_tex, 0);

	glViewport(0, 0, level_width, level_height);

	glDisable(GL_BLEND);
	glBindVertexArray(smoothness_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, smoothness_fbo);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Make sure the smoothness on the right and upper borders is zero.
	// We could have done this by making (W-1)xH and Wx(H-1) textures instead
	// (we're sampling smoothness with all-zero border color), but we'd
	// have to adjust the sampling coordinates, which is annoying.
	glClearTexSubImage(smoothness_x_tex, 0,  level_width - 1, 0, 0,   1, level_height, 1,  GL_RED, GL_FLOAT, nullptr);
	glClearTexSubImage(smoothness_y_tex, 0,  0, level_height - 1, 0,  level_width, 1, 1,   GL_RED, GL_FLOAT, nullptr);
}

// Set up the equations set (two equations in two unknowns, per pixel).
// We store five floats; the three non-redundant elements of the 2x2 matrix (A)
// as 32-bit floats, and the two elements on the right-hand side (b) as 16-bit
// floats. (Actually, we store the inverse of the diagonal elements, because
// we only ever need to divide by them.) This fits into four u32 values;
// R, G, B for the matrix (the last element is symmetric) and A for the two b values.
// All the values of the energy term (E_I, E_G, E_S), except the smoothness
// terms that depend on other pixels, are calculated in one pass.
//
// See variational_refinement.txt for more information.
class SetupEquations {
public:
	SetupEquations();
	void exec(GLuint I_x_y_tex, GLuint I_t_tex, GLuint diff_flow_tex, GLuint flow_tex, GLuint beta_0_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, GLuint equation_tex, int level_width, int level_height);

private:
	GLuint equations_vs_obj;
	GLuint equations_fs_obj;
	GLuint equations_program;
	GLuint equations_vao;

	GLuint uniform_I_x_y_tex, uniform_I_t_tex;
	GLuint uniform_diff_flow_tex, uniform_base_flow_tex;
	GLuint uniform_beta_0_tex;
	GLuint uniform_smoothness_x_tex, uniform_smoothness_y_tex;
	GLuint uniform_gamma, uniform_delta;
};

SetupEquations::SetupEquations()
{
	equations_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	equations_fs_obj = compile_shader(read_file("equations.frag"), GL_FRAGMENT_SHADER);
	equations_program = link_program(equations_vs_obj, equations_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &equations_vao);
	glBindVertexArray(equations_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(equations_program, "position");
	glEnableVertexArrayAttrib(equations_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_I_x_y_tex = glGetUniformLocation(equations_program, "I_x_y_tex");
	uniform_I_t_tex = glGetUniformLocation(equations_program, "I_t_tex");
	uniform_diff_flow_tex = glGetUniformLocation(equations_program, "diff_flow_tex");
	uniform_base_flow_tex = glGetUniformLocation(equations_program, "base_flow_tex");
	uniform_beta_0_tex = glGetUniformLocation(equations_program, "beta_0_tex");
	uniform_smoothness_x_tex = glGetUniformLocation(equations_program, "smoothness_x_tex");
	uniform_smoothness_y_tex = glGetUniformLocation(equations_program, "smoothness_y_tex");
	uniform_gamma = glGetUniformLocation(equations_program, "gamma");
	uniform_delta = glGetUniformLocation(equations_program, "delta");
}

void SetupEquations::exec(GLuint I_x_y_tex, GLuint I_t_tex, GLuint diff_flow_tex, GLuint base_flow_tex, GLuint beta_0_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, GLuint equation_tex, int level_width, int level_height)
{
	glUseProgram(equations_program);

	bind_sampler(equations_program, uniform_I_x_y_tex, 0, I_x_y_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_I_t_tex, 1, I_t_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_diff_flow_tex, 2, diff_flow_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_base_flow_tex, 3, base_flow_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_beta_0_tex, 4, beta_0_tex, nearest_sampler);
	bind_sampler(equations_program, uniform_smoothness_x_tex, 5, smoothness_x_tex, smoothness_sampler);
	bind_sampler(equations_program, uniform_smoothness_y_tex, 6, smoothness_y_tex, smoothness_sampler);
	glProgramUniform1f(equations_program, uniform_delta, vr_delta);
	glProgramUniform1f(equations_program, uniform_gamma, vr_gamma);

	GLuint equations_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &equations_fbo);
	glNamedFramebufferTexture(equations_fbo, GL_COLOR_ATTACHMENT0, equation_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(equations_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, equations_fbo);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Actually solve the equation sets made by SetupEquations, by means of
// successive over-relaxation (SOR).
//
// See variational_refinement.txt for more information.
class SOR {
public:
	SOR();
	void exec(GLuint diff_flow_tex, GLuint equation_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, int level_width, int level_height, int num_iterations);

private:
	GLuint sor_vs_obj;
	GLuint sor_fs_obj;
	GLuint sor_program;
	GLuint sor_vao;

	GLuint uniform_diff_flow_tex;
	GLuint uniform_equation_tex;
	GLuint uniform_smoothness_x_tex, uniform_smoothness_y_tex;
};

SOR::SOR()
{
	sor_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	sor_fs_obj = compile_shader(read_file("sor.frag"), GL_FRAGMENT_SHADER);
	sor_program = link_program(sor_vs_obj, sor_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &sor_vao);
	glBindVertexArray(sor_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(sor_program, "position");
	glEnableVertexArrayAttrib(sor_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_diff_flow_tex = glGetUniformLocation(sor_program, "diff_flow_tex");
	uniform_equation_tex = glGetUniformLocation(sor_program, "equation_tex");
	uniform_smoothness_x_tex = glGetUniformLocation(sor_program, "smoothness_x_tex");
	uniform_smoothness_y_tex = glGetUniformLocation(sor_program, "smoothness_y_tex");
}

void SOR::exec(GLuint diff_flow_tex, GLuint equation_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, int level_width, int level_height, int num_iterations)
{
	glUseProgram(sor_program);

	bind_sampler(sor_program, uniform_diff_flow_tex, 0, diff_flow_tex, nearest_sampler);
	bind_sampler(sor_program, uniform_smoothness_x_tex, 1, smoothness_x_tex, smoothness_sampler);
	bind_sampler(sor_program, uniform_smoothness_y_tex, 2, smoothness_y_tex, smoothness_sampler);
	bind_sampler(sor_program, uniform_equation_tex, 3, equation_tex, nearest_sampler);

	GLuint sor_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &sor_fbo);
	glNamedFramebufferTexture(sor_fbo, GL_COLOR_ATTACHMENT0, diff_flow_tex, 0);  // NOTE: Bind to same as we render from!

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(sor_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, sor_fbo);

	for (int i = 0; i < num_iterations; ++i) {
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (i != num_iterations - 1) {
			glTextureBarrier();
		}
	}
}

// Simply add the differential flow found by the variational refinement to the base flow.
// The output is in base_flow_tex; we don't need to make a new texture.
class AddBaseFlow {
public:
	AddBaseFlow();
	void exec(GLuint base_flow_tex, GLuint diff_flow_tex, int level_width, int level_height);

private:
	GLuint add_flow_vs_obj;
	GLuint add_flow_fs_obj;
	GLuint add_flow_program;
	GLuint add_flow_vao;

	GLuint uniform_diff_flow_tex;
};

AddBaseFlow::AddBaseFlow()
{
	add_flow_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	add_flow_fs_obj = compile_shader(read_file("add_base_flow.frag"), GL_FRAGMENT_SHADER);
	add_flow_program = link_program(add_flow_vs_obj, add_flow_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &add_flow_vao);
	glBindVertexArray(add_flow_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(add_flow_program, "position");
	glEnableVertexArrayAttrib(add_flow_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_diff_flow_tex = glGetUniformLocation(add_flow_program, "diff_flow_tex");
}

void AddBaseFlow::exec(GLuint base_flow_tex, GLuint diff_flow_tex, int level_width, int level_height)
{
	glUseProgram(add_flow_program);

	bind_sampler(add_flow_program, uniform_diff_flow_tex, 0, diff_flow_tex, nearest_sampler);

	GLuint add_flow_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &add_flow_fbo);
	glNamedFramebufferTexture(add_flow_fbo, GL_COLOR_ATTACHMENT0, base_flow_tex, 0);

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(add_flow_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, add_flow_fbo);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

class GPUTimers {
public:
	void print();
	pair<GLuint, GLuint> begin_timer(const string &name, int level);

private:
	struct Timer {
		string name;
		int level;
		pair<GLuint, GLuint> query;
	};
	vector<Timer> timers;
};

pair<GLuint, GLuint> GPUTimers::begin_timer(const string &name, int level)
{
	GLuint queries[2];
	glGenQueries(2, queries);
	glQueryCounter(queries[0], GL_TIMESTAMP);

	Timer timer;
	timer.name = name;
	timer.level = level;
	timer.query.first = queries[0];
	timer.query.second = queries[1];
	timers.push_back(timer);
	return timer.query;
}

// Take a copy of the flow, bilinearly interpolated and scaled up.
class ResizeFlow {
public:
	ResizeFlow();
	void exec(GLuint in_tex, GLuint out_tex, int input_width, int input_height, int output_width, int output_height);

private:
	GLuint resize_flow_vs_obj;
	GLuint resize_flow_fs_obj;
	GLuint resize_flow_program;
	GLuint resize_flow_vao;

	GLuint uniform_flow_tex;
	GLuint uniform_scale_factor;
};

ResizeFlow::ResizeFlow()
{
	resize_flow_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	resize_flow_fs_obj = compile_shader(read_file("resize_flow.frag"), GL_FRAGMENT_SHADER);
	resize_flow_program = link_program(resize_flow_vs_obj, resize_flow_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &resize_flow_vao);
	glBindVertexArray(resize_flow_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(resize_flow_program, "position");
	glEnableVertexArrayAttrib(resize_flow_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_flow_tex = glGetUniformLocation(resize_flow_program, "flow_tex");
	uniform_scale_factor = glGetUniformLocation(resize_flow_program, "scale_factor");
}

void ResizeFlow::exec(GLuint flow_tex, GLuint out_tex, int input_width, int input_height, int output_width, int output_height)
{
	glUseProgram(resize_flow_program);

	bind_sampler(resize_flow_program, uniform_flow_tex, 0, flow_tex, nearest_sampler);

	glProgramUniform2f(resize_flow_program, uniform_scale_factor, float(output_width) / input_width, float(output_height) / input_height);

	GLuint resize_flow_fbo;  // TODO: cleanup
	glCreateFramebuffers(1, &resize_flow_fbo);
	glNamedFramebufferTexture(resize_flow_fbo, GL_COLOR_ATTACHMENT0, out_tex, 0);

	glViewport(0, 0, output_width, output_height);
	glDisable(GL_BLEND);
	glBindVertexArray(resize_flow_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, resize_flow_fbo);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GPUTimers::print()
{
	for (const Timer &timer : timers) {
		// NOTE: This makes the CPU wait for the GPU.
		GLuint64 time_start, time_end;
		glGetQueryObjectui64v(timer.query.first, GL_QUERY_RESULT, &time_start);
		glGetQueryObjectui64v(timer.query.second, GL_QUERY_RESULT, &time_end);
		//fprintf(stderr, "GPU time used = %.1f ms\n", time_elapsed / 1e6);
		for (int i = 0; i < timer.level * 2; ++i) {
			fprintf(stderr, " ");
		}
		fprintf(stderr, "%-30s %4.1f ms\n", timer.name.c_str(), GLint64(time_end - time_start) / 1e6);
	}
}

// A simple RAII class for timing until the end of the scope.
class ScopedTimer {
public:
	ScopedTimer(const string &name, GPUTimers *timers)
		: timers(timers), level(0)
	{
		query = timers->begin_timer(name, level);
	}

	ScopedTimer(const string &name, ScopedTimer *parent_timer)
		: timers(parent_timer->timers),
		  level(parent_timer->level + 1)
	{
		query = timers->begin_timer(name, level);
	}

	~ScopedTimer()
	{
		end();
	}

	void end()
	{
		if (!ended) {
			glQueryCounter(query.second, GL_TIMESTAMP);
			ended = true;
		}
	}

private:
	GPUTimers *timers;
	int level;
	pair<GLuint, GLuint> query;
	bool ended = false;
};

int main(int argc, char **argv)
{
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
	SDL_Window *window = SDL_CreateWindow("OpenGL window",
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			64, 64,
			SDL_WINDOW_OPENGL);
	SDL_GLContext context = SDL_GL_CreateContext(window);
	assert(context != nullptr);

	// Load pictures.
	unsigned width1, height1, width2, height2;
	GLuint tex0 = load_texture(argc >= 2 ? argv[1] : "test1499.png", &width1, &height1);
	GLuint tex1 = load_texture(argc >= 3 ? argv[2] : "test1500.png", &width2, &height2);

	if (width1 != width2 || height1 != height2) {
		fprintf(stderr, "Image dimensions don't match (%dx%d versus %dx%d)\n",
			width1, height1, width2, height2);
		exit(1);
	}

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
	glCreateSamplers(1, &smoothness_sampler);
	glSamplerParameteri(smoothness_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(smoothness_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(smoothness_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glSamplerParameteri(smoothness_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float zero[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glSamplerParameterfv(smoothness_sampler, GL_TEXTURE_BORDER_COLOR, zero);

	float vertices[] = {
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
	};
	glCreateBuffers(1, &vertex_vbo);
	glNamedBufferData(vertex_vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	// Initial flow is zero, 1x1.
	GLuint initial_flow_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &initial_flow_tex);
	glTextureStorage2D(initial_flow_tex, 1, GL_RG16F, 1, 1);
	glClearTexImage(initial_flow_tex, 0, GL_RG, GL_FLOAT, nullptr);
	int prev_level_width = 1, prev_level_height = 1;

	GLuint prev_level_flow_tex = initial_flow_tex;

	Sobel sobel;
	MotionSearch motion_search;
	Densify densify;
	Prewarp prewarp;
	Derivatives derivatives;
	ComputeSmoothness compute_smoothness;
	SetupEquations setup_equations;
	SOR sor;
	AddBaseFlow add_base_flow;
	ResizeFlow resize_flow;

	GLuint query;
	glGenQueries(1, &query);
	glBeginQuery(GL_TIME_ELAPSED, query);

	GPUTimers timers;

	ScopedTimer total_timer("Total", &timers);
	for (int level = coarsest_level; level >= int(finest_level); --level) {
		char timer_name[256];
		snprintf(timer_name, sizeof(timer_name), "Level %d", level);
		ScopedTimer level_timer(timer_name, &total_timer);

		int level_width = width1 >> level;
		int level_height = height1 >> level;
		float patch_spacing_pixels = patch_size_pixels * (1.0f - patch_overlap_ratio);
		int width_patches = 1 + lrintf((level_width - patch_size_pixels) / patch_spacing_pixels);
		int height_patches = 1 + lrintf((level_height - patch_size_pixels) / patch_spacing_pixels);

		// Make sure we always read from the correct level; the chosen
		// mipmapping could otherwise be rather unpredictable, especially
		// during motion search.
		// TODO: create these beforehand, and stop leaking them.
		GLuint tex0_view, tex1_view;
		glGenTextures(1, &tex0_view);
		glTextureView(tex0_view, GL_TEXTURE_2D, tex0, GL_R8, level, 1, 0, 1);
		glGenTextures(1, &tex1_view);
		glTextureView(tex1_view, GL_TEXTURE_2D, tex1, GL_R8, level, 1, 0, 1);

		// Create a new texture; we could be fancy and render use a multi-level
		// texture, but meh.
		GLuint grad0_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &grad0_tex);
		glTextureStorage2D(grad0_tex, 1, GL_RG16F, level_width, level_height);

		// Find the derivative.
		{
			ScopedTimer timer("Sobel", &level_timer);
			sobel.exec(tex0_view, grad0_tex, level_width, level_height);
		}

		// Motion search to find the initial flow. We use the flow from the previous
		// level (sampled bilinearly; no fancy tricks) as a guide, then search from there.

		// Create an output flow texture.
		GLuint flow_out_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &flow_out_tex);
		glTextureStorage2D(flow_out_tex, 1, GL_RGB16F, width_patches, height_patches);

		// And draw.
		{
			ScopedTimer timer("Motion search", &level_timer);
			motion_search.exec(tex0_view, tex1_view, grad0_tex, prev_level_flow_tex, flow_out_tex, level_width, level_height, prev_level_width, prev_level_height, width_patches, height_patches);
		}

		// Densification.

		// Set up an output texture (initially zero).
		GLuint dense_flow_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &dense_flow_tex);
		glTextureStorage2D(dense_flow_tex, 1, GL_RGB16F, level_width, level_height);
		glClearTexImage(dense_flow_tex, 0, GL_RGB, GL_FLOAT, nullptr);

		// And draw.
		{
			ScopedTimer timer("Densification", &level_timer);
			densify.exec(tex0_view, tex1_view, flow_out_tex, dense_flow_tex, level_width, level_height, width_patches, height_patches);
		}

		// Everything below here in the loop belongs to variational refinement.
		ScopedTimer varref_timer("Variational refinement", &level_timer);

		// Prewarping; create I and I_t, and a normalized base flow (so we don't
		// have to normalize it over and over again, and also save some bandwidth).
		//
		// During the entire rest of the variational refinement, flow will be measured
		// in pixels, not 0..1 normalized OpenGL texture coordinates.
		// This is because variational refinement depends so heavily on derivatives,
		// which are measured in intensity levels per pixel.
		GLuint I_tex, I_t_tex, base_flow_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &I_tex);
		glCreateTextures(GL_TEXTURE_2D, 1, &I_t_tex);
		glCreateTextures(GL_TEXTURE_2D, 1, &base_flow_tex);
		glTextureStorage2D(I_tex, 1, GL_R16F, level_width, level_height);
		glTextureStorage2D(I_t_tex, 1, GL_R16F, level_width, level_height);
		glTextureStorage2D(base_flow_tex, 1, GL_RG16F, level_width, level_height);
		{
			ScopedTimer timer("Prewarping", &varref_timer);
			prewarp.exec(tex0_view, tex1_view, dense_flow_tex, I_tex, I_t_tex, base_flow_tex, level_width, level_height);
		}

		// Calculate I_x and I_y. We're only calculating first derivatives;
		// the others will be taken on-the-fly in order to sample from fewer
		// textures overall, since sampling from the L1 cache is cheap.
		// (TODO: Verify that this is indeed faster than making separate
		// double-derivative textures.)
		GLuint I_x_y_tex, beta_0_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &I_x_y_tex);
		glCreateTextures(GL_TEXTURE_2D, 1, &beta_0_tex);
		glTextureStorage2D(I_x_y_tex, 1, GL_RG16F, level_width, level_height);
		glTextureStorage2D(beta_0_tex, 1, GL_R16F, level_width, level_height);
		{
			ScopedTimer timer("First derivatives", &varref_timer);
			derivatives.exec(I_tex, I_x_y_tex, beta_0_tex, level_width, level_height);
		}

		// We need somewhere to store du and dv (the flow increment, relative
		// to the non-refined base flow u0 and v0). It starts at zero.
		GLuint du_dv_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &du_dv_tex);
		glTextureStorage2D(du_dv_tex, 1, GL_RG16F, level_width, level_height);
		glClearTexImage(du_dv_tex, 0, GL_RG, GL_FLOAT, nullptr);

		// And for smoothness.
		GLuint smoothness_x_tex, smoothness_y_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &smoothness_x_tex);
		glCreateTextures(GL_TEXTURE_2D, 1, &smoothness_y_tex);
		glTextureStorage2D(smoothness_x_tex, 1, GL_R16F, level_width, level_height);
		glTextureStorage2D(smoothness_y_tex, 1, GL_R16F, level_width, level_height);

		// And finally for the equation set. See SetupEquations for
		// the storage format.
		GLuint equation_tex;
		glCreateTextures(GL_TEXTURE_2D, 1, &equation_tex);
		glTextureStorage2D(equation_tex, 1, GL_RGBA32UI, level_width, level_height);

		for (int outer_idx = 0; outer_idx < level + 1; ++outer_idx) {
			// Calculate the smoothness terms between the neighboring pixels,
			// both in x and y direction.
			{
				ScopedTimer timer("Compute smoothness", &varref_timer);
				compute_smoothness.exec(base_flow_tex, du_dv_tex, smoothness_x_tex, smoothness_y_tex, level_width, level_height);
			}

			// Set up the 2x2 equation system for each pixel.
			{
				ScopedTimer timer("Set up equations", &varref_timer);
				setup_equations.exec(I_x_y_tex, I_t_tex, du_dv_tex, base_flow_tex, beta_0_tex, smoothness_x_tex, smoothness_y_tex, equation_tex, level_width, level_height);
			}

			// Run a few SOR (or quasi-SOR, since we're not really Jacobi) iterations.
			// Note that these are to/from the same texture.
			{
				ScopedTimer timer("SOR", &varref_timer);
				sor.exec(du_dv_tex, equation_tex, smoothness_x_tex, smoothness_y_tex, level_width, level_height, 5);
			}
		}

		// Add the differential flow found by the variational refinement to the base flow,
		// giving the final flow estimate for this level.
		// The output is in diff_flow_tex; we don't need to make a new texture.
		// You can comment out this part if you wish to test disabling of the variational refinement.
		{
			ScopedTimer timer("Add differential flow", &varref_timer);
			add_base_flow.exec(base_flow_tex, du_dv_tex, level_width, level_height);
		}

		prev_level_flow_tex = base_flow_tex;
		prev_level_width = level_width;
		prev_level_height = level_height;
	}
	total_timer.end();

	timers.print();

	// Scale up the flow to the final size (if needed).
	GLuint final_tex;
	if (finest_level == 0) {
		final_tex = prev_level_flow_tex;
	} else {
		glCreateTextures(GL_TEXTURE_2D, 1, &final_tex);
		glTextureStorage2D(final_tex, 1, GL_RG16F, width1, height1);
		resize_flow.exec(prev_level_flow_tex, final_tex, prev_level_width, prev_level_height, width1, height1);
	}

	unique_ptr<float[]> dense_flow(new float[width1 * height1 * 2]);
	glGetTextureImage(final_tex, 0, GL_RG, GL_FLOAT, width1 * height1 * 2 * sizeof(float), dense_flow.get());

	FILE *fp = fopen("flow.ppm", "wb");
	FILE *flowfp = fopen("flow.flo", "wb");
	fprintf(fp, "P6\n%d %d\n255\n", width1, height1);
	fprintf(flowfp, "FEIH");
	fwrite(&width1, 4, 1, flowfp);
	fwrite(&height1, 4, 1, flowfp);
	for (unsigned y = 0; y < unsigned(height1); ++y) {
		int yy = height1 - y - 1;
		for (unsigned x = 0; x < unsigned(width1); ++x) {
			float du = dense_flow[(yy * width1 + x) * 2 + 0];
			float dv = dense_flow[(yy * width1 + x) * 2 + 1];

			dv = -dv;

			fwrite(&du, 4, 1, flowfp);
			fwrite(&dv, 4, 1, flowfp);

			uint8_t r, g, b;
			flow2rgb(du, dv, &r, &g, &b);
			putc(r, fp);
			putc(g, fp);
			putc(b, fp);
		}
	}
	fclose(fp);
	fclose(flowfp);

	fprintf(stderr, "err = %d\n", glGetError());
}
