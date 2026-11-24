// Cross-Origin Isolation Service Worker for static hosts (GitHub Pages / Cloudflare Pages / Netlify).
//
// SharedArrayBuffer (used by Phase 5 lock-free input & 48 kHz AudioWorklet rings and Phase 6
// WebRTC/Winsock TX/RX rings) requires Cross-Origin-Opener-Policy: same-origin and
// Cross-Origin-Embedder-Policy: require-corp. When served behind tools/serve.py or a CDN with
// those headers already configured, play.mjs skips registration automatically. On static hosts
// without custom header rules, registering this service worker injects the headers transparently.

if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (ev) => ev.waitUntil(self.clients.claim()));

  self.addEventListener('fetch', (ev) => {
    const req = ev.request;
    if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
    ev.respondWith((async () => {
      const res = await fetch(req);
      if (res.status === 0) return res;
      const headers = new Headers(res.headers);
      headers.set('Cross-Origin-Opener-Policy', 'same-origin');
      headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
      if (!headers.has('Cross-Origin-Resource-Policy')) {
        headers.set('Cross-Origin-Resource-Policy', 'same-origin');
      }
      return new Response(res.body, {
        status: res.status,
        statusText: res.statusText,
        headers,
      });
    })());
  });
}
