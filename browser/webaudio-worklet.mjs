// AudioWorklet endpoint for browser/webaudio.mjs.
//
// Supports two paths without allocating in process():
//   1. Phase 5 lock-free SharedArrayBuffer SPSC stereo float32 ring ('init-sab'), written directly
//      by the engine WebWorker from SoundFL's 48 kHz 3D software mixer.
//   2. Legacy postMessage block queue ('pcm') kept for browser/audio_check.mjs compatibility.

const SAB_HEADER_INT32S = 16; // 64 bytes
const IDX_WRITE = 0;
const IDX_READ = 1;
const IDX_UNDERRUNS = 2;
const IDX_TOTAL_FRAMES = 3;
const IDX_MUTED = 4;

class Swbf2PcmProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    // Legacy postMessage block queue
    this.blocks = [];
    this.queuedFrames = 0;

    // Phase 5 SharedArrayBuffer lock-free stereo ring
    this.sabCtrl = null;
    this.sabL = null;
    this.sabR = null;
    this.sabCap = 0;
    this.sabMask = 0;
    this.lastL = 0;
    this.lastR = 0;
    this.wasUnderrun = false;

    this.port.onmessage = (event) => {
      const message = event.data;
      if (!message) return;

      if (message.type === 'init-sab') {
        const sab = message.sab;
        const cap = message.capacityFrames | 0;
        if (sab && cap > 0 && (cap & (cap - 1)) === 0) {
          this.sabCtrl = new Int32Array(sab, 0, SAB_HEADER_INT32S);
          const chBytes = cap * 4;
          const offL = SAB_HEADER_INT32S * 4;
          this.sabL = new Float32Array(sab, offL, cap);
          this.sabR = new Float32Array(sab, offL + chBytes, cap);
          this.sabCap = cap;
          this.sabMask = cap - 1;
        }
        return;
      }

      if (message.type === 'pcm') {
        const frames = message.frames | 0;
        if (frames <= 0 || !message.left || !message.right) return;
        this.blocks.push({
          left: message.left,
          right: message.right,
          frames,
          offset: 0,
        });
        this.queuedFrames += frames;
      }
    };
  }

  process(inputs, outputs) {
    const output = outputs[0];
    if (!output || !output.length) return true;

    const left = output[0];
    const right = output.length > 1 ? output[1] : output[0];
    const outLen = left.length;

    if (this.sabCtrl) {
      const writeIdx = Atomics.load(this.sabCtrl, IDX_WRITE);
      const readIdx = Atomics.load(this.sabCtrl, IDX_READ);
      const muted = Atomics.load(this.sabCtrl, IDX_MUTED) !== 0;
      let avail = (writeIdx - readIdx) | 0;
      if (avail < 0) avail = 0;
      if (avail > this.sabCap) avail = this.sabCap;

      const count = avail < outLen ? avail : outLen;
      if (count > 0) {
        const mask = this.sabMask;
        const sL = this.sabL;
        const sR = this.sabR;
        if (muted) {
          left.fill(0, 0, count);
          right.fill(0, 0, count);
          this.lastL = 0;
          this.lastR = 0;
        } else {
          const fadeIn = this.wasUnderrun ? (count < 32 ? count : 32) : 0;
          for (let i = 0; i < count; ++i) {
            const slot = (readIdx + i) & mask;
            let l = sL[slot];
            let r = sR[slot];
            if (i < fadeIn) {
              const g = (i + 1) / (fadeIn + 1);
              l *= g;
              r *= g;
            }
            left[i] = l;
            right[i] = r;
          }
          this.lastL = left[count - 1];
          this.lastR = right[count - 1];
          this.wasUnderrun = false;
        }
        Atomics.store(this.sabCtrl, IDX_READ, (readIdx + count) | 0);
        Atomics.add(this.sabCtrl, IDX_TOTAL_FRAMES, count);
      }
      if (count < outLen) {
        // Smoothly fade the tail to zero over up to 32 samples instead of hard-chopping to 0,
        // eliminating harsh digital pop/crackle clicks if a frame hitch causes an underrun.
        const rem = outLen - count;
        const fadeOut = rem < 32 ? rem : 32;
        if (fadeOut > 0 && (this.lastL !== 0 || this.lastR !== 0)) {
          const startL = this.lastL;
          const startR = this.lastR;
          for (let i = 0; i < fadeOut; ++i) {
            const g = 1 - (i + 1) / (fadeOut + 1);
            left[count + i] = startL * g;
            right[count + i] = startR * g;
          }
          this.lastL = 0;
          this.lastR = 0;
        } else {
          left.fill(0, count, count + fadeOut);
          right.fill(0, count, count + fadeOut);
        }
        if (count + fadeOut < outLen) {
          left.fill(0, count + fadeOut, outLen);
          right.fill(0, count + fadeOut, outLen);
        }
        this.wasUnderrun = true;
        if (writeIdx !== 0) {
          Atomics.add(this.sabCtrl, IDX_UNDERRUNS, 1);
        }
      }
      return true;
    }

    // Legacy postMessage block queue path
    left.fill(0);
    right.fill(0);

    let written = 0;
    while (written < outLen && this.blocks.length) {
      const block = this.blocks[0];
      const count = Math.min(outLen - written, block.frames - block.offset);
      left.set(block.left.subarray(block.offset, block.offset + count), written);
      right.set(block.right.subarray(block.offset, block.offset + count), written);
      written += count;
      block.offset += count;
      this.queuedFrames -= count;
      if (block.offset === block.frames) this.blocks.shift();
    }

    return true;
  }
}

registerProcessor('swbf2-pcm-sink', Swbf2PcmProcessor);
