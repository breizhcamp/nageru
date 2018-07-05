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
uniform vec2 image_size, inv_image_size;

void main()
{
	// Lock patch_bottom_left_texel to an integer, so that we never get
	// any bilinear artifacts for the gradient.
	vec2 base = round(patch_bottom_left_texel * image_size)
		* inv_image_size;

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
	vec2 grad_sum = vec2(0.0f);  // Used for patch normalization.
	float template_sum = 0.0f;
	for (uint y = 0; y < patch_size; ++y) {
		for (uint x = 0; x < patch_size; ++x) {
			vec2 tc = base + uvec2(x, y) * inv_image_size;
			vec2 grad = texture(grad0_tex, tc).xy;
			H[0][0] += grad.x * grad.x;
			H[1][1] += grad.y * grad.y;
			H[0][1] += grad.x * grad.y;

			template_sum += texture(image0_tex, tc).x;
			grad_sum += grad;
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

	// Fetch the initial guess for the flow. (We need the normalization step
	// because densification works by accumulating; see the comments on the
	// Densify class.)
	vec3 prev_flow = texture(flow_tex, flow_tc).xyz;
	vec2 initial_u;
	if (prev_flow.z < 1e-3) {
		initial_u = vec2(0.0, 0.0);
	} else {
		initial_u = prev_flow.xy / prev_flow.z;
	}

	// Note: The flow is in OpenGL coordinates [0..1], but the calculations
	// generally come out in pixels since the gradient is in pixels,
	// so we need to convert at the end.
	vec2 u = initial_u;

	for (uint i = 0; i < num_iterations; ++i) {
		vec2 du = vec2(0.0, 0.0);
		float warped_sum = 0.0f;
		for (uint y = 0; y < patch_size; ++y) {
			for (uint x = 0; x < patch_size; ++x) {
				vec2 tc = base + uvec2(x, y) * inv_image_size;
				vec2 grad = texture(grad0_tex, tc).xy;
				float t = texture(image0_tex, tc).x;
				float warped = texture(image1_tex, tc + u).x;
				du += grad * (warped - t);
				warped_sum += warped;
			}
		}

		// Subtract the mean for patch normalization. We've done our
		// sums without subtracting the means (because we didn't know them
		// beforehand), ie.:
		//
		//   sum(S^T * ((x + µ1) - (y + µ2))) = sum(S^T * (x - y)) + (µ1 – µ2) sum(S^T)
		//
		// which gives trivially
		//
		//   sum(S^T * (x - y)) = [what we calculated] - (µ1 - µ2) sum(S^T)
		//
		// so we can just subtract away the mean difference here.
		du -= grad_sum * (warped_sum - template_sum) * (1.0 / (patch_size * patch_size));

		// Do the actual update.
		u -= (H_inv * du) * inv_image_size;
	}

	// Reject if we moved too far. Also reject if the patch goes out-of-bounds
	// (the paper does not mention this, but the code does, and it seems to be
	// critical to avoid really bad behavior at the edges).
	if ((length((u - initial_u) * image_size) > patch_size) ||
	     u.x * image_size.x < -(patch_size * 0.5f) ||
	     (1.0 - u.x) * image_size.x < -(patch_size * 0.5f) ||
	     u.y * image_size.y < -(patch_size * 0.5f) ||
	     (1.0 - u.y) * image_size.y < -(patch_size * 0.5f)) {
		u = initial_u;
	}

	out_flow = u;
}
