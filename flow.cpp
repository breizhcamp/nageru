#define NO_SDL_GLEXT 1

#define WIDTH 1280
#define HEIGHT 720

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

#include <algorithm>
#include <memory>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

// Operating point 3 (10 Hz on CPU, excluding preprocessing).
constexpr float patch_overlap_ratio = 0.75f;
constexpr unsigned coarsest_level = 0;
constexpr unsigned finest_level = 0;
constexpr unsigned patch_size_pixels = 12;

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


GLuint load_texture(const char *filename, unsigned width, unsigned height)
{
	FILE *fp = fopen(filename, "rb");
	if (fp == nullptr) {
		perror(filename);
		exit(1);
	}
	unique_ptr<uint8_t[]> pix(new uint8_t[width * height]);
	if (fread(pix.get(), width * height, 1, fp) != 1) {
		fprintf(stderr, "Short read from %s\n", filename);
		exit(1);
	}
	fclose(fp);

	// Convert to bottom-left origin.
	for (unsigned y = 0; y < height / 2; ++y) {
		unsigned y2 = height - 1 - y;
		swap_ranges(&pix[y * width], &pix[y * width + width], &pix[y2 * width]);
	}

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

void bind_sampler(GLuint program, const char *uniform_name, GLuint texture_unit, GLuint tex, GLuint sampler)
{
	GLint location = glGetUniformLocation(program, uniform_name);
	if (location == -1) {
		return;
	}

	glBindTextureUnit(texture_unit, tex);
	glBindSampler(texture_unit, sampler);
	glProgramUniform1i(program, location, texture_unit);
}

int main(void)
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
	GLuint tex0 = load_texture("test1499.pgm", WIDTH, HEIGHT);
	GLuint tex1 = load_texture("test1500.pgm", WIDTH, HEIGHT);

	// Load shaders.
	GLuint motion_vs_obj = compile_shader(read_file("motion_search.vert"), GL_VERTEX_SHADER);
	GLuint motion_fs_obj = compile_shader(read_file("motion_search.frag"), GL_FRAGMENT_SHADER);
	GLuint motion_search_program = link_program(motion_vs_obj, motion_fs_obj);

	GLuint sobel_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	GLuint sobel_fs_obj = compile_shader(read_file("sobel.frag"), GL_FRAGMENT_SHADER);
	GLuint sobel_program = link_program(sobel_vs_obj, sobel_fs_obj);

	GLuint densify_vs_obj = compile_shader(read_file("densify.vert"), GL_VERTEX_SHADER);
	GLuint densify_fs_obj = compile_shader(read_file("densify.frag"), GL_FRAGMENT_SHADER);
	GLuint densify_program = link_program(densify_vs_obj, densify_fs_obj);

	// Make some samplers.
	GLuint nearest_sampler;
	glCreateSamplers(1, &nearest_sampler);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(nearest_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLuint linear_sampler;
	glCreateSamplers(1, &linear_sampler);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(linear_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLuint mipmap_sampler;
	glCreateSamplers(1, &mipmap_sampler);
	glSamplerParameteri(mipmap_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glSamplerParameteri(mipmap_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(mipmap_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(mipmap_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// Coarsest level.
	int level = coarsest_level;
	int level_width = WIDTH >> level;
	int level_height = HEIGHT >> level;
	float patch_spacing_pixels = patch_size_pixels * (1.0f - patch_overlap_ratio);
	int width_patches = 1 + lrintf((level_width - patch_size_pixels) / patch_spacing_pixels);
	int height_patches = 1 + lrintf((level_height - patch_size_pixels) / patch_spacing_pixels);

	// Make sure we always read from the correct level; the chosen
	// mipmapping could otherwise be rather unpredictable, especially
	// during motion search.
	GLuint tex0_view, tex1_view;
	glGenTextures(1, &tex0_view);
	glTextureView(tex0_view, GL_TEXTURE_2D, tex0, GL_R8, level, 1, 0, 1);
	glGenTextures(1, &tex1_view);
	glTextureView(tex1_view, GL_TEXTURE_2D, tex1, GL_R8, level, 1, 0, 1);

	// Compute gradients in every point, used for the motion search.
	// The DIS paper doesn't actually mention how these are computed,
	// but seemingly, a 3x3 Sobel operator is used here (at least in
	// later versions of the code), while a [1 -8 0 8 -1] kernel is
	// used for all the derivatives in the variational refinement part
	// (which borrows code from DeepFlow). This is inconsistent,
	// but I guess we're better off with staying with the original
	// decisions until we actually know having different ones would be better.

	// Create a new texture; we could be fancy and render use a multi-level
	// texture, but meh.
	GLuint grad0_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &grad0_tex);
	glTextureStorage2D(grad0_tex, 1, GL_RG16F, level_width, level_height);

	GLuint grad0_fbo;
	glCreateFramebuffers(1, &grad0_fbo);
	glNamedFramebufferTexture(grad0_fbo, GL_COLOR_ATTACHMENT0, grad0_tex, 0);

	glUseProgram(sobel_program);
	glBindTextureUnit(0, tex0_view);
	glBindSampler(0, nearest_sampler);
	glProgramUniform1i(sobel_program, glGetUniformLocation(sobel_program, "tex"), 0);
	glProgramUniform1f(sobel_program, glGetUniformLocation(sobel_program, "inv_width"), 1.0f / level_width);
	glProgramUniform1f(sobel_program, glGetUniformLocation(sobel_program, "inv_height"), 1.0f / level_height);

	// Set up the VAO containing all the required position/texcoord data.
	GLuint sobel_vao;
        glCreateVertexArrays(1, &sobel_vao);
        glBindVertexArray(sobel_vao);
	float vertices[] = {
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
	};
	GLuint vertex_vbo;
	glCreateBuffers(1, &vertex_vbo);
	glNamedBufferData(vertex_vbo, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	int position_attrib = glGetAttribLocation(sobel_program, "position");
	glEnableVertexArrayAttrib(sobel_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	int texcoord_attrib = glGetAttribLocation(sobel_program, "texcoord");
	glEnableVertexArrayAttrib(sobel_vao, texcoord_attrib);
	glVertexAttribPointer(texcoord_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Now finally draw.
	glViewport(0, 0, level_width, level_height);
	glBindFramebuffer(GL_FRAMEBUFFER, grad0_fbo);
	glUseProgram(sobel_program);
	glDisable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Motion search to find the initial flow.

	// Create a flow texture, initialized to zero.
	GLuint flow_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &flow_tex);
	glTextureStorage2D(flow_tex, 1, GL_RG16F, width_patches, height_patches);

	// And an output flow texture. (Well, we could have used texture barriers,
	// but I don't feel lucky today.)
	GLuint flow_out_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &flow_out_tex);
	glTextureStorage2D(flow_out_tex, 1, GL_RG16F, width_patches, height_patches);

	GLuint flow_fbo;
	glCreateFramebuffers(1, &flow_fbo);
	glNamedFramebufferTexture(flow_fbo, GL_COLOR_ATTACHMENT0, flow_out_tex, 0);

	glUseProgram(motion_search_program);

	bind_sampler(motion_search_program, "image0_tex", 0, tex0_view, nearest_sampler);
	bind_sampler(motion_search_program, "image1_tex", 1, tex1_view, linear_sampler);
	bind_sampler(motion_search_program, "grad0_tex", 2, grad0_tex, nearest_sampler);
	bind_sampler(motion_search_program, "flow_tex", 3, flow_tex, nearest_sampler);

	glProgramUniform1f(motion_search_program, glGetUniformLocation(motion_search_program, "image_width"), level_width);
	glProgramUniform1f(motion_search_program, glGetUniformLocation(motion_search_program, "image_height"), level_height);
	glProgramUniform1f(motion_search_program, glGetUniformLocation(motion_search_program, "inv_image_width"), 1.0f / level_width);
	glProgramUniform1f(motion_search_program, glGetUniformLocation(motion_search_program, "inv_image_height"), 1.0f / level_height);

//	printf("%d x %d patches on this level\n", width_patches, height_patches);

	// Set up the VAO containing all the required position/texcoord data.
	GLuint motion_search_vao;
        glCreateVertexArrays(1, &motion_search_vao);
        glBindVertexArray(motion_search_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	position_attrib = glGetAttribLocation(motion_search_program, "position");
	glEnableVertexArrayAttrib(motion_search_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	texcoord_attrib = glGetAttribLocation(motion_search_program, "texcoord");
	glEnableVertexArrayAttrib(motion_search_vao, texcoord_attrib);
	glVertexAttribPointer(texcoord_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// And draw.
	glViewport(0, 0, width_patches, height_patches);
	glBindFramebuffer(GL_FRAMEBUFFER, flow_fbo);
	glUseProgram(motion_search_program);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Do “densification”, ie., upsampling of the flow patches to the flow field
	// (the same size as the image at this level). We draw one quad per patch
	// over its entire covered area (using instancing in the vertex shader),
	// and then weight the contributions in the pixel shader by post-warp difference.
	// This is equation (3) in the paper.
	//
	// We accumulate the flow vectors in the R/G channels (for u/v) and the total
	// weight in the B channel. Dividing R and G by B gives the normalized values.

	// Set up an output texture.
	GLuint dense_flow_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &dense_flow_tex);
	//glTextureStorage2D(dense_flow_tex, 1, GL_RGB16F, level_width, level_height);
	glTextureStorage2D(dense_flow_tex, 1, GL_RGBA32F, level_width, level_height);

	GLuint dense_flow_fbo;
	glCreateFramebuffers(1, &dense_flow_fbo);
	glNamedFramebufferTexture(dense_flow_fbo, GL_COLOR_ATTACHMENT0, dense_flow_tex, 0);

	glUseProgram(densify_program);

	bind_sampler(densify_program, "image0_tex", 0, tex0_view, nearest_sampler);
	bind_sampler(densify_program, "image1_tex", 1, tex1_view, linear_sampler);
	bind_sampler(densify_program, "flow_tex", 2, flow_out_tex, nearest_sampler);

	glProgramUniform1i(densify_program, glGetUniformLocation(densify_program, "width_patches"), width_patches);
	glProgramUniform2f(densify_program, glGetUniformLocation(densify_program, "patch_size"),
		float(patch_size_pixels) / level_width,
		float(patch_size_pixels) / level_height);

	float patch_spacing_x = float(level_width - patch_size_pixels) / (width_patches - 1);
	float patch_spacing_y = float(level_height - patch_size_pixels) / (height_patches - 1);
	glProgramUniform2f(densify_program, glGetUniformLocation(densify_program, "patch_spacing"),
		patch_spacing_x / level_width,
		patch_spacing_y / level_height);

	// Set up the VAO containing all the required position/texcoord data.
	GLuint densify_vao;
        glCreateVertexArrays(1, &densify_vao);
        glBindVertexArray(densify_vao);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);

	position_attrib = glGetAttribLocation(densify_program, "position");
	glEnableVertexArrayAttrib(densify_vao, position_attrib);
	glVertexAttribPointer(position_attrib, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// And draw.
	glViewport(0, 0, level_width, level_height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindFramebuffer(GL_FRAMEBUFFER, dense_flow_fbo);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, width_patches * height_patches);

	// TODO: Variational refinement.

	unique_ptr<float[]> dense_flow(new float[level_width * level_height * 3]);
	glGetTextureImage(dense_flow_tex, 0, GL_RGB, GL_FLOAT, level_width * level_height * 3 * sizeof(float), dense_flow.get());

	FILE *fp = fopen("flow.ppm", "wb");
	fprintf(fp, "P6\n%d %d\n255\n", level_width, level_height);
	for (unsigned y = 0; y < level_height; ++y) {
		int yy = level_height - y - 1;
		for (unsigned x = 0; x < level_width; ++x) {
			float du = dense_flow[(yy * level_width + x) * 3 + 0];
			float dv = dense_flow[(yy * level_width + x) * 3 + 1];
			float w = dense_flow[(yy * level_width + x) * 3 + 2];

			du /= w;
			dv /= w;

			float angle = atan2(dv * level_width, du * level_height);
			float magnitude = min(hypot(du * level_width, dv * level_height) / 20.0, 1.0);
			
			// HSV to RGB (from Wikipedia). Saturation is 1.
			float c = magnitude;
			float h = (angle + M_PI) * 6.0 / (2.0 * M_PI);
			float X = c * (1.0 - fabs(fmod(h, 2.0) - 1.0));
			float r = 0.0f, g = 0.0f, b = 0.0f;
			if (h < 1.0f) {
				r = c; g = X;
			} else if (h < 2.0f) {
				r = X; g = c;
			} else if (h < 3.0f) {
				g = c; b = X;
			} else if (h < 4.0f) {
				g = X; b = c;
			} else if (h < 5.0f) {
				r = X; b = c;
			} else if (h < 6.0f) {
				r = c; b = X;
			} else {
				// h is NaN, so black is fine.
			}
			float m = magnitude - c;
			r += m; g += m; b += m;
			r = max(min(r, 1.0f), 0.0f);
			g = max(min(g, 1.0f), 0.0f);
			b = max(min(b, 1.0f), 0.0f);
			putc(lrintf(r * 255.0f), fp);
			putc(lrintf(g * 255.0f), fp);
			putc(lrintf(b * 255.0f), fp);
		}
	}
	fclose(fp);

	fprintf(stderr, "err = %d\n", glGetError());
}
