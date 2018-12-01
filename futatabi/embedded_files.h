#ifndef _EMBEDDED_FILES_H
#define _EMBEDDED_FILES_H 1

// Files that are embedded into the binary as part of the build process.
// They are used as a backup if the files are not available on disk
// (which is typically the case if the program is installed, as opposed to
// being run during development).

#include <stddef.h>

extern const unsigned char *_binary_add_base_flow_frag_data;
extern const size_t _binary_add_base_flow_frag_size;
extern const unsigned char *_binary_blend_frag_data;
extern const size_t _binary_blend_frag_size;
extern const unsigned char *_binary_chroma_subsample_frag_data;
extern const size_t _binary_chroma_subsample_frag_size;
extern const unsigned char *_binary_chroma_subsample_vert_data;
extern const size_t _binary_chroma_subsample_vert_size;
extern const unsigned char *_binary_densify_frag_data;
extern const size_t _binary_densify_frag_size;
extern const unsigned char *_binary_densify_vert_data;
extern const size_t _binary_densify_vert_size;
extern const unsigned char *_binary_derivatives_frag_data;
extern const size_t _binary_derivatives_frag_size;
extern const unsigned char *_binary_diffusivity_frag_data;
extern const size_t _binary_diffusivity_frag_size;
extern const unsigned char *_binary_equations_frag_data;
extern const size_t _binary_equations_frag_size;
extern const unsigned char *_binary_equations_vert_data;
extern const size_t _binary_equations_vert_size;
extern const unsigned char *_binary_gray_frag_data;
extern const size_t _binary_gray_frag_size;
extern const unsigned char *_binary_hole_blend_frag_data;
extern const size_t _binary_hole_blend_frag_size;
extern const unsigned char *_binary_hole_fill_frag_data;
extern const size_t _binary_hole_fill_frag_size;
extern const unsigned char *_binary_hole_fill_vert_data;
extern const size_t _binary_hole_fill_vert_size;
extern const unsigned char *_binary_motion_search_frag_data;
extern const size_t _binary_motion_search_frag_size;
extern const unsigned char *_binary_motion_search_vert_data;
extern const size_t _binary_motion_search_vert_size;
extern const unsigned char *_binary_prewarp_frag_data;
extern const size_t _binary_prewarp_frag_size;
extern const unsigned char *_binary_resize_flow_frag_data;
extern const size_t _binary_resize_flow_frag_size;
extern const unsigned char *_binary_sobel_frag_data;
extern const size_t _binary_sobel_frag_size;
extern const unsigned char *_binary_sor_frag_data;
extern const size_t _binary_sor_frag_size;
extern const unsigned char *_binary_sor_vert_data;
extern const size_t _binary_sor_vert_size;
extern const unsigned char *_binary_splat_frag_data;
extern const size_t _binary_splat_frag_size;
extern const unsigned char *_binary_splat_vert_data;
extern const size_t _binary_splat_vert_size;
extern const unsigned char *_binary_vs_vert_data;
extern const size_t _binary_vs_vert_size;

#endif  // !defined(_EMBEDDED_FILES_H)
