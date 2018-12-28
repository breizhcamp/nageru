#define NO_SDL_GLEXT 1

#include "flow.h"
#include "gpu_timers.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_video.h>
#include <algorithm>
#include <assert.h>
#include <deque>
#include <epoxy/gl.h>
#include <getopt.h>
#include <map>
#include <memory>
#include <stack>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

SDL_Window *window;

bool enable_warmup = false;
bool enable_variational_refinement = true;  // Just for debugging.
bool enable_interpolation = false;

extern float vr_alpha, vr_delta, vr_gamma;

// Structures for asynchronous readback. We assume everything is the same size (and GL_RG16F).
struct ReadInProgress {
	GLuint pbo;
	string filename0, filename1;
	string flow_filename, ppm_filename;  // Either may be empty for no write.
};
stack<GLuint> spare_pbos;
deque<ReadInProgress> reads_in_progress;

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

template<class Type>
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

template<class Type>
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

	OperatingPoint op = operating_point3;
	if (!enable_variational_refinement) {
		op.variational_refinement = false;
	}

	DISComputeFlow compute_flow(width1, height1, op);  // Must be initialized before gray.
	GrayscaleConversion gray;
	gray.exec(image_tex, tex_gray, width1, height1, /*num_layers=*/2);
	glGenerateTextureMipmap(tex_gray);

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
	Interpolate interpolate(op, /*split_ycbcr_output=*/false);

	GLuint tex_gray;
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex_gray);
	glTextureStorage3D(tex_gray, levels, GL_R8, width1, height1, 2);
	gray.exec(image_tex, tex_gray, width1, height1, /*num_layers=*/2);
	glGenerateTextureMipmap(tex_gray);

	if (enable_warmup) {
		in_warmup = true;
		for (int i = 0; i < 10; ++i) {
			GLuint bidirectional_flow_tex = compute_flow.exec(tex_gray, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::DO_NOT_RESIZE_FLOW);
			GLuint interpolated_tex = interpolate.exec(image_tex, tex_gray, bidirectional_flow_tex, width1, height1, 0.5f).first;
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
		GLuint interpolated_tex = interpolate.exec(image_tex, tex_gray, bidirectional_flow_tex, width1, height1, alpha).first;

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
		{ "disable-variational-refinement", no_argument, 0, 1001 },
		{ "interpolate", no_argument, 0, 1002 },
		{ "warmup", no_argument, 0, 1004 }
	};

	enable_timing = true;

	for (;;) {
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

	if (enable_interpolation) {
		interpolate_image(argc, argv, optind);
	} else {
		compute_flow_only(argc, argv, optind);
	}
}
