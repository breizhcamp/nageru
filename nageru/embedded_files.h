#ifndef _EMBEDDED_FILES_H
#define _EMBEDDED_FILES_H 1

// Files that are embedded into the binary as part of the build process.
// They are used as a backup if the files are not available on disk
// (which is typically the case if the program is installed, as opposed to
// being run during development).

#include <stddef.h>

extern const unsigned char *_binary_cbcr_subsample_vert_data;
extern const size_t _binary_cbcr_subsample_vert_size;
extern const unsigned char *_binary_cbcr_subsample_frag_data;
extern const size_t _binary_cbcr_subsample_frag_size;
extern const unsigned char *_binary_uyvy_subsample_vert_data;
extern const size_t _binary_uyvy_subsample_vert_size;
extern const unsigned char *_binary_uyvy_subsample_frag_data;
extern const size_t _binary_uyvy_subsample_frag_size;
extern const unsigned char *_binary_v210_subsample_comp_data;
extern const size_t _binary_v210_subsample_comp_size;
extern const unsigned char *_binary_timecode_vert_data;
extern const size_t _binary_timecode_vert_size;
extern const unsigned char *_binary_timecode_frag_data;
extern const size_t _binary_timecode_frag_size;
extern const unsigned char *_binary_timecode_10bit_frag_data;
extern const size_t _binary_timecode_10bit_frag_size;

#endif  // !defined(_EMBEDDED_FILES_H)
