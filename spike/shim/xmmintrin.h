#pragma once
/* Portable scalar SSE-1 subset for wasm32 and non-SSE i386 builds.
 * RedTerrain::GetHeight uses _mm_set_ss / _mm_sub_ss / _mm_mul_ss / _mm_cvtss_f32 to force
 * IEEE-754 single-precision rounding (avoiding x87 80-bit intermediate rounding on MSVC x86).
 */
#ifdef __SSE__
#include_next <xmmintrin.h>
#else
#include "intrin.h"
static inline __m128 _mm_set_ss(float a) {
  __m128 r = {{ a, 0.0f, 0.0f, 0.0f }};
  return r;
}
static inline __m128 _mm_set1_ps(float a) {
  __m128 r = {{ a, a, a, a }};
  return r;
}
static inline __m128 _mm_add_ss(__m128 a, __m128 b) {
  a.f[0] += b.f[0];
  return a;
}
static inline __m128 _mm_sub_ss(__m128 a, __m128 b) {
  a.f[0] -= b.f[0];
  return a;
}
static inline __m128 _mm_mul_ss(__m128 a, __m128 b) {
  a.f[0] *= b.f[0];
  return a;
}
static inline __m128 _mm_div_ss(__m128 a, __m128 b) {
  a.f[0] /= b.f[0];
  return a;
}
static inline float _mm_cvtss_f32(__m128 a) {
  return a.f[0];
}
#endif
