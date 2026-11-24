#!/usr/bin/env node
// Automated Discord Community Builder for SWBF2: Browser Edition (SWBF2O)
// Uses only the official Discord REST API v10 (zero external npm dependencies).
//
// Usage:
//   DISCORD_BOT_TOKEN="your_bot_token" DISCORD_GUILD_ID="your_server_id" \
//     node tools/setup_discord_server.mjs [--clean] [--wire-site]
//
// Flags:
//   --clean      Remove Discord's default starter channels (#general, General voice, Text/Voice Channels)
//   --wire-site  Automatically write the generated permanent discord.gg invite into browser/site_config.mjs

import fs from 'node:fs';
import path from 'node:path';

const API = 'https://discord.com/api/v10';
const TOKEN = (process.env.DISCORD_BOT_TOKEN || '').trim();
const GUILD_ID = (process.env.DISCORD_GUILD_ID || '').trim();
const CLEAN_DEFAULTS = process.argv.includes('--clean');
const WIRE_SITE = process.argv.includes('--wire-site');

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');

if (!TOKEN || !GUILD_ID) {
  console.error(
    'Usage:\n' +
    '  DISCORD_BOT_TOKEN="<bot_token>" DISCORD_GUILD_ID="<server_id>" \\\n' +
    '    node tools/setup_discord_server.mjs [--clean] [--wire-site]\n\n' +
    'Both DISCORD_BOT_TOKEN and DISCORD_GUILD_ID are required.'
  );
  process.exit(1);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function discordApi(method, endpoint, body = null, attempt = 0) {
  const headers = {
    Authorization: `Bot ${TOKEN}`,
    'User-Agent': 'SWBF2O-Server-Setup (https://swbf2o.com, 1.0)',
  };
  const opts = { method, headers };
  if (body !== null) {
    headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(`${API}${endpoint}`, opts);
  if (res.status === 429 && attempt < 6) {
    const data = await res.json().catch(() => ({}));
    const waitMs = Math.ceil(((Number(data.retry_after) || 1.5) + 0.35) * 1000);
    console.log(`  [rate-limit] waiting ${waitMs}ms...`);
    await sleep(waitMs);
    return discordApi(method, endpoint, body, attempt + 1);
  }
  if (res.status === 204) return null;
  const text = await res.text();
  let json = null;
  try { json = text ? JSON.parse(text) : null; } catch {}
  if (!res.ok) {
    throw new Error(`Discord API ${method} ${endpoint} failed (HTTP ${res.status}): ${text}`);
  }
  return json;
}

// Permission bitfields (as BigInt strings per Discord API v10)
const PERM_VIEW_CHANNEL = 1n << 10n;
const PERM_SEND_MESSAGES = 1n << 11n;
const PERM_EMBED_LINKS = 1n << 14n;
const PERM_ATTACH_FILES = 1n << 15n;
const PERM_READ_HISTORY = 1n << 16n;
const PERM_ADD_REACTIONS = 1n << 6n;
const PERM_CREATE_PUBLIC_THREADS = 1n << 35n;
const PERM_CREATE_PRIVATE_THREADS = 1n << 36n;
const PERM_SEND_IN_THREADS = 1n << 38n;
const PERM_MANAGE_MESSAGES = 1n << 13n;
const PERM_KICK_MEMBERS = 1n << 1n;
const PERM_MODERATE_MEMBERS = 1n << 40n;
const PERM_ADMINISTRATOR = 1n << 3n;

const READONLY_DENY = (
  PERM_SEND_MESSAGES |
  PERM_CREATE_PUBLIC_THREADS |
  PERM_CREATE_PRIVATE_THREADS |
  PERM_SEND_IN_THREADS
).toString();

const READONLY_ALLOW_EVERYONE = (
  PERM_VIEW_CHANNEL |
  PERM_READ_HISTORY |
  PERM_ADD_REACTIONS
).toString();

const READONLY_ALLOW_STAFF = (
  PERM_VIEW_CHANNEL |
  PERM_SEND_MESSAGES |
  PERM_EMBED_LINKS |
  PERM_ATTACH_FILES |
  PERM_READ_HISTORY
).toString();

const ROLES = [
  { name: 'Developer', color: 0xf5c451, hoist: true, permissions: PERM_ADMINISTRATOR.toString() },
  { name: 'Moderator', color: 0x4cc3ff, hoist: true, permissions: (PERM_MANAGE_MESSAGES | PERM_KICK_MEMBERS | PERM_MODERATE_MEMBERS).toString() },
  { name: 'Contributor', color: 0x38d379, hoist: true, permissions: '0' },
  { name: 'Galactic Republic', color: 0x3b82f6, hoist: false, permissions: '0' },
  { name: 'CIS', color: 0xa855f7, hoist: false, permissions: '0' },
  { name: 'Rebel Alliance', color: 0xef4444, hoist: false, permissions: '0' },
  { name: 'Galactic Empire', color: 0x94a3b8, hoist: false, permissions: '0' },
];

const BLUEPRINT = [
  {
    category: 'MOS EISLEY',
    channels: [
      { name: '👋-landing-pad', type: 0, readonly: true, topic: 'Welcome to Star Wars Battlefront II: Browser Edition (SWBF2O)!', systemWelcome: true },
      { name: '📜-rules', type: 0, readonly: true, topic: 'Community guidelines and Bring-Your-Own-Copy policy.', postKey: 'rules' },
    ],
  },
  {
    category: 'GALACTIC COMMUNITY',
    channels: [
      { name: '📢-announcements', type: 0, readonly: true, topic: 'Updates, patch notes, and community match nights.', postKey: 'announcements' },
      { name: 'ℹ️-server-information', type: 0, readonly: true, topic: 'Official links for swbf2o.com, GitHub, and server support.', postKey: 'serverInfo' },
      { name: '🧑‍💻-about', type: 0, readonly: true, topic: 'About Star Wars Battlefront II: Browser Edition & technical architecture.', postKey: 'about' },
      { name: '🎮-how-to-play', type: 0, readonly: true, topic: 'Step-by-step Bring-Your-Own-Copy setup guide and controls.', postKey: 'howToPlay' },
      { name: '🎬-media', type: 0, readonly: false, topic: 'Share gameplay clips, screenshots, Hero Assault moments, and space battles!' },
      { name: '💬-general-chat', type: 0, readonly: false, topic: 'Hang out and chat with the SWBF2O community.' },
      { name: '☕-off-topic', type: 0, readonly: false, topic: 'Star Wars lore, other games, memes, and everything else.' },
    ],
  },
  {
    category: 'BATTLEFRONT II',
    channels: [
      { name: '⚔️-battlefront-chat', type: 0, readonly: false, topic: 'Tactics, favorite maps, classes, heroes, and Galactic Conquest.' },
      { name: '🎯-multiplayer-lfg', type: 0, readonly: false, topic: 'Drop your swbf2o.com/play?join=... invite links here to fill lobbies!' },
      { name: '🏆-events-and-tournaments', type: 0, readonly: false, topic: 'Weekly XL Nights, Hero Assault Fridays, and Space Assault events.' },
    ],
  },
  {
    category: 'MODDING & TECHNICAL',
    channels: [
      { name: '🛠️-technical-support', type: 0, readonly: false, topic: 'Browser/WebGPU help, local GameData folder setup, audio, and mobile touch support.' },
      { name: '🐛-bug-reports', type: 0, readonly: false, topic: 'Report bugs with your browser version, map name, and reproduction steps.' },
      { name: '💻-dev-and-github', type: 0, readonly: false, topic: 'Open-source development, GitHub PRs, WebGPU/WGSL shaders, and WebRTC networking.' },
      { name: '🧩-modding', type: 0, readonly: false, topic: 'Custom maps, scripts, and save profile sharing.' },
    ],
  },
  {
    category: 'VOICE CHANNELS',
    channels: [
      { name: '🔊 Cantina Lounge', type: 2 },
      { name: '🔊 Squad Alpha (Republic / Rebels)', type: 2 },
      { name: '🔊 Squad Bravo (CIS / Empire)', type: 2 },
      { name: '🔊 Hero Assault Arena', type: 2 },
    ],
  },
];

const POSTS = {
  rules: {
    embeds: [
      {
        title: '📜 SWBF2: Browser Edition — Server Rules',
        color: 0xf5c451,
        description:
          'Welcome to the official **Star Wars Battlefront II: Browser Edition (SWBF2O)** community! Please follow these rules so everyone has a great time:',
        fields: [
          {
            name: '1. No Piracy or Sharing Game Files (Zero Tolerance)',
            value:
              'Do **not** ask for, link to, upload, or distribute copyrighted SWBF2 game files, ISOs, or cracks. This project is **Bring-Your-Own-Copy** and requires your own legal copy of *Star Wars Battlefront II (2005)* (Steam, GOG, or retail Disc).',
          },
          {
            name: '2. Be Respectful',
            value: 'No harassment, hate speech, slurs, doxxing, or personal attacks. Keep trash talk friendly.',
          },
          {
            name: '3. Play Fair in Multiplayer',
            value: 'No cheating, exploiting, griefing, or intentionally disrupting lobbies.',
          },
          {
            name: '4. Keep Channels On-Topic',
            value: 'Post lobby invite links in `#🎯-multiplayer-lfg`, bugs in `#🐛-bug-reports`, and setup help in `#🛠️-technical-support`.',
          },
          {
            name: '5. Follow Discord ToS',
            value: 'Standard Discord Terms of Service and Community Guidelines apply everywhere.',
          },
        ],
        footer: { text: 'SWBF2O Community · swbf2o.com' },
      },
    ],
  },
  about: {
    embeds: [
      {
        title: '🧑‍💻 About Star Wars Battlefront II: Browser Edition',
        color: 0x4cc3ff,
        description:
          '**SWBF2O** brings the classic 2005 *Star Wars Battlefront II* natively into modern web browsers using **WebAssembly**, **WebGPU (Direct3D 9 SM 1.1 → WGSL)**, **48 kHz 3D WebAudio**, and **WebRTC Peer-to-Peer Multiplayer**.\n\n' +
          'This is a **Bring-Your-Own-Copy** browser engine. You point the browser at your own legal Steam, GOG, or Disc `GameData` folder — your files stay **100% local on your device** and are never uploaded.',
        fields: [
          {
            name: 'What Is Currently Playable',
            value:
              '• **All 24 Classic Ground & Space Maps** (Conquest, Hero Assault, Space Assault, CTF, Hunt, XL Battles)\n' +
              '• **Single Player:** 501st Campaign, Galactic Conquest, Instant Action & Save Export/Import\n' +
              '• **Online Multiplayer:** Up to 64-player WebRTC lobbies, custom playlists, passwords, bot/hero rules, hot-join, text chat (`T`/`Y`), and voice chat (`V`)\n' +
              '• **Controls:** Keyboard & Mouse, Gamepad, plus Android & iOS touch controls\n' +
              '• **Open Source:** Browser port, WebGPU renderer & multiplayer portal on GitHub',
          },
          {
            name: 'Official Links',
            value:
              '• **Play Online:** https://swbf2o.com\n' +
              '• **GitHub:** https://github.com/adesas/swbf2o',
          },
        ],
        footer: { text: 'Independent non-commercial community project · Not affiliated with Lucasfilm/Disney' },
      },
    ],
  },
  howToPlay: {
    embeds: [
      {
        title: '🎮 How to Play in 60 Seconds (Bring Your Own Copy)',
        color: 0x38d379,
        fields: [
          {
            name: 'Step 1 — Own a Legal Copy of SWBF2 (Classic, 2005)',
            value: 'Available on Steam, GOG, or from your original 2005 PC CD/DVD install.',
          },
          {
            name: 'Step 2 — Open the Portal',
            value: 'Visit **https://swbf2o.com/play** in **Chrome, Edge, Brave, or Opera** (WebGPU enabled).',
          },
          {
            name: 'Step 3 — Select Your `GameData` Folder',
            value:
              'Click **Select GameData Folder…** and pick your game root, `GameData`, or `GameData/data/_lvl_pc`:\n' +
              '• **Steam (Windows):** `C:\\Program Files (x86)\\Steam\\steamapps\\common\\Star Wars Battlefront II Classic\\GameData`\n' +
              '• **Steam (Linux / Steam Deck):** `~/.local/share/Steam/steamapps/common/Star Wars Battlefront II Classic/GameData`\n' +
              '• **GOG Galaxy:** `C:\\Program Files (x86)\\GOG Galaxy\\Games\\Star Wars Battlefront II\\GameData`\n' +
              '*Your files never leave your device. On return visits, click **Reconnect GameData** (1 click).*',
          },
          {
            name: 'Step 4 — Deploy & Controls',
            value:
              'Pick any username (no registration required) and click **Play Online** or **Single Player**!\n' +
              '• `W A S D` Move · `Mouse` Aim/Fire · `Space / Shift / C` Jump/Sprint/Crouch · `E` Enter Vehicle\n' +
              '• `F1` Accept Hero · `Esc` Pause/Respawn (stays in fullscreen) · `Tab` Scoreboard\n' +
              '• `T` All Chat · `Y` Team Chat · `V` Push-to-Talk Voice · `F8` FPS/Net HUD',
          },
        ],
      },
    ],
  },
  serverInfo: {
    embeds: [
      {
        title: 'ℹ️ Server Information & Official Links',
        color: 0xf5c451,
        description:
          'Bookmark these official links for **Star Wars Battlefront II: Browser Edition (SWBF2O)**:\n\n' +
          '🌐 **Website & Portal:** https://swbf2o.com\n' +
          '🕹️ **Play Now:** https://swbf2o.com/play\n' +
          '💻 **GitHub Repository:** https://github.com/adesas/swbf2o\n\n' +
          'Looking for teammates? Drop your lobby `?join=...` link in `#🎯-multiplayer-lfg`!',
      },
    ],
  },
  announcements: {
    embeds: [
      {
        title: '📢 Welcome to the Official SWBF2: Browser Edition Discord!',
        color: 0xf5c451,
        description:
          'The community Discord and open-source GitHub repository are officially live!\n\n' +
          '• **Play in your browser:** https://swbf2o.com/play\n' +
          '• **Explore & contribute on GitHub:** https://github.com/adesas/swbf2o\n' +
          '• **Need help getting set up?** Check `#🎮-how-to-play` or ask in `#🛠️-technical-support`.\n\n' +
          'See you on the battlefront, troopers!',
      },
    ],
  },
};

function normName(s) {
  return String(s || '').trim().toLowerCase();
}

async function main() {
  const guild = await discordApi('GET', `/guilds/${GUILD_ID}`);
  console.log(`Connected to guild: "${guild.name}" (${guild.id})`);

  // 1. Fetch & create missing roles
  const existingRoles = await discordApi('GET', `/guilds/${GUILD_ID}/roles`);
  const roleByName = new Map(existingRoles.map((r) => [normName(r.name), r]));

  for (const spec of ROLES) {
    if (roleByName.has(normName(spec.name))) {
      console.log(`  [role] exists: ${spec.name}`);
      continue;
    }
    const created = await discordApi('POST', `/guilds/${GUILD_ID}/roles`, spec);
    roleByName.set(normName(spec.name), created);
    console.log(`  [role] created: ${spec.name}`);
    await sleep(300);
  }

  const devRoleId = roleByName.get('developer')?.id;
  const modRoleId = roleByName.get('moderator')?.id;

  const makeReadonlyOverwrites = () => {
    const ow = [
      { id: GUILD_ID, type: 0, allow: READONLY_ALLOW_EVERYONE, deny: READONLY_DENY },
    ];
    if (devRoleId) ow.push({ id: devRoleId, type: 0, allow: READONLY_ALLOW_STAFF, deny: '0' });
    if (modRoleId) ow.push({ id: modRoleId, type: 0, allow: READONLY_ALLOW_STAFF, deny: '0' });
    return ow;
  };

  // 2. Fetch existing channels (optionally clean Discord's starter defaults)
  let existingChannels = await discordApi('GET', `/guilds/${GUILD_ID}/channels`);
  if (CLEAN_DEFAULTS) {
    const starterNames = new Set(['general', 'text channels', 'voice channels']);
    for (const ch of existingChannels) {
      if (starterNames.has(normName(ch.name))) {
        console.log(`  [clean] deleting starter channel: ${ch.name}`);
        await discordApi('DELETE', `/channels/${ch.id}`);
        await sleep(300);
      }
    }
    existingChannels = await discordApi('GET', `/guilds/${GUILD_ID}/channels`);
  }

  const catByName = new Map(
    existingChannels.filter((c) => c.type === 4).map((c) => [normName(c.name), c])
  );
  const chanByKey = new Map(
    existingChannels.filter((c) => c.type !== 4).map((c) => [`${c.parent_id || ''}:${normName(c.name)}`, c])
  );

  let welcomeChannelId = null;
  let posCat = 0;

  for (const group of BLUEPRINT) {
    let cat = catByName.get(normName(group.category));
    if (!cat) {
      cat = await discordApi('POST', `/guilds/${GUILD_ID}/channels`, {
        name: group.category,
        type: 4,
        position: posCat,
      });
      catByName.set(normName(group.category), cat);
      console.log(`  [category] created: ${group.category}`);
      await sleep(300);
    } else {
      console.log(`  [category] exists: ${group.category}`);
    }
    posCat++;

    let posCh = 0;
    for (const chSpec of group.channels) {
      const key = `${cat.id}:${normName(chSpec.name)}`;
      let ch = chanByKey.get(key) || existingChannels.find((c) => c.type === chSpec.type && normName(c.name) === normName(chSpec.name));
      if (!ch) {
        const payload = {
          name: chSpec.name,
          type: chSpec.type,
          parent_id: cat.id,
          position: posCh,
        };
        if (chSpec.topic) payload.topic = chSpec.topic;
        if (chSpec.readonly) payload.permission_overwrites = makeReadonlyOverwrites();
        ch = await discordApi('POST', `/guilds/${GUILD_ID}/channels`, payload);
        chanByKey.set(key, ch);
        console.log(`    [channel] created: #${ch.name}`);
        await sleep(350);
      } else {
        console.log(`    [channel] exists: #${ch.name}`);
      }
      posCh++;

      if (chSpec.systemWelcome) {
        welcomeChannelId = ch.id;
      }

      // Post & pin welcome content only if the channel has no messages yet (idempotent!)
      if (chSpec.postKey && POSTS[chSpec.postKey]) {
        const recent = await discordApi('GET', `/channels/${ch.id}/messages?limit=5`);
        if (!recent || recent.length === 0) {
          const msg = await discordApi('POST', `/channels/${ch.id}/messages`, POSTS[chSpec.postKey]);
          await discordApi('PUT', `/channels/${ch.id}/pins/${msg.id}`).catch(() => {});
          console.log(`      [post] posted & pinned ${chSpec.postKey} in #${ch.name}`);
          await sleep(400);
        } else {
          console.log(`      [post] skipped #${ch.name} (already has messages)`);
        }
      }
    }
  }

  // 3. Point Discord's system welcome messages at #👋-landing-pad
  if (welcomeChannelId) {
    await discordApi('PATCH', `/guilds/${GUILD_ID}`, { system_channel_id: welcomeChannelId }).catch(() => {});
  }

  // 4. Generate a permanent never-expiring invite link
  let inviteUrl = '';
  if (welcomeChannelId) {
    const inv = await discordApi('POST', `/channels/${welcomeChannelId}/invites`, {
      max_age: 0,
      max_uses: 0,
      unique: false,
    });
    if (inv && inv.code) {
      inviteUrl = `https://discord.gg/${inv.code}`;
    }
  }

  console.log('\n========================================');
  console.log('DISCORD SERVER SETUP COMPLETE!');
  if (inviteUrl) {
    console.log(`Permanent Invite URL: ${inviteUrl}`);
  }
  console.log('========================================\n');

  if (WIRE_SITE && inviteUrl) {
    const cfgPath = path.join(ROOT, 'browser', 'site_config.mjs');
    const src = fs.readFileSync(cfgPath, 'utf8');
    const updated = src.replace(/discordUrl:\s*'[^']*'/, `discordUrl: '${inviteUrl}'`);
    if (updated !== src) {
      fs.writeFileSync(cfgPath, updated, 'utf8');
      console.log(`Updated ${cfgPath} with discordUrl: '${inviteUrl}'`);
    }
  } else if (inviteUrl) {
    console.log(`To wire this invite into swbf2o.com, paste into browser/site_config.mjs:\n  discordUrl: '${inviteUrl}',`);
  }
}

main().catch((err) => {
  console.error('\nSetup failed:', err.message || err);
  process.exit(1);
});
