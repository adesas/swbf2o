# The frame-pacing clock, measured

What the single-player slice needs first is a loop whose time is the engine's time rather than the
host's. This note records what the engine's loop actually does, measured rather than assumed, because
two of the assumptions behind the obvious plan turn out to be wrong.

Reproduce:

```sh
python3 tools/clock_oracle_check.py
```

It builds the engine's own `PblTimeBase.cpp` and `GameState.cpp` four ways — native at `-O0`
`-ffp-contract=off`, native at `-O2 -ffp-contract=fast -mfma`, wasm with the flags
`tools/compile_core.sh` uses, and wasm with `-ffp-contract=off` — drives `GameState::UpdateTime()`
from a scripted clock, and compares the numbers.

## The loop

`GameState::UpdateTime()` (`Battlefront2/Source/GameState.cpp:468`) has two branches:

* **Fixed rate** (`g_fFixedRate > 0`): the delta is `1/rate`, the clock is never read, and
  `_TimeBase.AdvanceTime(fDeltaTime)` is the only arithmetic. This is the deterministic branch and the
  one a headless oracle should use.
* **Variable rate**: the clock is read in a spin loop until the delta reaches `Frequency / 80`
  (12.5 ms at the shim's 1 MHz), a delta above `Frequency` (one second) is replaced by
  `Frequency >> 4` (62.5 ms), and the delta handed to the game is clamped to 100 ms (1 s with net
  enabled).

Measured, with synthetic deltas, in all four builds identically:

| asked | engine advances | game receives |
|---:|---:|---:|
| 16.667 ms | 16 667 ticks | 16.641 ms |
| 12.501 ms | 12 501 ticks | 12.417 ms |
| 10.000 ms | 12 500 ticks (2 502 clock reads) | 12.417 ms |
| 5.000 ms | 12 500 ticks (7 502 clock reads) | 12.417 ms |
| 100.000 ms | 100 000 ticks | 99.974 ms |
| 1000.000 ms | 1 000 000 ticks | 100.000 ms (clamped) |
| 1000.001 ms | 62 500 ticks (re-anchored) | 62.468 ms |

`SetIgnoreNextUpdate(true)` advances the game clock and leaves the total clock alone, which is what
the engine does after a load.

## Two assumptions that do not hold

**"Assert zero drift in accumulated sim time."** Drift against the nominal is not zero and cannot be:
the time base is an integer tick counter, and the smallest time it can report is one tick of the
shifted domain — 0.128 ms at the shim's 1 MHz. Over 1 000 fixed-rate ticks at 30 Hz the engine
accumulates 33.333076477 s against a nominal 33.333333333 s: **-0.257 ms**, and the deltas the game
actually receives sum to 33.282130957 s. The assertion that *is* true, and that the check makes, is
that all four builds produce the same numbers — the drift is a property of the engine, identical
everywhere, not a divergence between platforms.

**"-ffp-contract=off" matters for the frame pacing.** It does not, in this build. The one expression
in the pacing path a compiler may fuse is `iDeltaTicks = (int64)((float)iDeltaTicks *
g_fFrameRateScale + 0.5f)` (`GameState.cpp:524`), and **nothing in the tree ever writes
`g_fFrameRateScale`** — it is a global with no writer, so the path is dead code. The check exercises
it anyway (scales 0.5 and 0.1, tick counts chosen to stay above the 12.5 ms floor) and finds fused
and unfused results identical for every case. Over 800 million random (scale, tick count) pairs the
two differ in 4.4% of cases, so the hazard is real in the abstract — it just is not on this path, and
the flag is still worth pinning for the geometry and the model maths, where `docs/data-format.md`
shows the engine really does depend on the rounding.

## What does have to be pinned: the tick domain

`PblTimeBase`'s constructor takes `TICK_SHIFT` and `_fSecondsPerTick` from
`QueryPerformanceFrequency`, so the tick domain is a property of the platform clock:

| frequency | TICK_SHIFT | ticks/second | ticks/frame | smallest reportable step |
|---:|---:|---:|---:|---:|
| 1 000 000 (the shim) | 7 | 7 812 | 130 | 0.128 ms |
| 3 500 000 | 9 | 6 835 | 113 | 0.146 ms |
| 4 571 787 | 9 | 8 929 | 148 | 0.112 ms |
| 1 000 000 000 | 17 | 7 629 | 127 | 0.131 ms |

The shim pins 1 MHz for every build, which is why a native/Wasm comparison is possible at all. The
shipped game does not pin it: a native build against a real hardware QPF and a wasm build against a
pinned 1 MHz are in different tick domains from the first frame, and no amount of care with float
flags makes those agree. For the single-player slice this does not matter — there is one client — but
it is the reason the frequency has to be pinned before anything is compared against the original game.

## What the browser host should do with this

`browser/main.cpp`'s `swbf2_frame` currently advances its own accumulator at 30 Hz and calls
`Step()`, which does nothing but add to a float. Replacing it with the engine's loop means:

* driving `GameState::UpdateTime()` rather than a host-side accumulator, and taking `sDeltaTime` from
  the engine rather than from `requestAnimationFrame`;
* using the fixed-rate branch (`g_fFixedRate = 30`) in the browser, because the variable-rate branch
  spins on `QueryPerformanceCounter` and the shim's `Sleep` is a no-op — the spin would burn a core;
* expecting the game to receive a delta that is quantised to 0.128 ms and, in the variable branch,
  never less than 12.5 ms and never more than 100 ms.

The three numbers the engine exposes for this are `GameState::sDeltaTime`, `GameState::sSystemDeltaTime`
and `GameState::_TimeBase.GetTime()`.

## How the check is built

`spike/clock_oracle.cpp` calls the engine's `GameState::UpdateTime()` unmodified. It owns the clock
(`swbf2_now_ms` is the shim's only clock source) and the few globals `UpdateTime` reads, and it
reproduces `GameState::OneTimeInit`'s clock half — `QueryPerformanceFrequency`, `maxAdvanceTicks`,
`normalAdvanceTicks` — without the sound engine, the renderer or the heap. The two time bases and the
stamps are initialised the way the engine's state `Enter()` initialises them.

Two things about the harness are deliberate:

* The report is emitted from a global constructor and the process exits there. `GameState.cpp` has
  global objects whose constructors do real work over data that lives in TUs this check does not
  link, so letting the initializers run would make the numbers depend on stubs. Running the test as
  the first initializer — link order decides — means the only initializer that runs is the one the
  check controls.
* The stubs are generated from the objects' own relocations (`tools/clock_oracle_check.py`), not
  written by hand: call targets become no-op functions, data becomes a zero buffer, and a symbol is
  only stubbed if the link actually lacks it and no linked object defines it.

This is the same discipline as `tools/float_corpus_check.py`: the comparison is between builds of one
implementation, so it cannot tell us whether the engine's clock is *right* — only whether two
platforms compute the same one.

## What the browser host does now

The host drives the engine's clock (`browser/main.cpp`, `Step()` and `EngineClockInit()`):

* `EngineClockInit()` sets `g_fFixedRate = 30` and anchors `AppTimeStamp` / `SystemTimeStamp`
  against `GameState::_TimeBase` / `_SystemTimeBase`, so the engine's first `UpdateTime()` continues
  from the time base rather than from zero.
* `Step()` calls `GameState::UpdateTime()` and reads the delta the engine computed
  (`sDeltaTime`) and the engine's own elapsed time (`GameState::_TimeBase.GetTime()`). The host
  still decides *when* to tick - `requestAnimationFrame` at 30 Hz - and the engine decides *how
  much* time passed, which is the same split as the original game.
* The host exports `swbf2_sim_delta` and `swbf2_sim_ticks` next to `swbf2_sim_time`, so the page
  can show what the engine saw rather than what the host assumed.

The fixed-rate branch is the one the browser uses, for the reason measured above: the variable
branch spins on `QueryPerformanceCounter` until the delta reaches the threshold, and the shim's
`Sleep` is a no-op, so the spin would burn a core of the browser's main thread.

What this cost: `GameState::UpdateTime()` is in `GameState.o`, and `GameState.o` reaches
`GameLoop.o` and through it the whole engine, so the browser module stopped being a host with a
file reader in it and became the engine with a browser layer around it. The symbols that link then
demanded are listed, file by file, in `docs/backend-surface.md`; the whole-archive link now reports
zero unresolved symbols.

Measured result in the browser build (`browser/node_test.mjs`): 120 frames at 30 Hz give
`simTime = 2.00 s`, and the log line says which clock produced it:

```
clock: engine GameState::UpdateTime at 30 Hz (fixed-rate branch)
frames=120 simTime=2.00s
```
