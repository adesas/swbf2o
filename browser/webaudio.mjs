// WebAudio sink for the browser PCM queue.
//
// The JavaScript side does not decode a sound.  It moves PCM16 blocks that came out of the
// engine-side SoundFL bank reader into a real WebAudio graph.  The preferred graph is an
// AudioWorklet; older browsers get a ScriptProcessor fallback.  Both paths render the actual
// samples and both report failure honestly: start() succeeds only after the AudioContext is
// running and the output node is connected.

export const FORCE_PUSH_FIRE_HASH = 0x6aa27e0e;
export const FORCE_PUSH_FIRE_NAME = 'force_push_fire';

const DEFAULT_WORKLET_URL = new URL('./webaudio-worklet.mjs', import.meta.url).href;
const DEFAULT_MAX_FRAMES = 8192;

function asError(message, cause) {
  const error = new Error(message);
  if (cause !== undefined) error.cause = cause;
  return error;
}

// A tiny linear resampler used on the main thread.  It accepts interleaved PCM16 chunks with
// an arbitrary source rate and renders stereo float frames at the AudioContext rate.
class PcmStream {
  constructor(contextRate) {
    this.contextRate = contextRate;
    this.chunks = [];
  }

  clear() {
    this.chunks.length = 0;
  }

  push(pcm, channels, sampleRate, frames) {
    if (!frames || !channels) return;
    this.chunks.push({ pcm, channels, sampleRate, frames, position: 0 });
  }

  hasData() {
    return this.chunks.length !== 0;
  }

  render(left, right, frameCount) {
    left.fill(0, 0, frameCount);
    right.fill(0, 0, frameCount);

    for (let outputFrame = 0; outputFrame < frameCount; ++outputFrame) {
      let chunk = this.chunks[0];
      while (chunk && chunk.position >= chunk.frames) {
        this.chunks.shift();
        chunk = this.chunks[0];
      }
      if (!chunk) break;

      const sourceFrame = chunk.position;
      const frame0 = Math.floor(sourceFrame);
      const frame1 = Math.min(chunk.frames - 1, frame0 + 1);
      const mix = sourceFrame - frame0;
      const stride = chunk.channels;

      for (let channel = 0; channel < 2; ++channel) {
        const sourceChannel = Math.min(channel, stride - 1);
        const a = chunk.pcm[frame0 * stride + sourceChannel] / 32768;
        const b = chunk.pcm[frame1 * stride + sourceChannel] / 32768;
        const value = a + (b - a) * mix;
        if (channel === 0) left[outputFrame] = value;
        else right[outputFrame] = value;
      }

      chunk.position += chunk.sampleRate / this.contextRate;
    }
  }
}

export class WebAudioSink {
  constructor(options = {}) {
    if (!options.module) throw new Error('WebAudioSink needs the Emscripten module');
    this.module = options.module;
    this.context = options.context || null;
    this.workletUrl = options.workletUrl || DEFAULT_WORKLET_URL;
    this.maxFrames = options.maxFrames || DEFAULT_MAX_FRAMES;
    this.node = null;
    this.output = null;
    this.started = false;
    this.usingWorklet = false;
    this.timer = null;
    this.scratch = 0;
    this.ratePointer = 0;
    this.channelPointer = 0;
    this.stream = null;
    this.framesDelivered = 0;
    this.chunksDelivered = 0;
  }

  async start() {
    if (this.started) return this;

    const AudioContextCtor = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!this.context && !AudioContextCtor) {
      throw new Error('WebAudio is unavailable: neither AudioContext nor webkitAudioContext exists');
    }
    if (!this.context) this.context = new AudioContextCtor({ latencyHint: 'interactive' });

    if (this.context.state === 'suspended') {
      await this.context.resume();
    }
    if (this.context.state !== 'running') {
      throw new Error(`WebAudio sink did not start: AudioContext state is ${this.context.state}`);
    }

    this.stream = new PcmStream(this.context.sampleRate);
    this.scratch = this.module._malloc(this.maxFrames * 2 * 2);
    this.ratePointer = this.module._malloc(4);
    this.channelPointer = this.module._malloc(4);
    if (!this.scratch || !this.ratePointer || !this.channelPointer) {
      this.freeScratch();
      throw new Error('WebAudio sink could not allocate its PCM scratch buffer');
    }

    let node = null;
    if (this.context.audioWorklet && globalThis.AudioWorkletNode) {
      try {
        await this.context.audioWorklet.addModule(this.workletUrl);
        node = new globalThis.AudioWorkletNode(this.context, 'swbf2-pcm-sink', {
          numberOfInputs: 0,
          numberOfOutputs: 1,
          outputChannelCount: [2],
        });
        this.usingWorklet = true;
      } catch (error) {
        // A file:// worklet URL or a restrictive CSP can block addModule.  The ScriptProcessor
        // path below is still a real WebAudio output path, so use it instead of pretending that
        // the worklet started.
        node = null;
        this.usingWorklet = false;
        this.workletError = error;
      }
    }

    if (!node && this.context.createScriptProcessor) {
      node = this.context.createScriptProcessor(2048, 0, 2);
      node.onaudioprocess = (event) => {
        const output = event.outputBuffer;
        const left = output.getChannelData(0);
        const right = output.numberOfChannels > 1 ? output.getChannelData(1) : left;
        this.stream.render(left, right, output.length);
      };
      this.usingWorklet = false;
    }

    if (!node) {
      this.freeScratch();
      throw asError('WebAudio sink did not start: no AudioWorklet or ScriptProcessor is available');
    }

    node.connect(this.context.destination);
    this.node = node;
    this.output = node;
    this.started = true;

    // This flag is the C++/JS handshake.  It is set only now: the context is running and the
    // node is connected to the real destination.
    this.module.ccall('audio_browser_set_sink_ready', null, ['number'], [1]);

    this.timer = globalThis.setInterval(() => this.pump(), 20);
    return this;
  }

  pump() {
    if (!this.started || !this.stream) return 0;

    let copied = 0;
    for (;;) {
      const frames = this.module.ccall(
        'audio_browser_drain_pcm',
        'number',
        ['number', 'number', 'number', 'number'],
        [this.scratch, this.maxFrames, this.ratePointer, this.channelPointer],
      );
      if (frames <= 0) break;

      const rate = this.module.getValue(this.ratePointer, 'i32');
      const channels = this.module.getValue(this.channelPointer, 'i32');
      const count = frames * channels;
      const heap = this.module.HEAPU8;
      const pcm = new Int16Array(heap.buffer, this.scratch, count).slice();
      this.stream.push(pcm, channels, rate, frames);
      copied += frames;

      if (frames < this.maxFrames) break;
    }

    if (this.usingWorklet && (copied || this.stream.hasData())) {
      // Render roughly 20 ms at the device rate.  The worklet queues these float blocks and
      // emits them at the AudioContext's own pace.
      const frameCount = Math.max(128, Math.round(this.context.sampleRate * 0.02));
      const left = new Float32Array(frameCount);
      const right = new Float32Array(frameCount);
      this.stream.render(left, right, frameCount);
      this.node.port.postMessage(
        { type: 'pcm', frames: frameCount, left, right },
        [left.buffer, right.buffer],
      );
      this.framesDelivered += frameCount;
      this.chunksDelivered++;
    }

    return copied;
  }

  stop() {
    if (!this.started) return;
    this.started = false;
    this.module.ccall('audio_browser_set_sink_ready', null, ['number'], [0]);
    if (this.timer !== null) {
      globalThis.clearInterval(this.timer);
      this.timer = null;
    }
    if (this.node) {
      if (this.node.onaudioprocess) this.node.onaudioprocess = null;
      try { this.node.disconnect(); } catch {}
    }
    this.node = null;
    this.output = null;
    this.freeScratch();
    if (this.stream) this.stream.clear();
  }

  freeScratch() {
    if (this.scratch) this.module._free(this.scratch);
    if (this.ratePointer) this.module._free(this.ratePointer);
    if (this.channelPointer) this.module._free(this.channelPointer);
    this.scratch = 0;
    this.ratePointer = 0;
    this.channelPointer = 0;
  }

  stats() {
    return {
      started: this.started,
      usingWorklet: this.usingWorklet,
      contextState: this.context ? this.context.state : 'none',
      contextRate: this.context ? this.context.sampleRate : 0,
      framesDelivered: this.framesDelivered,
      chunksDelivered: this.chunksDelivered,
    };
  }
}

export function createWebAudioSink(options = {}) {
  return new WebAudioSink(options);
}

// Load the exact PCM16 sample from the engine-side bank reader, then start the WebAudio sink.
// A non-1 return from the C ABI is an error; the helper never reports success for a missing
// sample or a bank that could not be read.
export async function loadSampleAndStart(options = {}) {
  const module = options.module;
  if (!module) throw new Error('loadSampleAndStart needs the Emscripten module');

  const bankPath = options.bankPath || '/data/common.bnk';
  const sampleName = options.sampleName === undefined ? FORCE_PUSH_FIRE_NAME : options.sampleName;
  const sampleHash = options.sampleHash;

  let status;
  if (sampleHash !== undefined) {
    status = module.ccall(
      'audio_browser_load_sample',
      'number',
      ['string', 'number'],
      [bankPath, sampleHash >>> 0],
    );
  } else if (sampleName) {
    status = module.ccall(
      'audio_browser_load_sample_by_name',
      'number',
      ['string', 'string'],
      [bankPath, sampleName],
    );
  } else {
    throw new Error('loadSampleAndStart needs either sampleName or sampleHash');
  }
  if (status !== 1) {
    throw new Error(`SoundFL sample load failed (status ${status}) for ${bankPath}`);
  }

  const sink = createWebAudioSink(options);
  await sink.start();
  return sink;
}

// ---- Phase 5 Lock-Free SharedArrayBuffer Stereo 48 kHz AudioWorklet Ring --------------------
// Layout matches browser/webaudio-worklet.mjs:
//   [0..63]                   Int32Array(16): [writeIdx, readIdx, underruns, totalFrames, muted]
//   [64..64+cap*4-1]          Float32Array(cap) left channel
//   [64+cap*4..64+cap*8-1]    Float32Array(cap) right channel

export const AUDIO_SAB_HEADER_BYTES = 64;
export const AUDIO_SAB_DEFAULT_FRAMES = 16384; // ~341 ms @ 48 kHz (must be power of 2)
export const AUDIO_SAB_IDX_WRITE = 0;
export const AUDIO_SAB_IDX_READ = 1;
export const AUDIO_SAB_IDX_UNDERRUNS = 2;
export const AUDIO_SAB_IDX_TOTAL_FRAMES = 3;
export const AUDIO_SAB_IDX_MUTED = 4;

export function createAudioRingBuffer(capacityFrames = AUDIO_SAB_DEFAULT_FRAMES) {
  if ((capacityFrames & (capacityFrames - 1)) !== 0 || capacityFrames < 1024) {
    throw new Error(`Audio ring capacity must be a power of 2 >= 1024, got ${capacityFrames}`);
  }
  const byteLength = AUDIO_SAB_HEADER_BYTES + capacityFrames * 4 * 2;
  const sab = new SharedArrayBuffer(byteLength);
  return { sab, capacityFrames };
}

export class AudioRingWriter {
  constructor(sab, capacityFrames = AUDIO_SAB_DEFAULT_FRAMES) {
    this.sab = sab;
    this.capacityFrames = capacityFrames;
    this.mask = capacityFrames - 1;
    this.ctrl = new Int32Array(sab, 0, AUDIO_SAB_HEADER_BYTES / 4);
    this.left = new Float32Array(sab, AUDIO_SAB_HEADER_BYTES, capacityFrames);
    this.right = new Float32Array(sab, AUDIO_SAB_HEADER_BYTES + capacityFrames * 4, capacityFrames);
    this.overruns = 0;
    this.framesWritten = 0;
  }

  write(srcL, srcR, frames) {
    if (!frames || frames <= 0) return 0;
    const writeIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_WRITE);
    const readIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_READ);
    const used = Math.max(0, Math.min(this.capacityFrames, (writeIdx - readIdx) | 0));
    const free = this.capacityFrames - used;
    let toWrite = frames;
    if (toWrite > free) {
      this.overruns++;
      toWrite = free;
    }
    if (toWrite <= 0) return 0;
    const mask = this.mask;
    const dstL = this.left;
    const dstR = this.right;
    for (let i = 0; i < toWrite; ++i) {
      const slot = (writeIdx + i) & mask;
      dstL[slot] = srcL[i];
      dstR[slot] = srcR[i];
    }
    Atomics.store(this.ctrl, AUDIO_SAB_IDX_WRITE, (writeIdx + toWrite) | 0);
    this.framesWritten += toWrite;
    return toWrite;
  }

  reset() {
    const readIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_READ);
    Atomics.store(this.ctrl, AUDIO_SAB_IDX_WRITE, readIdx);
  }

  getStats() {
    const writeIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_WRITE);
    const readIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_READ);
    return {
      writeIdx,
      readIdx,
      queuedFrames: Math.max(0, (writeIdx - readIdx) | 0),
      underruns: Atomics.load(this.ctrl, AUDIO_SAB_IDX_UNDERRUNS),
      totalFramesRead: Atomics.load(this.ctrl, AUDIO_SAB_IDX_TOTAL_FRAMES),
      framesWritten: this.framesWritten,
      overruns: this.overruns,
    };
  }
}

export class WebAudioSabSink {
  constructor(options = {}) {
    this.sampleRate = options.sampleRate || 48000;
    this.capacityFrames = options.capacityFrames || AUDIO_SAB_DEFAULT_FRAMES;
    this.workletUrl = options.workletUrl || DEFAULT_WORKLET_URL;
    this.initialGain = options.initialGain !== undefined ? Math.max(0, Math.min(1, Number(options.initialGain))) : 1.0;
    const ring = options.sab
      ? { sab: options.sab, capacityFrames: this.capacityFrames }
      : createAudioRingBuffer(this.capacityFrames);
    this.sab = ring.sab;
    this.ctrl = new Int32Array(this.sab, 0, AUDIO_SAB_HEADER_BYTES / 4);
    this.context = options.context || null;
    this.node = null;
    this.gainNode = null;
    this.analyser = null;
    this.started = false;
    this.usingWorklet = false;
  }

  async start({ withAnalyser = false, allowSuspended = false } = {}) {
    if (this.started) return this;
    const AudioContextCtor = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!this.context && !AudioContextCtor) {
      throw new Error('WebAudio is unavailable: AudioContext missing');
    }
    if (!this.context) {
      this.context = new AudioContextCtor({
        sampleRate: this.sampleRate,
        latencyHint: 'interactive',
      });
    }
    if (this.context.sampleRate > 0) {
      this.sampleRate = this.context.sampleRate;
    }
    if (this.context.state === 'suspended') {
      if (allowSuspended) {
        await Promise.race([
          this.context.resume().catch(() => {}),
          new Promise((r) => setTimeout(r, 350)),
        ]);
      } else {
        await this.context.resume();
      }
    }
    if (!allowSuspended && this.context.state !== 'running') {
      throw new Error(`WebAudioSabSink AudioContext did not reach running state (${this.context.state})`);
    }

    let node = null;
    if (this.context.audioWorklet && globalThis.AudioWorkletNode) {
      await this.context.audioWorklet.addModule(this.workletUrl);
      node = new globalThis.AudioWorkletNode(this.context, 'swbf2-pcm-sink', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
      });
      node.port.postMessage({
        type: 'init-sab',
        sab: this.sab,
        capacityFrames: this.capacityFrames,
      });
      this.usingWorklet = true;
    } else if (this.context.createScriptProcessor) {
      const cap = this.capacityFrames;
      const mask = cap - 1;
      const leftRing = new Float32Array(this.sab, AUDIO_SAB_HEADER_BYTES, cap);
      const rightRing = new Float32Array(this.sab, AUDIO_SAB_HEADER_BYTES + cap * 4, cap);
      node = this.context.createScriptProcessor(1024, 0, 2);
      node.onaudioprocess = (ev) => {
        const outL = ev.outputBuffer.getChannelData(0);
        const outR = ev.outputBuffer.getChannelData(1);
        const writeIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_WRITE);
        const readIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_READ);
        const avail = Math.max(0, Math.min(cap, (writeIdx - readIdx) | 0));
        const count = Math.min(outL.length, avail);
        for (let i = 0; i < count; ++i) {
          const slot = (readIdx + i) & mask;
          outL[i] = leftRing[slot];
          outR[i] = rightRing[slot];
        }
        outL.fill(0, count);
        outR.fill(0, count);
        if (count > 0) {
          Atomics.store(this.ctrl, AUDIO_SAB_IDX_READ, (readIdx + count) | 0);
          Atomics.add(this.ctrl, AUDIO_SAB_IDX_TOTAL_FRAMES, count);
        }
      };
      this.usingWorklet = false;
    } else {
      throw new Error('WebAudioSabSink: neither AudioWorklet nor ScriptProcessor available');
    }

    this.gainNode = this.context.createGain();
    this.gainNode.gain.value = this.initialGain;
    node.connect(this.gainNode);

    if (withAnalyser) {
      this.analyser = this.context.createAnalyser();
      this.analyser.fftSize = 2048;
      this.gainNode.connect(this.analyser);
      this.analyser.connect(this.context.destination);
    } else {
      this.gainNode.connect(this.context.destination);
    }

    this.node = node;
    this.started = true;
    return this;
  }

  setGain(value, rampSec = 0.08) {
    const g = Math.max(0, Math.min(1, Number(value) || 0));
    this.initialGain = g;
    if (this.gainNode && this.context) {
      const now = this.context.currentTime;
      if (rampSec > 0 && typeof this.gainNode.gain.setTargetAtTime === 'function') {
        this.gainNode.gain.setTargetAtTime(g, now, rampSec);
      } else {
        this.gainNode.gain.value = g;
      }
    }
  }

  setMuted(muted) {
    Atomics.store(this.ctrl, AUDIO_SAB_IDX_MUTED, muted ? 1 : 0);
  }

  getStats() {
    const writeIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_WRITE);
    const readIdx = Atomics.load(this.ctrl, AUDIO_SAB_IDX_READ);
    return {
      started: this.started,
      usingWorklet: this.usingWorklet,
      contextState: this.context ? this.context.state : 'none',
      contextRate: this.context ? this.context.sampleRate : 0,
      writeIdx,
      readIdx,
      queuedFrames: Math.max(0, (writeIdx - readIdx) | 0),
      underruns: Atomics.load(this.ctrl, AUDIO_SAB_IDX_UNDERRUNS),
      totalFramesRead: Atomics.load(this.ctrl, AUDIO_SAB_IDX_TOTAL_FRAMES),
    };
  }

  async stop() {
    this.started = false;
    if (this.node) {
      try { this.node.disconnect(); } catch {}
      this.node = null;
    }
    if (this.gainNode) {
      try { this.gainNode.disconnect(); } catch {}
      this.gainNode = null;
    }
    if (this.analyser) {
      try { this.analyser.disconnect(); } catch {}
      this.analyser = null;
    }
    if (this.context) {
      try { await this.context.close(); } catch {}
      this.context = null;
    }
  }
}
