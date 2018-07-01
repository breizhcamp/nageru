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
	GLuint motion_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	GLuint motion_fs_obj = compile_shader(read_file("motion_search.frag"), GL_FRAGMENT_SHADER);
	GLuint motion_search_program = link_program(motion_vs_obj, motion_fs_obj);

	GLuint sobel_vs_obj = compile_shader(read_file("vs.vert"), GL_VERTEX_SHADER);
	GLuint sobel_fs_obj = compile_shader(read_file("sobel.frag"), GL_FRAGMENT_SHADER);
	GLuint sobel_program = link_program(sobel_vs_obj, sobel_fs_obj);

	// Coarsest level.
	int level_width = WIDTH >> coarsest_level;
	int level_height = HEIGHT >> coarsest_level;
	float patch_spacing_pixels = patch_size_pixels * (1.0f - patch_overlap_ratio);
	int width_patches = 1 + lrintf((level_width - patch_size_pixels) / patch_spacing_pixels);
	int height_patches = 1 + lrintf((level_height - patch_size_pixels) / patch_spacing_pixels);

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
	GLuint grad_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &grad_tex);
	glTextureStorage2D(grad_tex, 1, GL_RG16F, level_width, level_height);

	GLuint grad_fbo;
	glCreateFramebuffers(1, &grad_fbo);
	glNamedFramebufferTexture(grad_fbo, GL_COLOR_ATTACHMENT0, grad_tex, 0);

	glUseProgram(sobel_program);
	glBindTextureUnit(0, tex0);
	glUniform1i(glGetUniformLocation(sobel_program, "tex"), 0);
	glUniform1f(glGetUniformLocation(sobel_program, "inv_width"), 1.0f / level_width);
	glUniform1f(glGetUniformLocation(sobel_program, "inv_height"), 1.0f / level_height);

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
	glBindFramebuffer(GL_FRAMEBUFFER, grad_fbo);
	glUseProgram(sobel_program);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);

	GLuint flow_tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &flow_tex);
	glTextureStorage2D(flow_tex, 1, GL_RG16F, width_patches, height_patches);

//	printf("%d x %d patches on this level\n", width_patches, height_patches);
}
