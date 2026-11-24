# Browser WebAudio: the first real PCM slice

## What this is

This is a small, real browser audio path. It does **not** implement the full DirectSound
mixer yet.

The path is:

1. `browser/platform/audio_browser.cpp` opens the shipped `common.bnk` through the engine's own
   `PblFile` / `PblFileChunk` readers.
2. It walks the SoundFL bank metadata in the same order as
   `SoundFL/source/sndsamplebankbase.cpp` (`LoadBank` / `LoadSamples`), and checks the bank and
   sample format through `Snd::SampleInfo`.
3. It copies the selected PCM16 bytes into a bounded C ABI queue.
4. `browser/webaudio.mjs` drains that queue, linearly resamples it to the AudioContext rate, and
   sends real float PCM to `browser/webaudio-worklet.mjs`.
5. The worklet output is connected to the real `AudioContext.destination`.

There is no JavaScript sound decoder and no synthesised fallback sound. The PCM bytes measured by
the check are the bytes in the shipped bank.

`DirectSoundCreate8` still reports that there is no DirectSound device. That is deliberate: a
full `DSBufferRenderer` mixer is a separate, larger piece of work and is not needed to prove this
slice.

## The exact sample

The first sample is `force_push_fire`, a blaster sound.

| Property | Measured value |
|---|---:|
| Sound name | `force_push_fire` |
| SoundFL sample hash | `0x6aa27e0e` (1789033998) |
| Bank format | PCM16 (`Snd::SampleInfo::format_PCM16`, value 2) |
| Sample rate | 22050 Hz |
| Channels | 1 |
| PCM bytes | **90,986** |
| PCM frames | 45,493 |
| Duration | 2.063174603174603 s |
| PCM SHA-256 | `ba7b1ad54541c5499556de1ca8c7a30a2adaaedff879a2abb43d3e073e726391` |

The source bank is:

| Property | Measured value |
|---|---:|
| Path | `data/Star wars battlefront 2/GameData/data/_lvl_pc/sound/common.bnk` |
| File bytes | 77,942,784 |
| File SHA-256 | `43976f384fba818f0d06de322aafaaa985ffca52950591254dd4ab3a3be420f0` |
| Bank format | 2 (PCM16) |
| Samples in bank | 1567 |

The selected sample's PCM starts at file offset `0x0e44b6` (935094). The SHA-256 above is over
exactly the 90,986 PCM bytes, not over a decoded approximation.

## The mission's actual fire sample

The Stormtrooper's weapon class names `imp_weap_inf_rifle_fire` in `side/imp.lvl`. That is a
`Snd::Sound::Properties` record in `sound/tat.lvl`'s `tat2gcw` set, not a sample name in the
bank. Its weight list selects sample hash `0x2EB95650` in `sound/common.bnk`.

`tools/extract_sound_sample.py sound/common.bnk --hash 0x2EB95650` copies that record and its
exact PCM bytes into `build/class_data/sound/common/sample_2eb95650.lvl`.

| Property | Measured value |
|---|---:|
| Sample hash | `0x2EB95650` |
| Format | PCM16, mono |
| Rate | 44,100 Hz |
| Frames | 48,360 |
| PCM bytes | 96,720 |
| Extracted bank size | 96,844 bytes |
| PCM SHA-256 | `447e3598ccc144017cc718f908b21269436207a0e4becbb151aa2af097bc9f5a` |

The bank is 77,942,784 bytes in the original data; the extracted bank is 96,844 bytes. The PCM
hash matches the original bytes exactly. No decoder or replacement sound is involved.

The host stages that small bank and calls `audio_browser_load_sample(path, 0x2EB95650)` only
on an actual fire-trigger edge. The AudioContext itself is created/resumed from the first real
user gesture, as the browser requires; movement keys do not play a shot.

## C ABI

The exports are listed in `tools/build_browser.sh`.

| Function | Meaning |
|---|---|
| `audio_browser_load_sample(path, hash)` | Load one PCM16 sample by SoundFL hash. Returns 1 on success, 0 if not found, and a negative error code otherwise. |
| `audio_browser_load_sample_by_name(path, name)` | The same, with the name hashed by the engine's `PblHash` implementation. |
| `audio_browser_last_load_status()` | The status from the most recent load attempt. |
| `audio_browser_sample_hash()` | Loaded sample hash. |
| `audio_browser_sample_rate()` | Loaded sample rate in Hz. |
| `audio_browser_sample_bytes()` | Loaded PCM byte count. |
| `audio_browser_sample_channels()` | Loaded channel count. |
| `audio_browser_sample_frames()` | Loaded frame count. |
| `audio_browser_push_pcm(pcm, frames, channels, rate)` | Copy an interleaved PCM16 block into the generic queue. This is the future mixer seam. |
| `audio_browser_drain_pcm(out, maxFrames, rateOut, channelsOut)` | Copy up to `maxFrames` interleaved frames from the queue head. Returns the number copied. |
| `audio_browser_pending_frames()` | Frames currently queued. |
| `audio_browser_clear()` | Drop queued PCM. |
| `audio_browser_set_sink_ready(ready)` | The JavaScript sink sets this only after the AudioContext is running and its output node is connected. |
| `audio_browser_sink_ready()` | Read the sink-ready handshake. |

A load returning 1 means that the real PCM was found and queued. It does not claim that an
AudioContext is running. `WebAudioSink.start()` is the operation that claims playback: it returns
only after the AudioContext state is `running` and the output node is connected. The sink then
sets `audio_browser_set_sink_ready(1)`.

The queue accepts interleaved PCM16, mono or stereo, and is capped at 16 MiB.

## JavaScript API

`browser/webaudio.mjs` exports:

```js
import {
  createWebAudioSink,
  loadSampleAndStart,
  FORCE_PUSH_FIRE_HASH,
  FORCE_PUSH_FIRE_NAME,
} from './webaudio.mjs';

const sink = await loadSampleAndStart({
  module: Module,
  bankPath: '/data/common.bnk',
  sampleName: 'force_push_fire',
});

// The AudioContext must normally be created/resumed from a user gesture.
sink.stop();
```

`createWebAudioSink()` can also be used by itself when PCM is pushed with
`audio_browser_push_pcm`. It prefers an `AudioWorklet`; if `addModule()` is unavailable it falls
back to a `ScriptProcessor`. Both are real WebAudio output paths.

## Measurable check

Run:

```sh
bash tools/build_browser.sh
NODE_OPTIONS= node browser/audio_check.mjs
```

The check performs two measurements:

1. **Byte identity (Node):** it loads the bank through the C++ reader, drains the queue and
   hashes every PCM byte.
2. **Playback signal (headless Chromium):** it starts a real 48 kHz AudioContext, attaches an
   analyser after the worklet output, and measures the signal energy.

Measured in this workspace:

| Measurement | Result |
|---|---:|
| C++ load status | 1 |
| Drained frames | 45,493 / 45,493 |
| Drained PCM SHA-256 | `ba7b1ad54541c5499556de1ca8c7a30a2adaaedff879a2abb43d3e073e726391` |
| PCM min / max | -31508 / 32767 |
| Non-zero PCM samples | 45,490 |
| Browser AudioContext | `running`, 48000 Hz |
| Browser path | AudioWorklet (`usingWorklet: true`) |
| Measured WebAudio peak | 0.999969482421875 |
| Non-zero analyser samples | 82,649 |

`--no-browser` runs only the byte-exact Node half.

## Remaining integration

The shared harness, `browser/main.cpp`, `browser/host.js`, and `browser/index.html` were not
changed. The remaining integration is therefore explicit:

1. Stage `common.bnk` in the wasm VFS at `/data/common.bnk` (or extract just the selected bank
   record in a build step if the 78 MiB transfer is too expensive).
2. Import `browser/webaudio.mjs` from the host.
3. Create/resume the `AudioContext` from a user gesture and call `loadSampleAndStart()`.
4. For the real fire event, trigger the loaded sample at the weapon fire event. The current slice
   loads and plays one sample; it is not yet attached to `LuaCallbacks_Sound.cpp` or the weapon
   fire path.
5. If the engine's own mixer is implemented later, route `DSBufferRenderer` output through
   `audio_browser_push_pcm()` instead of using the one-shot sample loader. That is the point at
   which the full DirectSound buffer layer becomes useful.

Until those steps are done, this is an independent, tested browser audio backend rather than a
change to the shared game harness.
