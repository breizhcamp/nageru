#ifndef _FLOW_H
#define _FLOW_H 1

// Code for computing optical flow between two images, and using it to interpolate
// in-between frames. The main user interface is the DISComputeFlow and Interpolate
// classes (also GrayscaleConversion can be useful).

#include <stdint.h>
#include <epoxy/gl.h>
#include <array>
#include <map>
#include <mutex>
#include <vector>
#include <utility>

class ScopedTimer;

// Predefined operating points from the paper.
struct OperatingPoint {
	unsigned coarsest_level;  // TODO: Adjust dynamically based on the resolution?
	unsigned finest_level;
	unsigned search_iterations;  // Halved from the paper.
	unsigned patch_size_pixels;
	float patch_overlap_ratio;
	bool variational_refinement;

	// Not part of the original paper; used for interpolation.
	// NOTE: Values much larger than 1.0 seems to trigger Haswell's “PMA stall”;
	// the problem is not present on Broadwell and higher (there's a mitigation
	// in the hardware, but Mesa doesn't enable it at the time of writing).
	// Since we have hole filling, the holes from 1.0 are not critical,
	// but larger values seem to do better than hole filling for large
	// motion, blurs etc. since we have more candidates.
	float splat_size;
};

// Operating point 1 (600 Hz on CPU, excluding preprocessing).
static constexpr OperatingPoint operating_point1 = {
	5,      // Coarsest level.
	3,      // Finest level.
	8,      // Search iterations.
	8,      // Patch size (pixels).
	0.30f,  // Overlap ratio.
	false,  // Variational refinement.
	1.0f	// Splat size (pixels).
};

// Operating point 2 (300 Hz on CPU, excluding preprocessing).
static constexpr OperatingPoint operating_point2 = {
	5,      // Coarsest level.
	3,      // Finest level.
	6,      // Search iterations.
	8,      // Patch size (pixels).
	0.40f,  // Overlap ratio.
	true,   // Variational refinement.
	1.0f	// Splat size (pixels).
};

// Operating point 3 (10 Hz on CPU, excluding preprocessing).
// This is the only one that has been thorougly tested.
static constexpr OperatingPoint operating_point3 = {
	5,      // Coarsest level.
	1,      // Finest level.
	8,      // Search iterations.
	12,     // Patch size (pixels).
	0.75f,  // Overlap ratio.
	true,   // Variational refinement.
	4.0f	// Splat size (pixels).
};

// Operating point 4 (0.5 Hz on CPU, excluding preprocessing).
static constexpr OperatingPoint operating_point4 = {
	5,      // Coarsest level.
	0,      // Finest level.
	128,    // Search iterations.
	12,     // Patch size (pixels).
	0.75f,  // Overlap ratio.
	true,   // Variational refinement.
	8.0f	// Splat size (pixels).
};

int find_num_levels(int width, int height);

// A class that caches FBOs that render to a given set of textures.
// It never frees anything, so it is only suitable for rendering to
// the same (small) set of textures over and over again.
template<size_t num_elements>
class PersistentFBOSet {
public:
	void render_to(const std::array<GLuint, num_elements> &textures);

	// Convenience wrappers.
	void render_to(GLuint texture0) {
		render_to({{texture0}});
	}

	void render_to(GLuint texture0, GLuint texture1) {
		render_to({{texture0, texture1}});
	}

	void render_to(GLuint texture0, GLuint texture1, GLuint texture2) {
		render_to({{texture0, texture1, texture2}});
	}

	void render_to(GLuint texture0, GLuint texture1, GLuint texture2, GLuint texture3) {
		render_to({{texture0, texture1, texture2, texture3}});
	}

private:
	// TODO: Delete these on destruction.
	std::map<std::array<GLuint, num_elements>, GLuint> fbos;
};


// Same, but with a depth texture.
template<size_t num_elements>
class PersistentFBOSetWithDepth {
public:
	void render_to(GLuint depth_rb, const std::array<GLuint, num_elements> &textures);

	// Convenience wrappers.
	void render_to(GLuint depth_rb, GLuint texture0) {
		render_to(depth_rb, {{texture0}});
	}

	void render_to(GLuint depth_rb, GLuint texture0, GLuint texture1) {
		render_to(depth_rb, {{texture0, texture1}});
	}

	void render_to(GLuint depth_rb, GLuint texture0, GLuint texture1, GLuint texture2) {
		render_to(depth_rb, {{texture0, texture1, texture2}});
	}

	void render_to(GLuint depth_rb, GLuint texture0, GLuint texture1, GLuint texture2, GLuint texture3) {
		render_to(depth_rb, {{texture0, texture1, texture2, texture3}});
	}

private:
	// TODO: Delete these on destruction.
	std::map<std::pair<GLuint, std::array<GLuint, num_elements>>, GLuint> fbos;
};

// Convert RGB to grayscale, using Rec. 709 coefficients.
class GrayscaleConversion {
public:
	GrayscaleConversion();
	void exec(GLint tex, GLint gray_tex, int width, int height, int num_layers);

private:
	PersistentFBOSet<1> fbos;
	GLuint gray_vs_obj;
	GLuint gray_fs_obj;
	GLuint gray_program;
	GLuint gray_vao;

	GLuint uniform_tex;
};

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
	void exec(GLint tex_view, GLint grad_tex, int level_width, int level_height, int num_layers);

private:
	PersistentFBOSet<1> fbos;
	GLuint sobel_vs_obj;
	GLuint sobel_fs_obj;
	GLuint sobel_program;

	GLuint uniform_tex;
};

// Motion search to find the initial flow. See motion_search.frag for documentation.
class MotionSearch {
public:
	MotionSearch(const OperatingPoint &op);
	void exec(GLuint tex_view, GLuint grad_tex, GLuint flow_tex, GLuint flow_out_tex, int level_width, int level_height, int prev_level_width, int prev_level_height, int width_patches, int height_patches, int num_layers);

private:
	const OperatingPoint op;
	PersistentFBOSet<1> fbos;

	GLuint motion_vs_obj;
	GLuint motion_fs_obj;
	GLuint motion_search_program;

	GLuint uniform_inv_image_size, uniform_inv_prev_level_size, uniform_out_flow_size;
	GLuint uniform_image_tex, uniform_grad_tex, uniform_flow_tex;
	GLuint uniform_patch_size, uniform_num_iterations;
};

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
	Densify(const OperatingPoint &op);
	void exec(GLuint tex_view, GLuint flow_tex, GLuint dense_flow_tex, int level_width, int level_height, int width_patches, int height_patches, int num_layers);

private:
	OperatingPoint op;
	PersistentFBOSet<1> fbos;

	GLuint densify_vs_obj;
	GLuint densify_fs_obj;
	GLuint densify_program;

	GLuint uniform_patch_size;
	GLuint uniform_image_tex, uniform_flow_tex;
};

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
	void exec(GLuint tex_view, GLuint flow_tex, GLuint normalized_flow_tex, GLuint I_tex, GLuint I_t_tex, int level_width, int level_height, int num_layers);

private:
	PersistentFBOSet<3> fbos;

	GLuint prewarp_vs_obj;
	GLuint prewarp_fs_obj;
	GLuint prewarp_program;

	GLuint uniform_image_tex, uniform_flow_tex;
};

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
	void exec(GLuint input_tex, GLuint I_x_y_tex, GLuint beta_0_tex, int level_width, int level_height, int num_layers);

private:
	PersistentFBOSet<2> fbos;

	GLuint derivatives_vs_obj;
	GLuint derivatives_fs_obj;
	GLuint derivatives_program;

	GLuint uniform_tex;
};

// Calculate the diffusivity for each pixels, g(x,y). Smoothness (s) will
// be calculated in the shaders on-the-fly by sampling in-between two
// neighboring g(x,y) pixels, plus a border tweak to make sure we get
// zero smoothness at the border.
//
// See variational_refinement.txt for more information.
class ComputeDiffusivity {
public:
	ComputeDiffusivity();
	void exec(GLuint flow_tex, GLuint diff_flow_tex, GLuint diffusivity_tex, int level_width, int level_height, bool zero_diff_flow, int num_layers);

private:
	PersistentFBOSet<1> fbos;

	GLuint diffusivity_vs_obj;
	GLuint diffusivity_fs_obj;
	GLuint diffusivity_program;

	GLuint uniform_flow_tex, uniform_diff_flow_tex;
	GLuint uniform_alpha, uniform_zero_diff_flow;
};

// Set up the equations set (two equations in two unknowns, per pixel).
// We store five floats; the three non-redundant elements of the 2x2 matrix (A)
// as 32-bit floats, and the two elements on the right-hand side (b) as 16-bit
// floats. (Actually, we store the inverse of the diagonal elements, because
// we only ever need to divide by them.) This fits into four u32 values;
// R, G, B for the matrix (the last element is symmetric) and A for the two b values.
// All the values of the energy term (E_I, E_G, E_S), except the smoothness
// terms that depend on other pixels, are calculated in one pass.
//
// The equation set is split in two; one contains only the pixels needed for
// the red pass, and one only for the black pass (see sor.frag). This reduces
// the amount of data the SOR shader has to pull in, at the cost of some
// complexity when the equation texture ends up with half the size and we need
// to adjust texture coordinates.  The contraction is done along the horizontal
// axis, so that on even rows (0, 2, 4, ...), the “red” texture will contain
// pixels 0, 2, 4, 6, etc., and on odd rows 1, 3, 5, etc..
//
// See variational_refinement.txt for more information about the actual
// equations in use.
class SetupEquations {
public:
	SetupEquations();
	void exec(GLuint I_x_y_tex, GLuint I_t_tex, GLuint diff_flow_tex, GLuint flow_tex, GLuint beta_0_tex, GLuint diffusivity_tex, GLuint equation_red_tex, GLuint equation_black_tex, int level_width, int level_height, bool zero_diff_flow, int num_layers);

private:
	PersistentFBOSet<2> fbos;

	GLuint equations_vs_obj;
	GLuint equations_fs_obj;
	GLuint equations_program;

	GLuint uniform_I_x_y_tex, uniform_I_t_tex;
	GLuint uniform_diff_flow_tex, uniform_base_flow_tex;
	GLuint uniform_beta_0_tex;
	GLuint uniform_diffusivity_tex;
	GLuint uniform_gamma, uniform_delta, uniform_zero_diff_flow;
};

// Actually solve the equation sets made by SetupEquations, by means of
// successive over-relaxation (SOR).
//
// See variational_refinement.txt for more information.
class SOR {
public:
	SOR();
	void exec(GLuint diff_flow_tex, GLuint equation_red_tex, GLuint equation_black_tex, GLuint diffusivity_tex, int level_width, int level_height, int num_iterations, bool zero_diff_flow, int num_layers, ScopedTimer *sor_timer);

private:
	PersistentFBOSet<1> fbos;

	GLuint sor_vs_obj;
	GLuint sor_fs_obj;
	GLuint sor_program;

	GLuint uniform_diff_flow_tex;
	GLuint uniform_equation_red_tex, uniform_equation_black_tex;
	GLuint uniform_diffusivity_tex;
	GLuint uniform_phase, uniform_num_nonzero_phases;
};

// Simply add the differential flow found by the variational refinement to the base flow.
// The output is in base_flow_tex; we don't need to make a new texture.
class AddBaseFlow {
public:
	AddBaseFlow();
	void exec(GLuint base_flow_tex, GLuint diff_flow_tex, int level_width, int level_height, int num_layers);

private:
	PersistentFBOSet<1> fbos;

	GLuint add_flow_vs_obj;
	GLuint add_flow_fs_obj;
	GLuint add_flow_program;

	GLuint uniform_diff_flow_tex;
};

// Take a copy of the flow, bilinearly interpolated and scaled up.
class ResizeFlow {
public:
	ResizeFlow();
	void exec(GLuint in_tex, GLuint out_tex, int input_width, int input_height, int output_width, int output_height, int num_layers);

private:
	PersistentFBOSet<1> fbos;

	GLuint resize_flow_vs_obj;
	GLuint resize_flow_fs_obj;
	GLuint resize_flow_program;

	GLuint uniform_flow_tex;
	GLuint uniform_scale_factor;
};

// All operations, except construction and destruction, are thread-safe.
class TexturePool {
public:
	GLuint get_texture(GLenum format, GLuint width, GLuint height, GLuint num_layers = 0);
	void release_texture(GLuint tex_num);
	GLuint get_renderbuffer(GLenum format, GLuint width, GLuint height);
	void release_renderbuffer(GLuint tex_num);

private:
	struct Texture {
		GLuint tex_num;
		GLenum format;
		GLuint width, height, num_layers;
		bool in_use = false;
		bool is_renderbuffer = false;
	};
	std::mutex mu;
	std::vector<Texture> textures;  // Under mu.
};

class DISComputeFlow {
public:
	DISComputeFlow(int width, int height, const OperatingPoint &op);

	enum FlowDirection {
		FORWARD,
		FORWARD_AND_BACKWARD
	};
	enum ResizeStrategy {
		DO_NOT_RESIZE_FLOW,
		RESIZE_FLOW_TO_FULL_SIZE
	};

	// The texture must have two layers (first and second frame).
	// Returns a texture that must be released with release_texture()
	// after use.
	GLuint exec(GLuint tex, FlowDirection flow_direction, ResizeStrategy resize_strategy);

	void release_texture(GLuint tex) {
		pool.release_texture(tex);
	}

private:
	int width, height;
	GLuint initial_flow_tex;
	GLuint vertex_vbo, vao;
	TexturePool pool;
	const OperatingPoint op;

	// The various passes.
	Sobel sobel;
	MotionSearch motion_search;
	Densify densify;
	Prewarp prewarp;
	Derivatives derivatives;
	ComputeDiffusivity compute_diffusivity;
	SetupEquations setup_equations;
	SOR sor;
	AddBaseFlow add_base_flow;
	ResizeFlow resize_flow;
};

// Forward-warp the flow half-way (or rather, by alpha). A non-zero “splatting”
// radius fills most of the holes.
class Splat {
public:
	Splat(const OperatingPoint &op);

	// alpha is the time of the interpolated frame (0..1).
	void exec(GLuint image_tex, GLuint bidirectional_flow_tex, GLuint flow_tex, GLuint depth_rb, int width, int height, float alpha);

private:
	const OperatingPoint op;
	PersistentFBOSetWithDepth<1> fbos;

	GLuint splat_vs_obj;
	GLuint splat_fs_obj;
	GLuint splat_program;

	GLuint uniform_splat_size, uniform_alpha;
	GLuint uniform_image_tex, uniform_flow_tex;
	GLuint uniform_inv_flow_size;
};

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
	void exec(GLuint flow_tex, GLuint depth_rb, GLuint temp_tex[3], int width, int height);

private:
	PersistentFBOSetWithDepth<1> fbos;

	GLuint fill_vs_obj;
	GLuint fill_fs_obj;
	GLuint fill_program;

	GLuint uniform_tex;
	GLuint uniform_z, uniform_sample_offset;
};

// Blend the four directions from HoleFill into one pixel, so that single-pixel
// holes become the average of their four neighbors.
class HoleBlend {
public:
	HoleBlend();

	void exec(GLuint flow_tex, GLuint depth_rb, GLuint temp_tex[3], int width, int height);

private:
	PersistentFBOSetWithDepth<1> fbos;

	GLuint blend_vs_obj;
	GLuint blend_fs_obj;
	GLuint blend_program;

	GLuint uniform_left_tex, uniform_right_tex, uniform_up_tex, uniform_down_tex;
	GLuint uniform_z, uniform_sample_offset;
};

class Blend {
public:
	Blend();
	void exec(GLuint image_tex, GLuint flow_tex, GLuint output_tex, int width, int height, float alpha);

private:
	PersistentFBOSet<1> fbos;
	GLuint blend_vs_obj;
	GLuint blend_fs_obj;
	GLuint blend_program;

	GLuint uniform_image_tex, uniform_flow_tex;
	GLuint uniform_alpha, uniform_flow_consistency_tolerance;
};

class Interpolate {
public:
	Interpolate(int width, int height, const OperatingPoint &op);

	// Returns a texture that must be released with release_texture()
	// after use. image_tex must be a two-layer RGBA8 texture with mipmaps
	// (unless flow_level == 0).
	GLuint exec(GLuint image_tex, GLuint bidirectional_flow_tex, GLuint width, GLuint height, float alpha);

	void release_texture(GLuint tex) {
		pool.release_texture(tex);
	}

private:
	int width, height, flow_level;
	GLuint vertex_vbo, vao;
	TexturePool pool;
	const OperatingPoint op;

	Splat splat;
	HoleFill hole_fill;
	HoleBlend hole_blend;
	Blend blend;
};

#endif  // !defined(_FLOW_H)
