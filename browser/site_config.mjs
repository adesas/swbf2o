// Central public web publishing configuration for SWBF2O (swbf2o.com).
// Fill in discordUrl and githubUrl below to automatically show Discord & GitHub
// buttons across both the landing page (/) and game portal (/play).

export const SITE_CONFIG = {
  siteName: 'SWBF2O — Battlefront II Online',
  domain: 'https://swbf2o.com',
  tagline: 'Classic 2005 Star Wars Battlefront II — playable natively in your browser with your own game files, WebGPU & WebRTC multiplayer.',

  // Bring-Your-Own-Copy (BYOC) mode: when true, players load their own local Steam/GOG/CD
  // SWBF2 GameData folder directly in the browser unless the server explicitly enables
  // --serve-gamedata for private/local dev self-hosting.
  byocDefault: true,

  // Community links (leave empty string '' to hide until ready).
  discordUrl: 'https://discord.gg/WrBnZJQNtJ',
  githubUrl: 'https://github.com/adesas/swbf2o',

  // Public WebRTC ICE (STUN/TURN) servers for Internet P2P connections across NATs.
  // If P2P is blocked by a strict symmetric NAT, BrowserNetClient automatically falls back
  // to the WebSocket binary UDP relay on swbf2o.com.
  iceServers: [
    { urls: 'stun:stun.cloudflare.com:3478' },
    { urls: 'stun:stun.l.google.com:19302' },
    // Optional TURN server (e.g. Cloudflare Realtime TURN or coturn) for strict NATs:
    // { urls: 'turn:turn.swbf2o.com:3478', username: 'swbf2', credential: 'secret' },
  ],
};
