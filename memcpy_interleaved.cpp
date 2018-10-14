#include <algorithm>
#include <assert.h>
#include <cstdint>
#if __SSE2__
#include <immintrin.h>
#endif

using namespace std;

// TODO: Support stride.
void memcpy_interleaved_slow(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	assert(n % 2 == 0);
	uint8_t *dptr1 = dest1;
	uint8_t *dptr2 = dest2;

	for (size_t i = 0; i < n; i += 2) {
		*dptr1++ = *src++;
		*dptr2++ = *src++;
	}
}

#ifdef __SSE2__

// Returns the number of bytes consumed.
size_t memcpy_interleaved_fastpath(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	const uint8_t *limit = src + n;
	size_t consumed = 0;

	// Align end to 32 bytes.
	limit = (const uint8_t *)(intptr_t(limit) & ~31);

	if (src >= limit) {
		return 0;
	}

	// Process [0,31] bytes, such that start gets aligned to 32 bytes.
	const uint8_t *aligned_src = (const uint8_t *)(intptr_t(src + 31) & ~31);
	if (aligned_src != src) {
		size_t n2 = aligned_src - src;
		memcpy_interleaved_slow(dest1, dest2, src, n2);
		dest1 += n2 / 2;
		dest2 += n2 / 2;
		if (n2 % 2) {
			swap(dest1, dest2);
		}
		src = aligned_src;
		consumed += n2;
	}

	// Make the length a multiple of 64.
	if (((limit - src) % 64) != 0) {
		limit -= 32;
	}
	assert(((limit - src) % 64) == 0);

#if __AVX2__
	const __m256i *__restrict in = (const __m256i *)src;
	__m256i *__restrict out1 = (__m256i *)dest1;
	__m256i *__restrict out2 = (__m256i *)dest2;

	__m256i shuffle_cw = _mm256_set_epi8(
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0,
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0);
	while (in < (const __m256i *)limit) {
		// Note: For brevity, comments show lanes as if they were 2x64-bit (they're actually 2x128).
		__m256i data1 = _mm256_stream_load_si256(in);         // AaBbCcDd EeFfGgHh
		__m256i data2 = _mm256_stream_load_si256(in + 1);     // IiJjKkLl MmNnOoPp

		data1 = _mm256_shuffle_epi8(data1, shuffle_cw);       // ABCDabcd EFGHefgh
		data2 = _mm256_shuffle_epi8(data2, shuffle_cw);       // IJKLijkl MNOPmnop
	
		data1 = _mm256_permute4x64_epi64(data1, 0b11011000);  // ABCDEFGH abcdefgh
		data2 = _mm256_permute4x64_epi64(data2, 0b11011000);  // IJKLMNOP ijklmnop

		__m256i lo = _mm256_permute2x128_si256(data1, data2, 0b00100000);
		__m256i hi = _mm256_permute2x128_si256(data1, data2, 0b00110001);

		_mm256_storeu_si256(out1, lo);
		_mm256_storeu_si256(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 64;
	}
#else
	const __m128i * __restrict in = (const __m128i *)src;
	__m128i * __restrict out1 = (__m128i *)dest1;
	__m128i * __restrict out2 = (__m128i *)dest2;

	__m128i mask_lower_byte = _mm_set1_epi16(0x00ff);
	while (in < (const __m128i *)limit) {
		__m128i data1 = _mm_load_si128(in);
		__m128i data2 = _mm_load_si128(in + 1);
		__m128i data1_lo = _mm_and_si128(data1, mask_lower_byte);
		__m128i data2_lo = _mm_and_si128(data2, mask_lower_byte);
		__m128i data1_hi = _mm_srli_epi16(data1, 8);
		__m128i data2_hi = _mm_srli_epi16(data2, 8);
		__m128i lo = _mm_packus_epi16(data1_lo, data2_lo);
		_mm_storeu_si128(out1, lo);
		__m128i hi = _mm_packus_epi16(data1_hi, data2_hi);
		_mm_storeu_si128(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 32;
	}
#endif

	return consumed;
}

#endif  // defined(__SSE2__)

void memcpy_interleaved(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
#ifdef __SSE2__
	size_t consumed = memcpy_interleaved_fastpath(dest1, dest2, src, n);
	src += consumed;
	dest1 += consumed / 2;
	dest2 += consumed / 2;
	if (consumed % 2) {
		swap(dest1, dest2);
	}
	n -= consumed;

	if (n > 0) {
		memcpy_interleaved_slow(dest1, dest2, src, n);
	}
#else
	memcpy_interleaved_slow(dest1, dest2, src, n);
#endif
}
