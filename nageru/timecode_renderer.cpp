#include "timecode_renderer.h"

#include <memory>
#include <string>
#include <vector>

#include <QImage>
#include <QPainter>

#include <epoxy/gl.h>
#include <movit/effect_util.h>
#include <movit/resource_pool.h>
#include <movit/util.h>
#include <sys/time.h>

#include "flags.h"
#include "embedded_files.h"
#include "shared/read_file.h"

using namespace std;
using namespace movit;

TimecodeRenderer::TimecodeRenderer(movit::ResourcePool *resource_pool, unsigned display_width, unsigned display_height)
	: resource_pool(resource_pool), display_width(display_width), display_height(display_height), height(28)
{
	string vert_shader = read_file("timecode.vert", _binary_timecode_vert_data, _binary_timecode_vert_size);
	string frag_shader;
	if (global_flags.ten_bit_output) {
		frag_shader = read_file("timecode_10bit.frag", _binary_timecode_10bit_frag_data, _binary_timecode_10bit_frag_size);
	} else {
		frag_shader = read_file("timecode.frag", _binary_timecode_frag_data, _binary_timecode_frag_size);
	}

	vector<string> frag_shader_outputs;
	program_num = resource_pool->compile_glsl_program(vert_shader, frag_shader, frag_shader_outputs);
	check_error();

	texture_sampler_uniform = glGetUniformLocation(program_num, "tex");
	check_error();
	position_attribute_index = glGetAttribLocation(program_num, "position");
	check_error();
	texcoord_attribute_index = glGetAttribLocation(program_num, "texcoord");
	check_error();

	// Shared between the two.
	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	vbo = generate_vbo(2, GL_FLOAT, sizeof(vertices), vertices);
	check_error();

	tex = resource_pool->create_2d_texture(GL_R8, display_width, height);

	image.reset(new QImage(display_width, height, QImage::Format_Grayscale8));
}

TimecodeRenderer::~TimecodeRenderer()
{
	resource_pool->release_2d_texture(tex);
        check_error();
	resource_pool->release_glsl_program(program_num);
        check_error();
	glDeleteBuffers(1, &vbo);
        check_error();
}

string TimecodeRenderer::get_timecode_text(double pts, unsigned frame_num)
{
	// Find the wall time, and round it to the nearest millisecond.
	timeval now;
	gettimeofday(&now, nullptr);
	time_t unixtime = now.tv_sec;
	unsigned msecs = (now.tv_usec + 500) / 1000;
	if (msecs >= 1000) {
		msecs -= 1000;
		++unixtime;
	}

	tm utc_tm;
	gmtime_r(&unixtime, &utc_tm);
	char clock_text[256];
	strftime(clock_text, sizeof(clock_text), "%Y-%m-%d %H:%M:%S", &utc_tm);

	// Make the stream timecode, rounded to the nearest millisecond.
	long stream_time = lrint(pts * 1e3);
	assert(stream_time >= 0);
	unsigned stream_time_ms = stream_time % 1000;
	stream_time /= 1000;
	unsigned stream_time_sec = stream_time % 60;
	stream_time /= 60;
	unsigned stream_time_min = stream_time % 60;
	unsigned stream_time_hour = stream_time / 60;

	char timecode_text[512];
	snprintf(timecode_text, sizeof(timecode_text), "Nageru - %s.%03u UTC - Stream time %02u:%02u:%02u.%03u (frame %u)",
		clock_text, msecs, stream_time_hour, stream_time_min, stream_time_sec, stream_time_ms, frame_num);
	return timecode_text;
}

void TimecodeRenderer::render_timecode(GLuint fbo, const string &text)
{
	render_string_to_buffer(text);
	render_buffer_to_fbo(fbo);
}

void TimecodeRenderer::render_string_to_buffer(const string &text)
{
	image->fill(0);
	QPainter painter(image.get());

	painter.setPen(Qt::white);
	QFont font = painter.font();
	font.setPointSize(16);
	painter.setFont(font);

	painter.drawText(QRectF(0, 0, display_width, height), Qt::AlignCenter, QString::fromStdString(text));
}

void TimecodeRenderer::render_buffer_to_fbo(GLuint fbo)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	check_error();

	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	glViewport(0, display_height - height, display_width, height);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, display_width, height, GL_RED, GL_UNSIGNED_BYTE, image->bits());
        check_error();

	glUseProgram(program_num);
	check_error();
	glUniform1i(texture_sampler_uniform, 0);
        check_error();

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        check_error();

	for (GLint attr_index : { position_attribute_index, texcoord_attribute_index }) {
		if (attr_index == -1) continue;
		glEnableVertexAttribArray(attr_index);
		check_error();
		glVertexAttribPointer(attr_index, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
		check_error();
	}

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	for (GLint attr_index : { position_attribute_index, texcoord_attribute_index }) {
		if (attr_index == -1) continue;
		glDisableVertexAttribArray(attr_index);
		check_error();
	}

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glUseProgram(0);
	check_error();

	glDeleteVertexArrays(1, &vao);
	check_error();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();
}
