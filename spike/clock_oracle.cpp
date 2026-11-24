/* The frame-pacing clock, run as the engine runs it.

   This is not a model of GameState::UpdateTime — it calls the engine's own function, compiled
   unmodified from build/src_fixed, and drives it from a scripted clock instead of a measured one.
   That is what makes a native/Wasm comparison possible at all: with a real clock, two machines
   cannot agree on a delta.

   What is scripted:
   * swbf2_now_ms() is the shim's only clock source. It returns a value the harness sets, and after
     the first read in a frame it moves forward one tick per read, so the engine's own "spin until
     the minimum delta" loop runs for real and produces the floor by itself rather than by assertion.
   * OneTimeInit's clock half (maxAdvanceTicks, normalAdvanceTicks) is reproduced here because the
     rest of OneTimeInit is the sound engine, the renderer and the heap. The stamps are marked the
     way the engine's state Enter() marks them.
   Everything else — the 80 Hz floor, the >1 s spike re-anchor, the delta clamp, the tick
   arithmetic, the rounding in the frame-rate-scale path — is the engine's code.

   Three things this measures, in order of how much they matter:

   1. Constants that come out of QueryPerformanceFrequency. PblTimeBase derives TICK_SHIFT and
      _fSecondsPerTick from whatever the platform clock reports, so the tick domain is only
      comparable between builds if the frequency is pinned. The shim pins 1 MHz; the real game does
      not. A native-vs-Wasm check against the shipped game therefore has to pin the same number.

   2. Accumulated drift over 1,000 ticks at a fixed rate. Each tick the engine converts a float
      delta to whole ticks and throws the remainder away, so the accumulated sim time is not
      n × dt; the size of that residue is the number, and it must be the same on both sides.

   3. The rounding in the frame-scale path, `(int64)((float)ticks * scale + 0.5f)`, which is the
      one expression in this function that a compiler may fuse. That is why -ffp-contract=off has to
      be a flag in the build, not a hope: run the same source with contraction allowed and see which
      numbers move.

   Run through tools/clock_oracle_check.py, which builds this three ways (native with
   -ffp-contract=off, native with -ffp-contract=fast -mfma, and Wasm with the engine's own flags)
   and compares. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "PebbleFL/Common/PblTimeBase.h"
#include "Battlefront2/Source/GameState.h"

/* --- the scripted clock --- */

static long long g_clock;      /* in ticks, against the frequency the shim pins */
static int g_reads;            /* reads in the current frame */
static long long g_step;       /* ticks each read after the first advances the clock */
static long long g_last;       /* the last value the engine read */

double swbf2_now_ms(void)
{
    long long v = g_clock;
    if (g_reads) g_clock += g_step;
    ++g_reads;
    g_last = v;
    return (double)v / 1000.0;
}

/* What the engine declares at global scope; the definitions live elsewhere in the tree, so this
   harness owns them here. GameLoop::sPauseMode is inside its namespace, so it is spelled the same. */
extern float sDeltaTime, sSystemDeltaTime;
extern PblTimeBase::Stamp AppTimeStamp, SystemTimeStamp;
extern bool sIgnoreNextTime;
extern LARGE_INTEGER Frequency, perfCount, oldPerfCount;
extern long long maxAdvanceTicks, normalAdvanceTicks;
extern float g_fFrameRateScale;
extern bool g_bNoRender;
extern bool netInShell, netEnabled, netEnabledNext;
namespace GameLoop { extern bool sPauseMode; }
extern float g_fFixedRate;

/* --- report --- */

static char out[65536];
static int pos;

static void emit(const char* fmt, ...)
{
    if (pos >= (int)sizeof(out) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(out + pos, sizeof(out) - pos, fmt, ap);
    va_end(ap);
}

static void start_clock(long long ticks)
{
    g_clock = ticks;
    g_reads = 0;
}

static void reset()
{
    /* No global constructors run here, so this is the engine's own initialisation path called
       explicitly: constructing a PblTimeBase is what pins the tick constants, and SetRate(1.0) is what
       the constructor does for each base the game owns. */
    PblTimeBase probe;          /* sets TICK_SHIFT, _iTicksPerSecond and the constants, from the clock */
    /* GameState::OneTimeInit's clock half, without the sound engine, the renderer or the heap. Note
       that PblTimeBase's own constructor uses a *local* Frequency, so the global the frame-pacing
       loop reads is only set here - that is why this has to be done explicitly. */
    QueryPerformanceFrequency(&Frequency);
    maxAdvanceTicks = Frequency.QuadPart;
    normalAdvanceTicks = Frequency.QuadPart >> 4;
    GameState::_TimeBase.SetRate(1.0f);
    GameState::_SystemTimeBase.SetRate(1.0f);
    GameState::_TotalTimeBase.SetRate(1.0f);
    GameState::_TimeBase._iValue = 0;
    GameState::_SystemTimeBase._iValue = 0;
    GameState::_TotalTimeBase._iValue = 0;
    AppTimeStamp._iValue = 0;
    SystemTimeStamp._iValue = 0;
    /* The engine marks the stamps against the time bases when a state is entered; do the same, or
       GetElapsed() has no base to measure from. */
    AppTimeStamp.Mark(&GameState::_TimeBase);
    SystemTimeStamp.Mark(&GameState::_SystemTimeBase);

    oldPerfCount.QuadPart = 0;
    sIgnoreNextTime = false;
    g_fFixedRate = 0.0f;
    g_fFrameRateScale = 0.0f;
    g_bNoRender = false;
    g_step = 1;
    netInShell = netEnabled = netEnabledNext = false;   /* keeps NetGame::FrameLock out of the loop */
    GameLoop::sPauseMode = false;

    start_clock(0);
    g_last = 0;
}

static void section_constants(void)
{
    /* The values the engine actually holds, read from the objects it owns, after its own constructor
       has run (reset() runs first, and reset() constructs a PblTimeBase the way the game does). */
    double stepms = ((double)(1LL << PblTimeBase::TICK_SHIFT) / (double)Frequency.QuadPart) * 1000.0;
    emit("{\"frequency\":%lld,\"tick_shift\":%d,\"ticks_per_second\":%llu,"
         "\"ticks_per_frame\":%llu,\"seconds_per_tick_as_float\":%.12f,"
         "\"smallest_time_the_clock_can_report_ms\":%.6f,\"floor_ticks\":%lld,"
         "\"max_advance_ticks\":%lld,\"normal_advance_ticks\":%lld}",
         Frequency.QuadPart, PblTimeBase::TICK_SHIFT, PblTimeBase::_iTicksPerSecond,
         PblTimeBase::_iTicksPerFrame, (double)PblTimeBase::_fSecondsPerTick, stepms,
         Frequency.QuadPart / 80, maxAdvanceTicks, normalAdvanceTicks);
}

/* What the same constructor would produce against a real hardware clock. This is not run through the
   engine; it is the arithmetic that decides whether two platforms can ever share a tick domain. */
static void section_shift_table(void)
{
    const long long freqs[] = {1000000LL, 3500000LL, 1000000000LL, 4571787LL};
    emit("[");
    for (int i = 0; i < 4; ++i) {
        long long f = freqs[i];
        int shift = 0;
        long long t = f;
        while (t > 12000) { t >>= 1; ++shift; }
        if (i) emit(",");
        emit("{\"frequency\":%lld,\"tick_shift\":%d,\"ticks_per_second\":%lld,"
             "\"ticks_per_frame\":%lld,\"smallest_time_the_clock_can_report_ms\":%.6f}",
             f, shift, t, t / 60, ((double)(1LL << shift) / (double)f) * 1000.0);
    }
    emit("]");
}

/* 1,000 ticks at a fixed rate, from the engine's own fixed-rate branch. */
static void section_fixed_rate(int n, float rate, float scale)
{
    reset();
    g_fFixedRate = rate;
    g_fFrameRateScale = scale;

    double sumDelta = 0.0;
    double maxDelta = 0.0;
    int zeroTicks = 0;
    int firstDiffAt = -1;
    double expected = 0.0;
    long long nominalTicks = 0;

    for (int i = 0; i < n; ++i) {
        long long before = (long long)GameState::_TimeBase._iValue;
        GameState::UpdateTime();
        long long advanced = (long long)GameState::_TimeBase._iValue - before;
        if (advanced == 0) ++zeroTicks;
        sumDelta += sDeltaTime;
        if (sDeltaTime > maxDelta) maxDelta = sDeltaTime;
        expected += 1.0 / (double)rate;
        nominalTicks += (long long)((double)Frequency.QuadPart / (double)rate);
        if (firstDiffAt < 0 && sDeltaTime > 1.0 / (double)rate + 1e-9) firstDiffAt = i;
    }

    emit("{\"calls\":%d,\"rate\":%.6f,\"scale\":%.6f,\"sim_time_engine\":%.12f,"
         "\"sim_time_nominal\":%.12f,\"drift_ms\":%.6f,\"sum_of_engine_deltas\":%.12f,"
         "\"max_single_delta_ms\":%.6f,\"frames_that_earned_zero_ticks\":%d,"
         "\"first_frame_where_delta_exceeds_nominal\":%d}",
         n, rate, scale, (double)GameState::_TimeBase.GetTime(), expected,
         ((double)GameState::_TimeBase.GetTime() - expected) * 1000.0, sumDelta,
         maxDelta * 1000.0, zeroTicks, firstDiffAt);
}

/* Variable rate: the engine reads the clock, so the clock is what we hand it. */
static void run_one(long long deltaTicks)
{
    /* The engine remembers the last counter value it read, so the next frame's delta is measured
       from there - not from the time base, which the frame-rate scale may have moved by a
       different amount. */
    start_clock(g_last + deltaTicks);
    /* The engine's spin loop reads the clock; if what we asked for is below the floor it keeps
       reading, and the harness advances one tick per read until the engine is satisfied. */
    GameState::UpdateTime();
}

static void section_variable(const long long* deltas, int n, const char* label)
{
    reset();
    emit("{\"label\":\"%s\",\"calls\":%d,\"entries\":[", label, n);
    for (int i = 0; i < n; ++i) {
        long long before = (long long)GameState::_TimeBase._iValue;
        long long beforeTotal = (long long)GameState::_TotalTimeBase._iValue;
        run_one(deltas[i]);
        long long advanced = (long long)GameState::_TimeBase._iValue - before;
        long long advancedTotal = (long long)GameState::_TotalTimeBase._iValue - beforeTotal;
        long long reads = g_reads;
        if (i) emit(",");
        emit("{\"asked_ms\":%.3f,\"ticks_advanced\":%lld,\"clock_reads\":%lld,"
             "\"delta_the_game_received_ms\":%.6f,\"system_delta_ms\":%.6f,"
             "\"total_base_ticks_advanced\":%lld}",
             (double)deltas[i] / 1000.0, advanced, reads, sDeltaTime * 1000.0,
             sSystemDeltaTime * 1000.0, advancedTotal);
    }
    emit("]}");
}

static void section_ignore_next(void)
{
    reset();
    run_one(20000);
    long long firstGame = (long long)GameState::_TimeBase._iValue;
    long long firstTotal = (long long)GameState::_TotalTimeBase._iValue;
    GameState::SetIgnoreNextUpdate(true);
    run_one(20000);
    emit("{\"first_game_ticks\":%lld,\"first_total_ticks\":%lld,"
         "\"second_game_ticks\":%lld,\"second_total_ticks\":%lld}",
         firstGame, firstTotal,
         (long long)GameState::_TimeBase._iValue - firstGame,
         (long long)GameState::_TotalTimeBase._iValue - firstTotal);
}

extern "C" const char* clock_report(void)
{
    pos = 0;
    reset();
    emit("{\"clock\":");
    section_constants();
    emit(",\"shift_table\":");
    section_shift_table();
    emit(",\"fixed_rate_30hz_1000\":");
    section_fixed_rate(1000, 30.0f, 0.0f);
    emit(",\"fixed_rate_30hz_scale_0p5\":");
    section_fixed_rate(1000, 30.0f, 0.5f);
    emit(",\"variable\":[");

    /* Above the floor, below the max-advance re-anchor, above it, and the clamp boundary. */
    static const long long plain[] = {16667, 12501, 20000, 100000, 100001, 1000000, 1000001, 62500};
    section_variable(plain, 8, "synthetic_deltas");

    static const long long floorcases[] = {10000, 5000, 12499, 12500};
    emit(",");
    section_variable(floorcases, 4, "below_the_80hz_floor");

    static const long long scalecases[] = {40000, 40001, 20000};
    reset();
    g_fFrameRateScale = 0.5f;
    emit(",{\"label\":\"frame_scale_half\",\"calls\":3,\"entries\":[");
    for (int i = 0; i < 3; ++i) {
        long long before = (long long)GameState::_TimeBase._iValue;
        run_one(scalecases[i]);
        long long advanced = (long long)GameState::_TimeBase._iValue - before;
        long long naive = (long long)((float)scalecases[i] * 0.5f);
        long long engine_form = (long long)((float)scalecases[i] * 0.5f + 0.5f);
        if (i) emit(",");
        emit("{\"asked_ms\":%.3f,\"ticks_advanced\":%lld,\"cast_truncates\":%lld,"
             "\"rounds_half_up\":%lld,\"engine_is\":\"%s\"}",
             (double)scalecases[i] / 1000.0, advanced, naive, engine_form,
             advanced == engine_form ? "half_up" : (advanced == naive ? "truncate" : "other"));
    }
    emit("]}");

    /* The same expression with a scale whose product is not exact in float: 0.1f times a large tick
       count is rounded, and rounding it before the +0.5f or after it can differ by one tick. This is
       the case that tells -ffp-contract=fast apart from -ffp-contract=off. */
    static const long long tenthcases[] = {20000, 123456, 99999, 777777};  /* all above the 12.5 ms floor */
    reset();
    g_fFrameRateScale = 0.1f;
    emit(",{\"label\":\"frame_scale_tenth\",\"calls\":4,\"entries\":[");
    for (int i = 0; i < 4; ++i) {
        long long before = (long long)GameState::_TimeBase._iValue;
        run_one(tenthcases[i]);
        long long advanced = (long long)GameState::_TimeBase._iValue - before;
        long long unfused = (long long)((float)tenthcases[i] * 0.1f + 0.5f);
        long long fused = (long long)fmaf((float)tenthcases[i], 0.1f, 0.5f);
        if (i) emit(",");
        emit("{\"asked_ms\":%.3f,\"ticks_advanced\":%lld,\"unfused\":%lld,\"fused\":%lld,"
             "\"fma_would_change_it\":\"%s\"}",
             (double)tenthcases[i] / 1000.0, advanced, unfused, fused,
             unfused == fused ? "no" : "yes");
    }
    emit("]}");

    emit("]");
    emit(",\"ignore_next_time\":");
    section_ignore_next();
    emit("}");
    out[pos] = 0;
    return out;
}

/* The report is emitted from a global constructor, and the process exits there.
 *
 * GameState.cpp has global objects whose constructors do real work over data that lives in TUs this
 * check does not link, so the initializers cannot be left to run: they would read a null pointer or
 * call a no-op stub, and the numbers would then come from whatever the stub happened to return.
 * Running the test as the first initializer and exiting there means the only initializer that runs is
 * the one this check controls; the link order is what puts this object first, and the same rule holds
 * on the wasm side, where __wasm_call_ctors walks init_array in link order. */
__attribute__((constructor)) void run(void)
{
    clock_report();
    puts(out);
    fflush(stdout);
    _exit(0);
}

int main(void) { return 0; }   /* never reached: the test is the first initializer */