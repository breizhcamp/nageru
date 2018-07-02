#version 450 core

/*
  The motion search is one of the two major components of DIS. It works more or less
  like you'd expect; there's a bunch of overlapping patches (8x8 or 12x12 pixels) in
  a grid, and for each patch, there's a search to try to find the most similar patch
  in the other frame.

  Unlike in a typical video codec, the DIS patch search is based on gradient descent;
  conceptually, you start with an initial guess (the value from the previous level,
  or the zero flow for the very first level), subtract the reference (“template”)
  patch from the candidate, look at the gradient to see in what direction there is
  a lower difference, and then inch a bit toward that direction. (There is seemingly
  nothing like AdaM, Momentum or similar, but the searched value is only in two
  dimensions, so perhaps it doesn't matter as much then.)

  DIS does a tweak to this concept. Since the procedure as outlined above requires
  computing the gradient of the candidate patch, it uses the reference patch as
  candidate (thus the “inverse” name), and thus uses _its_ gradient to understand
  in which direction to move. (This is a bit dodgy, but not _that_ dodgy; after
  all, the two patches are supposed to be quite similar, so their surroundings and
  thus also gradients should also be quite similar.) It's not entirely clear whether
  this is still a win on GPU, where calculations are much cheaper, especially
  the way we parallelize the search, but we've kept it around for now.

  The inverse search is explained and derived in the supplementary material of the
  paper, section A. Do note that there's a typo; the text under equation 9 claims
  that the matrix H is n x n (where presumably n is the patch size), while in reality,
  it's 2x2.

  Our GPU parallellization is fairly dumb right now; we do one patch per fragment
  (ie., parallellize only over patches, not within each patch), which may not
  be optimal. In particular, in the initial level, we only have 40 patches,
  which is on the low side for a GPU, and the memory access patterns may also not
  be ideal.
 */

const uint patch_size = 12;
const uint num_iterations = 16;

in vec2 flow_tc;
in vec2 patch_bottom_left_texel;  // Center of bottom-left texel of patch.
out vec2 out_flow;

uniform sampler2D flow_tex, grad0_tex, image0_tex, image1_tex;
uniform float image_width, image_height, inv_image_width, inv_image_height;

void main()
{
	// Lock patch_bottom_left_texel to an integer, so that we never get
	// any bilinear artifacts for the gradient.
	vec2 base = round(patch_bottom_left_texel * vec2(image_width, image_height))
		* vec2(inv_image_width, inv_image_height);

	// First, precompute the pseudo-Hessian for the template patch.
	// This is the part where we really save by the inverse search
	// (ie., we can compute it up-front instead of anew for each
	// patch).
	//
	//  H = sum(S^T S)
	//
	// where S is the gradient at each point in the patch. Note that
	// this is an outer product, so we get a (symmetric) 2x2 matrix,
	// not a scalar.
	mat2 H = mat2(0.0f);
	for (uint y = 0; y < patch_size; ++y) {
		for (uint x = 0; x < patch_size; ++x) {
			vec2 tc;
			tc.x = base.x + x * inv_image_width;
			tc.y = base.y + y * inv_image_height;
			vec2 grad = texture(grad0_tex, tc).xy;
			H[0][0] += grad.x * grad.x;
			H[1][1] += grad.y * grad.y;
			H[0][1] += grad.x * grad.y;
		}
	}
	H[1][0] = H[0][1];

	// Make sure we don't get a singular matrix even if e.g. the picture is
	// all black. (The paper doesn't mention this, but the reference code
	// does it, and it seems like a reasonable hack to avoid NaNs. With such
	// a H, we'll go out-of-bounds pretty soon, though.)
	if (determinant(H) < 1e-6) {
		H[0][0] += 1e-6;
		H[1][1] += 1e-6;
	}

	mat2 H_inv = inverse(H);

	// Fetch the initial guess for the flow.
	vec2 initial_u = texture(flow_tex, flow_tc).xy;
	vec2 u = initial_u;

	for (uint i = 0; i < num_iterations; ++i) {
		vec2 du = vec2(0.0, 0.0);
		for (uint y = 0; y < patch_size; ++y) {
			for (uint x = 0; x < patch_size; ++x) {
				vec2 tc;
				tc.x = base.x + x * inv_image_width;
				tc.y = base.y + y * inv_image_height;
				vec2 grad = texture(grad0_tex, tc).xy;
				float t = texture(image0_tex, tc).x;
				float warped = texture(image1_tex, tc + u).x;
				du += grad * (warped - t);
			}
		}
		u += H_inv * du * vec2(inv_image_width, inv_image_height);
	}

	// TODO: reject if moving too far

	out_flow = u;
}
