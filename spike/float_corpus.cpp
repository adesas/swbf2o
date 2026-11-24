/* Same code, compiled three ways. The point is not rounding in the abstract: it is whether a value
 * the file stored one way comes out the same way in a native build and in a browser build. If they
 * disagree, a desync between two clients is a rounding difference, not a network problem, and nothing
 * in the packet format will explain it.

   What each case records:
     q_div        round(x / step)                 the form the pinned model uses
     q_mul        round(x * (1 / step))           the form a host is likely to write. Not the same.
     half_even    nearbyint(x / step)             differs from half-up only on an exact half step
     floor_fma    floor(fmaf(x, recip, 0.5))      a common host way to round half up
     floor_plain  floor(x * recip + 0.5)          the same idea without a fused multiply
   The height path is integer arithmetic and must be identical everywhere; if it is not, the integer
   types are wrong, not the float behaviour.

   Run through tools/float_corpus_check.py, which builds this native with -ffp-contract=off, native
   with -ffp-contract=fast, and wasm, then compares all three. */
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static char out[65536];
static int pos = 0;

static void emit(const char* fmt, ...)
{
    if (pos >= (int)sizeof(out) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(out + pos, sizeof(out) - pos, fmt, ap);
    va_end(ap);
}

static long long half_up(float x) { return (long long)roundf(x); }
static long long half_even(float x) { return (long long)nearbyintf(x); }

static const float inputs[] = {
    0.0f, 0.125f, -0.125f, 1.0f, -1.0f, 0.5f, -0.5f, 4.5f, -4.5f,
    12.3456f, -12.3456f, 1023.9999f, 6.5535f, 32767.5f, -32768.0f, 0.4999999f, 0.5000001f
};

extern "C" const char* float_corpus_report(void)
{
    pos = 0;
    emit("{\"cases\":[");

    /* Extents as they occur in the data: a patch is patchPoints * metersPerPoint wide. */
    const float steps[] = { 1.0f, 9.0f, 2.25f, 1.0f / 65535.0f, 0.001f, 62.5f };

    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        float step = steps[s];
        float recip = 1.0f / step;
        for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
            float x = inputs[i];
            long long byDiv = half_up(x / step);
            long long byMul = half_up(x * recip);
            long long byEven = half_even(x / step);
            long long floorFma = (long long)floorf(fmaf(x, recip, 0.5f));
            long long floorPlain = (long long)floorf(x * recip + 0.5f);

            emit("%s{\"step\":%.9g,\"x\":%.9g,\"q_div\":%lld,\"q_mul\":%lld,\"half_even\":%lld,"
                 "\"floor_fma\":%lld,\"floor_plain\":%lld,\"div_ne_mul\":%d,\"up_ne_even\":%d,"
                 "\"fma_ne_plain\":%d}",
                 (s || i) ? "," : "", (double)step, (double)x, byDiv, byMul, byEven, floorFma, floorPlain,
                 (byDiv != byMul) ? 1 : 0, (byDiv != byEven) ? 1 : 0, (floorFma != floorPlain) ? 1 : 0);
        }
    }
    emit("]");

    emit(",\"height_path\":[");
    const int mins[] = { -32768, -100, 0, 512 };
    const int shifts[] = { 0, 1, 3, 7 };
    const int heights[] = { 0, 1, 15, -20000, 32767 };
    int n = 0;
    for (size_t a = 0; a < sizeof(mins) / sizeof(mins[0]); ++a)
        for (size_t b = 0; b < sizeof(shifts) / sizeof(shifts[0]); ++b)
            for (size_t c = 0; c < sizeof(heights) / sizeof(heights[0]); ++c) {
                long long h = heights[c];
                int shift = shifts[b];
                long long stored = (h << shift) - 32768;
                long long back = (stored + 32768) >> shift;
                emit("%s{\"min\":%d,\"shift\":%d,\"height\":%d,\"stored\":%lld,\"back\":%lld,\"exact\":%d}",
                     n++ ? "," : "", mins[a], shift, (int)h, stored, back,
                     (back == h) ? 1 : 0);
            }
    emit("]");

    /* A stored value decoded to the meter lattice and quantised back: the smallest place where a
     * fused multiply can change the last bit. The step cannot be exact in binary. */
    emit(",\"roundtrip\":[");
    for (size_t i = 0; i < 12; ++i) {
        float q = (float)(i * 4096) - 32768.0f;
        float step = 0.0109375f;
        float world = (q + 32768.0f) * step;
        float backDiv = world / step - 32768.0f;
        float backMul = world * (1.0f / step) - 32768.0f;
        unsigned bits = 0;
        memcpy(&bits, &world, sizeof(bits));
        emit("%s{\"q\":%lld,\"world_bits\":%u,\"back_div\":%lld,\"back_mul\":%lld,\"same\":%d}",
             i ? "," : "", (long long)q, bits, (long long)roundf(backDiv), (long long)roundf(backMul),
             (roundf(backDiv) == roundf(backMul)) ? 1 : 0);
    }
    emit("]");
    emit("}");
    return out;
}
