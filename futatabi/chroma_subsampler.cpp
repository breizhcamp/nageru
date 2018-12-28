#include "chroma_subsampler.h"

#include "embedded_files.h"

#include <movit/util.h>
#include <string>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

string read_file(const string &filename, const unsigned char *start = nullptr, const size_t size = 0);
GLuint compile_shader(const string &shader_src, GLenum type);
GLuint link_program(GLuint vs_obj, GLuint fs_obj);
void bind_sampler(GLuint program, GLint location, GLuint texture_unit, GLuint tex, GLuint sampler);

extern GLuint linear_sampler;

ChromaSubsampler::ChromaSubsampler()
{
	// Set up stuff for 4:2:2 conversion.
	//
	// Note: Due to the horizontally co-sited chroma/luma samples in H.264
	// (chroma position is left for horizontal),
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

	cbcr_vs_obj = compile_shader(read_file("chroma_subsample.vert", _binary_chroma_subsample_vert_data, _binary_chroma_subsample_vert_size), GL_VERTEX_SHADER);
	cbcr_fs_obj = compile_shader(read_file("chroma_subsample.frag", _binary_chroma_subsample_frag_data, _binary_chroma_subsample_frag_size), GL_FRAGMENT_SHADER);
	cbcr_program = link_program(cbcr_vs_obj, cbcr_fs_obj);

	// Set up the VAO containing all the required position data.
	glCreateVertexArrays(1, &vao);
	glBindVertexArray(vao);

	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	glCreateBuffers(1, &vbo);
	glNamedBufferData(vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);

	GLint position_attrib = 0;  // Hard-coded in every vertex shader.
	glEnableVertexArrayAttrib(vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	uniform_cbcr_tex = glGetUniformLocation(cbcr_program, "cbcr_tex");
	uniform_chroma_offset_0 = glGetUniformLocation(cbcr_program, "chroma_offset_0");
	uniform_chroma_offset_1 = glGetUniformLocation(cbcr_program, "chroma_offset_1");
}

ChromaSubsampler::~ChromaSubsampler()
{
	glDeleteProgram(cbcr_program);
	check_error();
	glDeleteBuffers(1, &vbo);
	check_error();
	glDeleteVertexArrays(1, &vao);
	check_error();
}

void ChromaSubsampler::subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint cb_tex, GLuint cr_tex)
{
	glUseProgram(cbcr_program);
	bind_sampler(cbcr_program, uniform_cbcr_tex, 0, cbcr_tex, linear_sampler);
	glProgramUniform2f(cbcr_program, uniform_chroma_offset_0, -1.0f / width, 0.0f);
	glProgramUniform2f(cbcr_program, uniform_chroma_offset_1, -0.0f / width, 0.0f);

	glViewport(0, 0, width / 2, height);
	fbos.render_to(cb_tex, cr_tex);

	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}
