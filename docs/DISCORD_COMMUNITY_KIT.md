# SWBF2: Browser Edition — Discord Community Launch Kit

Inspired by the *Empire at War: Browser Edition* community layout, this kit gives you a ready-to-paste server structure, role hierarchy, and channel templates designed for a **Bring-Your-Own-Copy (BYOC)** open-source browser community.

> **One-Command Automated Setup:** Instead of creating every category, channel, role, and pinned message manually, run `tools/setup_discord_server.mjs` against a blank Discord server:
> ```sh
> DISCORD_BOT_TOKEN="<your_temp_bot_token>" DISCORD_GUILD_ID="<your_server_id>" \
>   node tools/setup_discord_server.mjs --clean --wire-site
> ```
> It creates all 5 categories, 16 channels + 4 voice rooms, 8 colored roles, read-only permissions, pinned `#rules`/`#about`/`#how-to-play`/`#server-information`/`#announcements` embeds, generates a permanent `discord.gg/...` invite link, and automatically writes it into `browser/site_config.mjs`!
>
> **Manual link wiring:** Or paste your invite link directly into `browser/site_config.mjs`:
> ```js
> discordUrl: 'https://discord.gg/WrBnZJQNtJ',
> githubUrl: 'https://github.com/adesas/swbf2o',
> ```
> Every Discord & GitHub button across `/` (`index.html`) and `/play` (`browser/play.html`) will automatically appear!

---

## 1. Recommended Server Structure

### Category: `MOS EISLEY` (Read-Only Onboarding)
- `#👋-landing-pad` — Welcome messages & member joins
- `#📜-rules` — Community & anti-piracy rules (paste template below)

### Category: `GALACTIC COMMUNITY`
- `#📢-announcements` — Release notes, community match nights, updates (Read-Only)
- `#ℹ️-server-information` — Live links (`swbf2o.com`, GitHub, status) (Read-Only)
- `#🧑‍💻-about` — Project story, tech overview, and roadmap (paste template below)
- `#🎮-how-to-play` — Step-by-step BYOC setup for Steam/GOG/Disc + controls (paste template below)
- `#🎬-media` — Clips, screenshots, epic Hero Assault & Space battle moments
- `#💬-general-chat` — Main hangout
- `#☕-off-topic` — Everything else

### Category: `BATTLEFRONT II`
- `#⚔️-battlefront-chat` — Gameplay talk, tactics, favorite maps & eras
- `#🎯-multiplayer-lfg` — Drop `?join=...` lobby invite links to fill matches fast!
- `#🏆-events-and-tournaments` — Weekly XL Night, Hero Assault Fridays, Space Battles

### Category: `MODDING & TECHNICAL`
- `#🛠️-technical-support` — Browser/WebGPU troubleshooting, BYOC folder help, mobile touch help
- `#🐛-bug-reports` — Forum channel (`WebGPU / Graphics`, `Audio`, `Multiplayer`, `Controls / Mobile`, `BYOC Loader`)
- `#💻-dev-and-github` — GitHub PRs, WGSL shaders, WebAssembly/WebRTC engineering talk
- `#🧩-modding` — Addon maps, custom scripts, save profile sharing

### Category: `VOICE CHANNELS`
- `🔊 Cantina Lounge`
- `🔊 Squad Alpha (Republic / Rebels)`
- `🔊 Squad Bravo (CIS / Empire)`
- `🔊 Hero Assault Arena`

---

## 2. Recommended Roles

- **Developer** (Gold) — Project creator & core maintainers
- **Moderator** (Blue) — Community moderators
- **Contributor** (Cyan) — Merged GitHub PRs / bug hunters
- **Self-Assignable Faction Roles** (via Discord Onboarding):
  - `Galactic Republic` · `CIS` · `Rebel Alliance` · `Galactic Empire`

---

## 3. Copy-Paste Channel Templates

### `#🧑‍💻-about`

```markdown
**Welcome to Star Wars Battlefront II: Browser Edition (SWBF2O)!**

This is a community-driven browser port that brings the classic 2005 *Star Wars Battlefront II* natively into modern web browsers using **WebAssembly**, **WebGPU (Direct3D 9 SM 1.1 → WGSL)**, **48 kHz 3D WebAudio**, and **WebRTC Peer-to-Peer Multiplayer**.

**How does it work? (Bring Your Own Copy)**
We do **not** host or distribute any copyrighted game files. Instead, you point your browser directly at your own legal copy of *Star Wars Battlefront II (Classic, 2005)* from Steam, GOG, or original Disc. Your game files stay **100% local on your machine** and are never uploaded — the browser simply reads them straight from your disk on demand!

**What is currently playable:**
- All 24 classic ground & space maps (Conquest, Hero Assault, Space Assault, CTF, Hunt, XL Battles)
- Single Player: 501st Campaign, Galactic Conquest, Instant Action & persistent save Export/Import
- Online Multiplayer: up to 64-player WebRTC lobbies, custom multi-map playlists, passwords, bot/hero rules, hot-join, text chat (`T`/`Y`), and built-in voice chat (`V` Push-to-Talk)
- Desktop (Keyboard + Mouse + Gamepad) & Mobile/Tablet (Android & iOS touch controls)
- Open-source browser port & multiplayer portal on GitHub!

**Links:**
- Play Online: https://swbf2o.com
- GitHub: https://github.com/adesas/swbf2o
```

---

### `#🎮-how-to-play`

```markdown
**How to Play in 60 Seconds (Bring Your Own Copy)**

**Step 1 — Own a legal copy of Star Wars Battlefront II (Classic, 2005)**
Available on Steam or GOG (or use your original 2005 PC CD/DVD install).

**Step 2 — Open the Portal**
Visit **https://swbf2o.com/play** in **Chrome, Edge, Brave, or Opera** (WebGPU enabled).

**Step 3 — Select your `GameData` folder**
Click **Select GameData Folder…** and pick your local install folder (the main game folder, `GameData`, or `GameData/data/_lvl_pc` all work automatically):
- **Steam (Windows):** `C:\Program Files (x86)\Steam\steamapps\common\Star Wars Battlefront II Classic\GameData`
- **Steam (Linux / Steam Deck):** `~/.local/share/Steam/steamapps/common/Star Wars Battlefront II Classic/GameData`
- **GOG Galaxy:** `C:\Program Files (x86)\GOG Galaxy\Games\Star Wars Battlefront II\GameData`

*On return visits, just click **Reconnect GameData** (1 click — nothing is ever copied or uploaded).*

**Step 4 — Deploy!**
Pick any username (no account needed) and click **Play Online** or **Single Player**.

**Quick Controls:**
- `W A S D` Move · `Mouse` Aim/Fire · `Space / Shift / C` Jump/Sprint/Crouch · `E` Enter Vehicle
- `F1` Accept Hero · `Esc` Pause / Respawn (stays in fullscreen!)
- `T` All Chat · `Y` Team Chat · `V` Push-to-Talk Voice · `Tab` Scoreboard · `F8` Toggle FPS/Net HUD
```

---

### `#📜-rules`

```markdown
**Server Rules**

1. **No Piracy or Sharing Game Files (Zero Tolerance):** Do not ask for, link to, upload, or distribute copyrighted SWBF2 game files, ISOs, or cracks. This project strictly requires your own legal copy of *Star Wars Battlefront II (2005)* (Steam, GOG, or Disc).
2. **Be Respectful:** No harassment, hate speech, slurs, doxxing, or personal attacks. Keep banter friendly.
3. **Play Fair in Multiplayer:** No cheating, exploiting, griefing, or intentionally crashing lobbies.
4. **Keep Channels On-Topic:** Post lobby invite links in `#multiplayer-lfg`, bugs in `#bug-reports`, and setup help in `#technical-support`.
5. **No Spam or Unsolicited Self-Promotion.**
6. **Follow Discord's Terms of Service & Community Guidelines.**
```
