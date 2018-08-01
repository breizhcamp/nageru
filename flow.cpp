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

// Operating point 3 (10 Hz on CPU, excluding preprocessing).
constexpr float patch_overlap_ratio = 0.75f;
constexpr unsigned coarsest_level = 5;
constexpr unsigned finest_level = 1;
constexpr unsigned patch_size_pixels = 12;

// Weighting constants for the different parts of the variational refinement.
// These don't correspond 1:1 to the values given in the DIS paper,
// since we have different normalizations and ranges in some cases.
// These are found through a simple grid search on some MPI-Sintel data,
// although the error (EPE) seems to be fairly insensitive to the precise values.
// Only the relative values matter, so we fix alpha (the smoothness constant)
// at unity and tweak the others.
float vr_alpha = 1.0f, vr_delta = 0.25f, vr_gamma = 0.25f;

bool enable_timing = true;
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

// A class that caches FBOs that render to a given set of textures.
// It never frees anything, so it is only suitable for rendering to
// the same (small) set of textures over and over again.
template<size_t num_elements>
class PersistentFBOSet {
public:
	void render_to(const array<GLuint, num_elements> &textures);

	// Convenience wrappers.
	void render_to(GLuint texture0, enable_if<num_elements == 1> * = nullptr) {
		render_to({{texture0}});
	}

	void render_to(GLuint texture0, GLuint texture1, enable_if<num_elements == 2> * = nullptr) {
		render_to({{texture0, texture1}});
	}

	void render_to(GLuint texture0, GLuint texture1, GLuint texture2, enable_if<num_elements == 3> * = nullptr) {
		render_to({{texture0, texture1, texture2}});
	}

	void render_to(GLuint texture0, GLuint texture1, GLuint texture2, GLuint texture3, enable_if<num_elements == 4> * = nullptr) {
		render_to({{texture0, texture1, texture2, texture3}});
	}

private:
	// TODO: Delete these on destruction.
	map<array<GLuint, num_elements>, GLuint> fbos;
};

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

// Convert RGB to grayscale, using Rec. 709 coefficients.
class GrayscaleConversion {
public:
	GrayscaleConversion();
	void exec(GLint tex, GLint gray_tex, int width, int height);

private:
	PersistentFBOSet<1> fbos;
	GLuint gray_vs_obj;
	GLuint gray_fs_obj;
	GLuint gray_program;
	GLuint gray_vao;

	GLuint uniform_tex;
};

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

void GrayscaleConversion::exec(GLint tex, GLint gray_tex, int width, int height)
{
	glUseProgram(gray_program);
	bind_sampler(gray_program, uniform_tex, 0, tex, nearest_sampler);

	glViewport(0, 0, width, height);
	fbos.render_to(gray_tex);
	glBindVertexArray(gray_vao);
	glUseProgram(gray_program);
	glDisable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
	PersistentFBOSet<1> fbos;
	GLuint sobel_vs_obj;
	GLuint sobel_fs_obj;
	GLuint sobel_program;
	GLuint sobel_vao;

	GLuint uniform_tex;
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
	bind_sampler(sobel_program, uniform_tex, 0, tex0_view, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	fbos.render_to(grad0_tex);
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
	PersistentFBOSet<1> fbos;

	GLuint motion_vs_obj;
	GLuint motion_fs_obj;
	GLuint motion_search_program;
	GLuint motion_search_vao;

	GLuint uniform_inv_image_size, uniform_inv_prev_level_size;
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
	bind_sampler(motion_search_program, uniform_grad0_tex, 2, grad0_tex, zero_border_sampler);
	bind_sampler(motion_search_program, uniform_flow_tex, 3, flow_tex, linear_sampler);

	glProgramUniform2f(motion_search_program, uniform_inv_image_size, 1.0f / level_width, 1.0f / level_height);
	glProgramUniform2f(motion_search_program, uniform_inv_prev_level_size, 1.0f / prev_level_width, 1.0f / prev_level_height);

	glViewport(0, 0, width_patches, height_patches);
	fbos.render_to(flow_out_tex);
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
	PersistentFBOSet<1> fbos;

	GLuint densify_vs_obj;
	GLuint densify_fs_obj;
	GLuint densify_program;
	GLuint densify_vao;

	GLuint uniform_patch_size;
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

	uniform_patch_size = glGetUniformLocation(densify_program, "patch_size");
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

	glProgramUniform2f(densify_program, uniform_patch_size,
		float(patch_size_pixels) / level_width,
		float(patch_size_pixels) / level_height);

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(densify_vao);
	fbos.render_to(dense_flow_tex);
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
	PersistentFBOSet<3> fbos;

	GLuint prewarp_vs_obj;
	GLuint prewarp_fs_obj;
	GLuint prewarp_program;
	GLuint prewarp_vao;

	GLuint uniform_image0_tex, uniform_image1_tex, uniform_flow_tex;
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
}

void Prewarp::exec(GLuint tex0_view, GLuint tex1_view, GLuint flow_tex, GLuint I_tex, GLuint I_t_tex, GLuint normalized_flow_tex, int level_width, int level_height)
{
	glUseProgram(prewarp_program);

	bind_sampler(prewarp_program, uniform_image0_tex, 0, tex0_view, nearest_sampler);
	bind_sampler(prewarp_program, uniform_image1_tex, 1, tex1_view, linear_sampler);
	bind_sampler(prewarp_program, uniform_flow_tex, 2, flow_tex, nearest_sampler);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(prewarp_vao);
	fbos.render_to(I_tex, I_t_tex, normalized_flow_tex);
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
	PersistentFBOSet<2> fbos;

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

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(derivatives_vao);
	fbos.render_to(I_x_y_tex, beta_0_tex);
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
	PersistentFBOSet<2> fbos;

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

	glViewport(0, 0, level_width, level_height);

	glDisable(GL_BLEND);
	glBindVertexArray(smoothness_vao);
	fbos.render_to(smoothness_x_tex, smoothness_y_tex);
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
	PersistentFBOSet<1> fbos;

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
	bind_sampler(equations_program, uniform_smoothness_x_tex, 5, smoothness_x_tex, zero_border_sampler);
	bind_sampler(equations_program, uniform_smoothness_y_tex, 6, smoothness_y_tex, zero_border_sampler);
	glProgramUniform1f(equations_program, uniform_delta, vr_delta);
	glProgramUniform1f(equations_program, uniform_gamma, vr_gamma);

	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(equations_vao);
	fbos.render_to(equation_tex);
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
	PersistentFBOSet<1> fbos;

	GLuint sor_vs_obj;
	GLuint sor_fs_obj;
	GLuint sor_program;
	GLuint sor_vao;

	GLuint uniform_diff_flow_tex;
	GLuint uniform_equation_tex;
	GLuint uniform_smoothness_x_tex, uniform_smoothness_y_tex;
	GLuint uniform_phase;
};

SOR::SOR()
{
	sor_vs_obj = compile_shader(read_file("sor.vert"), GL_VERTEX_SHADER);
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
	uniform_phase = glGetUniformLocation(sor_program, "phase");
}

void SOR::exec(GLuint diff_flow_tex, GLuint equation_tex, GLuint smoothness_x_tex, GLuint smoothness_y_tex, int level_width, int level_height, int num_iterations)
{
	glUseProgram(sor_program);

	bind_sampler(sor_program, uniform_diff_flow_tex, 0, diff_flow_tex, nearest_sampler);
	bind_sampler(sor_program, uniform_smoothness_x_tex, 1, smoothness_x_tex, zero_border_sampler);
	bind_sampler(sor_program, uniform_smoothness_y_tex, 2, smoothness_y_tex, zero_border_sampler);
	bind_sampler(sor_program, uniform_equation_tex, 3, equation_tex, nearest_sampler);

	// NOTE: We bind to the texture we are rendering from, but we never write any value
	// that we read in the same shader pass (we call discard for red values when we compute
	// black, and vice versa), and we have barriers between the passes, so we're fine
	// as per the spec.
	glViewport(0, 0, level_width, level_height);
	glDisable(GL_BLEND);
	glBindVertexArray(sor_vao);
	fbos.render_to(diff_flow_tex);

	for (int i = 0; i < num_iterations; ++i) {
		glProgramUniform1i(sor_program, uniform_phase, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTextureBarrier();
		glProgramUniform1i(sor_program, uniform_phase, 1);
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
	PersistentFBOSet<1> fbos;

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

	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(add_flow_vao);
	fbos.render_to(base_flow_tex);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Take a copy of the flow, bilinearly interpolated and scaled up.
class ResizeFlow {
public:
	ResizeFlow();
	void exec(GLuint in_tex, GLuint out_tex, int input_width, int input_height, int output_width, int output_height);

private:
	PersistentFBOSet<1> fbos;

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

	glViewport(0, 0, output_width, output_height);
	glDisable(GL_BLEND);
	glBindVertexArray(resize_flow_vao);
	fbos.render_to(out_tex);

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
	if (!enable_timing) {
		return make_pair(0, 0);
	}

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
		if (enable_timing && !ended) {
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

class TexturePool {
public:
	GLuint get_texture(GLenum format, GLuint width, GLuint height);
	void release_texture(GLuint tex_num);

private:
	struct Texture {
		GLuint tex_num;
		GLenum format;
		GLuint width, height;
		bool in_use = false;
	};
	vector<Texture> textures;
};

class DISComputeFlow {
public:
	DISComputeFlow(int width, int height);

	enum ResizeStrategy {
		DO_NOT_RESIZE_FLOW,
		RESIZE_FLOW_TO_FULL_SIZE
	};

	// Returns a texture that must be released with release_texture()
	// after use.
	GLuint exec(GLuint tex0, GLuint tex1, ResizeStrategy resize_strategy);

	void release_texture(GLuint tex) {
		pool.release_texture(tex);
	}

private:
	int width, height;
	GLuint initial_flow_tex;
	TexturePool pool;

	// The various passes.
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
};

DISComputeFlow::DISComputeFlow(int width, int height)
	: width(width), height(height)
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
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glSamplerParameteri(zero_border_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float zero[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glSamplerParameterfv(zero_border_sampler, GL_TEXTURE_BORDER_COLOR, zero);

	// Initial flow is zero, 1x1.
	glCreateTextures(GL_TEXTURE_2D, 1, &initial_flow_tex);
	glTextureStorage2D(initial_flow_tex, 1, GL_RG16F, 1, 1);
	glClearTexImage(initial_flow_tex, 0, GL_RG, GL_FLOAT, nullptr);
}

GLuint DISComputeFlow::exec(GLuint tex0, GLuint tex1, ResizeStrategy resize_strategy)
{
	int prev_level_width = 1, prev_level_height = 1;
	GLuint prev_level_flow_tex = initial_flow_tex;

	GPUTimers timers;

	ScopedTimer total_timer("Total", &timers);
	for (int level = coarsest_level; level >= int(finest_level); --level) {
		char timer_name[256];
		snprintf(timer_name, sizeof(timer_name), "Level %d", level);
		ScopedTimer level_timer(timer_name, &total_timer);

		int level_width = width >> level;
		int level_height = height >> level;
		float patch_spacing_pixels = patch_size_pixels * (1.0f - patch_overlap_ratio);

		// Make sure we have patches at least every Nth pixel, e.g. for width=9
		// and patch_spacing=3 (the default), we put out patch centers in
		// x=0, x=3, x=6, x=9, which is four patches. The fragment shader will
		// lock all the centers to integer coordinates if needed.
		int width_patches = 1 + ceil(level_width / patch_spacing_pixels);
		int height_patches = 1 + ceil(level_height / patch_spacing_pixels);

		// Make sure we always read from the correct level; the chosen
		// mipmapping could otherwise be rather unpredictable, especially
		// during motion search.
		GLuint tex0_view, tex1_view;
		glGenTextures(1, &tex0_view);
		glTextureView(tex0_view, GL_TEXTURE_2D, tex0, GL_R8, level, 1, 0, 1);
		glGenTextures(1, &tex1_view);
		glTextureView(tex1_view, GL_TEXTURE_2D, tex1, GL_R8, level, 1, 0, 1);

		// Create a new texture; we could be fancy and render use a multi-level
		// texture, but meh.
		GLuint grad0_tex = pool.get_texture(GL_RG16F, level_width, level_height);

		// Find the derivative.
		{
			ScopedTimer timer("Sobel", &level_timer);
			sobel.exec(tex0_view, grad0_tex, level_width, level_height);
		}

		// Motion search to find the initial flow. We use the flow from the previous
		// level (sampled bilinearly; no fancy tricks) as a guide, then search from there.

		// Create an output flow texture.
		GLuint flow_out_tex = pool.get_texture(GL_RGB16F, width_patches, height_patches);

		// And draw.
		{
			ScopedTimer timer("Motion search", &level_timer);
			motion_search.exec(tex0_view, tex1_view, grad0_tex, prev_level_flow_tex, flow_out_tex, level_width, level_height, prev_level_width, prev_level_height, width_patches, height_patches);
		}
		pool.release_texture(grad0_tex);

		// Densification.

		// Set up an output texture (initially zero).
		GLuint dense_flow_tex = pool.get_texture(GL_RGB16F, level_width, level_height);
		glClearTexImage(dense_flow_tex, 0, GL_RGB, GL_FLOAT, nullptr);

		// And draw.
		{
			ScopedTimer timer("Densification", &level_timer);
			densify.exec(tex0_view, tex1_view, flow_out_tex, dense_flow_tex, level_width, level_height, width_patches, height_patches);
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
		GLuint I_tex = pool.get_texture(GL_R16F, level_width, level_height);
		GLuint I_t_tex = pool.get_texture(GL_R16F, level_width, level_height);
		GLuint base_flow_tex = pool.get_texture(GL_RG16F, level_width, level_height);
		{
			ScopedTimer timer("Prewarping", &varref_timer);
			prewarp.exec(tex0_view, tex1_view, dense_flow_tex, I_tex, I_t_tex, base_flow_tex, level_width, level_height);
		}
		pool.release_texture(dense_flow_tex);
		glDeleteTextures(1, &tex0_view);
		glDeleteTextures(1, &tex1_view);

		// Calculate I_x and I_y. We're only calculating first derivatives;
		// the others will be taken on-the-fly in order to sample from fewer
		// textures overall, since sampling from the L1 cache is cheap.
		// (TODO: Verify that this is indeed faster than making separate
		// double-derivative textures.)
		GLuint I_x_y_tex = pool.get_texture(GL_RG16F, level_width, level_height);
		GLuint beta_0_tex = pool.get_texture(GL_R16F, level_width, level_height);
		{
			ScopedTimer timer("First derivatives", &varref_timer);
			derivatives.exec(I_tex, I_x_y_tex, beta_0_tex, level_width, level_height);
		}
		pool.release_texture(I_tex);

		// We need somewhere to store du and dv (the flow increment, relative
		// to the non-refined base flow u0 and v0). It starts at zero.
		GLuint du_dv_tex = pool.get_texture(GL_RG16F, level_width, level_height);
		glClearTexImage(du_dv_tex, 0, GL_RG, GL_FLOAT, nullptr);

		// And for smoothness.
		GLuint smoothness_x_tex = pool.get_texture(GL_R16F, level_width, level_height);
		GLuint smoothness_y_tex = pool.get_texture(GL_R16F, level_width, level_height);

		// And finally for the equation set. See SetupEquations for
		// the storage format.
		GLuint equation_tex = pool.get_texture(GL_RGBA32UI, level_width, level_height);

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

		pool.release_texture(I_t_tex);
		pool.release_texture(I_x_y_tex);
		pool.release_texture(beta_0_tex);
		pool.release_texture(smoothness_x_tex);
		pool.release_texture(smoothness_y_tex);
		pool.release_texture(equation_tex);

		// Add the differential flow found by the variational refinement to the base flow,
		// giving the final flow estimate for this level.
		// The output is in diff_flow_tex; we don't need to make a new texture.
		//
		// Disabling this doesn't save any time (although we could easily make it so that
		// it is more efficient), but it helps debug the motion search.
		if (enable_variational_refinement) {
			ScopedTimer timer("Add differential flow", &varref_timer);
			add_base_flow.exec(base_flow_tex, du_dv_tex, level_width, level_height);
		}
		pool.release_texture(du_dv_tex);

		if (prev_level_flow_tex != initial_flow_tex) {
			pool.release_texture(prev_level_flow_tex);
		}
		prev_level_flow_tex = base_flow_tex;
		prev_level_width = level_width;
		prev_level_height = level_height;
	}
	total_timer.end();

	timers.print();

	// Scale up the flow to the final size (if needed).
	if (finest_level == 0 || resize_strategy == DO_NOT_RESIZE_FLOW) {
		return prev_level_flow_tex;
	} else {
		GLuint final_tex = pool.get_texture(GL_RG16F, width, height);
		resize_flow.exec(prev_level_flow_tex, final_tex, prev_level_width, prev_level_height, width, height);
		pool.release_texture(prev_level_flow_tex);
		return final_tex;
	}
}

// Forward-warp the flow half-way (or rather, by alpha). A non-zero “splatting”
// radius fills most of the holes.
class Splat {
public:
	Splat();

	// alpha is the time of the interpolated frame (0..1).
	void exec(GLuint tex0, GLuint tex1, GLuint forward_flow_tex, GLuint backward_flow_tex, GLuint flow_tex, GLuint depth_tex, int width, int height, float alpha);

private:
	PersistentFBOSet<2> fbos;

	GLuint splat_vs_obj;
	GLuint splat_fs_obj;
	GLuint splat_program;
	GLuint splat_vao;

	GLuint uniform_invert_flow, uniform_splat_size, uniform_alpha;
	GLuint uniform_image0_tex, uniform_image1_tex, uniform_flow_tex;
	GLuint uniform_inv_flow_size;
};

Splat::Splat()
{
	splat_vs_obj = compile_shader(read_file("splat.vert"), GL_VERTEX_SHADER);
	splat_fs_obj = compile_shader(read_file("splat.frag"), GL_FRAGMENT_SHADER);
	splat_program = link_program(splat_vs_obj, splat_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &splat_vao);
	glBindVertexArray(splat_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(splat_program, "position");
	glEnableVertexArrayAttrib(splat_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_invert_flow = glGetUniformLocation(splat_program, "invert_flow");
	uniform_splat_size = glGetUniformLocation(splat_program, "splat_size");
	uniform_alpha = glGetUniformLocation(splat_program, "alpha");
	uniform_image0_tex = glGetUniformLocation(splat_program, "image0_tex");
	uniform_image1_tex = glGetUniformLocation(splat_program, "image1_tex");
	uniform_flow_tex = glGetUniformLocation(splat_program, "flow_tex");
	uniform_inv_flow_size = glGetUniformLocation(splat_program, "inv_flow_size");
}

void Splat::exec(GLuint tex0, GLuint tex1, GLuint forward_flow_tex, GLuint backward_flow_tex, GLuint flow_tex, GLuint depth_tex, int width, int height, float alpha)
{
	glUseProgram(splat_program);

	bind_sampler(splat_program, uniform_image0_tex, 0, tex0, linear_sampler);
	bind_sampler(splat_program, uniform_image1_tex, 1, tex1, linear_sampler);

	// FIXME: This is set to 1.0 right now so not to trigger Haswell's “PMA stall”.
	// Move to 2.0 later, or even 4.0.
	// (Since we have hole filling, it's not critical, but larger values seem to do
	// better than hole filling for large motion, blurs etc.)
	float splat_size = 1.0f;  // 4x4 splat means 16x overdraw, 2x2 splat means 4x overdraw.
	glProgramUniform2f(splat_program, uniform_splat_size, splat_size / width, splat_size / height);
	glProgramUniform1f(splat_program, uniform_alpha, alpha);
	glProgramUniform2f(splat_program, uniform_inv_flow_size, 1.0f / width, 1.0f / height);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);  // We store the difference between I_0 and I_1, where less difference is good. (Default 1.0 is effectively +inf, which always loses.)
	glBindVertexArray(splat_vao);

	// FIXME: Get this into FBOSet, so we can reuse FBOs across frames.
	GLuint fbo;
	glCreateFramebuffers(1, &fbo);
	glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, flow_tex, 0);
	glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	// Do forward splatting.
	bind_sampler(splat_program, uniform_flow_tex, 2, forward_flow_tex, nearest_sampler);
	glProgramUniform1i(splat_program, uniform_invert_flow, 0);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width * height);

	// Do backward splatting.
	bind_sampler(splat_program, uniform_flow_tex, 2, backward_flow_tex, nearest_sampler);
	glProgramUniform1i(splat_program, uniform_invert_flow, 1);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width * height);

	glDisable(GL_DEPTH_TEST);

	glDeleteFramebuffers(1, &fbo);
}

// Doing good and fast hole-filling on a GPU is nontrivial. We choose an option
// that's fairly simple (given that most holes are really small) and also hopefully
// cheap should the holes not be so small. Conceptually, we look for the first
// non-hole to the left of us (ie., shoot a ray until we hit something), then
// the first non-hole to the right of us, then up and down, and then average them
// all together. It's going to create “stars” if the holes are big, but OK, that's
// a tradeoff.
//
// Our implementation here is efficient assuming that the hierarchical Z-buffer is
// on even for shaders that do discard (this typically kills early Z, but hopefully
// not hierarchical Z); we set up Z so that only holes are written to, which means
// that as soon as a hole is filled, the rasterizer should just skip it. Most of the
// fullscreen quads should just be discarded outright, really.
class HoleFill {
public:
	HoleFill();

	// Output will be in flow_tex, temp_tex[0, 1, 2], representing the filling
	// from the down, left, right and up, respectively. Use HoleBlend to merge
	// them into one.
	void exec(GLuint flow_tex, GLuint depth_tex, GLuint temp_tex[3], int width, int height);

private:
	PersistentFBOSet<2> fbos;

	GLuint fill_vs_obj;
	GLuint fill_fs_obj;
	GLuint fill_program;
	GLuint fill_vao;

	GLuint uniform_tex;
	GLuint uniform_z, uniform_sample_offset;
};

HoleFill::HoleFill()
{
	fill_vs_obj = compile_shader(read_file("hole_fill.vert"), GL_VERTEX_SHADER);
	fill_fs_obj = compile_shader(read_file("hole_fill.frag"), GL_FRAGMENT_SHADER);
	fill_program = link_program(fill_vs_obj, fill_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &fill_vao);
	glBindVertexArray(fill_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(fill_program, "position");
	glEnableVertexArrayAttrib(fill_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_tex = glGetUniformLocation(fill_program, "tex");
	uniform_z = glGetUniformLocation(fill_program, "z");
	uniform_sample_offset = glGetUniformLocation(fill_program, "sample_offset");
}

void HoleFill::exec(GLuint flow_tex, GLuint depth_tex, GLuint temp_tex[3], int width, int height)
{
	glUseProgram(fill_program);

	bind_sampler(fill_program, uniform_tex, 0, flow_tex, nearest_sampler);

	glProgramUniform1f(fill_program, uniform_z, 1.0f - 1.0f / 1024.0f);

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);  // Only update the values > 0.999f (ie., only invalid pixels).
	glBindVertexArray(fill_vao);

	// FIXME: Get this into FBOSet, so we can reuse FBOs across frames.
	GLuint fbo;
	glCreateFramebuffers(1, &fbo);
	glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, flow_tex, 0);  // NOTE: Reading and writing to the same texture.
	glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

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

	glDeleteFramebuffers(1, &fbo);
}

// Blend the four directions from HoleFill into one pixel, so that single-pixel
// holes become the average of their four neighbors.
class HoleBlend {
public:
	HoleBlend();

	void exec(GLuint flow_tex, GLuint depth_tex, GLuint temp_tex[3], int width, int height);

private:
	PersistentFBOSet<2> fbos;

	GLuint blend_vs_obj;
	GLuint blend_fs_obj;
	GLuint blend_program;
	GLuint blend_vao;

	GLuint uniform_left_tex, uniform_right_tex, uniform_up_tex, uniform_down_tex;
	GLuint uniform_z, uniform_sample_offset;
};

HoleBlend::HoleBlend()
{
	blend_vs_obj = compile_shader(read_file("hole_fill.vert"), GL_VERTEX_SHADER);  // Reuse the vertex shader from the fill.
	blend_fs_obj = compile_shader(read_file("hole_blend.frag"), GL_FRAGMENT_SHADER);
	blend_program = link_program(blend_vs_obj, blend_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &blend_vao);
	glBindVertexArray(blend_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	GLint position_attrib = glGetAttribLocation(blend_program, "position");
	glEnableVertexArrayAttrib(blend_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_left_tex = glGetUniformLocation(blend_program, "left_tex");
	uniform_right_tex = glGetUniformLocation(blend_program, "right_tex");
	uniform_up_tex = glGetUniformLocation(blend_program, "up_tex");
	uniform_down_tex = glGetUniformLocation(blend_program, "down_tex");
	uniform_z = glGetUniformLocation(blend_program, "z");
	uniform_sample_offset = glGetUniformLocation(blend_program, "sample_offset");
}

void HoleBlend::exec(GLuint flow_tex, GLuint depth_tex, GLuint temp_tex[3], int width, int height)
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
	glBindVertexArray(blend_vao);

	// FIXME: Get this into FBOSet, so we can reuse FBOs across frames.
	GLuint fbo;
	glCreateFramebuffers(1, &fbo);
	glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, flow_tex, 0);  // NOTE: Reading and writing to the same texture.
	glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisable(GL_DEPTH_TEST);

	glDeleteFramebuffers(1, &fbo);
}

class Blend {
public:
	Blend();
	void exec(GLuint tex0, GLuint tex1, GLuint flow_tex, GLuint output_tex, int width, int height, float alpha);

private:
	PersistentFBOSet<1> fbos;
	GLuint blend_vs_obj;
	GLuint blend_fs_obj;
	GLuint blend_program;
	GLuint blend_vao;

	GLuint uniform_image0_tex, uniform_image1_tex, uniform_flow_tex;
	GLuint uniform_alpha, uniform_flow_consistency_tolerance;
};

Blend::Blend()
{
	blend_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	blend_fs_obj = compile_shader(read_file("blend.frag"), GL_FRAGMENT_SHADER);
	blend_program = link_program(blend_vs_obj, blend_fs_obj);

	// Set up the VAO containing all the required position/texcoord data.
	glCreateVertexArrays(1, &blend_vao);
	glBindVertexArray(blend_vao);

	GLint position_attrib = glGetAttribLocation(blend_program, "position");
	glEnableVertexArrayAttrib(blend_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_image0_tex = glGetUniformLocation(blend_program, "image0_tex");
	uniform_image1_tex = glGetUniformLocation(blend_program, "image1_tex");
	uniform_flow_tex = glGetUniformLocation(blend_program, "flow_tex");
	uniform_alpha = glGetUniformLocation(blend_program, "alpha");
	uniform_flow_consistency_tolerance = glGetUniformLocation(blend_program, "flow_consistency_tolerance");
}

void Blend::exec(GLuint tex0, GLuint tex1, GLuint flow_tex, GLuint output_tex, int level_width, int level_height, float alpha)
{
	glUseProgram(blend_program);
	bind_sampler(blend_program, uniform_image0_tex, 0, tex0, linear_sampler);
	bind_sampler(blend_program, uniform_image1_tex, 1, tex1, linear_sampler);
	bind_sampler(blend_program, uniform_flow_tex, 2, flow_tex, linear_sampler);  // May be upsampled.
	glProgramUniform1f(blend_program, uniform_alpha, alpha);
	//glProgramUniform1f(blend_program, uniform_flow_consistency_tolerance, 1.0f / 

	glViewport(0, 0, level_width, level_height);
	fbos.render_to(output_tex);
	glBindVertexArray(blend_vao);
	glUseProgram(blend_program);
	glDisable(GL_BLEND);  // A bit ironic, perhaps.
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

class Interpolate {
public:
	Interpolate(int width, int height, int flow_level);

	// Returns a texture that must be released with release_texture()
	// after use. tex0 and tex1 must be RGBA8 textures with mipmaps
	// (unless flow_level == 0).
	GLuint exec(GLuint tex0, GLuint tex1, GLuint forward_flow_tex, GLuint backward_flow_tex, GLuint width, GLuint height, float alpha);

	void release_texture(GLuint tex) {
		pool.release_texture(tex);
	}

private:
	int width, height, flow_level;
	TexturePool pool;
	Splat splat;
	HoleFill hole_fill;
	HoleBlend hole_blend;
	Blend blend;
};

Interpolate::Interpolate(int width, int height, int flow_level)
	: width(width), height(height), flow_level(flow_level) {}

GLuint Interpolate::exec(GLuint tex0, GLuint tex1, GLuint forward_flow_tex, GLuint backward_flow_tex, GLuint width, GLuint height, float alpha)
{
	GPUTimers timers;

	ScopedTimer total_timer("Total", &timers);

	// Pick out the right level to test splatting results on.
	GLuint tex0_view, tex1_view;
	glGenTextures(1, &tex0_view);
	glTextureView(tex0_view, GL_TEXTURE_2D, tex0, GL_RGBA8, flow_level, 1, 0, 1);
	glGenTextures(1, &tex1_view);
	glTextureView(tex1_view, GL_TEXTURE_2D, tex1, GL_RGBA8, flow_level, 1, 0, 1);

	int flow_width = width >> flow_level;
	int flow_height = height >> flow_level;

	GLuint flow_tex = pool.get_texture(GL_RG16F, flow_width, flow_height);
	GLuint depth_tex = pool.get_texture(GL_DEPTH_COMPONENT32F, flow_width, flow_height);  // Used for ranking flows.
	{
		ScopedTimer timer("Clear", &total_timer);
		float invalid_flow[] = { 1000.0f, 1000.0f };
		glClearTexImage(flow_tex, 0, GL_RG, GL_FLOAT, invalid_flow);
		float infinity = 1.0f;
		glClearTexImage(depth_tex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &infinity);
	}

	{
		ScopedTimer timer("Splat", &total_timer);
		splat.exec(tex0_view, tex1_view, forward_flow_tex, backward_flow_tex, flow_tex, depth_tex, flow_width, flow_height, alpha);
	}
	glDeleteTextures(1, &tex0_view);
	glDeleteTextures(1, &tex1_view);

	GLuint temp_tex[3];
	temp_tex[0] = pool.get_texture(GL_RG16F, flow_width, flow_height);
	temp_tex[1] = pool.get_texture(GL_RG16F, flow_width, flow_height);
	temp_tex[2] = pool.get_texture(GL_RG16F, flow_width, flow_height);

	{
		ScopedTimer timer("Fill holes", &total_timer);
		hole_fill.exec(flow_tex, depth_tex, temp_tex, flow_width, flow_height);
		hole_blend.exec(flow_tex, depth_tex, temp_tex, flow_width, flow_height);
	}

	pool.release_texture(temp_tex[0]);
	pool.release_texture(temp_tex[1]);
	pool.release_texture(temp_tex[2]);
	pool.release_texture(depth_tex);

	GLuint output_tex = pool.get_texture(GL_RGBA8, width, height);
	{
		ScopedTimer timer("Blend", &total_timer);
		blend.exec(tex0, tex1, flow_tex, output_tex, width, height, alpha);
	}
	total_timer.end();
	timers.print();

	return output_tex;
}

GLuint TexturePool::get_texture(GLenum format, GLuint width, GLuint height)
{
	for (Texture &tex : textures) {
		if (!tex.in_use && tex.format == format &&
		    tex.width == width && tex.height == height) {
			tex.in_use = true;
			return tex.tex_num;
		}
	}

	Texture tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex.tex_num);
	glTextureStorage2D(tex.tex_num, 1, format, width, height);
	tex.format = format;
	tex.width = width;
	tex.height = height;
	tex.in_use = true;
	textures.push_back(tex);
	return tex.tex_num;
}

void TexturePool::release_texture(GLuint tex_num)
{
	for (Texture &tex : textures) {
		if (tex.tex_num == tex_num) {
			assert(tex.in_use);
			tex.in_use = false;
			return;
		}
	}
	assert(false);
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

	// Set up some PBOs to do asynchronous readback.
	GLuint pbos[5];
	glCreateBuffers(5, pbos);
	for (int i = 0; i < 5; ++i) {
		glNamedBufferData(pbos[i], width1 * height1 * 2 * sizeof(float), nullptr, GL_STREAM_READ);
		spare_pbos.push(pbos[i]);
	}

	int levels = find_num_levels(width1, height1);
	GLuint tex0_gray, tex1_gray;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex0_gray);
	glCreateTextures(GL_TEXTURE_2D, 1, &tex1_gray);
	glTextureStorage2D(tex0_gray, levels, GL_R8, width1, height1);
	glTextureStorage2D(tex1_gray, levels, GL_R8, width1, height1);

	GrayscaleConversion gray;
	gray.exec(tex0, tex0_gray, width1, height1);
	glDeleteTextures(1, &tex0);
	glGenerateTextureMipmap(tex0_gray);

	gray.exec(tex1, tex1_gray, width1, height1);
	glDeleteTextures(1, &tex1);
	glGenerateTextureMipmap(tex1_gray);

	DISComputeFlow compute_flow(width1, height1);
	GLuint final_tex = compute_flow.exec(tex0_gray, tex1_gray, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);

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
		gray.exec(tex0, tex0_gray, width, height);
		glGenerateTextureMipmap(tex0_gray);
		glDeleteTextures(1, &tex0);

		GLuint tex1 = load_texture(filename1, &width, &height, WITHOUT_MIPMAPS);
		if (width != width1 || height != height1) {
			fprintf(stderr, "%s: Image dimensions don't match (%dx%d versus %dx%d)\n",
				filename1, width, height, width1, height1);
			exit(1);
		}
		gray.exec(tex1, tex1_gray, width, height);
		glGenerateTextureMipmap(tex1_gray);
		glDeleteTextures(1, &tex1);

		GLuint final_tex = compute_flow.exec(tex0_gray, tex1_gray, DISComputeFlow::RESIZE_FLOW_TO_FULL_SIZE);

		schedule_read<FlowType>(final_tex, width1, height1, filename0, filename1, flow_filename, "");
		compute_flow.release_texture(final_tex);
	}
	glDeleteTextures(1, &tex0_gray);
	glDeleteTextures(1, &tex1_gray);

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

	// Set up some PBOs to do asynchronous readback.
	GLuint pbos[5];
	glCreateBuffers(5, pbos);
	for (int i = 0; i < 5; ++i) {
		glNamedBufferData(pbos[i], width1 * height1 * 4 * sizeof(uint8_t), nullptr, GL_STREAM_READ);
		spare_pbos.push(pbos[i]);
	}

	DISComputeFlow compute_flow(width1, height1);
	GrayscaleConversion gray;
	Interpolate interpolate(width1, height1, finest_level);

	int levels = find_num_levels(width1, height1);
	GLuint tex0_gray, tex1_gray;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex0_gray);
	glCreateTextures(GL_TEXTURE_2D, 1, &tex1_gray);
	glTextureStorage2D(tex0_gray, levels, GL_R8, width1, height1);
	glTextureStorage2D(tex1_gray, levels, GL_R8, width1, height1);

	gray.exec(tex0, tex0_gray, width1, height1);
	glGenerateTextureMipmap(tex0_gray);

	gray.exec(tex1, tex1_gray, width1, height1);
	glGenerateTextureMipmap(tex1_gray);

	GLuint forward_flow_tex = compute_flow.exec(tex0_gray, tex1_gray, DISComputeFlow::DO_NOT_RESIZE_FLOW);
	GLuint backward_flow_tex = compute_flow.exec(tex1_gray, tex0_gray, DISComputeFlow::DO_NOT_RESIZE_FLOW);

	for (int frameno = 1; frameno < 60; ++frameno) {
		char ppm_filename[256];
		snprintf(ppm_filename, sizeof(ppm_filename), "interp%04d.ppm", frameno);

		float alpha = frameno / 60.0f;
		GLuint interpolated_tex = interpolate.exec(tex0, tex1, forward_flow_tex, backward_flow_tex, width1, height1, alpha);

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
		{ "ignore-variational-refinement", no_argument, 0, 1001 },  // Still calculates it, just doesn't apply it.
		{ "interpolate", no_argument, 0, 1002 }
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
