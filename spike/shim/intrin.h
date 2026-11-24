#pragma once
/* MSVC intrinsic header; wasm has no x86 intrinsics. Provided so files that include it parse. */
#include <stdint.h>
typedef struct { float  f[4]; } __m128;
typedef struct { int    i[4]; } __m128i;
typedef struct { double d[2]; } __m128d;
static inline unsigned long __rdtsc(void) { return 0; }
static inline unsigned char _BitScanForward(unsigned long* idx, unsigned long mask) {
  if (!mask) return 0; *idx = (unsigned long)__builtin_ctz(mask); return 1;
}
static inline unsigned char _BitScanReverse(unsigned long* idx, unsigned long mask) {
  if (!mask) return 0; *idx = (unsigned long)(31 - __builtin_clz(mask)); return 1;
}
static inline void __cpuid(int out[4], int) { out[0] = out[1] = out[2] = out[3] = 0; }
