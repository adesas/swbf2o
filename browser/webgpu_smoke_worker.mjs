// Dedicated Worker hosting the emdawnwebgpu Phase 0 smoke test on an OffscreenCanvas.
import createSmoke from '../build/webgpu_smoke/wgpu_smoke.js';

self.onmessage = async (ev) => {
  const { canvas, width, height } = ev.data || {};
  try {
    if (!navigator.gpu) {
      self.postMessage({ ok: false, error: 'navigator.gpu unavailable in Dedicated Worker' });
      return;
    }
    const mod = await createSmoke({
      canvas,
      onSmokeComplete(jsonStr) {
        const res = JSON.parse(jsonStr);
        res.inDedicatedWorker = true;
        self.postMessage(res);
      },
    });
    mod._wgpu_smoke_run(width || 256, height || 256);
  } catch (err) {
    self.postMessage({
      ok: false,
      inDedicatedWorker: true,
      error: String(err && err.stack ? err.stack : err),
    });
  }
};
