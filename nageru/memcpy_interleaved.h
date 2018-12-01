#ifndef _MEMCPY_INTERLEAVED_H
#define _MEMCPY_INTERLEAVED_H 1

#include <stddef.h>
#include <stdint.h>

// Copies every other byte from src to dest1 and dest2.
// TODO: Support stride.
void memcpy_interleaved(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n);

#endif  // !defined(_MEMCPY_INTERLEAVED_H)
