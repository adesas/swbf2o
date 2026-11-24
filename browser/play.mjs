// Production Web Portal Launcher for Star Wars Battlefront II (browser/play.html).
// Clean web.nfsmwo.com-style landing page with no-registration username selection,
// Play Online multiplayer lobby browser across all 24 retail maps, full custom lobby creator,
// live multiplayer staging lobby with chat & hot-join, and Single Player launch.

import { InputBridgeWriter, createInputBuffer } from './input_bridge.mjs';
import { TouchControls, detectTouchDevice } from './touch_controls.mjs';
import { WebAudioSabSink, AUDIO_SAB_DEFAULT_FRAMES } from './webaudio.mjs';
import { BrowserNetClient } from './net_client.mjs';
import { SITE_CONFIG } from './site_config.mjs';
import {
  supportsDirectoryPicker,
  pickLocalGameDataDirectory,
  scanDirectoryHandle,
  scanFileList,
  checkSavedDirectoryState,
  reconnectSavedDirectory,
  buildByocIndex,
} from './byoc_store.mjs';
import {
  ALL_SCRIPTS,
  PUBLIC_LOBBY_PRESETS,
  getScriptInfo,
  getScriptLabel,
} from './maps_catalog.mjs';

const $ = (id) => document.getElementById(id);
const canvas = $('gameCanvas');
const launcher = $('launcher');
const launcherCard = $('launcherCard');
const mainView = $('mainView');
const lobbyView = $('lobbyView');
const createLobbyView = $('createLobbyView');
const mpLobbyOverlay = $('mpLobbyOverlay');
const errMsg = $('errMsg');
const drawer = $('drawer');
const hudBar = $('hudBar');
const lockHint = $('lockHint');
const loadingOverlay = $('loadingOverlay');
const loadBannerOverlay = $('loadBannerOverlay');
const loadBannerCanvas = $('loadBannerCanvas');
const streamToast = $('streamToast');

const params = new URLSearchParams(location.search);
const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
const DEFAULT_MP_SERVER = params.get('mp') || `${wsProto}//${location.host || '127.0.0.1:8123'}`;
const DEFAULT_MP_HTTP = params.get('mp')
  ? DEFAULT_MP_SERVER.replace(/^ws:/i, 'http:').replace(/^wss:/i, 'https:')
  : location.origin;

// WebGPU, SharedArrayBuffer (COOP/COEP), and OPFS require a Secure Context (HTTPS or localhost).
// If a LAN device visits plain http://<LAN-IP>:8123, automatically redirect it to https://<LAN-IP>:8443.
if (!window.isSecureContext && location.protocol === 'http:' &&
    location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') {
  const tlsPort = params.get('tlsPort') || '8443';
  const httpsUrl = `https://${location.hostname}:${tlsPort}${location.pathname}${location.search}`;
  if (errMsg) {
    errMsg.style.display = 'block';
    errMsg.innerHTML = `LAN browsers require HTTPS for WebGPU &amp; Multiplayer. Redirecting to <a href="${httpsUrl}" style="color:var(--gold);text-decoration:underline">${httpsUrl}</a>…`;
  }
  location.replace(httpsUrl);
}

let worker = null;
let audioSink = null;
let inputWriter = null;
let touchControls = null;
let netClient = null;
let muted = false;
let booted = false;
let booting = false;
let reqId = 1;
const pending = new Map();
let lastFrameCount = 0;
let lastFpsMs = performance.now();
let currentFps = 0;
let lastStatus = null;
let lastInviteUrl = '';
let activeLobbyFilter = 'all';
let lobbyRefreshTimer = 0;
let customPlaylist = ['tat2g_con'];
let activeMpSession = null; // { role, room, title, playlist, config, autoStart, matchLaunched }
let voiceMode = 'off'; // 'off' | 'ptt' | 'open'

// ---- Bring-Your-Own-Copy (BYOC) Local GameData State ----
let serverGamedataHosted = false;
let serverByocPrompt = true;
let forceByocMode = params.get('byoc') === '1' || params.get('byoc') === 'true';
let byocIndex = null; // { valid, rootName, fileCount, totalBytes, mapCount, missingCore, files }
let byocPickError = '';
let savedByocHandle = null;

function isByocRequired() {
  return forceByocMode || serverByocPrompt || !serverGamedataHosted;
}

function fmtGb(bytes) {
  const b = Math.max(0, Number(bytes) || 0);
  if (b >= 1073741824) return `${(b / 1073741824).toFixed(2)} GB`;
  if (b >= 1048576) return `${(b / 1048576).toFixed(1)} MB`;
  return `${Math.round(b / 1024)} KB`;
}

function renderByocCard() {
  const card = $('byocCard');
  if (!card) return;
  const needByoc = isByocRequired();
  card.classList.toggle('visible', needByoc || !!byocIndex);
  card.classList.remove('attention');

  const titleEl = $('byocTitle');
  const badgeEl = $('byocBadge');
  const statusEl = $('byocStatus');
  const btnReconnect = $('btnByocReconnect');
  const btnPick = $('btnByocPick');
  const btnFallback = $('btnByocFallback');

  if (byocIndex && byocIndex.valid) {
    card.classList.add('ready');
    titleEl.textContent = '✓ Local GameData Ready';
    badgeEl.textContent = '100% Local';
    badgeEl.className = 'tag live';
    statusEl.innerHTML =
      `Mounted <strong>${byocIndex.rootName}</strong> · <strong>${byocIndex.fileCount}</strong> files · ` +
      `<strong>${byocIndex.mapCount}</strong> retail maps (${fmtGb(byocIndex.totalBytes)}). Reading directly from disk.`;
    btnReconnect.style.display = 'none';
    btnPick.textContent = 'Change Folder…';
    btnFallback.style.display = 'none';
    return;
  }

  card.classList.remove('ready');
  if (byocPickError) {
    titleEl.textContent = 'Folder Access Blocked';
    badgeEl.textContent = 'Use Compatibility Mode';
    badgeEl.className = 'tag warn';
    statusEl.innerHTML = byocPickError;
  } else if (byocIndex && !byocIndex.valid) {
    titleEl.textContent = byocIndex.detectedInstaller
      ? 'Uninstalled GOG Installer Detected'
      : byocIndex.detectedArchive
        ? 'Compressed Archive Detected'
        : 'Missing Required Game Files';
    badgeEl.textContent = 'Incomplete Folder';
    badgeEl.className = 'tag warn';
    if (byocIndex.detectedInstaller) {
      statusEl.innerHTML =
        `Found an uninstalled setup package in <strong>${byocIndex.rootName}</strong> (<code>setup_*.exe</code> / <code>.bin</code>). ` +
        `Run the installer first (or extract with <code>innoextract</code>), then select the installed <strong>GameData</strong> folder.`;
    } else if (byocIndex.detectedArchive) {
      statusEl.innerHTML =
        `Found a compressed archive (<code>.rar</code> / <code>.zip</code> / <code>.iso</code>) in <strong>${byocIndex.rootName}</strong>. ` +
        `Extract it first, then select the extracted <strong>GameData</strong> or <strong>_lvl_pc</strong> folder.`;
    } else {
      statusEl.innerHTML =
        `Could not find <code>${byocIndex.missingCore.join(', ')}</code> in <strong>${byocIndex.rootName}</strong> ` +
        `(scanned ${byocIndex.scannedEntries || 0} files). ` +
        `Select your SWBF2 <strong>GameData</strong> or <strong>GameData/data/_lvl_pc</strong> folder.`;
    }
  } else if (savedByocHandle) {
    titleEl.textContent = 'Reconnect Your GameData Folder';
    badgeEl.textContent = 'Saved Folder';
    badgeEl.className = 'tag gold';
    statusEl.innerHTML =
      `Previously selected <strong>${savedByocHandle.name || 'GameData'}</strong>. Click below to re-grant read-only access (files stay 100% local).`;
  } else {
    titleEl.textContent = 'Local Game Files Required';
    badgeEl.textContent = 'Bring Your Own Copy';
    badgeEl.className = 'tag gold';
    statusEl.innerHTML =
      `Select your legal <strong>Star Wars Battlefront II (2005)</strong> folder (Steam, GOG, or Disc). ` +
      `Files stay 100% on your device and are never uploaded.`;
  }

  btnReconnect.style.display = savedByocHandle ? 'inline-flex' : 'none';
  btnPick.textContent = savedByocHandle ? 'Choose Different Folder…' : 'Select GameData Folder…';
  if (btnFallback) {
    btnFallback.textContent = 'Compatibility Picker…';
    btnFallback.style.display = (byocPickError || (byocIndex && !byocIndex.valid)) ? 'inline-flex' : 'none';
  }
}

async function applyByocIndex(index) {
  byocPickError = '';
  byocIndex = index;
  renderByocCard();
  if (index && index.valid) {
    errMsg.style.display = 'none';
    if (worker) {
      await callWorker('setLocalGameData', { localFiles: index.files }).catch(() => {});
    }
    if (window.__playPreflight?.webgpu && window.__playPreflight?.coi) {
      $('readyText').textContent = 'Ready to play (local GameData)';
      $('readyDot').style.background = 'var(--ok)';
    }
  }
  return index;
}

async function initByocState() {
  try {
    const res = await fetch('/api/server-config', { cache: 'no-store' });
    if (res.ok) {
      const cfg = await res.json();
      serverGamedataHosted = !!cfg.gamedataHosted;
      serverByocPrompt = typeof cfg.byocPrompt === 'boolean' ? cfg.byocPrompt : !serverGamedataHosted;
    } else {
      serverGamedataHosted = !SITE_CONFIG.byocDefault;
      serverByocPrompt = !serverGamedataHosted;
    }
  } catch {
    serverGamedataHosted = !SITE_CONFIG.byocDefault;
    serverByocPrompt = !serverGamedataHosted;
  }

  if (isByocRequired()) {
    const saved = await checkSavedDirectoryState();
    if (saved.handle) {
      savedByocHandle = saved.handle;
      if (saved.status === 'granted') {
        try {
          const index = await scanDirectoryHandle(saved.handle);
          if (index.valid) {
            await applyByocIndex(index);
            return;
          }
        } catch {}
      }
    }
  }
  renderByocCard();
}

async function handlePickByocFolder() {
  const statusEl = $('byocStatus');
  byocPickError = '';
  if (supportsDirectoryPicker()) {
    try {
      if (statusEl) statusEl.textContent = 'Scanning selected GameData folder…';
      const { dirHandle, index } = await pickLocalGameDataDirectory({
        onScanProgress: (n) => {
          if (statusEl) statusEl.textContent = `Scanning local GameData… (${n} files found)`;
        },
      });
      if (index.valid) savedByocHandle = dirHandle;
      await applyByocIndex(index);
      return;
    } catch (err) {
      if (err && err.name === 'AbortError') {
        renderByocCard();
        return;
      }
      // Note: Do NOT call $('fileByocFolder').click() after an await — browsers block
      // programmatic file-picker opens once user activation expires. Instead, reveal
      // Compatibility Picker so the user gets a fresh click gesture.
      console.warn('[byoc] showDirectoryPicker error:', err);
      const msg = String(err?.message || err || '');
      const isProtected = err?.name === 'SecurityError' || /system files|not allowed|permission/i.test(msg);
      byocPickError = isProtected
        ? `Your browser blocked direct access to that folder (common for <code>C:\\Program Files</code> or a drive root). ` +
          `Click <strong>Compatibility Picker…</strong> below, or choose the <strong>GameData</strong> / <strong>_lvl_pc</strong> subfolder directly.`
        : `Could not open that folder directly (${msg || err?.name || 'error'}). Click <strong>Compatibility Picker…</strong> below to load it instead.`;
      renderByocCard();
      return;
    }
  }
  $('fileByocFolder')?.click();
}

async function handleReconnectByocFolder() {
  if (!savedByocHandle) return handlePickByocFolder();
  const statusEl = $('byocStatus');
  try {
    if (statusEl) statusEl.textContent = 'Reconnecting saved GameData folder…';
    const { index } = await reconnectSavedDirectory(savedByocHandle, {
      onScanProgress: (n) => {
        if (statusEl) statusEl.textContent = `Verifying local GameData… (${n} files found)`;
      },
    });
    await applyByocIndex(index);
  } catch (err) {
    console.warn('[byoc] reconnect failed:', err);
    if (err && err.name !== 'AbortError') {
      errMsg.textContent = String(err.message || err);
      errMsg.style.display = 'block';
    }
    renderByocCard();
  }
}

$('btnByocPick')?.addEventListener('click', handlePickByocFolder);
$('btnByocReconnect')?.addEventListener('click', handleReconnectByocFolder);
$('btnByocFallback')?.addEventListener('click', () => {
  byocPickError = '';
  $('fileByocFolder')?.click();
});
$('fileByocFolder')?.addEventListener('change', async (ev) => {
  const files = ev.target.files;
  if (!files || !files.length) return;
  const index = scanFileList(files);
  await applyByocIndex(index);
  ev.target.value = '';
});

function openByocHelpModal() {
  const modal = $('byocHelpModal');
  if (!modal) return;
  const lead = $('byocHelpLead');
  if (lead) {
    lead.textContent = supportsDirectoryPicker()
      ? 'Folder access is available in this browser. If the button still does nothing:'
      : 'Folder access is available in this browser via compatibility mode. If the button still does nothing:';
  }
  $('devUnlockBox')?.classList.remove('open');
  if ($('inpDevPassword')) $('inpDevPassword').value = '';
  if ($('devUnlockMsg')) $('devUnlockMsg').textContent = '';
  modal.classList.add('open');
}

function closeByocHelpModal() {
  $('byocHelpModal')?.classList.remove('open');
}

$('btnByocHelpClose')?.addEventListener('click', closeByocHelpModal);
$('btnByocHelpCloseX')?.addEventListener('click', closeByocHelpModal);
$('byocHelpModal')?.addEventListener('click', (ev) => {
  if (ev.target === $('byocHelpModal')) closeByocHelpModal();
});
window.addEventListener('keydown', (ev) => {
  if (ev.key === 'Escape' && $('byocHelpModal')?.classList.contains('open')) {
    closeByocHelpModal();
  }
});
$('btnByocCompatMode')?.addEventListener('click', (ev) => {
  ev.preventDefault();
  closeByocHelpModal();
  $('fileByocFolder')?.click();
});
$('btnDevToggle')?.addEventListener('click', (ev) => {
  ev.preventDefault();
  const box = $('devUnlockBox');
  if (!box) return;
  box.classList.toggle('open');
  if (box.classList.contains('open')) {
    setTimeout(() => $('inpDevPassword')?.focus(), 30);
  }
});
$('devUnlockBox')?.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const inp = $('inpDevPassword');
  const msg = $('devUnlockMsg');
  const btn = $('btnDevUnlock');
  const password = inp?.value || '';
  if (!password) {
    if (msg) msg.textContent = 'Enter developer password.';
    return;
  }
  if (btn) btn.disabled = true;
  if (msg) { msg.style.color = 'var(--dim)'; msg.textContent = 'Verifying…'; }
  try {
    const res = await fetch('/api/dev-unlock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'same-origin',
      body: JSON.stringify({ password }),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || !data.ok) {
      if (msg) {
        msg.style.color = 'var(--err)';
        msg.textContent = data.error || `Unlock failed (HTTP ${res.status})`;
      }
      if (inp) { inp.value = ''; inp.focus(); }
      return;
    }
    serverGamedataHosted = true;
    serverByocPrompt = false;
    closeByocHelpModal();
    errMsg.style.display = 'none';
    renderByocCard();
    if (window.__playPreflight?.webgpu && window.__playPreflight?.coi) {
      $('readyText').textContent = 'Ready to play (Dev session)';
      $('readyDot').style.background = 'var(--ok)';
    }
  } catch (err) {
    if (msg) {
      msg.style.color = 'var(--err)';
      msg.textContent = String(err.message || err);
    }
  } finally {
    if (btn) btn.disabled = false;
  }
});

// Wire optional community links from SITE_CONFIG.
function wireCommunityLinks() {
  if (SITE_CONFIG.discordUrl && $('portalDiscordLink')) {
    $('portalDiscordLink').href = SITE_CONFIG.discordUrl;
    $('portalDiscordLink').style.display = 'inline';
  }
  if (SITE_CONFIG.githubUrl && $('portalGithubLink')) {
    $('portalGithubLink').href = SITE_CONFIG.githubUrl;
    $('portalGithubLink').style.display = 'inline';
  }
}
wireCommunityLinks();

// ---- Audio Volume, Music Streaming Toggle & Automatic Lobby Ducking ----
const LOBBY_DUCK_FACTOR = 0.25; // Soft ambient lobby/menu music (~-12 dB vs in-match gameplay)
let masterVolume = 0.8;
let musicStreamingEnabled = false; // Disabled by default to save bandwidth; SFX remain enabled
try {
  const savedVol = localStorage.getItem('swbf2_volume');
  if (savedVol !== null) {
    const v = Number(savedVol);
    if (Number.isFinite(v) && v >= 0 && v <= 100) masterVolume = v / 100;
  }
  const savedMusic = localStorage.getItem('swbf2_music_streaming');
  if (savedMusic === '1' || savedMusic === 'true') musicStreamingEnabled = true;
} catch {}
let lastAppliedGain = -1;

function syncMusicCheckboxes() {
  if ($('chkMusicInit')) $('chkMusicInit').checked = musicStreamingEnabled;
  if ($('chkMusicDrawer')) $('chkMusicDrawer').checked = musicStreamingEnabled;
}

async function setMusicStreamingEnabled(enabled) {
  musicStreamingEnabled = !!enabled;
  syncMusicCheckboxes();
  try { localStorage.setItem('swbf2_music_streaming', musicStreamingEnabled ? '1' : '0'); } catch {}
  if (booted) {
    await callWorker('interactiveAction', { action: 'setMusicStreaming', enabled: musicStreamingEnabled }).catch(() => {});
  }
}

syncMusicCheckboxes();
$('chkMusicInit')?.addEventListener('change', (ev) => setMusicStreamingEnabled(ev.target.checked));
$('chkMusicDrawer')?.addEventListener('change', (ev) => setMusicStreamingEnabled(ev.target.checked));

function applyAudioGain(gameState = 'shell') {
  if (!audioSink) return;
  const inMenuOrLobby = gameState !== 'mission' || mpLobbyOverlay.classList.contains('open') || launcher.style.display !== 'none';
  const target = muted ? 0 : (masterVolume * (inMenuOrLobby ? LOBBY_DUCK_FACTOR : 1.0));
  if (Math.abs(target - lastAppliedGain) > 0.005) {
    lastAppliedGain = target;
    audioSink.setGain(target, 0.12);
  }
}

if ($('rngVolume') && $('lblVolume')) {
  const pct = Math.round(masterVolume * 100);
  $('rngVolume').value = String(pct);
  $('lblVolume').textContent = `${pct}%`;
  $('rngVolume').addEventListener('input', () => {
    const p = Math.max(0, Math.min(100, Number($('rngVolume').value) || 0));
    masterVolume = p / 100;
    $('lblVolume').textContent = `${p}%`;
    try { localStorage.setItem('swbf2_volume', String(p)); } catch {}
    lastAppliedGain = -1;
    applyAudioGain(lastStatus?.status?.state || 'shell');
  });
}

// ---- Real-Time Download & Map Loading Progress State ----
let lastLoadProgress = null;
let loadEventCount = 0;
let lastInteractiveStatusMs = 0;
let lastVfsHttpBytes = 0;
let mapBurstStartBytes = 0;
let mapLoadActive = false;
let mapLoadExplicit = false;
let mapLoadTargetLabel = '';
let mapPendingTimer = 0;
let mapLoadIdleTimer = 0;
let toastIdleTimer = 0;
let speedSamples = []; // [{ t, bytes }]
let totalStreamBytesMono = 0;
let lastWasmLoadedBytes = 0;

function fmtBytes(bytes) {
  const b = Math.max(0, Number(bytes) || 0);
  if (b >= 1048576) return `${(b / 1048576).toFixed(1)} MB`;
  if (b >= 1024) return `${Math.round(b / 1024)} KB`;
  return `${Math.round(b)} B`;
}

function recordSpeedBytes(deltaBytes) {
  const now = performance.now();
  if (deltaBytes > 0) {
    totalStreamBytesMono += deltaBytes;
    speedSamples.push({ t: now, bytes: totalStreamBytesMono });
  }
  const cutoff = now - 1600;
  while (speedSamples.length > 2 && speedSamples[0].t < cutoff) {
    speedSamples.shift();
  }
  if (speedSamples.length < 2) return 0;
  const first = speedSamples[0];
  const last = speedSamples[speedSamples.length - 1];
  const dt = (last.t - first.t) / 1000;
  if (dt < 0.08) return 0;
  return (last.bytes - first.bytes) / dt;
}

function fmtSpeed(bps) {
  if (!bps || bps < 1024) return '';
  if (bps >= 1048576) return `${(bps / 1048576).toFixed(1)} MB/s`;
  return `${Math.round(bps / 1024)} KB/s`;
}

function openLoadingOverlay({ kicker = 'DOWNLOADING GAME ASSETS', title = 'Loading Battlefront II', stage = 'Preparing engine…', pctText = '0%', barPct = 4, streaming = false, bytesText = 'Starting download…', speedText = '', fileText = '', tipText = '' } = {}) {
  $('loadKicker').textContent = kicker;
  $('loadTitle').textContent = title;
  $('loadStage').textContent = stage;
  $('loadPct').textContent = pctText;
  const fill = $('loadBarFill');
  fill.style.width = `${Math.max(3, Math.min(100, barPct))}%`;
  fill.classList.toggle('streaming', !!streaming);
  $('loadBytes').textContent = bytesText;
  $('loadSpeed').textContent = speedText;
  if (fileText) {
    $('loadFileRow').style.display = 'flex';
    $('loadFile').textContent = fileText;
  } else {
    $('loadFileRow').style.display = 'none';
  }
  if (tipText) $('loadTip').textContent = tipText;
  loadingOverlay.classList.add('open');
  streamToast.classList.remove('open');
}

function closeLoadingOverlay() {
  loadingOverlay.classList.remove('open');
  mapLoadActive = false;
  mapLoadExplicit = false;
  if (mapPendingTimer) { clearTimeout(mapPendingTimer); mapPendingTimer = 0; }
  if (mapLoadIdleTimer) { clearTimeout(mapLoadIdleTimer); mapLoadIdleTimer = 0; }
}

let lbLedTimer = 0;
let lbLedLitCount = 1;

function rgbaToDataUrl(rgba, width, height) {
  const c = document.createElement('canvas');
  c.width = width;
  c.height = height;
  const ctx2d = c.getContext('2d');
  if (!ctx2d) return '';
  const clamped = new Uint8ClampedArray(rgba.buffer, rgba.byteOffset, rgba.byteLength);
  ctx2d.putImageData(new ImageData(clamped, width, height), 0, 0);
  return c.toDataURL('image/png');
}

function applyLoadBannerAsset(msg) {
  if (!loadBannerOverlay || !msg.rgba || !msg.width || !msg.height) return;
  const url = rgbaToDataUrl(msg.rgba, msg.width, msg.height);
  if (!url) return;
  if (msg.kind === 'tipsBox') {
    loadBannerOverlay.style.setProperty('--lb-tips-box-img', `url("${url}")`);
    window.__lbHasTipsBoxTex = true;
  } else if (msg.kind === 'ledIcon') {
    loadBannerOverlay.style.setProperty('--lb-led-icon-img', `url("${url}")`);
    window.__lbHasLedIconTex = true;
  }
}

function setLoadBannerLeds(litCount) {
  const leds = $('lbLeds')?.children;
  if (!leds) return;
  const clamped = Math.max(1, Math.min(leds.length, litCount | 0));
  lbLedLitCount = clamped;
  for (let i = 0; i < leds.length; ++i) {
    leds[i].classList.toggle('lit', i < clamped);
  }
}

function showLoadBanner(msg) {
  if (!loadBannerOverlay || !loadBannerCanvas) return;
  if (msg.rgba && msg.width > 0 && msg.height > 0) {
    loadBannerCanvas.width = msg.width;
    loadBannerCanvas.height = msg.height;
    const ctx2d = loadBannerCanvas.getContext('2d');
    if (ctx2d) {
      const clamped = new Uint8ClampedArray(msg.rgba.buffer, msg.rgba.byteOffset, msg.rgba.byteLength);
      const imgData = new ImageData(clamped, msg.width, msg.height);
      ctx2d.putImageData(imgData, 0, 0);
    }
  }
  if ($('lbTitle')) $('lbTitle').textContent = (msg.title || mapLoadTargetLabel || 'Loading').trim();
  if ($('lbMode')) $('lbMode').textContent = (msg.mode || '').trim();
  const tipBox = $('lbTipBox');
  if (tipBox) {
    const tip = (msg.tip || '').trim();
    if (tip) {
      tipBox.textContent = tip;
      tipBox.style.display = 'block';
    } else {
      tipBox.style.display = 'none';
    }
  }
  if ($('lbStreamMeta')) $('lbStreamMeta').textContent = '';
  setLoadBannerLeds(1);
  if (lbLedTimer) clearInterval(lbLedTimer);
  // Advance red SWBF2 loading icons steadily left-to-right just like LoadDisplay::ProgressIndicator.
  lbLedTimer = setInterval(() => {
    if (!loadBannerOverlay.classList.contains('open')) {
      clearInterval(lbLedTimer);
      lbLedTimer = 0;
      return;
    }
    if (lbLedLitCount < 11) setLoadBannerLeds(lbLedLitCount + 1);
  }, 650);
  loadBannerOverlay.classList.add('open');
  streamToast.classList.remove('open');
  window.__lastLoadBanner = {
    open: true,
    width: msg.width || 0,
    height: msg.height || 0,
    title: msg.title || '',
    mode: msg.mode || '',
    hasTip: !!(msg.tip && msg.tip.trim()),
  };
}

function hideLoadBanner() {
  if (lbLedTimer) { clearInterval(lbLedTimer); lbLedTimer = 0; }
  if (!loadBannerOverlay) return;
  setLoadBannerLeds(12);
  loadBannerOverlay.classList.remove('open');
  if (window.__lastLoadBanner) window.__lastLoadBanner.open = false;
}

function showStreamToast(text, barPct = 40) {
  if (loadingOverlay.classList.contains('open')) return;
  if (loadBannerOverlay && loadBannerOverlay.classList.contains('open')) {
    if ($('lbStreamMeta')) $('lbStreamMeta').textContent = text;
    const targetLeds = Math.max(lbLedLitCount, Math.min(11, Math.round((barPct / 100) * 11) + 1));
    setLoadBannerLeds(targetLeds);
    return;
  }
  $('streamToastText').textContent = text;
  const fill = $('streamToastFill');
  if (fill) fill.style.width = `${Math.max(12, Math.min(100, barPct))}%`;
  streamToast.classList.add('open');
  if (toastIdleTimer) clearTimeout(toastIdleTimer);
  toastIdleTimer = setTimeout(() => {
    streamToast.classList.remove('open');
    toastIdleTimer = 0;
  }, 1500);
}

function handleLoadProgress(msg) {
  lastLoadProgress = msg;
  ++loadEventCount;
  const phase = msg.phase || '';

  if (phase === 'map_start') {
    // Keep SWBF2's original in-game planet loading banners unobstructed!
    // Reset burst stats so the compact bottom progress bar tracks this map's download cleanly.
    mapBurstStartBytes = lastVfsHttpBytes;
    mapLoadExplicit = true;
    mapLoadTargetLabel = msg.script ? getScriptLabel(msg.script) : (activeMpSession?.title || 'Battlefield');
    return;
  }

  if (booting) {
    if (phase === 'manifest') {
      openLoadingOverlay({
        kicker: 'INITIALIZING ENGINE',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Loading game asset catalog…',
        pctText: '5%',
        barPct: 5,
        bytesText: 'Opening local browser cache (OPFS)…',
        speedText: 'Step 1 / 5',
      });
      return;
    }
    if (phase === 'js') {
      const loaded = msg.loaded || 0;
      const total = msg.total || 0;
      const pct = total > 0 ? Math.min(10, 6 + Math.round((loaded / total) * 4)) : 8;
      openLoadingOverlay({
        kicker: 'DOWNLOADING ENGINE',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Downloading engine runtime…',
        pctText: `${pct}%`,
        barPct: pct,
        bytesText: total > 0 ? `${fmtBytes(loaded)} / ${fmtBytes(total)}` : (loaded > 0 ? `${fmtBytes(loaded)} downloaded` : 'Fetching runtime…'),
        speedText: 'Step 2 / 5',
        fileText: 'swbf2.js',
      });
      return;
    }
    if (phase === 'wasm') {
      const loaded = msg.loaded || 0;
      const total = msg.total || 0;
      const delta = Math.max(0, loaded - lastWasmLoadedBytes);
      lastWasmLoadedBytes = loaded;
      const bps = recordSpeedBytes(delta);
      const ratio = total > 0 ? Math.min(1, loaded / total) : 0;
      const barPct = total > 0 ? (10 + Math.round(ratio * 35)) : Math.min(44, 12 + Math.round(loaded / 200000));
      const pctLabel = total > 0 ? `${Math.round(ratio * 100)}%` : fmtBytes(loaded);
      openLoadingOverlay({
        kicker: 'DOWNLOADING ENGINE',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Downloading Battlefront II engine…',
        pctText: pctLabel,
        barPct,
        streaming: total === 0,
        bytesText: total > 0 ? `${fmtBytes(loaded)} / ${fmtBytes(total)}` : `${fmtBytes(loaded)} downloaded`,
        speedText: fmtSpeed(bps) || 'Step 2 / 5',
        fileText: 'swbf2.wasm',
      });
      return;
    }
    if (phase === 'compile') {
      openLoadingOverlay({
        kicker: 'INITIALIZING ENGINE',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Compiling WebAssembly engine…',
        pctText: '48%',
        barPct: 48,
        streaming: true,
        bytesText: msg.total ? `${fmtBytes(msg.total)} engine ready` : 'Instantiating engine…',
        speedText: 'Step 3 / 5',
        fileText: 'swbf2.wasm',
      });
      return;
    }
    if (phase === 'webgpu') {
      openLoadingOverlay({
        kicker: 'INITIALIZING GRAPHICS',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Initializing WebGPU graphics…',
        pctText: '54%',
        barPct: 54,
        streaming: true,
        bytesText: 'Setting up shaders & GPU pipelines…',
        speedText: 'Step 3 / 5',
      });
      return;
    }
    if (phase === 'boot') {
      openLoadingOverlay({
        kicker: 'DOWNLOADING GAME ASSETS',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Streaming game shell & menus…',
        pctText: '58%',
        barPct: 58,
        streaming: true,
        bytesText: 'Checking local cache & streaming core assets…',
        speedText: 'Step 4 / 5',
      });
      return;
    }
    if (phase === 'gamedata') {
      const totalHttp = msg.totalHttpBytes || 0;
      const delta = Math.max(0, totalHttp - lastVfsHttpBytes);
      lastVfsHttpBytes = totalHttp;
      const bps = recordSpeedBytes(delta);
      const barPct = 58 + Math.round(35 * (1 - Math.exp(-totalHttp / (16 * 1048576))));
      const cached = msg.cachedBytes || 0;
      const fileDetail = msg.fileSize
        ? `${msg.file} (${fmtBytes(msg.fileHttpBytes || 0)} / ${fmtBytes(msg.fileSize)})`
        : (msg.file || '');
      openLoadingOverlay({
        kicker: 'DOWNLOADING GAME ASSETS',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: `Downloading ${msg.file || 'game data'}…`,
        pctText: totalHttp > 0 ? fmtBytes(totalHttp) : 'Streaming',
        barPct,
        streaming: true,
        bytesText: `${fmtBytes(totalHttp)} downloaded${cached > 0 ? ` · ${fmtBytes(cached)} cached` : ''}`,
        speedText: fmtSpeed(bps) || 'Step 4 / 5',
        fileText: fileDetail,
      });
      return;
    }
    if (phase === 'login') {
      openLoadingOverlay({
        kicker: 'ALMOST READY',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: msg.label || 'Signing into player profile…',
        pctText: '95%',
        barPct: 95,
        streaming: true,
        bytesText: lastVfsHttpBytes > 0 ? `${fmtBytes(lastVfsHttpBytes)} downloaded` : 'Loaded from local cache',
        speedText: 'Step 5 / 5',
      });
      return;
    }
    if (phase === 'ready') {
      openLoadingOverlay({
        kicker: 'READY',
        title: $('loadTitle').textContent || 'Loading Battlefront II',
        stage: 'Ready!',
        pctText: '100%',
        barPct: 100,
        streaming: false,
        bytesText: lastVfsHttpBytes > 0 ? `${fmtBytes(lastVfsHttpBytes)} downloaded` : 'Loaded from local cache',
        speedText: 'Complete',
      });
      return;
    }
    return;
  }

  // ---- Post-Boot On-Demand Map / Asset Streaming ----
  // Keep SWBF2's original in-game planet loading banners fully visible; show only the
  // compact bottom download bar so players see live download speed & filename without covering the banner.
  if (booted && phase === 'gamedata') {
    const totalHttp = msg.totalHttpBytes || 0;
    const delta = Math.max(0, totalHttp - lastVfsHttpBytes);
    lastVfsHttpBytes = totalHttp;
    const bps = recordSpeedBytes(delta);
    const burstBytes = Math.max(0, totalHttp - mapBurstStartBytes);
    const barPct = Math.min(96, 15 + Math.round(80 * (1 - Math.exp(-burstBytes / (20 * 1048576)))));
    const spd = fmtSpeed(bps);
    if (msg.file) {
      const mbStr = burstBytes >= 262144 ? ` · ${fmtBytes(burstBytes)}` : '';
      showStreamToast(`Downloading ${msg.file}${mbStr}${spd ? ` (${spd})` : ''}`, barPct);
    }
  }
}

// Community links are wired by wireCommunityLinks() from site_config.mjs.

function updateVoiceButtons(pttSpeaking = false) {
  let label = 'Voice: Off';
  let active = false;
  if (voiceMode === 'ptt') {
    label = pttSpeaking ? 'Voice: Talking (V)' : 'Voice: Hold V';
    active = pttSpeaking;
  } else if (voiceMode === 'open') {
    label = 'Voice: Open Mic';
    active = true;
  }
  for (const id of ['btnOverlayVoice', 'btnVoiceHud']) {
    const b = $(id);
    if (!b) continue;
    b.textContent = label;
    b.classList.toggle('accent', active);
  }
}

async function cycleVoiceMode() {
  if (!netClient) return;
  try {
    if (voiceMode === 'off') {
      await netClient.enableVoice({ startMuted: true });
      voiceMode = 'ptt';
      appendLobbyChat('', 'Voice chat enabled (Push-to-Talk: hold V to speak; click again for Open Mic).', true);
    } else if (voiceMode === 'ptt') {
      netClient.setVoiceMuted(false);
      voiceMode = 'open';
      appendLobbyChat('', 'Voice chat switched to Open Mic (click again to turn off).', true);
    } else {
      netClient.disableVoice();
      voiceMode = 'off';
      appendLobbyChat('', 'Voice chat turned off.', true);
    }
    updateVoiceButtons(false);
  } catch (err) {
    appendLobbyChat('', `Voice chat unavailable: ${err.message || err}`, true);
  }
}

// Restore saved username if present (no registration required).
try {
  const savedName = params.get('name') || localStorage.getItem('swbf2_username');
  if (savedName) $('inpPlayerName').value = savedName.slice(0, 24);
  $('inpPlayerName').addEventListener('input', () => {
    try { localStorage.setItem('swbf2_username', $('inpPlayerName').value.trim()); } catch {}
  });
} catch {}

// If user opened a direct invite URL (?join=<room>), display a 1-click Join Banner on main view.
const initialJoinParam = params.get('join') || '';
if (initialJoinParam) {
  $('inviteBanner').style.display = 'block';
  $('inviteTitle').textContent = `Invite ready: Join lobby "${initialJoinParam}"`;
  $('btnAcceptInvite').addEventListener('click', () => {
    $('inviteBanner').style.display = 'none';
    launch({ joinRoom: initialJoinParam });
  });
}

// Register COI service worker only if the host did not already supply COOP/COEP headers.
if (!self.crossOriginIsolated && 'serviceWorker' in navigator && location.protocol !== 'file:') {
  navigator.serviceWorker.register('/browser/coi-serviceworker.js').then((reg) => {
    if (reg.active && !navigator.serviceWorker.controller) {
      location.reload();
    }
  }).catch(() => {});
}

// Resume WebAudio on first user gesture (required on iOS Safari & Chrome autoplay policy).
function resumeAudioNow() {
  if (audioSink?.context && audioSink.context.state === 'suspended') {
    audioSink.context.resume().catch(() => {});
  }
}
function armAudioGestureResume() {
  window.addEventListener('pointerdown', resumeAudioNow, { passive: true });
  window.addEventListener('touchend', resumeAudioNow, { passive: true });
  window.addEventListener('keydown', resumeAudioNow, { passive: true });
}
armAudioGestureResume();

// Initialize Android & iOS multi-touch controls (auto-enabled on touch devices).
let savedTouchMode = 'auto';
let savedTouchSens = 1.2;
try {
  const m = localStorage.getItem('swbf2_touch_mode');
  if (m === 'auto' || m === 'on' || m === 'off') savedTouchMode = m;
  const s = Number(localStorage.getItem('swbf2_touch_sens'));
  if (Number.isFinite(s) && s >= 0.4 && s <= 2.5) savedTouchSens = s;
} catch {}

touchControls = new TouchControls({
  rootEl: $('touchControls'),
  canvas,
  getInputWriter: () => inputWriter,
  mode: savedTouchMode,
  sensitivity: savedTouchSens,
  onResumeAudio: resumeAudioNow,
});

if ($('selTouchMode')) {
  $('selTouchMode').value = savedTouchMode;
  $('selTouchMode').addEventListener('change', () => {
    const v = $('selTouchMode').value;
    touchControls.setMode(v);
    try { localStorage.setItem('swbf2_touch_mode', v); } catch {}
  });
}
if ($('rngTouchSens') && $('lblTouchSens')) {
  $('rngTouchSens').value = String(Math.round(savedTouchSens * 100));
  $('lblTouchSens').textContent = `${savedTouchSens.toFixed(1)}×`;
  $('rngTouchSens').addEventListener('input', () => {
    const s = Math.max(0.4, Math.min(2.5, (Number($('rngTouchSens').value) || 120) / 100));
    touchControls.setSensitivity(s);
    $('lblTouchSens').textContent = `${s.toFixed(1)}×`;
    try { localStorage.setItem('swbf2_touch_sens', s.toFixed(2)); } catch {}
  });
}

// On small mobile screens, default to crisp 1280x720 HD for smooth mobile GPU frame rates
// (desktop stays 1600x900; users can still change resolution in Settings/Options anytime).
if (detectTouchDevice() && Math.min(window.screen?.width || 999, window.screen?.height || 999) <= 500) {
  if ($('selInitRes')) $('selInitRes').value = '1280x720';
  if ($('selRes')) $('selRes').value = '1280x720';
}

// Hide Fullscreen button on browsers that don't support Fullscreen API (e.g., iOS Safari).
if ($('btnFs') && !document.documentElement.requestFullscreen && !document.documentElement.webkitRequestFullscreen) {
  $('btnFs').style.display = 'none';
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function getSelectedUsername(fallback = 'Trooper') {
  const val = ($('inpPlayerName').value || params.get('name') || fallback).trim();
  return val || fallback;
}

function difficultyLabel(d) {
  const n = Number(d);
  if (n === 1) return 'Easy AI';
  if (n === 3) return 'Elite AI';
  return 'Normal AI';
}

function buildConfigKv(cfg = {}) {
  const pairs = [];
  for (const [k, v] of Object.entries(cfg)) {
    if (v === undefined || v === null || v === '') continue;
    const cleanVal = String(v).replace(/[\r\n;&]/g, '');
    pairs.push(`${k}=${cleanVal}`);
  }
  return pairs.join('\n');
}

function ensureWorker() {
  if (worker) return worker;
  worker = new Worker('/browser/worker.mjs', { type: 'module' });
  worker.onerror = (ev) => {
    console.error('[worker.onerror]', ev.message || ev);
  };
  worker.onmessage = (ev) => {
    const msg = ev.data || {};
    if (msg.type === 'loadProgress') {
      handleLoadProgress(msg);
      return;
    }
    if (msg.type === 'loadBanner') {
      if (msg.action === 'asset') applyLoadBannerAsset(msg);
      else if (msg.action === 'show') showLoadBanner(msg);
      else hideLoadBanner();
      return;
    }
    if (msg.type === 'interactiveStatus') {
      lastStatus = msg;
      const now = performance.now();
      lastInteractiveStatusMs = now;
      const df = (msg.frames || 0) - lastFrameCount;
      const dt = (now - lastFpsMs) / 1000;
      if (dt >= 0.4 && df >= 0) {
        currentFps = Math.round(df / dt);
        lastFrameCount = msg.frames || 0;
        lastFpsMs = now;
      }
      const s = msg.status || {};
      applyAudioGain(s.state || 'shell');
      if (mapLoadActive) {
        closeLoadingOverlay();
        mapBurstStartBytes = lastVfsHttpBytes;
        mapLoadTargetLabel = '';
      } else if (mapPendingTimer && s.state === 'mission') {
        clearTimeout(mapPendingTimer);
        mapPendingTimer = 0;
        mapLoadExplicit = false;
        mapBurstStartBytes = lastVfsHttpBytes;
        mapLoadTargetLabel = '';
      }
      if (s.screenWidth > 0 && s.screenHeight > 0) {
        inputWriter?.setGameSize(s.screenWidth, s.screenHeight);
      }
      if (s.preferredWidth > 0 && s.preferredHeight > 0) {
        const prefKey = `${s.preferredWidth}x${s.preferredHeight}`;
        if ($('selRes') && [...$('selRes').options].some((o) => o.value === prefKey) && $('selRes').value !== prefKey) {
          $('selRes').value = prefKey;
          if ($('selInitRes')) $('selInitRes').value = prefKey;
          try { localStorage.setItem('swbf2_resolution', prefKey); } catch {}
        }
      }
      if (!s.pointerLockWanted && document.pointerLockElement === canvas) {
        if (inputWriter) inputWriter.exitPointerLock();
        else { try { document.exitPointerLock?.(); } catch {} }
      }
      canvas.style.cursor = (!s.pointerLockWanted && !(inputWriter && inputWriter.pointerLocked))
        ? 'none' : 'default';
      const a = msg.audioStats || {};
      const n = msg.netStatus || null;
      $('stFps').textContent = String(currentFps);
      $('stState').textContent = s.state || '—';
      $('stScreen').textContent = s.currentScreen || (s.playerAlive ? 'live' : 'spawn');
      $('stDraws').textContent = String(s.draws || 0);
      $('stTris').textContent = String(s.triangles || 0);
      const rms = Math.max(a.rmsL || 0, a.rmsR || 0);
      $('stAudio').textContent = muted ? 'muted' : `${a.playingBuffers || 0}v`;
      $('stAudioBar').style.width = `${Math.min(100, Math.round(rms * 350))}%`;
      const locked = !!(inputWriter && inputWriter.pointerLocked);
      $('stLock').textContent = locked
        ? 'cursor: locked'
        : (s.pointerLockWanted ? 'cursor: click to aim' : 'cursor: menu');
      const overlayBlocked = drawer.classList.contains('open') ||
        mpLobbyOverlay.classList.contains('open') ||
        loadingOverlay.classList.contains('open') ||
        (loadBannerOverlay && loadBannerOverlay.classList.contains('open')) ||
        launcher.style.display !== 'none';
      touchControls?.setState({
        booted,
        combatMode: !!s.pointerLockWanted,
        overlayBlocked,
      });
      const touchActive = !!(touchControls && touchControls.isEnabled());
      lockHint.style.display = (booted && s.pointerLockWanted && !locked && !overlayBlocked && !touchActive)
        ? 'block' : 'none';

      if (n && netClient && activeMpSession) {
        const nc = netClient.getStats();
        const link = nc.webrtcOpenChannels > 0 ? 'WebRTC' : 'WS';
        const pCount = Math.max(n.connectedPlayers || 0, n.hostPlayerCount || 0, nc.peerCount + 1);
        $('stNet').style.display = 'inline-flex';
        $('stNetVal').textContent = `${activeMpSession.title || nc.room} · ${pCount} player${pCount === 1 ? '' : 's'} · ${link}`;
        $('btnLaunchMp').style.display = (s.state === 'shell' && n.hostActive && n.hostLaunchReady) ? 'inline-block' : 'none';
        $('btnVoiceHud').style.display = 'inline-block';
        $('btnLeaveMpHud').style.display = 'inline-block';
        $('mpDrawerInfo').textContent = `${nc.localIp} (${link}) · room=${nc.room} · tx=${n.txPackets} rx=${n.rxPackets} · turn=${n.clientTurn}`;
        updateStagingOverlay(s, n, nc);
      }
      return;
    }
    if (msg.type === 'log') {
      console.log(msg.text);
      return;
    }
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.ok) resolve(msg);
      else reject(new Error(msg.error || msg.stage || 'worker error'));
    }
  };
  return worker;
}

function callWorker(cmd, opts = {}, transfers = []) {
  ensureWorker();
  const id = reqId++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    worker.postMessage({ id, cmd, opts }, transfers);
  });
}

function parseRes(val) {
  const [w, h] = String(val || '1600x900').split('x').map(Number);
  return { width: (w > 0 ? w : 1600) | 0, height: (h > 0 ? h : 900) | 0 };
}

async function runPreflight() {
  const pf = { webgpu: false, bc: false, coi: !!self.crossOriginIsolated, opfs: false, audio: false };
  try {
    if (navigator.gpu) {
      let ad = null;
      for (let attempt = 0; attempt < 10 && !ad; ++attempt) {
        ad = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
        if (!ad) await new Promise((r) => setTimeout(r, 150));
      }
      if (ad) {
        pf.webgpu = true;
        pf.bc = ad.features.has('texture-compression-bc');
        $('pfGpu').textContent = pf.bc ? 'OK (BCn)' : 'OK';
      } else {
        $('pfGpu').textContent = 'No adapter';
      }
    } else {
      $('pfGpu').textContent = 'Unavailable';
    }
  } catch {
    $('pfGpu').textContent = 'Error';
  }
  $('pfCoi').textContent = pf.coi ? 'OK' : 'Missing';
  try {
    const res = await callWorker('vfsOp', { op: 'stats' });
    const vs = res.vfsStats || {};
    pf.opfs = !!vs.opfsPersistent;
    const mb = ((vs.cachedBytes || 0) / 1048576).toFixed(1);
    $('pfOpfs').textContent = pf.opfs ? `OK (${mb} MB)` : 'RAM';
    $('pfCacheInfo').textContent = `${vs.cachedBlocks || 0} blocks · ${(vs.saveFiles || []).length} saves`;
    updateVfsDrawer(vs);
  } catch {
    $('pfOpfs').textContent = 'RAM';
  }
  pf.audio = !!(globalThis.AudioContext || globalThis.webkitAudioContext);
  $('pfAudio').textContent = pf.audio ? 'OK' : 'Off';

  if (pf.webgpu && pf.coi) {
    $('readyText').textContent = 'Ready to play';
    $('readyDot').style.background = 'var(--ok)';
  } else {
    $('readyText').textContent = pf.webgpu ? 'Waiting for cross-origin isolation…' : 'WebGPU required';
    $('readyDot').style.background = 'var(--warn)';
  }

  window.__playPreflight = pf;
  return pf;
}

function updateVfsDrawer(vs) {
  if (!vs) return;
  if (vs.mode === 'local') {
    const localMb = ((vs.localBytes || 0) / 1048576).toFixed(1);
    $('vfsDrawerInfo').textContent =
      `BYOC Local: ${vs.localFileCount || 0} files · read ${localMb} MB from disk · saves: ${(vs.saveFiles || []).length}`;
    return;
  }
  const cachedMb = ((vs.cachedBytes || 0) / 1048576).toFixed(1);
  const httpMb = ((vs.httpBytes || 0) / 1048576).toFixed(1);
  $('vfsDrawerInfo').textContent =
    `OPFS: ${vs.opfsPersistent ? 'persistent' : 'RAM'} · ${vs.cachedBlocks || 0} blocks (${cachedMb} MB) · streamed ${httpMb} MB · saves: ${(vs.saveFiles || []).length}`;
}

// ---- Map Selectors & Custom Playlist Builder ----
function populateMapDropdowns() {
  const eraF = $('selEraFilter').value;
  const modeF = $('selModeFilter').value;
  const sel = $('selMpMap');
  const prev = sel.value;
  sel.innerHTML = '';
  const filtered = ALL_SCRIPTS.filter((s) =>
    (eraF === 'all' || s.era === eraF) && (modeF === 'all' || s.mode === modeF)
  );
  for (const s of filtered) {
    const opt = document.createElement('option');
    opt.value = s.id;
    opt.textContent = s.label;
    sel.appendChild(opt);
  }
  if (filtered.some((s) => s.id === prev)) sel.value = prev;

  // Also populate in-game Quick Launch map selector once.
  const qSel = $('selQuickMap');
  if (qSel && qSel.options.length === 0) {
    for (const s of ALL_SCRIPTS) {
      const opt = document.createElement('option');
      opt.value = s.id;
      opt.textContent = s.label;
      qSel.appendChild(opt);
    }
  }
}

function renderPlaylistChips() {
  const el = $('playlistChips');
  el.innerHTML = '';
  if (customPlaylist.length === 0) {
    el.innerHTML = '<span style="font-size:11px; color:var(--dim); padding:3px 4px;">No maps queued — selected dropdown map will be used.</span>';
    return;
  }
  customPlaylist.forEach((scriptId, idx) => {
    const chip = document.createElement('span');
    chip.className = 'map-chip';
    chip.innerHTML = `<span>${idx + 1}. ${escapeHtml(getScriptLabel(scriptId))}</span>`;
    const rm = document.createElement('button');
    rm.type = 'button';
    rm.textContent = '×';
    rm.title = 'Remove map';
    rm.onclick = () => {
      customPlaylist.splice(idx, 1);
      renderPlaylistChips();
    };
    chip.appendChild(rm);
    el.appendChild(chip);
  });
}

function readCustomLobbyConfig() {
  const playlist = customPlaylist.length ? [...customPlaylist] : [$('selMpMap').value || 'tat2g_con'];
  const firstMap = playlist[0];
  const rawTitle = ($('inpRoom').value || '').trim();
  const title = rawTitle || `${getScriptInfo(firstMap).mapName} Custom`;
  const roomId = rawTitle
    ? rawTitle.toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 28) || `custom_${Math.floor(100 + Math.random() * 900)}`
    : `custom_${Math.floor(1000 + Math.random() * 9000)}`;
  const timeLimit = Number($('selTimeLimit').value) || 0;
  const config = {
    maxPlayers: Number($('selMaxPlayers').value) || 16,
    numBots: Number($('selNumBots').value),
    difficulty: Number($('selDifficulty').value) || 2,
    reinfMult: Number($('selReinfMult').value) || 100,
    teamDamage: $('chkTeamDamage').checked ? 1 : 0,
    autoAssign: $('chkAutoAssign').checked ? 1 : 0,
    showNames: $('chkShowNames').checked ? 1 : 0,
    autoAim: $('chkAutoAim').checked ? 1 : 0,
    awards: $('chkAwards').checked ? 1 : 0,
    spawnInvinc: Number($('selSpawnInvinc').value) || 0,
    warmUp: Number($('selWarmUp').value) || 0,
    heroes: $('chkHeroes').checked ? 1 : 0,
    heroUnlock: Number($('selHeroUnlock').value) || 5,
    heroUnlockVal: Number($('inpHeroUnlockVal').value) || 10,
    heroPlayer: Number($('selHeroPlayer').value) || 1,
    heroTeam: Number($('selHeroTeam').value) || 3,
    heroRespawn: Number($('selHeroRespawn').value),
    conTime: timeLimit,
    ctfScore: Number($('selCtfScore').value) || 5,
    ctfTime: timeLimit,
    assScore: Number($('selAssScore').value) || 180,
    huntScore: Number($('selHuntScore').value) || 50,
    huntTime: timeLimit > 0 ? timeLimit : 5,
    randomize: $('chkRandomize').checked ? 1 : 0,
    password: ($('inpRoomPassword').value || '').trim(),
  };
  return { roomId, title, map: firstMap, playlist, config };
}

// ---- Lobby Browser ----
function classifyCategory(mapId, presetCategory) {
  if (presetCategory) return presetCategory;
  const info = getScriptInfo(mapId);
  if (info.category === 'space' || info.mode === 'ass') return 'space';
  if (info.mode === 'eli') return 'eli';
  if (info.mode === 'ctf' || info.mode === '1flag') return 'ctf';
  if (info.mode === 'hunt' || info.mode === 'xl') return 'hunt_xl';
  return 'con';
}

async function refreshRooms() {
  const el = $('roomList');
  let liveRooms = [];
  let serverOnline = false;
  try {
    const res = await fetch(`${DEFAULT_MP_HTTP}/api/rooms`, { cache: 'no-store' });
    if (res.ok) {
      const data = await res.json();
      liveRooms = data.rooms || [];
      serverOnline = true;
    }
  } catch {}

  window.__playRooms = liveRooms;
  const liveById = new Map(liveRooms.map((r) => [r.id, r]));
  const totalOnline = liveRooms.reduce((acc, r) => acc + (Number(r.players) || 0), 0);

  $('onlineCountBadge').textContent = `${totalOnline} Player${totalOnline === 1 ? '' : 's'} Online`;
  $('btnOnlineBadge').textContent = totalOnline > 0
    ? `${totalOnline} Playing →`
    : `${PUBLIC_LOBBY_PRESETS.length} Lobbies →`;

  const combined = [];
  for (const r of liveRooms) {
    const preset = PUBLIC_LOBBY_PRESETS.find((p) => p.id === r.id);
    const mapId = r.map || (preset ? preset.map : 'tat2g_con');
    combined.push({
      id: r.id,
      title: r.title || (preset ? preset.title : r.id),
      hostName: r.name || 'Host',
      map: mapId,
      playlist: Array.isArray(r.playlist) && r.playlist.length ? r.playlist : (preset ? preset.playlist : [mapId]),
      players: Number(r.players) || 1,
      maxPlayers: Number(r.maxPlayers) || 16,
      numBots: r.numBots ?? 16,
      difficulty: r.difficulty ?? 2,
      reinfMult: r.reinfMult ?? 100,
      heroes: r.heroes !== false,
      teamDamage: !!r.teamDamage,
      hasPassword: !!r.hasPassword,
      status: r.status || 'lobby',
      category: classifyCategory(mapId, preset?.category),
      isLive: true,
    });
  }
  for (const p of PUBLIC_LOBBY_PRESETS) {
    if (liveById.has(p.id)) continue;
    combined.push({
      id: p.id,
      title: p.title,
      hostName: 'Public Lobby',
      map: p.map,
      playlist: p.playlist,
      players: 0,
      maxPlayers: p.rules?.maxPlayers || 16,
      numBots: p.rules?.numBots ?? 16,
      difficulty: p.rules?.difficulty ?? 2,
      reinfMult: p.rules?.reinfMult ?? 100,
      heroes: p.rules?.heroes !== 0,
      teamDamage: !!p.rules?.teamDamage,
      hasPassword: false,
      status: 'open',
      category: p.category,
      rules: p.rules || {},
      isLive: false,
    });
  }

  const query = ($('inpLobbySearch')?.value || '').trim().toLowerCase();
  const visible = combined.filter((r) => {
    if (activeLobbyFilter === 'live' && !r.isLive) return false;
    if (activeLobbyFilter !== 'all' && activeLobbyFilter !== 'live' && r.category !== activeLobbyFilter) return false;
    if (query) {
      const hay = `${r.title} ${r.id} ${r.hostName} ${getScriptLabel(r.map)}`.toLowerCase();
      if (!hay.includes(query)) return false;
    }
    return true;
  });

  el.innerHTML = '';
  if (visible.length === 0) {
    const empty = document.createElement('div');
    empty.style.cssText = 'text-align:center; color:var(--dim); padding:26px 12px; font-size:13px;';
    empty.textContent = activeLobbyFilter === 'live'
      ? 'No active lobbies right now — host any public lobby or click "+ Create Custom Lobby"!'
      : 'No lobbies match your filter.';
    el.appendChild(empty);
  }

  for (const r of visible) {
    const row = document.createElement('div');
    row.className = `room-row ${r.isLive ? 'live' : ''}`;
    const mapLabel = getScriptLabel(r.map);
    const playlistBadge = r.playlist.length > 1 ? `<span class="tag">${r.playlist.length} Maps</span>` : '';
    const statusBadge = r.isLive
      ? (r.status === 'playing'
        ? '<span class="tag playing">In Match · Joinable</span>'
        : '<span class="tag live">In Lobby</span>')
      : '';
    const lockBadge = r.hasPassword ? '<span class="tag warn">🔒 Password</span>' : '';
    const ruleBits = [
      `${r.numBots} Bots`,
      difficultyLabel(r.difficulty),
      r.heroes ? 'Heroes' : 'No Heroes',
      r.reinfMult !== 100 ? `${r.reinfMult}% Tickets` : '',
      r.teamDamage ? 'FF On' : '',
    ].filter(Boolean).join(' · ');

    const subText = r.isLive
      ? `${escapeHtml(mapLabel)} · Host: ${escapeHtml(r.hostName)} · ${escapeHtml(ruleBits)}`
      : `${escapeHtml(mapLabel)} · ${escapeHtml(ruleBits)}`;

    row.innerHTML = `
      <div class="room-meta">
        <div class="room-title">${escapeHtml(r.title)} ${statusBadge} ${lockBadge} ${playlistBadge}</div>
        <div class="room-sub">${subText}</div>
      </div>
      <div class="room-right">
        <span class="player-count ${r.players > 0 ? 'active' : ''}">${r.players} / ${r.maxPlayers} Players</span>
      </div>
    `;
    const btn = document.createElement('button');
    btn.textContent = r.isLive ? 'Join' : 'Join Lobby';
    btn.className = r.isLive ? 'primary' : 'accent';
    btn.style.padding = '6px 14px';
    btn.onclick = () => {
      if (r.isLive) {
        let pw = '';
        if (r.hasPassword) {
          pw = window.prompt(`Enter password for "${r.title}":`, '') ?? '';
          if (!pw) return;
        }
        launch({ joinRoom: r.id, joinTitle: r.title, password: pw });
      } else {
        launch({
          hostRoom: r.id,
          hostTitle: r.title,
          mpMap: r.map,
          playlist: r.playlist,
          mpConfig: r.rules || {},
        });
      }
    };
    row.querySelector('.room-right').appendChild(btn);
    el.appendChild(row);
  }

  if (!serverOnline && liveRooms.length === 0) {
    const note = document.createElement('div');
    note.className = 'mono';
    note.style.cssText = 'color:var(--dim); font-size:11px; text-align:center; padding-top:4px;';
    note.textContent = `Signaling server offline (${DEFAULT_MP_HTTP})`;
    el.appendChild(note);
  }
  return liveRooms;
}

function showView(viewName) {
  mainView.style.display = viewName === 'main' ? 'block' : 'none';
  lobbyView.style.display = viewName === 'lobby' ? 'block' : 'none';
  createLobbyView.style.display = viewName === 'create' ? 'block' : 'none';
  launcherCard.classList.toggle('wide', viewName === 'lobby');
  launcherCard.classList.toggle('creator', viewName === 'create');
  errMsg.style.display = 'none';

  if (lobbyRefreshTimer) {
    clearInterval(lobbyRefreshTimer);
    lobbyRefreshTimer = 0;
  }
  if (viewName === 'lobby') {
    refreshRooms();
    lobbyRefreshTimer = setInterval(() => {
      if (launcher.style.display !== 'none' && lobbyView.style.display !== 'none') {
        refreshRooms();
      }
    }, 4000);
  }
}

function showLobbyView() { showView('lobby'); }
function showMainView() { showView('main'); }
function showCreateLobbyView() {
  populateMapDropdowns();
  renderPlaylistChips();
  showView('create');
}

// ---- Live Staging Lobby Overlay ----
function appendLobbyChat(name, text, isSys = false) {
  const log = $('lobbyChatLog');
  if (!log) return;
  const line = document.createElement('div');
  line.className = `chat-msg ${isSys ? 'sys' : ''}`;
  if (isSys) {
    line.textContent = text;
  } else {
    line.innerHTML = `<b>${escapeHtml(name)}:</b> ${escapeHtml(text)}`;
  }
  log.appendChild(line);
  log.scrollTop = log.scrollHeight;
}

function renderStagingRoster() {
  const el = $('lobbyRoster');
  if (!el || !netClient || !activeMpSession) return;
  const nc = netClient.getStats();
  const me = {
    name: getSelectedUsername(activeMpSession.role === 'host' ? 'Host' : 'Trooper'),
    role: activeMpSession.role,
    ip: nc.localIp,
    dcState: 'local',
  };
  const all = [me, ...nc.peers];
  const maxP = netClient.roomMeta?.maxPlayers || activeMpSession.config?.maxPlayers || 16;
  $('stagingPlayerCount').textContent = `${all.length} / ${maxP}`;
  $('stagingNetLink').textContent = nc.webrtcOpenChannels > 0 ? 'WebRTC P2P' : 'WS Relay';

  el.innerHTML = '';
  for (const p of all) {
    const row = document.createElement('div');
    row.className = 'roster-item';
    const roleTag = p.role === 'host' ? '<span class="tag gold">Host</span>' : '<span class="tag">Player</span>';
    const linkTag = p.dcState === 'local'
      ? '<span class="tag live">You</span>'
      : (p.dcState === 'open' ? '<span class="tag live">WebRTC</span>' : '<span class="tag playing">WS</span>');
    row.innerHTML = `
      <span style="display:flex;align-items:center;gap:7px;"><strong>${escapeHtml(p.name)}</strong> ${roleTag}</span>
      <span>${linkTag}</span>
    `;
    el.appendChild(row);
  }
}

function openStagingOverlay() {
  if (!activeMpSession) return;
  const meta = netClient?.roomMeta || {};
  const title = meta.title || activeMpSession.title || activeMpSession.room;
  const playlist = (meta.playlist && meta.playlist.length ? meta.playlist : activeMpSession.playlist) || ['tat2g_con'];
  const map0 = meta.map || playlist[0];
  const cfg = activeMpSession.config || {};
  const bots = meta.numBots ?? cfg.numBots ?? 16;
  const diff = difficultyLabel(meta.difficulty ?? cfg.difficulty ?? 2);
  const reinf = meta.reinfMult ?? cfg.reinfMult ?? 100;
  const heroes = meta.heroes !== undefined ? meta.heroes : (cfg.heroes !== 0);
  const ff = meta.teamDamage !== undefined ? meta.teamDamage : !!cfg.teamDamage;

  $('stagingRoleBadge').textContent = activeMpSession.role === 'host' ? 'HOSTING MULTIPLAYER LOBBY' : 'JOINED MULTIPLAYER LOBBY';
  $('stagingTitle').textContent = title;
  $('stagingMetaLine').textContent =
    `${getScriptLabel(map0)} · ${bots} Bots · ${diff} · ${reinf}% Tickets · ${heroes ? 'Heroes ON' : 'No Heroes'}${ff ? ' · Friendly Fire ON' : ''}`;
  $('stagingPlaylistSummary').textContent = playlist.length > 1
    ? `Rotation (${playlist.length} maps): ${playlist.map((m) => getScriptInfo(m).mapName).join(' → ')}`
    : `Map: ${getScriptLabel(map0)}`;
  $('btnOverlayStartMatch').style.display = activeMpSession.role === 'host' ? 'inline-block' : 'none';
  $('btnOverlayStartMatch').disabled = true;
  $('stagingWaitNote').textContent = activeMpSession.role === 'host'
    ? 'Click Start Match whenever you are ready (friends can also hot-join mid-match!)'
    : 'Waiting for Host to start the match…';
  mpLobbyOverlay.classList.add('open');
  renderStagingRoster();
}

function updateStagingOverlay(s, n, nc) {
  if (!activeMpSession) return;
  // Automatically close the staging overlay once the engine enters MissionState!
  if (s.state === 'mission') {
    if (mpLobbyOverlay.classList.contains('open')) {
      mpLobbyOverlay.classList.remove('open');
      canvas.focus();
    }
    return;
  }

  renderStagingRoster();
  const bar = $('stagingStatusBar');
  const txt = $('stagingStatusText');
  bar.classList.remove('ready', 'error');

  if (n.gameError) {
    bar.classList.add('error');
    txt.textContent = `Network notice: ${n.gameError}`;
    return;
  }

  if (activeMpSession.role === 'host') {
    if (n.hostActive && n.hostLaunchReady) {
      bar.classList.add('ready');
      const total = Math.max(n.hostPlayerCount || 1, nc.peerCount + 1);
      txt.textContent = `Lobby live! ${total} player${total === 1 ? '' : 's'} ready. Click "Start Match" to deploy.`;
      $('btnOverlayStartMatch').disabled = false;
      if (activeMpSession.autoStart && !activeMpSession.matchLaunched) {
        activeMpSession.matchLaunched = true;
        triggerHostLaunchMatch();
      }
    } else {
      txt.textContent = 'Preparing LAN host session…';
    }
  } else {
    if (n.joinFailed) {
      bar.classList.add('error');
      txt.textContent = 'Could not join host (wrong password or lobby full).';
    } else if (n.joinHostPlaying) {
      bar.classList.add('ready');
      txt.textContent = 'Match is already in progress — hot-joining battlefield now…';
      $('stagingWaitNote').textContent = 'Loading map…';
    } else if (n.joinConnected) {
      bar.classList.add('ready');
      txt.textContent = 'Connected to Host! Waiting for Host to click Start Match…';
    } else if ((n.findGameCount || 0) >= 1) {
      txt.textContent = `Found host "${n.foundName || activeMpSession.room}" — handshaking…`;
    } else {
      txt.textContent = 'Discovering host over virtual LAN…';
    }
  }
}

async function triggerHostLaunchMatch() {
  if (!booted || !activeMpSession || activeMpSession.role !== 'host') return;
  $('btnOverlayStartMatch').disabled = true;
  $('stagingStatusText').textContent = 'Launching match for all players…';
  netClient?.updateRoomMeta({ status: 'playing' });
  const res = await callWorker('interactiveAction', { action: 'launchMatch' });
  if (!res.ok) {
    // If ifs_saveop was finishing, retry briefly.
    setTimeout(() => {
      if (activeMpSession && lastStatus?.status?.state === 'shell') {
        callWorker('interactiveAction', { action: 'launchMatch' }).catch(() => {});
      }
    }, 350);
  }
}

async function leaveMultiplayerLobby({ returnToBrowser = true } = {}) {
  activeMpSession = null;
  voiceMode = 'off';
  updateVoiceButtons(false);
  mpLobbyOverlay.classList.remove('open');
  $('stNet').style.display = 'none';
  $('btnLaunchMp').style.display = 'none';
  $('btnCopyInvite').style.display = 'none';
  $('btnVoiceHud').style.display = 'none';
  $('btnLeaveMpHud').style.display = 'none';
  $('mpDrawerInfo').textContent = 'Offline';
  if (netClient) {
    netClient.close();
    netClient = null;
  }
  if (booted) {
    await callWorker('interactiveAction', { action: 'disconnectNet' }).catch(() => {});
  }
  if (returnToBrowser) {
    launcher.style.display = 'flex';
    $('btnStart').disabled = false;
    $('btnPlayOnline').disabled = false;
    showLobbyView();
  }
}

async function connectMultiplayer({
  role,
  room,
  title = '',
  map = 'tat2g_con',
  playlist = null,
  config = {},
  password = '',
  autoStart = false,
}) {
  if (!booted) return;
  if (netClient) {
    netClient.close();
    netClient = null;
  }
  const playerName = getSelectedUsername(role === 'host' ? 'Host' : 'Trooper');
  const effPlaylist = Array.isArray(playlist) && playlist.length ? playlist : [map];
  const effMap = effPlaylist[0] || map || 'tat2g_con';
  const effTitle = title || room;
  const effPassword = password || config.password || '';

  $('lobbyChatLog').innerHTML = '';
  activeMpSession = {
    role,
    room,
    title: effTitle,
    playlist: effPlaylist,
    config: { ...config, password: effPassword },
    autoStart,
    matchLaunched: false,
  };

  $('stNet').style.display = 'inline-flex';
  $('stNetVal').textContent = `Connecting to ${effTitle}…`;
  openStagingOverlay();
  appendLobbyChat('', `Connecting to lobby "${effTitle}" as ${playerName}…`, true);

  netClient = new BrowserNetClient({
    serverUrl: DEFAULT_MP_SERVER,
    room,
    role,
    name: playerName,
    map: effMap,
    title: effTitle,
    password: effPassword,
    playlist: effPlaylist,
    meta: {
      maxPlayers: config.maxPlayers || 16,
      numBots: config.numBots ?? 16,
      difficulty: config.difficulty || 2,
      reinfMult: config.reinfMult || 100,
      heroes: config.heroes !== 0,
      teamDamage: !!config.teamDamage,
      autoAssign: !!config.autoAssign,
    },
    enableWebRtc: true,
    iceServers: SITE_CONFIG.iceServers || [],
    onPeersChanged: () => renderStagingRoster(),
    onRoomUpdated: () => openStagingOverlay(),
    onChat: (msg) => appendLobbyChat(msg.name || 'Player', msg.text || ''),
    onHostLeft: () => {
      appendLobbyChat('', 'The host has closed this lobby.', true);
      const bar = $('stagingStatusBar');
      bar.classList.remove('ready');
      bar.classList.add('error');
      $('stagingStatusText').textContent = 'Host disconnected — this lobby has closed. Click Leave Lobby to return.';
      $('stagingWaitNote').textContent = 'Lobby closed';
    },
  });

  const welcome = await netClient.connect();
  // Server may transparently downgrade a concurrent host click on an open public lobby to client.
  const actualRole = welcome.role || role;
  activeMpSession.role = actualRole;
  if (welcome.roomMeta) {
    activeMpSession.title = welcome.roomMeta.title || activeMpSession.title;
    if (welcome.roomMeta.playlist?.length) activeMpSession.playlist = welcome.roomMeta.playlist;
  }
  openStagingOverlay();

  await callWorker('interactiveAction', {
    action: 'attachNet',
    txSab: netClient.txSab,
    rxSab: netClient.rxSab,
    localIpInt: welcome.ipInt,
  });

  lastInviteUrl = `${location.origin}/play?join=${encodeURIComponent(room)}`;
  $('btnCopyInvite').style.display = 'inline-block';
  $('btnVoiceHud').style.display = 'inline-block';
  $('btnLeaveMpHud').style.display = 'inline-block';

  if (actualRole === 'host') {
    const kv = buildConfigKv(activeMpSession.config);
    await callWorker('interactiveAction', {
      action: 'hostLanConfig',
      roomName: effTitle.slice(0, 31),
      playlistCsv: effPlaylist.join(','),
      playerName,
      configKv: kv,
    });
    $('btnLaunchMp').style.display = 'inline-block';
    $('stNetVal').textContent = `Lobby: ${effTitle} (Waiting…)`;
    appendLobbyChat('', `Lobby created! Share the invite link or wait for LAN players, then click Start Match.`, true);
  } else {
    await callWorker('interactiveAction', { action: 'findLan', playerName, password: effPassword });
    $('stNetVal').textContent = `Joining ${effTitle}…`;
    appendLobbyChat('', `Connected to signaling room. Joining host LAN session…`, true);

    const sessionRef = activeMpSession;
    (async () => {
      for (let i = 0; i < 120 && netClient && activeMpSession === sessionRef; ++i) {
        const res = await callWorker('interactiveAction', { action: 'getNetStatus' });
        const ns = res.netStatus || {};
        if (ns.joinConnected || ns.joinLaunchReady || lastStatus?.status?.state === 'mission') break;
        if (ns.joinFailed) break;
        if (!ns.joinActive && (ns.findGameCount || 0) >= 1) {
          await callWorker('interactiveAction', { action: 'joinFound', index: 0 });
        } else if (i > 0 && i % 20 === 0 && !ns.joinActive && (ns.findGameCount || 0) === 0) {
          // Re-issue LAN discovery broadcast if the host finished booting slightly after us.
          await callWorker('interactiveAction', { action: 'findLan', playerName, password: effPassword });
        }
        await new Promise((r) => setTimeout(r, 250));
      }
    })().catch((e) => console.warn('[mp] auto-join:', e));
  }
}

async function launch({
  initialMission = '',
  hostRoom = '',
  hostTitle = '',
  joinRoom = '',
  joinTitle = '',
  mpMap = 'tat2g_con',
  playlist = null,
  mpConfig = {},
  password = '',
  autoStart = false,
} = {}) {
  if (booting) return;
  const hRoom = hostRoom || params.get('host') || '';
  const jRoom = joinRoom || '';
  const isMultiplayer = !!(hRoom || jRoom);

  errMsg.style.display = 'none';

  // Enforce Bring-Your-Own-Copy (BYOC) check before booting when server doesn't host GameData.
  if (!booted && isByocRequired() && !(byocIndex && byocIndex.valid)) {
    showMainView();
    const card = $('byocCard');
    if (card) {
      card.classList.add('visible', 'attention');
      card.scrollIntoView({ behavior: 'smooth', block: 'center' });
    }
    errMsg.textContent = 'Select your Star Wars Battlefront II (2005) GameData folder first to play.';
    const sep = document.createTextNode(' ');
    const link = document.createElement('a');
    link.id = 'btnByocHelpLink';
    link.href = '#';
    link.textContent = 'Button not working?';
    link.style.cssText = 'font-size:11px; color:#5ea8ff; text-decoration:underline; cursor:pointer; font-weight:500;';
    link.addEventListener('click', (ev) => {
      ev.preventDefault();
      openByocHelpModal();
    });
    errMsg.appendChild(sep);
    errMsg.appendChild(link);
    errMsg.style.display = 'block';
    return;
  }

  // If engine is already booted (e.g., player left one lobby and clicked another!), reuse the worker cleanly!
  if (booted) {
    launcher.style.display = 'none';
    if (hRoom) {
      try {
        await connectMultiplayer({
          role: 'host',
          room: hRoom,
          title: hostTitle || hRoom,
          map: mpMap || params.get('map') || 'tat2g_con',
          playlist,
          config: mpConfig,
          password,
          autoStart,
        });
      } catch (err) {
        await leaveMultiplayerLobby({ returnToBrowser: true });
        errMsg.textContent = String(err.message || err);
        errMsg.style.display = 'block';
      }
    } else if (jRoom) {
      try {
        await connectMultiplayer({
          role: 'client',
          room: jRoom,
          title: joinTitle || jRoom,
          password,
        });
      } catch (err) {
        await leaveMultiplayerLobby({ returnToBrowser: true });
        errMsg.textContent = String(err.message || err);
        errMsg.style.display = 'block';
      }
    } else {
      canvas.focus();
    }
    return;
  }

  booting = true;
  $('btnStart').disabled = true;
  $('btnPlayOnline').disabled = true;
  speedSamples = [];
  totalStreamBytesMono = 0;
  lastWasmLoadedBytes = 0;
  lastVfsHttpBytes = 0;
  mapBurstStartBytes = 0;
  const useLocalFiles = !!(byocIndex && byocIndex.valid);
  const loadTitle = hRoom
    ? `Hosting ${hostTitle || hRoom}`
    : (jRoom ? `Joining ${joinTitle || jRoom}` : 'Launching Battlefront II');
  openLoadingOverlay({
    kicker: useLocalFiles ? 'LOADING LOCAL GAME FILES' : 'DOWNLOADING GAME ASSETS',
    title: loadTitle,
    stage: 'Preparing game engine…',
    pctText: '0%',
    barPct: 3,
    streaming: true,
    bytesText: useLocalFiles ? 'Reading from your local GameData folder…' : 'Connecting to game server…',
    speedText: 'Step 1 / 5',
    tipText: useLocalFiles
      ? 'Your game files stay 100% on your device — reading directly from your local SWBF2 installation.'
      : 'First-time loads stream & cache game data locally in your browser — future visits and maps load nearly instantaneously.',
  });
  const t0 = performance.now();

  try {
    if (!self.crossOriginIsolated) {
      throw new Error('Cross-Origin Isolation (COOP/COEP) is required for SharedArrayBuffer.');
    }
    const { width, height } = parseRes($('selInitRes').value);
    $('selRes').value = `${width}x${height}`;
    canvas.width = width;
    canvas.height = height;

    audioSink = new WebAudioSabSink({
      sampleRate: 48000,
      capacityFrames: AUDIO_SAB_DEFAULT_FRAMES,
      initialGain: muted ? 0 : (masterVolume * LOBBY_DUCK_FACTOR),
    });
    lastAppliedGain = muted ? 0 : (masterVolume * LOBBY_DUCK_FACTOR);
    // allowSuspended prevents autoplay policy from deadlocking launch; first click/key resumes audio.
    await audioSink.start({ allowSuspended: true });

    const inputSab = createInputBuffer();
    inputWriter = new InputBridgeWriter({ sab: inputSab, canvas, gameWidth: width, gameHeight: height });
    inputWriter.attach();

    const offscreen = canvas.transferControlToOffscreen();
    const startRes = await callWorker('startInteractive', {
      canvas: offscreen,
      width,
      height,
      initialMission: initialMission || params.get('mission') || '',
      autoLogin: isMultiplayer,
      musicStreaming: musicStreamingEnabled,
      localFiles: (byocIndex && byocIndex.valid) ? byocIndex.files : null,
      inputSab,
      audioSab: audioSink.sab,
      audioCapacityFrames: audioSink.capacityFrames,
      audioSampleRate: audioSink.sampleRate || 48000,
    }, [offscreen]);
    const st0 = startRes?.snapshot?.status;
    if (st0?.screenWidth > 0 && st0?.screenHeight > 0) {
      inputWriter.setGameSize(st0.screenWidth, st0.screenHeight);
    }

    booted = true;
    booting = false;
    lastInteractiveStatusMs = performance.now();
    mapBurstStartBytes = lastVfsHttpBytes;
    closeLoadingOverlay();
    launcher.style.display = 'none';
    canvas.focus();
    window.__playBootMs = Math.round(performance.now() - t0);

    if (hRoom) {
      await connectMultiplayer({
        role: 'host',
        room: hRoom,
        title: hostTitle || hRoom,
        map: mpMap || params.get('map') || 'tat2g_con',
        playlist,
        config: mpConfig,
        password,
        autoStart,
      });
    } else if (jRoom) {
      await connectMultiplayer({
        role: 'client',
        room: jRoom,
        title: joinTitle || jRoom,
        password,
      });
    }
  } catch (err) {
    booting = false;
    closeLoadingOverlay();
    console.error(err);
    if (activeMpSession) {
      await leaveMultiplayerLobby({ returnToBrowser: true });
    }
    errMsg.textContent = String(err.message || err);
    errMsg.style.display = 'block';
    $('btnStart').disabled = false;
    $('btnPlayOnline').disabled = false;
  }
}

async function exportSavesFile() {
  const res = await callWorker('vfsOp', { op: 'exportSaves' });
  const blob = new Blob([JSON.stringify(res.bundle, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `swbf2-saves-${new Date().toISOString().slice(0, 10)}.json`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  return res.bundle;
}

function uint8ToBase64(bytes) {
  let bin = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    bin += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(bin);
}

async function importSavesFromFiles(fileList, statusEl = null) {
  const files = Array.from(fileList || []);
  if (!files.length) return { imported: 0 };
  const setStatus = (msg, isErr = false) => {
    if (!statusEl) return;
    statusEl.textContent = msg;
    statusEl.style.color = isErr ? 'var(--err)' : 'var(--ok)';
    statusEl.style.display = 'block';
  };
  try {
    const mergedEntries = [];
    for (const file of files) {
      if (file.name.toLowerCase().endsWith('.json')) {
        const parsed = JSON.parse(await file.text());
        if (!parsed || !Array.isArray(parsed.files)) {
          throw new Error(`"${file.name}" is not a valid SWBF2 save backup JSON.`);
        }
        for (const entry of parsed.files) {
          if (entry && typeof entry.name === 'string' && typeof entry.dataBase64 === 'string') {
            mergedEntries.push(entry);
          }
        }
      } else {
        const raw = new Uint8Array(await file.arrayBuffer());
        mergedEntries.push({
          name: file.name,
          bytes: raw.byteLength,
          dataBase64: uint8ToBase64(raw),
        });
      }
    }
    if (!mergedEntries.length) {
      throw new Error('No valid save files found in the selected file(s).');
    }
    const res = await callWorker('vfsOp', {
      op: 'importSaves',
      bundle: { version: 1, files: mergedEntries },
    });
    const vs = res.vfsStats || {};
    updateVfsDrawer(vs);
    if ($('pfCacheInfo')) {
      $('pfCacheInfo').textContent = `${vs.cachedBlocks || 0} blocks · ${(vs.saveFiles || []).length} saves`;
    }
    const n = res.imported || mergedEntries.length;
    setStatus(`Imported ${n} save file${n === 1 ? '' : 's'}! Available in Profile Management / Load.`);
    return res;
  } catch (err) {
    console.error('[saves] import failed:', err);
    setStatus(`Import failed: ${err && err.message ? err.message : err}`, true);
    return { imported: 0, error: String(err) };
  }
}

// Restore persisted resolution preference into both launcher and drawer selectors.
try {
  const savedRes = localStorage.getItem('swbf2_resolution');
  if (savedRes) {
    if ($('selInitRes') && [...$('selInitRes').options].some((o) => o.value === savedRes)) {
      $('selInitRes').value = savedRes;
    }
    if ($('selRes') && [...$('selRes').options].some((o) => o.value === savedRes)) {
      $('selRes').value = savedRes;
    }
  }
} catch {}

async function applySelectedResolution(resStr) {
  const { width, height } = parseRes(resStr);
  const key = `${width}x${height}`;
  if ($('selInitRes') && [...$('selInitRes').options].some((o) => o.value === key)) $('selInitRes').value = key;
  if ($('selRes') && [...$('selRes').options].some((o) => o.value === key)) $('selRes').value = key;
  try { localStorage.setItem('swbf2_resolution', key); } catch {}
  if (!booted) return null;
  const res = await callWorker('interactiveAction', { action: 'resize', width, height });
  const st = res?.snapshot?.status;
  if (st?.screenWidth > 0 && st?.screenHeight > 0) {
    inputWriter?.setGameSize(st.screenWidth, st.screenHeight);
  }
  if ($('resStatusMsg')) {
    $('resStatusMsg').textContent = (st?.state === 'mission')
      ? `Applied ${width} × ${height}`
      : `Saved ${width} × ${height} (applies in battle)`;
  }
  return res;
}

async function copyInviteToClipboard(btnEl) {
  if (!lastInviteUrl) return;
  try {
    if (navigator.clipboard) {
      await navigator.clipboard.writeText(lastInviteUrl);
    } else {
      window.prompt('Copy invite URL:', lastInviteUrl);
    }
    const prev = btnEl.textContent;
    btnEl.textContent = 'Copied!';
    setTimeout(() => { btnEl.textContent = prev; }, 1500);
  } catch {
    window.prompt('Copy invite URL:', lastInviteUrl);
  }
}

// ---- Wire Controls ----
$('btnStart').addEventListener('click', () => launch());
$('btnPlayOnline').addEventListener('click', showLobbyView);
$('btnBackMain').addEventListener('click', showMainView);
$('btnOpenCreate').addEventListener('click', showCreateLobbyView);
$('btnBackLobbies').addEventListener('click', showLobbyView);
$('btnToggleAdv').addEventListener('click', () => $('advPanel').classList.toggle('open'));

$('btnRefreshRooms').addEventListener('click', refreshRooms);
$('inpLobbySearch').addEventListener('input', refreshRooms);
for (const tab of $('lobbyFilterTabs').querySelectorAll('.filter-tab')) {
  tab.addEventListener('click', () => {
    for (const t of $('lobbyFilterTabs').querySelectorAll('.filter-tab')) t.classList.remove('active');
    tab.classList.add('active');
    activeLobbyFilter = tab.dataset.filter || 'all';
    refreshRooms();
  });
}

$('selEraFilter').addEventListener('change', populateMapDropdowns);
$('selModeFilter').addEventListener('change', populateMapDropdowns);
$('btnAddMap').addEventListener('click', () => {
  const m = $('selMpMap').value;
  if (m && customPlaylist.length < 32) {
    customPlaylist.push(m);
    renderPlaylistChips();
  }
});
$('btnAddAllMatching').addEventListener('click', () => {
  for (const opt of $('selMpMap').options) {
    if (customPlaylist.length >= 32) break;
    if (!customPlaylist.includes(opt.value)) customPlaylist.push(opt.value);
  }
  renderPlaylistChips();
});
$('btnClearPlaylist').addEventListener('click', () => {
  customPlaylist = [];
  renderPlaylistChips();
});

$('btnHostLaunch').addEventListener('click', () => {
  const c = readCustomLobbyConfig();
  launch({
    hostRoom: c.roomId,
    hostTitle: c.title,
    mpMap: c.map,
    playlist: c.playlist,
    mpConfig: c.config,
    password: c.config.password,
    autoStart: false,
  });
});
$('btnHostLaunchNow').addEventListener('click', () => {
  const c = readCustomLobbyConfig();
  launch({
    hostRoom: c.roomId,
    hostTitle: c.title,
    mpMap: c.map,
    playlist: c.playlist,
    mpConfig: c.config,
    password: c.config.password,
    autoStart: true,
  });
});

// Staging lobby controls
$('btnOverlayStartMatch').addEventListener('click', triggerHostLaunchMatch);
$('btnOverlayLeave').addEventListener('click', () => leaveMultiplayerLobby({ returnToBrowser: true }));
$('btnLeaveMpHud').addEventListener('click', () => leaveMultiplayerLobby({ returnToBrowser: true }));
$('btnOverlayInvite').addEventListener('click', () => copyInviteToClipboard($('btnOverlayInvite')));
$('btnOverlayVoice').addEventListener('click', cycleVoiceMode);
$('btnVoiceHud').addEventListener('click', cycleVoiceMode);
$('lobbyChatForm').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const inp = $('inpLobbyChat');
  const txt = inp.value.trim();
  if (txt && netClient) {
    netClient.sendChat(txt);
    inp.value = '';
  }
});

$('selInitRes')?.addEventListener('change', () => {
  applySelectedResolution($('selInitRes').value).catch(() => {});
});
$('btnLauncherExport').addEventListener('click', exportSavesFile);
$('btnLauncherImport')?.addEventListener('click', () => $('fileLauncherImportSaves')?.click());
$('fileLauncherImportSaves')?.addEventListener('change', async (ev) => {
  await importSavesFromFiles(ev.target.files, $('saveLauncherStatus'));
  ev.target.value = '';
});
$('btnLauncherClear').addEventListener('click', async () => {
  const res = await callWorker('vfsOp', { op: 'clearCache' });
  updateVfsDrawer(res.vfsStats);
  await runPreflight();
});

$('btnDrawer').addEventListener('click', async () => {
  drawer.classList.toggle('open');
  touchControls?.setState({ overlayBlocked: drawer.classList.contains('open') });
  if (drawer.classList.contains('open')) {
    const res = await callWorker('vfsOp', { op: 'stats' });
    updateVfsDrawer(res.vfsStats);
  }
});
async function returnToPortalMain() {
  drawer.classList.remove('open');
  if (document.pointerLockElement === canvas) {
    if (inputWriter) inputWriter.exitPointerLock();
    else { try { document.exitPointerLock?.(); } catch {} }
  }
  if (activeMpSession || netClient) {
    await leaveMultiplayerLobby({ returnToBrowser: false });
  }
  if (booted) {
    await callWorker('interactiveAction', { action: 'returnToShell' }).catch(() => {});
  }
  closeLoadingOverlay();
  hideLoadBanner();
  launcher.style.display = 'flex';
  $('btnStart').disabled = false;
  $('btnPlayOnline').disabled = false;
  showMainView();
  applyAudioGain('shell');
}

$('btnShell').addEventListener('click', () => { returnToPortalMain(); });
$('btnTat').addEventListener('click', () => booted && callWorker('interactiveAction', { action: 'launchMission', script: 'tat2g_con' }));
$('btnEli').addEventListener('click', () => booted && callWorker('interactiveAction', { action: 'launchMission', script: 'tat2g_eli' }));
$('btnSpa').addEventListener('click', () => booted && callWorker('interactiveAction', { action: 'launchMission', script: 'spa1g_ass' }));
$('btnQuickMapGo').addEventListener('click', () => {
  const script = $('selQuickMap').value;
  if (booted && script) callWorker('interactiveAction', { action: 'launchMission', script });
});
$('btnBackToLobbies').addEventListener('click', () => {
  drawer.classList.remove('open');
  leaveMultiplayerLobby({ returnToBrowser: true });
});

$('selRes').addEventListener('change', () => {
  applySelectedResolution($('selRes').value).catch(() => {});
});
$('btnApplyRes').addEventListener('click', () => {
  applySelectedResolution($('selRes').value).catch(() => {});
});

$('btnHostMp').addEventListener('click', () => {
  const room = ($('inpRoomDrawer').value || 'mos_eisley').trim();
  if (room) {
    drawer.classList.remove('open');
    connectMultiplayer({ role: 'host', room, title: room, map: 'tat2g_con' });
  }
});
$('btnJoinMp').addEventListener('click', () => {
  const room = ($('inpRoomDrawer').value || 'mos_eisley').trim();
  if (room) {
    drawer.classList.remove('open');
    connectMultiplayer({ role: 'client', room, title: room });
  }
});
$('btnLaunchMp').addEventListener('click', triggerHostLaunchMatch);
$('btnCopyInvite').addEventListener('click', () => copyInviteToClipboard($('btnCopyInvite')));

$('btnExportSaves').addEventListener('click', exportSavesFile);
$('btnImportSaves').addEventListener('click', () => $('fileImportSaves').click());
$('fileImportSaves').addEventListener('change', async (ev) => {
  await importSavesFromFiles(ev.target.files, $('saveDrawerStatus'));
  ev.target.value = '';
});
$('btnClearCache').addEventListener('click', async () => {
  const res = await callWorker('vfsOp', { op: 'clearCache' });
  updateVfsDrawer(res.vfsStats);
});

$('btnMute').addEventListener('click', () => {
  if (!audioSink) return;
  muted = !muted;
  audioSink.setMuted(muted);
  lastAppliedGain = -1;
  applyAudioGain(lastStatus?.status?.state || 'shell');
  $('btnMute').textContent = muted ? 'Unmute' : 'Mute';
});
function syncFullscreenUi() {
  const isFs = !!(document.fullscreenElement || document.webkitFullscreenElement);
  $('stage')?.classList.toggle('is-fullscreen', isFs);
  if (isFs) {
    if (drawer.classList.contains('open')) {
      drawer.classList.remove('open');
      touchControls?.setState({ overlayBlocked: false });
    }
    // Lock Escape in fullscreen (Chromium Keyboard Lock API) so tapping Esc pauses/resumes
    // SWBF2 in-game (e.g. to Respawn) instead of kicking the player out of fullscreen.
    // Holding Esc (~1.5s) still exits fullscreen per browser spec.
    if (navigator.keyboard && typeof navigator.keyboard.lock === 'function') {
      navigator.keyboard.lock(['Escape']).catch(() => {});
    }
  } else {
    if (navigator.keyboard && typeof navigator.keyboard.unlock === 'function') {
      try { navigator.keyboard.unlock(); } catch {}
    }
  }
}
document.addEventListener('fullscreenchange', syncFullscreenUi);
document.addEventListener('webkitfullscreenchange', syncFullscreenUi);

$('btnFs').addEventListener('click', () => {
  const fsEl = document.fullscreenElement || document.webkitFullscreenElement;
  if (!fsEl) {
    const stageEl = $('stage');
    if (stageEl.requestFullscreen) stageEl.requestFullscreen().catch(() => {});
    else if (stageEl.webkitRequestFullscreen) stageEl.webkitRequestFullscreen();
  } else {
    if (document.exitFullscreen) document.exitFullscreen().catch(() => {});
    else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
  }
});
window.addEventListener('keydown', (ev) => {
  if (ev.code === 'F8' && booted) {
    hudBar.classList.toggle('show-diag');
  }
  if (ev.code === 'KeyV' && !ev.repeat && voiceMode === 'ptt' && netClient &&
      document.activeElement?.tagName !== 'INPUT' && document.activeElement?.tagName !== 'TEXTAREA') {
    netClient.setVoiceMuted(false);
    updateVoiceButtons(true);
  }
});
window.addEventListener('keyup', (ev) => {
  if (ev.code === 'KeyV' && voiceMode === 'ptt' && netClient) {
    netClient.setVoiceMuted(true);
    updateVoiceButtons(false);
  }
});

// Expose clean automation hooks for browser/phase7_release_check.mjs, byoc_check.mjs & portal E2E checks.
window.__playApi = {
  runPreflight,
  refreshRooms,
  showLobbyView,
  showCreateLobbyView,
  showMainView,
  launch,
  triggerHostLaunchMatch,
  leaveMultiplayerLobby,
  callWorker,
  mountLocalEntries: async (entries, rootName = 'GameData') => {
    const idx = buildByocIndex(entries, rootName);
    return applyByocIndex(idx);
  },
  getState: () => ({
    booted,
    bootMs: window.__playBootMs || 0,
    fps: currentFps,
    status: lastStatus,
    preflight: window.__playPreflight || null,
    rooms: window.__playRooms || [],
    activeMpSession,
    stagingOpen: mpLobbyOverlay.classList.contains('open'),
    loadingOpen: loadingOverlay.classList.contains('open'),
    loadEventCount,
    lastLoadProgress,
    touchControls: touchControls ? touchControls.getState() : null,
    byoc: {
      required: isByocRequired(),
      serverGamedataHosted,
      ready: !!(byocIndex && byocIndex.valid),
      fileCount: byocIndex ? byocIndex.fileCount : 0,
      mapCount: byocIndex ? byocIndex.mapCount : 0,
    },
  }),
};

populateMapDropdowns();
renderPlaylistChips();
initByocState().then(() => runPreflight());
refreshRooms();
if (location.hash === '#lobbies') {
  showLobbyView();
}
