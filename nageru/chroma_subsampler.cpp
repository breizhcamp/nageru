#include "chroma_subsampler.h"
#include "v210_converter.h"

#include <vector>

#include <movit/effect_util.h>
#include <movit/resource_pool.h>
#include <movit/util.h>

#include "embedded_files.h"
#include "shared/read_file.h"

using namespace movit;
using namespace std;

ChromaSubsampler::ChromaSubsampler(ResourcePool *resource_pool)
	: resource_pool(resource_pool)
{
	vector<string> frag_shader_outputs;

	// Set up stuff for NV12 conversion.
	//
	// Note: Due to the horizontally co-sited chroma/luma samples in H.264
	// (chrome position is left for horizontal and center for vertical),
	// we need to be a bit careful in our subsampling. A diagram will make
	// this clearer, showing some luma and chroma samples:
	//
	//     a   b   c   d
	//   +---+---+---+---+
	//   |   |   |   |   |
	//   | Y | Y | Y | Y |
	//   |   |   |   |   |
	//   +---+---+---+---+
	//
	// +-------+-------+
	// |       |       |
	// |   C   |   C   |
	// |       |       |
	// +-------+-------+
	//
	// Clearly, the rightmost chroma sample here needs to be equivalent to
	// b/4 + c/2 + d/4. (We could also implement more sophisticated filters,
	// of course, but as long as the upsampling is not going to be equally
	// sophisticated, it's probably not worth it.) If we sample once with
	// no mipmapping, we get just c, ie., no actual filtering in the
	// horizontal direction. (For the vertical direction, we can just
	// sample in the middle to get the right filtering.) One could imagine
	// we could use mipmapping (assuming we can create mipmaps cheaply),
	// but then, what we'd get is this:
	//
	//    (a+b)/2 (c+d)/2
	//   +-------+-------+
	//   |       |       |
	//   |   Y   |   Y   |
	//   |       |       |
	//   +-------+-------+
	//
	// +-------+-------+
	// |       |       |
	// |   C   |   C   |
	// |       |       |
	// +-------+-------+
	//
	// which ends up sampling equally from a and b, which clearly isn't right. Instead,
	// we need to do two (non-mipmapped) chroma samples, both hitting exactly in-between
	// source pixels.
	//
	// Sampling in-between b and c gives us the sample (b+c)/2, and similarly for c and d.
	// Taking the average of these gives of (b+c)/4 + (c+d)/4 = b/4 + c/2 + d/4, which is
	// exactly what we want.
	//
	// See also http://www.poynton.com/PDFs/Merging_RGB_and_422.pdf, pages 6–7.

	// Cb/Cr shader.
	string cbcr_vert_shader = read_file("cbcr_subsample.vert", _binary_cbcr_subsample_vert_data, _binary_cbcr_subsample_vert_size);
	string cbcr_frag_shader = read_file("cbcr_subsample.frag", _binary_cbcr_subsample_frag_data, _binary_cbcr_subsample_frag_size);
	cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader, frag_shader_outputs);
	check_error();
	cbcr_chroma_offset_0_location = get_uniform_location(cbcr_program_num, "foo", "chroma_offset_0");
	check_error();
	cbcr_chroma_offset_1_location = get_uniform_location(cbcr_program_num, "foo", "chroma_offset_1");
	check_error();

	cbcr_texture_sampler_uniform = glGetUniformLocation(cbcr_program_num, "cbcr_tex");
	check_error();
	cbcr_position_attribute_index = glGetAttribLocation(cbcr_program_num, "position");
	check_error();
	cbcr_texcoord_attribute_index = glGetAttribLocation(cbcr_program_num, "texcoord");
	check_error();

	// Same, for UYVY conversion.
	string uyvy_vert_shader = read_file("uyvy_subsample.vert", _binary_uyvy_subsample_vert_data, _binary_uyvy_subsample_vert_size);
	string uyvy_frag_shader = read_file("uyvy_subsample.frag", _binary_uyvy_subsample_frag_data, _binary_uyvy_subsample_frag_size);

	uyvy_program_num = resource_pool->compile_glsl_program(uyvy_vert_shader, uyvy_frag_shader, frag_shader_outputs);
	check_error();
	uyvy_luma_offset_0_location = get_uniform_location(uyvy_program_num, "foo", "luma_offset_0");
	check_error();
	uyvy_luma_offset_1_location = get_uniform_location(uyvy_program_num, "foo", "luma_offset_1");
	check_error();
	uyvy_chroma_offset_0_location = get_uniform_location(uyvy_program_num, "foo", "chroma_offset_0");
	check_error();
	uyvy_chroma_offset_1_location = get_uniform_location(uyvy_program_num, "foo", "chroma_offset_1");
	check_error();

	uyvy_y_texture_sampler_uniform = glGetUniformLocation(uyvy_program_num, "y_tex");
	check_error();
	uyvy_cbcr_texture_sampler_uniform = glGetUniformLocation(uyvy_program_num, "cbcr_tex");
	check_error();
	uyvy_position_attribute_index = glGetAttribLocation(uyvy_program_num, "position");
	check_error();
	uyvy_texcoord_attribute_index = glGetAttribLocation(uyvy_program_num, "texcoord");
	check_error();

	// Shared between the two.
	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	vbo = generate_vbo(2, GL_FLOAT, sizeof(vertices), vertices);
	check_error();

	// v210 compute shader.
	if (v210Converter::has_hardware_support()) {
		string v210_shader_src = read_file("v210_subsample.comp", _binary_v210_subsample_comp_data, _binary_v210_subsample_comp_size);
		GLuint shader_num = movit::compile_shader(v210_shader_src, GL_COMPUTE_SHADER);
		check_error();
		v210_program_num = glCreateProgram();
		check_error();
		glAttachShader(v210_program_num, shader_num);
		check_error();
		glLinkProgram(v210_program_num);
		check_error();

		GLint success;
		glGetProgramiv(v210_program_num, GL_LINK_STATUS, &success);
		check_error();
		if (success == GL_FALSE) {
			GLchar error_log[1024] = {0};
			glGetProgramInfoLog(v210_program_num, 1024, nullptr, error_log);
			fprintf(stderr, "Error linking program: %s\n", error_log);
			exit(1);
		}

		v210_in_y_pos = glGetUniformLocation(v210_program_num, "in_y");
		check_error();
		v210_in_cbcr_pos = glGetUniformLocation(v210_program_num, "in_cbcr");
		check_error();
		v210_outbuf_pos = glGetUniformLocation(v210_program_num, "outbuf");
		check_error();
		v210_inv_width_pos = glGetUniformLocation(v210_program_num, "inv_width");
		check_error();
		v210_inv_height_pos = glGetUniformLocation(v210_program_num, "inv_height");
		check_error();
	} else {
		v210_program_num = 0;
	}
}

ChromaSubsampler::~ChromaSubsampler()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	check_error();
	resource_pool->release_glsl_program(uyvy_program_num);
	check_error();
	glDeleteBuffers(1, &vbo);
	check_error();
	if (v210_program_num != 0) {
		glDeleteProgram(v210_program_num);
		check_error();
	}
}

void ChromaSubsampler::subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex, GLuint dst2_tex)
{
	GLuint vao = resource_pool->create_vec2_vao({ cbcr_position_attribute_index, cbcr_texcoord_attribute_index }, vbo);
	glBindVertexArray(vao);
	check_error();

	// Extract Cb/Cr.
	GLuint fbo;
	if (dst2_tex <= 0) {
		fbo = resource_pool->create_fbo(dst_tex);
	} else {
		fbo = resource_pool->create_fbo(dst_tex, dst2_tex);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width/2, height/2);
	check_error();

	glUseProgram(cbcr_program_num);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, cbcr_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glUniform2f(cbcr_chroma_offset_0_location, -1.0f / width, 0.0f);
	check_error();
	glUniform2f(cbcr_chroma_offset_1_location, -0.0f / width, 0.0f);
	check_error();
	glUniform1i(cbcr_texture_sampler_uniform, 0);

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	glUseProgram(0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();
	glBindVertexArray(0);
	check_error();

	resource_pool->release_fbo(fbo);
	resource_pool->release_vec2_vao(vao);
}

void ChromaSubsampler::create_uyvy(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex)
{
	GLuint vao = resource_pool->create_vec2_vao({ cbcr_position_attribute_index, cbcr_texcoord_attribute_index }, vbo);
	glBindVertexArray(vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	GLuint fbo = resource_pool->create_fbo(dst_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width/2, height);
	check_error();

	glUseProgram(uyvy_program_num);
	check_error();

	glUniform1i(uyvy_y_texture_sampler_uniform, 0);
	check_error();
	glUniform1i(uyvy_cbcr_texture_sampler_uniform, 1);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, y_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glActiveTexture(GL_TEXTURE1);
	check_error();
	glBindTexture(GL_TEXTURE_2D, cbcr_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glUniform2f(uyvy_luma_offset_0_location, -0.5f / width, 0.0f);
	check_error();
	glUniform2f(uyvy_luma_offset_1_location,  0.5f / width, 0.0f);
	check_error();
	glUniform2f(uyvy_chroma_offset_0_location, -1.0f / width, 0.0f);
	check_error();
	glUniform2f(uyvy_chroma_offset_1_location, -0.0f / width, 0.0f);
	check_error();

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	check_error();

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glUseProgram(0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();
	glBindVertexArray(0);
	check_error();

	resource_pool->release_fbo(fbo);
	resource_pool->release_vec2_vao(vao);
}

void ChromaSubsampler::create_v210(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex)
{
	assert(v210_program_num != 0);

	glUseProgram(v210_program_num);
	check_error();

	glUniform1i(v210_in_y_pos, 0);
	check_error();
	glUniform1i(v210_in_cbcr_pos, 1);
	check_error();
	glUniform1i(v210_outbuf_pos, 2);
	check_error();
	glUniform1f(v210_inv_width_pos, 1.0 / width);
	check_error();
	glUniform1f(v210_inv_height_pos, 1.0 / height);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, y_tex);  // We don't actually need to bind it, but we need to set the state.
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();
	glBindImageTexture(0, y_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16);  // This is the real bind.
	check_error();

	glActiveTexture(GL_TEXTURE1);
	check_error();
	glBindTexture(GL_TEXTURE_2D, cbcr_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glBindImageTexture(2, dst_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGB10_A2);
	check_error();

	// Actually run the shader. We use workgroups of size 2x16 threadst , and each thread
	// processes 6x1 input pixels, so round up to number of 12x16 pixel blocks.
	glDispatchCompute((width + 11) / 12, (height + 15) / 16, 1);

	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	glActiveTexture(GL_TEXTURE0);
	check_error();
	glUseProgram(0);
	check_error();
}
