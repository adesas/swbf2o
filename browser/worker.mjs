// Dedicated Worker hosting the SWBF2 WASM engine + OPFS on-demand GameData streaming VFS.
//
// In Phase 1, this worker owns:
//   - OpfsVfs (256 KiB sparse block store over HTTP Range + OPFS SyncAccessHandle + /saves)
//   - Emscripten SWBF2 module (loaded via SWBF2 factory from build/browser/swbf2.js)
// In Phase 2+, this same worker also owns the transferred OffscreenCanvas + WebGPU device +
// SharedArrayBuffer input/audio rings and drives the 30 Hz sim / display-rate render loop.

import { OpfsVfs } from './vfs_opfs.mjs';
import { InputBridgeReader } from './input_bridge.mjs';
import { AudioRingWriter } from './webaudio.mjs';
import { WorkerNetBridge } from './net_client.mjs';

let vfs = null;
let M = null;
let interactive = null;
let pendingLocalFiles = null;

function emitLoadProgress(payload) {
  self.postMessage({ type: 'loadProgress', ts: performance.now(), ...payload });
}

async function fetchWithProgress(url, { phase, label, step, totalSteps }) {
  const res = await fetch(url, { cache: 'no-store' });
  if (!res.ok) throw new Error(`Failed to fetch ${url}: HTTP ${res.status}`);
  const xTotal = Number(res.headers.get('x-total-size') || 0);
  const hasEncoding = !!res.headers.get('content-encoding');
  const cl = (!hasEncoding ? Number(res.headers.get('content-length') || 0) : 0);
  const total = xTotal > 0 ? xTotal : (cl > 0 ? cl : 0);

  if (!res.body || typeof res.body.getReader !== 'function') {
    const buf = new Uint8Array(await res.arrayBuffer());
    emitLoadProgress({ phase, label, loaded: buf.byteLength, total: buf.byteLength, step, totalSteps });
    return buf;
  }

  const reader = res.body.getReader();
  const chunks = [];
  let loaded = 0;
  let lastEmit = 0;
  emitLoadProgress({ phase, label, loaded: 0, total, step, totalSteps });

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    if (value && value.byteLength) {
      chunks.push(value);
      loaded += value.byteLength;
      const now = performance.now();
      if (now - lastEmit >= 50) {
        lastEmit = now;
        emitLoadProgress({ phase, label, loaded, total: total >= loaded ? total : 0, step, totalSteps });
      }
    }
  }

  const out = new Uint8Array(loaded);
  let pos = 0;
  for (const c of chunks) {
    out.set(c, pos);
    pos += c.byteLength;
  }
  emitLoadProgress({ phase, label, loaded, total: loaded, step, totalSteps });
  return out;
}

async function ensureBooted({ manifestUrl = '/browser/gamedata-manifest.json', baseUrl = '/GameData/', localFiles = null, clearCache = false, canvas = null } = {}) {
  if (localFiles) {
    pendingLocalFiles = localFiles;
  }
  const effectiveLocal = localFiles || pendingLocalFiles;

  const onVfsProgress = (info) => {
    emitLoadProgress({
      phase: 'gamedata',
      label: `Downloading ${info.file}`,
      file: info.file,
      fileSize: info.fileSize,
      fileHttpBytes: info.fileHttpBytes,
      totalHttpBytes: info.totalHttpBytes,
      httpRequests: info.httpRequests,
      cachedBytes: info.cachedBytes,
      stage: info.stage,
    });
  };
  if (!vfs) {
    emitLoadProgress({
      phase: 'manifest',
      label: effectiveLocal ? 'Mounting local GameData folder…' : 'Loading game asset catalog…',
      step: 1,
      totalSteps: 5,
    });
    vfs = new OpfsVfs({ manifestUrl, baseUrl, localFiles: effectiveLocal, onProgress: onVfsProgress });
    await vfs.init();
  } else {
    if (effectiveLocal && vfs.mode !== 'local') {
      await vfs.setLocalFiles(effectiveLocal);
    } else if (!effectiveLocal && vfs.mode === 'saves-only') {
      await vfs.init();
    }
    vfs.setOnProgress(onVfsProgress);
  }
  if (clearCache) {
    vfs.clearCache();
  }
  if (!M) {
    // Load build/browser/swbf2.js (UMD/MODULARIZE global SWBF2 factory) and stream swbf2.wasm
    // with real chunk-by-chunk byte progress before compiling via Emscripten's factory.
    emitLoadProgress({ phase: 'js', label: 'Downloading engine runtime…', step: 2, totalSteps: 5 });
    const jsBytes = await fetchWithProgress('/build/browser/swbf2.js', {
      phase: 'js',
      label: 'Downloading engine runtime…',
      step: 2,
      totalSteps: 5,
    });
    const jsCode = new TextDecoder().decode(jsBytes);
    const factory = new Function(`${jsCode}\nreturn SWBF2;`)();

    const wasmBytes = await fetchWithProgress('/build/browser/swbf2.wasm', {
      phase: 'wasm',
      label: 'Downloading Battlefront II engine…',
      step: 2,
      totalSteps: 5,
    });

    emitLoadProgress({
      phase: 'compile',
      label: 'Compiling WebAssembly engine…',
      loaded: wasmBytes.byteLength,
      total: wasmBytes.byteLength,
      step: 3,
      totalSteps: 5,
    });

    const modOpts = {
      locateFile: (f) => '/build/browser/' + f,
      wasmBinary: wasmBytes,
      print: (txt) => { console.log('[swbf2]', txt); self.postMessage({ type: 'log', text: '[swbf2] ' + txt }); },
      printErr: (txt) => { console.warn('[swbf2:err]', txt); self.postMessage({ type: 'log', text: '[swbf2:err] ' + txt }); },
      onAbort: (what) => {
        const st = new Error('abort: ' + what).stack;
        self.postMessage({ type: 'log', text: '[swbf2:abort] ' + st });
      },
    };
    if (canvas) {
      modOpts.canvas = canvas;
    }
    M = await factory(modOpts);
    M.onLoadBanner = (payload) => {
      if (!payload) return;
      const transfers = payload.rgba?.buffer ? [payload.rgba.buffer] : [];
      self.postMessage({ type: 'loadBanner', ...payload }, transfers);
    };
    vfs.attachToModule(M);
  } else if (canvas) {
    M.canvas = canvas;
  }
  return { vfs, M };
}

async function sha256Hex(uint8) {
  const digest = await crypto.subtle.digest('SHA-256', uint8);
  return Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, '0')).join('');
}

self.onmessage = async (ev) => {
  const { id, cmd, opts = {} } = ev.data || {};
  try {
    if (cmd === 'runPhase1Check') {
      const t0 = performance.now();
      await ensureBooted({ clearCache: !!opts.clearCache });
      vfs.resetStats();

      // Verify zero whole-file copies in MEMFS /data
      let memfsEntries = [];
      try {
        memfsEntries = M.FS.readdir('/data').filter((n) => n !== '.' && n !== '..');
      } catch {
        memfsEntries = [];
      }

      // Initialize headless host (no WebGL context inside Phase 1 worker check)
      M.ccall('swbf2_init', 'number', ['number'], [0]);

      // 1. Load tat/tat2.lvl through the engine's real LoadUtil::ReadDataFileOnHeap (plus core.lvl fonts)
      const levelOk = M.ccall('swbf2_load_level', 'number', ['string'], ['tat/tat2']) === 1;
      const collisionMeshes = M.ccall('swbf2_collision_meshes', 'number');

      // 2. Ground probe at (0, 0) and (25, 25)
      const probeGround = (x, z) => {
        const ptr = M._malloc(16);
        const ok = M.ccall('swbf2_ground_probe_result', 'number',
          ['number', 'number', 'number', 'number', 'number', 'number'],
          [x, z, ptr, ptr + 4, ptr + 8, ptr + 12]) === 1;
        const res = {
          ok,
          terrain: M.getValue(ptr, 'float'),
          ground: M.getValue(ptr + 4, 'float'),
          rest: M.getValue(ptr + 8, 'float'),
          steps: M.getValue(ptr + 12, 'i32'),
        };
        M._free(ptr);
        return res;
      };
      const g0 = probeGround(0, 0);
      const g25 = probeGround(25, 25);

      // 3. Load base classes from ingame.lvl (14.3 MB) and rep_inf_ep3_rifleman straight out of the
      //    RAW 148 MB retail side/rep.lvl — sparse block streaming skips unused classes!
      const baseOk = M.ccall('swbf2_load_class', 'number', ['string'], ['ingame.lvl;com_inf_default']) === 1;
      const classOk = M.ccall('swbf2_load_class', 'number', ['string'], ['side/rep.lvl;rep_inf_ep3_rifleman']) === 1;

      // 4. Spawn the Republic trooper via the engine's real EntitySoldierClass::Build + 60 updates
      const outPtr = M._malloc(32);
      const spawnOk = M.ccall('swbf2_spawn_soldier', 'number',
        ['string', 'number', 'number', 'number', 'number', 'number', 'number', 'number', 'number',
         'number', 'number', 'number', 'number'],
        ['rep_inf_ep3_rifleman', 0, g0.rest, 0, 0,
         outPtr, outPtr + 4, outPtr + 8, outPtr + 12, outPtr + 16,
         outPtr + 20, outPtr + 24, outPtr + 28]) === 1;
      for (let i = 0; i < 60; ++i) M.ccall('swbf2_update', 'number', ['number'], [1 / 30]);
      const spawnState = {
        x: M.getValue(outPtr, 'float'),
        y: M.getValue(outPtr + 4, 'float'),
        z: M.getValue(outPtr + 8, 'float'),
        health: M.getValue(outPtr + 12, 'float'),
        maxHealth: M.getValue(outPtr + 16, 'float'),
        classWeapons: M.getValue(outPtr + 20, 'i32'),
        builtWeapons: M.getValue(outPtr + 24, 'i32'),
        collisionBodies: M.getValue(outPtr + 28, 'i32'),
      };
      M._free(outPtr);

      // 5. Stream force_push_fire (0x6aa27e0e) out of the RAW 78 MB retail sound/common.bnk
      const audioOk = M.ccall('audio_browser_load_sample_by_name', 'number', ['string', 'string'],
                              ['sound/common.bnk', 'force_push_fire']) === 1;
      const audioFrames = M.ccall('audio_browser_sample_frames', 'number');
      const audioRate = M.ccall('audio_browser_sample_rate', 'number');
      const audioChannels = M.ccall('audio_browser_sample_channels', 'number');
      const audioBytes = M.ccall('audio_browser_sample_bytes', 'number');
      let audioSha256 = '';
      if (audioOk && audioFrames > 0) {
        const ptr = M._malloc(audioFrames * 2);
        const drained = M.ccall('audio_browser_drain_pcm', 'number', ['number', 'number'], [ptr, audioFrames]);
        const pcmCopy = M.HEAPU8.slice(ptr, ptr + drained * 2);
        M._free(ptr);
        audioSha256 = await sha256Hex(pcmCopy);
      }

      // 6. Test OPFS persistent /saves read/write via real PblFile
      let saveReadBack = '';
      if (opts.writeSaveText) {
        M.ccall('swbf2_vfs_write_save', 'number', ['string', 'string'], ['/saves/profile_test.sav', opts.writeSaveText]);
      }
      saveReadBack = M.ccall('swbf2_vfs_read_save', 'string', ['string'], ['/saves/profile_test.sav']);

      const elapsedMs = Math.round(performance.now() - t0);
      const vfsStats = vfs.getStats();

      // Close OPFS SyncAccessHandles cleanly if caller is about to terminate and spawn a warm worker
      if (opts.closeHandlesOnExit) {
        vfs.closeHandles();
      }

      self.postMessage({
        id,
        ok: true,
        elapsedMs,
        memfsEntries,
        levelOk,
        collisionMeshes,
        ground0: g0,
        ground25: g25,
        baseOk,
        classOk,
        spawnOk,
        spawnState,
        audio: {
          ok: audioOk,
          rate: audioRate,
          channels: audioChannels,
          bytes: audioBytes,
          frames: audioFrames,
          pcmSha256: audioSha256,
        },
        saveReadBack,
        vfsStats,
      });
      return;
    }

    if (cmd === 'runPhase2RenderCheck') {
      const t0 = performance.now();
      const width = opts.width || 640;
      const height = opts.height || 360;
      await ensureBooted({ canvas: opts.canvas || null });
      vfs.resetStats();

      // 1. Async WebGPU initialization against the transferred OffscreenCanvas (!canvas).
      if (navigator.gpu && typeof navigator.gpu.requestAdapter === 'function') {
        for (let attempt = 0; attempt < 10; ++attempt) {
          try {
            const ad = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
            if (ad) break;
          } catch {}
          await new Promise((r) => setTimeout(r, 200));
        }
      }
      self.postMessage({ type: 'log', text: `[worker] navigator.gpu=${!!navigator.gpu} canvas=${!!M.canvas} GPUDevice=${typeof GPUDevice}` });
      const wgpuReady = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('swbf2_wgpu_init timed out')), 15000);
        M.onWgpuReady = (jsonStr) => {
          clearTimeout(timer);
          try { resolve(JSON.parse(jsonStr)); } catch (e) { reject(e); }
        };
        M.ccall('swbf2_wgpu_init', null, ['number', 'number'], [width, height]);
      });
      if (!wgpuReady.ok) {
        self.postMessage({ id, ok: false, stage: 'wgpu_init', wgpuReady });
        return;
      }

      // 2. Boot real retail RedRenderer + FLRenderer + RedTexture + RedFont + SkyManager.
      const initOk = M.ccall('swbf2_engine_render_init', 'number', ['number', 'number'], [width, height]) === 1;
      if (!initOk) {
        self.postMessage({ id, ok: false, stage: 'engine_render_init', wgpuReady });
        return;
      }

      // 3. Load core.lvl + common.lvl + ingame.lvl + tat/tat2.lvl + side/rep.lvl;rep_inf_ep3_rifleman.
      const tLoad0 = performance.now();
      const loadOk = M.ccall('swbf2_engine_render_load_tat2', 'number') === 1;
      const loadMs = Math.round(performance.now() - tLoad0);
      if (!loadOk) {
        self.postMessage({ id, ok: false, stage: 'engine_render_load_tat2', wgpuReady });
        return;
      }

      const readbackFrame = () => new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('swbf2_wgpu_readback_frame timed out')), 15000);
        M.onWgpuReadback = (jsonStr, pixels, w, h) => {
          clearTimeout(timer);
          try {
            resolve({ meta: JSON.parse(jsonStr), pixels, width: w, height: h });
          } catch (e) {
            reject(e);
          }
        };
        M.ccall('swbf2_wgpu_readback_frame', null, [], []);
      });

      // 4. Render Camera Pose A (courtyard looking at spawned Republic trooper + Mos Eisley buildings).
      for (let i = 0; i < 3; ++i) {
        M.ccall('swbf2_engine_render_frame', 'number', ['number'], [1 / 60]);
      }
      const rbA = await readbackFrame();

      // 4b. Hide trooper on the exact same Camera Pose A to measure pixels owned by the trooper.
      M.ccall('swbf2_engine_render_set_trooper_visible', null, ['number'], [0]);
      for (let i = 0; i < 2; ++i) {
        M.ccall('swbf2_engine_render_frame', 'number', ['number'], [1 / 60]);
      }
      const rbAHidden = await readbackFrame();
      M.ccall('swbf2_engine_render_set_trooper_visible', null, ['number'], [1]);

      // 5. Render Camera Pose B (elevated Mos Eisley vista looking across town + terrain + sky).
      M.ccall('swbf2_engine_render_set_camera', null,
        ['number', 'number', 'number', 'number', 'number', 'number'],
        [22.0, 14.0, -24.0, 0.0, 2.0, 12.0]);
      for (let i = 0; i < 3; ++i) {
        M.ccall('swbf2_engine_render_frame', 'number', ['number'], [1 / 60]);
      }
      const rbB = await readbackFrame();

      // Compute pixel differences (Camera A vs Camera B, and Trooper visible vs hidden on Camera A).
      const computeDiffStats = (px1, px2) => {
        let diffPx = 0;
        let minX = width, minY = height, maxX = -1, maxY = -1;
        const totalPx = width * height;
        if (px1 && px2 && px1.length === px2.length) {
          for (let y = 0; y < height; ++y) {
            for (let x = 0; x < width; ++x) {
              const i = (y * width + x) * 4;
              const dr = Math.abs(px1[i] - px2[i]);
              const dg = Math.abs(px1[i + 1] - px2[i + 1]);
              const db = Math.abs(px1[i + 2] - px2[i + 2]);
              if (dr + dg + db > 15) {
                ++diffPx;
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
              }
            }
          }
        }
        return {
          pct: totalPx > 0 ? Number(((100 * diffPx) / totalPx).toFixed(2)) : 0,
          diffPx,
          box: diffPx > 0 ? [minX, minY, maxX, maxY] : null,
        };
      };
      const frameDiffPct = computeDiffStats(rbA.pixels, rbB.pixels).pct;
      const trooperDiff = computeDiffStats(rbA.pixels, rbAHidden.pixels);
      const trooperDiffPct = trooperDiff.pct;

      const stats = JSON.parse(M.ccall('swbf2_engine_render_stats', 'string'));
      const shaderValidation = JSON.parse(M.ccall('swbf2_wgpu_validate_all_shaders', 'string'));
      const lastWgpuError = M.ccall('swbf2_wgpu_last_error', 'string');
      const vfsStats = vfs.getStats();
      const elapsedMs = Math.round(performance.now() - t0);

      if (opts.closeHandlesOnExit) {
        vfs.closeHandles();
      }

      const transfers = [];
      if (rbA.pixels?.buffer) transfers.push(rbA.pixels.buffer);
      if (rbB.pixels?.buffer) transfers.push(rbB.pixels.buffer);

      self.postMessage({
        id,
        ok: true,
        elapsedMs,
        loadMs,
        wgpuReady,
        initOk,
        loadOk,
        stats,
        shaderValidation,
        lastWgpuError,
        frameA: rbA.meta,
        frameAHidden: rbAHidden.meta,
        frameB: rbB.meta,
        frameDiffPct,
        trooperDiffPct,
        trooperDiffPx: trooperDiff.diffPx,
        trooperDiffBox: trooperDiff.box,
        pixelsA: rbA.pixels ? rbA.pixels.buffer : null,
        pixelsB: rbB.pixels ? rbB.pixels.buffer : null,
        width,
        height,
        vfsStats,
      }, transfers);
      return;
    }

    if (cmd === 'runPhase4GameStateCheck') {
      const t0 = performance.now();
      const width = opts.width || 800;
      const height = opts.height || 600;
      await ensureBooted({ canvas: opts.canvas || null });
      vfs.resetStats();

      if (navigator.gpu && typeof navigator.gpu.requestAdapter === 'function') {
        for (let attempt = 0; attempt < 10; ++attempt) {
          try {
            const ad = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
            if (ad) break;
          } catch {}
          await new Promise((r) => setTimeout(r, 200));
        }
      }
      const wgpuReady = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('swbf2_wgpu_init timed out')), 15000);
        M.onWgpuReady = (jsonStr) => {
          clearTimeout(timer);
          try { resolve(JSON.parse(jsonStr)); } catch (e) { reject(e); }
        };
        M.ccall('swbf2_wgpu_init', null, ['number', 'number'], [width, height]);
      });
      if (!wgpuReady.ok) {
        self.postMessage({ id, ok: false, stage: 'wgpu_init', wgpuReady });
        return;
      }

      const readbackFrame = () => new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('swbf2_wgpu_readback_frame timed out')), 15000);
        M.onWgpuReadback = (jsonStr, pixels, w, h) => {
          clearTimeout(timer);
          try {
            resolve({ meta: JSON.parse(jsonStr), pixels, width: w, height: h });
          } catch (e) {
            reject(e);
          }
        };
        M.ccall('swbf2_wgpu_readback_frame', null, [], []);
      });
      const getStatus = () => JSON.parse(M.ccall('swbf2_gamestate_status', 'string'));
      const tickN = (n) => {
        for (let i = 0; i < n; ++i) {
          M.ccall('swbf2_gamestate_tick', 'number', ['number'], [1 / 60]);
        }
      };
      const guiProbe = () => JSON.parse(M.ccall('swbf2_gamestate_gui_probe', 'string'));
      const logGuiProbe = (where) => {
        self.postMessage({ type: 'log', text: `[gui-probe] ${where} ${M.ccall('swbf2_gamestate_gui_probe', 'string')}` });
      };
      const tapKey = (dik, holdTicks = 3, afterTicks = 25) => {
        M.ccall('swbf2_input_set_key', null, ['number', 'number'], [dik, 1]);
        tickN(holdTicks);
        M.ccall('swbf2_input_set_key', null, ['number', 'number'], [dik, 0]);
        tickN(afterTicks);
      };
      const clickAt = (x, y, holdTicks = 3, afterTicks = 20) => {
        M.ccall('swbf2_input_set_cursor', null, ['number', 'number', 'number'], [x, y, 0]);
        tickN(3);
        M.ccall('swbf2_input_set_cursor', null, ['number', 'number', 'number'], [x, y, 1]);
        tickN(holdTicks);
        M.ccall('swbf2_input_set_cursor', null, ['number', 'number', 'number'], [x, y, 0]);
        tickN(afterTicks);
      };
      // Debug aid only (see swbf2_wgpu_debug_set_clear_colour): a dim forced scene clear turns
      // "nothing was drawn here" holes into that colour, so a black surface can be told apart from a
      // hole. The colour has to stay dim: the engine's HDREffect bloom bright-pass saturates a bright
      // clear and washes out the whole frame, which destroys the very thing being measured.
      // Debug aid: hide a range of draw ids (see swbf2_wgpu_debug_skip_draws). The mission flow sets
      // it right before the hero-assault spawn, where the flat black panel is reproduced.
      if (Array.isArray(opts.debugSkipDraws)) {
        M.ccall('swbf2_wgpu_debug_skip_draws', null, ['number', 'number'],
                [opts.debugSkipDraws[0] | 0, opts.debugSkipDraws[1] | 0]);
        self.postMessage({ type: 'log', text: `[debug] draws [${opts.debugSkipDraws[0]}, ${opts.debugSkipDraws[1]}) hidden` });
      }
      // Debug aid: capture per-draw records (no shader changes) so ids seen in a screenshot can be
      // looked up with swbf2_wgpu_debug_draw_record_json.
      if (opts.debugRecordDraws) {
        M.ccall('swbf2_wgpu_debug_record_draws', null, ['number'], [1]);
      }
      if (opts.debugPsMode) {
        M.ccall('swbf2_wgpu_debug_set_ps_mode', null, ['number'], [opts.debugPsMode | 0]);
        self.postMessage({ type: 'log', text: `[debug] pixel shader mode ${opts.debugPsMode | 0} (1 = show stage-0 UVs, 2 = opaque white)` });
      }
      if (opts.debugClearColour) {
        const c = opts.debugClearColour;
        M.ccall('swbf2_wgpu_debug_set_clear_colour', null, ['number', 'number', 'number', 'number'], [1, c[0], c[1], c[2]]);
        self.postMessage({ type: 'log', text: `[debug] scene clear forced to (${c[0]}, ${c[1]}, ${c[2]}) - undrawn regions read that colour` });
      }

      const DIK_RETURN = 0x1c;
      const DIK_DOWN = 0xd0;

      // 1. Boot into native GameState::ShellState (core.lvl + common.lvl + shell.lvl + shell_interface).
      const tBoot0 = performance.now();
      const bootOk = M.ccall('swbf2_gamestate_boot', 'number', ['number', 'number', 'string'],
                             [width, height, '']) === 1;
      const bootMs = Math.round(performance.now() - tBoot0);
      if (!bootOk) {
        self.postMessage({ id, ok: false, stage: 'gamestate_boot', wgpuReady, status: getStatus() });
        return;
      }

      // Tick Shell for 60 frames (~1.0 s) so initial ifs_login screen settles.
      tickN(60);
      const shellStatus = getStatus();
      const rbShell = await readbackFrame();
      self.postMessage({ type: 'log', text: `[phase4] shell screen=${shellStatus.currentScreen} draws=${shellStatus.draws} movieOpen=${shellStatus.movieOpen}` });

      // 2. Drive ifs_login via real DirectInput8 DIK_RETURN so SaveDevice persists the player profile
      //    into OPFS /saves/savegames/ and transitions through ifs_saveop into the PC main menu
      //    (ifs_sp_campaign / ifs_main) naturally.
      const isShellMenu = (sc) => sc === 'ifs_main' || sc === 'ifs_sp_campaign';
      tapKey(DIK_RETURN, 4, 20);
      for (let wait = 0; wait < 20; ++wait) {
        if (isShellMenu(getStatus().currentScreen)) break;
        tickN(15);
      }
      let curScreenAfterLogin = getStatus().currentScreen;
      self.postMessage({ type: 'log', text: `[phase4] after DIK_RETURN login screen=${curScreenAfterLogin} probe=${M.ccall('swbf2_gamestate_gui_probe', 'string')}` });
      if (!isShellMenu(curScreenAfterLogin)) {
        tapKey(DIK_RETURN, 4, 40);
        for (let wait = 0; wait < 10 && !isShellMenu(getStatus().currentScreen); ++wait) tickN(15);
        curScreenAfterLogin = getStatus().currentScreen;
      }
      const savedProfiles = vfs.listSaves('/saves/savegames/');

      // Start retail shell.mvs segment 0 ("cor1tran1") via ScriptCB_PlayMovie and decode real Bink1 frames.
      M.ccall('swbf2_gamestate_lua_exec', 'number', ['string'], [
        'if ScriptCB_PlayMovie then ScriptCB_PlayMovie("cor1tran1", 1, "", 0, 0, 800, 600) end'
      ]);
      for (let i = 0; i < 3; ++i) {
        M.ccall('swbf2_gamestate_tick', 'number', ['number'], [1 / 60]);
        await new Promise((r) => setTimeout(r, 35));
      }
      const rbMainEarly = await readbackFrame();
      for (let i = 0; i < 10; ++i) {
        M.ccall('swbf2_gamestate_tick', 'number', ['number'], [1 / 60]);
        await new Promise((r) => setTimeout(r, 35));
      }
      tickN(24);
      const mainStatus = getStatus();
      const rbMain = await readbackFrame();
      let binkChangedPx = 0;
      if (rbMainEarly.pixels && rbMain.pixels && rbMainEarly.pixels.length === rbMain.pixels.length) {
        for (let i = 0; i < rbMain.pixels.length; i += 4) {
          const dr = Math.abs(rbMain.pixels[i] - rbMainEarly.pixels[i]);
          const dg = Math.abs(rbMain.pixels[i + 1] - rbMainEarly.pixels[i + 1]);
          const db = Math.abs(rbMain.pixels[i + 2] - rbMainEarly.pixels[i + 2]);
          if (dr + dg + db > 12) ++binkChangedPx;
        }
      }
      const binkAnimatedPct = (rbMain.meta?.width && rbMain.meta?.height)
        ? +((binkChangedPx * 100) / (rbMain.meta.width * rbMain.meta.height)).toFixed(2)
        : 0;
      self.postMessage({ type: 'log', text: `[phase4] main screen=${mainStatus.currentScreen} draws=${mainStatus.draws} moviePlaying=${mainStatus.moviePlaying} movieFrame=${mainStatus.movieFrame}/${mainStatus.movieFrames} ${mainStatus.movieWidth}x${mainStatus.movieHeight} animPct=${binkAnimatedPct}% saves=${JSON.stringify(savedProfiles)}` });

      const spawnIntoMission = (scriptName) => {
        const tM0 = performance.now();
        M.ccall('swbf2_gamestate_launch_mission', 'number', ['string'], [scriptName]);
        tickN(45);
        // Step A: On ifs_sideselect1 (spawnMode === 0), move real DirectInput8 cursor onto Team 1
        // hotspot and click to select Team 1 -> eSPAWNDISPLAY.
        let st = getStatus();
        if (st.spawnMode === 0 || st.currentScreen === 'ifs_sideselect1') {
          const tx = (st.sideTeam1X >= 0 ? st.sideTeam1X : 540) + 40;
          const ty = (st.sideTeam1Y >= 0 ? st.sideTeam1Y : 292) + 2;
          clickAt(tx, ty, 3, 20);
          st = getStatus();
        }
        // Step B: On eSPAWNDISPLAY (spawnMode === 2), click the real "SPAWN" hotspot (m_AcceptBox) via DirectInput8 mouse.
        if (!st.playerAlive && st.spawnAcceptX >= 0 && st.spawnAcceptY >= 0) {
          clickAt(st.spawnAcceptX, st.spawnAcceptY, 3, 20);
          st = getStatus();
        }
        // Tick until SpawnDisplay's launch timer expires and the local player spawns into the 3D world.
        for (let wait = 0; wait < 20 && !st.playerAlive; ++wait) {
          tickN(15);
          st = getStatus();
          if (!st.playerAlive && st.spawnMode === 2 && st.spawnAcceptX >= 0) {
            clickAt(st.spawnAcceptX, st.spawnAcceptY, 3, 15);
            st = getStatus();
          }
        }
        // Let live gameplay settle for 30 frames (0.5 s) so HUD + 3D world + AI combat render cleanly.
        tickN(30);
        const ms = Math.round(performance.now() - tM0);
        return { ms, status: getStatus() };
      };

      // 3. Instant Action Ground Conquest: tat2g_con (Mos Eisley GCW) — spawn into live gameplay.
      const tatRun = spawnIntoMission('tat2g_con');
      const tatMs = tatRun.ms;
      const tatStatus = tatRun.status;
      const rbTat = await readbackFrame();
      self.postMessage({ type: 'log', text: `[phase4] tat2g_con state=${tatStatus.state} screen='${tatStatus.currentScreen}' draws=${tatStatus.draws} tris=${tatStatus.triangles} skinned=${tatStatus.skinnedDraws}/${tatStatus.skinnedTriangles}` });

      // 4. Clean round-trip: MissionState::Exit() -> ShellState::Enter().
      const tRet0 = performance.now();
      M.ccall('swbf2_gamestate_return_to_shell', 'number');
      tickN(20);
      const retMs = Math.round(performance.now() - tRet0);
      const returnShellStatus = getStatus();
      self.postMessage({ type: 'log', text: `[phase4] returned to shell state=${returnShellStatus.state} screen=${returnShellStatus.currentScreen}` });

      // 5. Instant Action Hero Assault: tat2g_eli (Mos Eisley Heroes vs Villains) — spawn into live gameplay.
      const eliRun = spawnIntoMission('tat2g_eli');
      const eliMs = eliRun.ms;
      const eliStatus = eliRun.status;
      const rbEli = await readbackFrame();

      // Characterise the black regions of this mission, one sample per sim tick. tat2g_eli is
      // reproducible across runs (identical pixel statistics every time), unlike tat2g_con, whose
      // spawn settles at a different camera pose per run - so before/after comparisons of the same
      // screen region are only meaningful here.
      const classifyFrame = ({ pixels, width, height }) => {
        const c = opts.debugClearColour || [0.3, 0.0, 0.3];
        const tr = Math.round(c[0] * 255), tg = Math.round(c[1] * 255), tb = Math.round(c[2] * 255);
        let black = 0, forced = 0, n = 0;
        for (let i = 0; i + 3 < pixels.length; i += 4) {
          const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
          n++;
          if (r + g + b < 30) ++black;
          if (Math.abs(r - tr) < 40 && Math.abs(g - tg) < 40 && Math.abs(b - tb) < 40) ++forced;
        }
        return { blackPct: +(100 * black / n).toFixed(2), forcedClearPct: +(100 * forced / n).toFixed(2) };
      };
      if (opts.debugRecordDraws) {
        for (let id = (opts.debugRecordRange?.[0] ?? 0); id <= (opts.debugRecordRange?.[1] ?? 60); ++id) {
          const rec = M.ccall('swbf2_wgpu_debug_draw_record_json', 'string', ['number'], [id]);
          if (rec !== '{}') self.postMessage({ type: 'log', text: `[debug] draw ${id} ${rec}` });
        }
        self.postMessage({ type: 'log', text: `[debug] frame textures ${M.ccall('swbf2_wgpu_debug_frame_texture_summary', 'string')}` });
      }
      const eliSamples = [];
      for (let i = 0; i < 12; ++i) {
        tickN(1);
        const s = await readbackFrame();
        eliSamples.push({ f: i, draws: s.meta.draws, tris: s.meta.triangles, ...classifyFrame(s) });
        self.postMessage({ type: 'log', text: `[debug] eli sample ${JSON.stringify(eliSamples[i])}` });
      }

      // ps mode 4 paints each draw with its own id (r + 256*g), so a pixel of interest names the draw
      // that produced it; the same id names the record logged here with the full submitted state.
      if ((opts.debugPsMode | 0) === 4) {
        self.postMessage({ type: 'log', text: `[debug] frame textures ${M.ccall('swbf2_wgpu_debug_frame_texture_summary', 'string')}` });
        // Read the ids out of a frame captured now, so they match the records of the frame that is
        // still current when the lookup runs (the sampler above has already advanced the sim).
        const rbProbe = await readbackFrame();
        const probePts = opts.debugProbePoints || [[400, 470], [300, 480], [520, 460], [700, 470], [400, 400]];
        for (const [px, py] of probePts) {
          const idx = (py * rbProbe.width + px) * 4;
          const r = rbProbe.pixels[idx], g = rbProbe.pixels[idx + 1], b = rbProbe.pixels[idx + 2];
          const id = r + 256 * g + 65536 * b;
          const rec = M.ccall('swbf2_wgpu_debug_draw_record_json', 'string', ['number'], [id]);
          self.postMessage({ type: 'log', text: `[debug] pixel(${px},${py}) rgb=(${r},${g},${b}) drawId=${id} record=${rec}` });
        }
      }
      self.postMessage({ type: 'log', text: `[phase4] tat2g_eli state=${eliStatus.state} screen='${eliStatus.currentScreen}' draws=${eliStatus.draws} tris=${eliStatus.triangles} skinned=${eliStatus.skinnedDraws}/${eliStatus.skinnedTriangles}` });

      // 6. Second clean round-trip -> Space Assault: spa1g_ass (Space Yavin) — spawn into live gameplay.
      M.ccall('swbf2_gamestate_return_to_shell', 'number');
      tickN(20);
      const spaRun = spawnIntoMission('spa1g_ass');
      const spaMs = spaRun.ms;
      const spaStatus = spaRun.status;
      const rbSpa = await readbackFrame();
      self.postMessage({ type: 'log', text: `[phase4] spa1g_ass state=${spaStatus.state} screen='${spaStatus.currentScreen}' draws=${spaStatus.draws} tris=${spaStatus.triangles} skinned=${spaStatus.skinnedDraws}/${spaStatus.skinnedTriangles}` });

      const vfsStats = vfs.getStats();
      const elapsedMs = Math.round(performance.now() - t0);
      if (opts.closeHandlesOnExit) vfs.closeHandles();

      const transfers = [];
      if (rbShell.pixels?.buffer) transfers.push(rbShell.pixels.buffer);
      if (rbMain.pixels?.buffer) transfers.push(rbMain.pixels.buffer);
      if (rbTat.pixels?.buffer) transfers.push(rbTat.pixels.buffer);
      if (rbEli.pixels?.buffer) transfers.push(rbEli.pixels.buffer);
      if (rbSpa.pixels?.buffer) transfers.push(rbSpa.pixels.buffer);

      self.postMessage({
        id,
        ok: true,
        elapsedMs,
        bootMs,
        tatMs,
        retMs,
        eliMs,
        spaMs,
        wgpuReady,
        bootOk,
        curScreenAfterLogin,
        savedProfiles,
        shellStatus,
        frameShell: rbShell.meta,
        mainStatus,
        binkAnimatedPct,
        frameMain: rbMain.meta,
        tatStatus,
        frameTat: rbTat.meta,
        returnShellStatus,
        eliStatus,
        frameEli: rbEli.meta,
        spaStatus,
        frameSpa: rbSpa.meta,
        pixelsShell: rbShell.pixels ? rbShell.pixels.buffer : null,
        pixelsMain: rbMain.pixels ? rbMain.pixels.buffer : null,
        pixelsTat: rbTat.pixels ? rbTat.pixels.buffer : null,
        pixelsEli: rbEli.pixels ? rbEli.pixels.buffer : null,
        pixelsSpa: rbSpa.pixels ? rbSpa.pixels.buffer : null,
        width,
        height,
        vfsStats,
      }, transfers);
      return;
    }

    if (cmd === 'setLocalGameData') {
      pendingLocalFiles = opts.localFiles || null;
      if (vfs && pendingLocalFiles) {
        await vfs.setLocalFiles(pendingLocalFiles);
      } else if (!vfs && pendingLocalFiles) {
        vfs = new OpfsVfs({ localFiles: pendingLocalFiles });
        await vfs.init();
      }
      self.postMessage({
        id,
        ok: true,
        fileCount: vfs ? (vfs.localFiles ? vfs.localFiles.size : 0) : 0,
        vfsStats: vfs ? vfs.getStats() : null,
      });
      return;
    }

    if (cmd === 'startInteractive') {
      if (interactive) {
        interactive.stop();
        interactive = null;
      }
      const width = opts.width || 800;
      const height = opts.height || 600;
      await ensureBooted({ canvas: opts.canvas || null, localFiles: opts.localFiles || null });
      vfs.resetStats();

      emitLoadProgress({ phase: 'webgpu', label: 'Initializing WebGPU graphics…', step: 3, totalSteps: 5 });
      if (navigator.gpu && typeof navigator.gpu.requestAdapter === 'function') {
        for (let attempt = 0; attempt < 10; ++attempt) {
          try {
            const ad = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
            if (ad) break;
          } catch {}
          await new Promise((r) => setTimeout(r, 200));
        }
      }
      const wgpuReady = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('swbf2_wgpu_init timed out')), 15000);
        M.onWgpuReady = (jsonStr) => {
          clearTimeout(timer);
          try { resolve(JSON.parse(jsonStr)); } catch (e) { reject(e); }
        };
        M.ccall('swbf2_wgpu_init', null, ['number', 'number'], [width, height]);
      });
      if (!wgpuReady.ok) {
        self.postMessage({ id, ok: false, stage: 'wgpu_init', wgpuReady });
        return;
      }

      if (opts.audioEnabled === false) {
        M.ccall('swbf2_audio_set_enabled', null, ['number'], [0]);
      } else {
        M.ccall('swbf2_audio_set_enabled', null, ['number'], [1]);
      }
      const audioSampleRate = (opts.audioSampleRate && opts.audioSampleRate >= 22050 && opts.audioSampleRate <= 96000)
        ? (opts.audioSampleRate | 0) : 48000;
      if (typeof M._swbf2_audio_set_sample_rate === 'function') {
        M._swbf2_audio_set_sample_rate(audioSampleRate);
      }
      const musicStreaming = opts.musicStreaming !== false;
      vfs.setMusicStreaming(musicStreaming);
      if (typeof M._swbf2_audio_set_music_streaming === 'function') {
        M._swbf2_audio_set_music_streaming(musicStreaming ? 1 : 0);
      }

      emitLoadProgress({
        phase: 'boot',
        label: opts.initialMission ? `Streaming battlefield (${opts.initialMission})…` : 'Streaming game shell & menus…',
        step: 4,
        totalSteps: 5,
      });
      const bootOk = M.ccall('swbf2_gamestate_boot', 'number', ['number', 'number', 'string'],
                             [width, height, opts.initialMission || '']) === 1;
      if (!bootOk) {
        self.postMessage({ id, ok: false, stage: 'gamestate_boot', wgpuReady });
        return;
      }

      // Helper to cleanly sign into a retail SWBF2 profile via DirectInput8 DIK_RETURN so
      // multiplayer lobbies never sit on ifs_login (Profile Management) or lack a saved profile.
      const signInRetailProfile = () => {
        const DIK_RETURN = 0x1c;
        const stepN = (n) => {
          for (let i = 0; i < n; ++i) M._swbf2_gamestate_tick(1 / 60);
        };
        const getSt = () => JSON.parse(M.ccall('swbf2_gamestate_status', 'string'));
        const isShellMenu = (sc) => sc === 'ifs_main' || sc === 'ifs_sp_campaign';
        let st = getSt();
        if (st.state !== 'shell' || isShellMenu(st.currentScreen)) return st;
        // Let ifs_login finish opening.
        stepN(45);
        st = getSt();
        for (let attempt = 0; attempt < 3 && !isShellMenu(st.currentScreen) && st.state === 'shell'; ++attempt) {
          if (st.currentScreen === 'ifs_login') {
            M._swbf2_input_set_key(DIK_RETURN, 1);
            stepN(4);
            M._swbf2_input_set_key(DIK_RETURN, 0);
          }
          for (let wait = 0; wait < 20; ++wait) {
            stepN(15);
            st = getSt();
            if (isShellMenu(st.currentScreen)) break;
          }
        }
        // Finish any trailing ifs_saveop tick.
        for (let wait = 0; wait < 15 && st.currentScreen === 'ifs_saveop'; ++wait) {
          stepN(10);
          st = getSt();
        }
        return st;
      };

      if (opts.autoLogin && !opts.initialMission) {
        emitLoadProgress({ phase: 'login', label: 'Signing into player profile…', step: 5, totalSteps: 5 });
        const signedSt = signInRetailProfile();
        self.postMessage({
          type: 'log',
          text: `[worker] autoLogin complete: screen=${signedSt.currentScreen} profileSignedIn=${signedSt.profileSignedIn}`,
        });
      }

      const inputReader = opts.inputSab ? new InputBridgeReader(opts.inputSab, M) : null;
      const audioWriter = opts.audioSab
        ? new AudioRingWriter(opts.audioSab, opts.audioCapacityFrames || 16384)
        : null;
      let netBridge = (opts.netTxSab && opts.netRxSab && opts.localIpInt)
        ? new WorkerNetBridge({ txSab: opts.netTxSab, rxSab: opts.netRxSab, module: M, localIpInt: opts.localIpInt })
        : null;

      const maxDrain = 4096;
      const pcmLPtr = M._malloc(maxDrain * 4);
      const pcmRPtr = M._malloc(maxDrain * 4);
      let running = true;
      let timerId = 0;
      let rafId = 0;
      let lastTickMs = performance.now();
      let lastStatusPostMs = 0;
      let frameCount = 0;
      let stereoDiffAccum = 0;
      let stereoDiffSamples = 0;

      const scheduleNextFrame = (frameStartMs) => {
        if (!running) return;
        if (typeof self.requestAnimationFrame === 'function') {
          rafId = self.requestAnimationFrame(stepFrame);
        } else {
          const elapsed = performance.now() - frameStartMs;
          const waitMs = Math.max(1, Math.round(16 - elapsed));
          timerId = setTimeout(stepFrame, waitMs);
        }
      };

      const drainAudioToRing = () => {
        if (!audioWriter) return;
        for (let iter = 0; iter < 4; ++iter) {
          const drained = M._swbf2_audio_drain_stereo(pcmLPtr, pcmRPtr, maxDrain);
          if (drained <= 0) break;
          // Refresh Float32Array views each call in case WASM memory grew.
          const buf = M.HEAPU8.buffer;
          const viewL = new Float32Array(buf, pcmLPtr, drained);
          const viewR = new Float32Array(buf, pcmRPtr, drained);
          for (let i = 0; i < drained; ++i) {
            const d = Math.abs(viewL[i] - viewR[i]);
            if (d > 1e-6) stereoDiffAccum += d;
          }
          stereoDiffSamples += drained;
          audioWriter.write(viewL, viewR, drained);
          if (drained < maxDrain) break;
        }
      };

      const targetRingFrames = Math.max(1024, Math.min(2560, Math.round(audioSampleRate * 0.036)));
      const stepFrame = () => {
        if (!running) return;
        const now = performance.now();
        const rawDt = (now - lastTickMs) / 1000;
        lastTickMs = now;
        const dt = Math.max(1 / 240, Math.min(1 / 20, rawDt || 1 / 60));

        if (inputReader) inputReader.sync();
        // Closed-loop audio clock sync: mix only the frames needed to maintain a tight ~36 ms
        // cushion in the AudioWorklet SAB ring. Eliminates clock drift, 144/240 Hz overruns,
        // 44.1 kHz mismatch crackles, and stale 341 ms buffer bloat.
        if (audioWriter && typeof M._swbf2_audio_set_next_mix_frames === 'function') {
          const queued = audioWriter.getStats().queuedFrames;
          const deficit = targetRingFrames - queued;
          const mixFrames = deficit >= 128 ? Math.min(deficit, 3072) : 0;
          M._swbf2_audio_set_next_mix_frames(mixFrames);
        }
        try {
          M._swbf2_gamestate_tick(dt);
        } catch (tickErr) {
          running = false;
          self.postMessage({ type: 'log', text: `[worker] tick exception: ${tickErr && tickErr.stack ? tickErr.stack : tickErr}` });
          return;
        }
        drainAudioToRing();
        ++frameCount;

        if (now - lastStatusPostMs >= 100 || frameCount === 1) {
          lastStatusPostMs = now;
          const status = JSON.parse(M.ccall('swbf2_gamestate_status', 'string'));
          if (inputReader) inputReader.setPointerLockWanted(!!status.pointerLockWanted);
          const audioStats = JSON.parse(M.ccall('swbf2_audio_stats', 'string'));
          const netStatus = netBridge ? JSON.parse(M.ccall('swbf2_net_status', 'string')) : null;
          self.postMessage({
            type: 'interactiveStatus',
            status,
            audioStats,
            netStatus,
            ringStats: audioWriter ? audioWriter.getStats() : null,
            stereoMeanDiff: stereoDiffSamples > 0 ? +(stereoDiffAccum / stereoDiffSamples).toFixed(6) : 0,
            frames: frameCount,
          });
        }

        if (running) {
          scheduleNextFrame(now);
        }
      };

      interactive = {
        inputReader,
        audioWriter,
        attachNet({ txSab, rxSab, localIpInt }) {
          if (netBridge) netBridge.dispose();
          netBridge = new WorkerNetBridge({ txSab, rxSab, module: M, localIpInt });
          return netBridge;
        },
        detachNet() {
          if (netBridge) {
            netBridge.dispose();
            netBridge = null;
          }
        },
        getSnapshot() {
          const status = JSON.parse(M.ccall('swbf2_gamestate_status', 'string'));
          if (inputReader) inputReader.setPointerLockWanted(!!status.pointerLockWanted);
          const audioStats = JSON.parse(M.ccall('swbf2_audio_stats', 'string'));
          const netStatus = netBridge ? JSON.parse(M.ccall('swbf2_net_status', 'string')) : null;
          return {
            status,
            audioStats,
            netStatus,
            ringStats: audioWriter ? audioWriter.getStats() : null,
            stereoMeanDiff: stereoDiffSamples > 0 ? +(stereoDiffAccum / stereoDiffSamples).toFixed(6) : 0,
            frames: frameCount,
            vfsStats: vfs.getStats(),
          };
        },
        resetAudioMetrics() {
          M.ccall('swbf2_audio_reset_stats', null, [], []);
          stereoDiffAccum = 0;
          stereoDiffSamples = 0;
        },
        stop() {
          running = false;
          if (timerId) { clearTimeout(timerId); timerId = 0; }
          if (rafId && typeof self.cancelAnimationFrame === 'function') { self.cancelAnimationFrame(rafId); rafId = 0; }
          if (netBridge) { netBridge.dispose(); netBridge = null; }
          if (inputReader) inputReader.dispose();
          if (pcmLPtr) M._free(pcmLPtr);
          if (pcmRPtr) M._free(pcmRPtr);
        },
      };

      // Discard any stale audio accumulated during boot/autoLogin so live playback starts
      // with zero backlog, then kick off the display-synced interactive loop.
      while (M._swbf2_audio_drain_stereo(pcmLPtr, pcmRPtr, maxDrain) > 0) {}
      if (audioWriter) audioWriter.reset();
      timerId = setTimeout(stepFrame, 0);

      emitLoadProgress({ phase: 'ready', label: 'Ready!', pct: 100, step: 5, totalSteps: 5 });

      self.postMessage({
        id,
        ok: true,
        wgpuReady,
        snapshot: interactive.getSnapshot(),
      });
      return;
    }

    if (cmd === 'interactiveAction') {
      if (!interactive || !M) {
        self.postMessage({ id, ok: false, error: 'interactive session not running' });
        return;
      }
      const action = opts.action;
      if (action === 'launchMission') {
        emitLoadProgress({ phase: 'map_start', label: 'Loading battlefield…', script: opts.script || 'tat2g_con' });
        M.ccall('swbf2_gamestate_launch_mission', 'number', ['string'], [opts.script || 'tat2g_con']);
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'returnToShell') {
        M.ccall('swbf2_gamestate_return_to_shell', 'number');
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'resetAudioMetrics') {
        interactive.resetAudioMetrics();
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'getSnapshot') {
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'resize') {
        M.ccall('swbf2_gamestate_set_resolution', null, ['number', 'number'], [opts.width | 0, opts.height | 0]);
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'setMusicStreaming') {
        const en = !!opts.enabled;
        vfs.setMusicStreaming(en);
        if (typeof M._swbf2_audio_set_music_streaming === 'function') {
          M._swbf2_audio_set_music_streaming(en ? 1 : 0);
        }
        self.postMessage({ id, ok: true, musicStreaming: en, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'attachNet') {
        interactive.attachNet(opts);
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'hostLan') {
        const rc = M.ccall('swbf2_net_host_lan', 'number', ['string', 'string', 'string'],
                           [opts.roomName || 'SWBF2-Web', opts.mission || 'tat2g_con', opts.playerName || 'Host']);
        self.postMessage({ id, ok: rc === 1, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'hostLanConfig') {
        const rc = M.ccall('swbf2_net_host_lan_config', 'number',
                           ['string', 'string', 'string', 'string'],
                           [
                             opts.roomName || 'SWBF2-Web',
                             opts.playlistCsv || opts.mission || 'tat2g_con',
                             opts.playerName || 'Host',
                             opts.configKv || '',
                           ]);
        self.postMessage({ id, ok: rc === 1, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'setPassword') {
        M.ccall('swbf2_net_set_password', 'number', ['string'], [opts.password || '']);
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'findLan') {
        if (opts.password !== undefined) {
          M.ccall('swbf2_net_set_password', 'number', ['string'], [opts.password || '']);
        }
        const rc = M.ccall('swbf2_net_find_lan', 'number', ['string'], [opts.playerName || 'Client']);
        self.postMessage({ id, ok: rc === 1, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'joinFound') {
        const rc = M.ccall('swbf2_net_join_found', 'number', ['number'], [opts.index | 0]);
        self.postMessage({ id, ok: rc === 1, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'launchMatch') {
        emitLoadProgress({ phase: 'map_start', label: 'Launching multiplayer match…' });
        const rc = M.ccall('swbf2_net_launch_match', 'number', [], []);
        self.postMessage({ id, ok: rc === 1, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'disconnectNet') {
        interactive.detachNet();
        M.ccall('swbf2_net_disconnect', 'number', [], []);
        self.postMessage({ id, ok: true, snapshot: interactive.getSnapshot() });
        return;
      }
      if (action === 'getNetStatus') {
        self.postMessage({ id, ok: true, netStatus: JSON.parse(M.ccall('swbf2_net_status', 'string')) });
        return;
      }
      if (action === 'readbackFrame') {
        const rb = await new Promise((resolve, reject) => {
          const timer = setTimeout(() => reject(new Error('swbf2_wgpu_readback_frame timed out')), 15000);
          M.onWgpuReadback = (jsonStr, pixels, w, h) => {
            clearTimeout(timer);
            try { resolve({ meta: JSON.parse(jsonStr), pixels, width: w, height: h }); }
            catch (e) { reject(e); }
          };
          M.ccall('swbf2_wgpu_readback_frame', null, [], []);
        });
        const transfers = rb.pixels?.buffer ? [rb.pixels.buffer] : [];
        self.postMessage({
          id,
          ok: true,
          meta: rb.meta,
          width: rb.width,
          height: rb.height,
          pixels: rb.pixels ? rb.pixels.buffer : null,
        }, transfers);
        return;
      }
      self.postMessage({ id, ok: false, error: `unknown interactiveAction: ${action}` });
      return;
    }

    if (cmd === 'stopInteractive') {
      if (interactive) {
        interactive.stop();
        interactive = null;
      }
      if (opts.closeHandlesOnExit) vfs?.closeHandles();
      self.postMessage({ id, ok: true });
      return;
    }

    if (cmd === 'checkOpfsSaves') {
      await ensureBooted({});
      const saves = vfs.listSaves(opts.prefix || '/saves/savegames/');
      if (opts.closeHandlesOnExit) vfs.closeHandles();
      self.postMessage({ id, ok: true, saves });
      return;
    }

    if (cmd === 'vfsOp') {
      if (!vfs) {
        vfs = new OpfsVfs({
          manifestUrl: opts.manifestUrl || '/browser/gamedata-manifest.json',
          baseUrl: opts.baseUrl || '/GameData/',
          localFiles: pendingLocalFiles,
        });
        if (pendingLocalFiles) {
          await vfs.init();
        } else {
          try {
            await vfs.init();
          } catch {
            await vfs.init({ savesOnly: true });
          }
        }
      }
      const op = opts.op;
      if (op === 'stats') {
        self.postMessage({ id, ok: true, vfsStats: vfs.getStats() });
        return;
      }
      if (op === 'clearCache') {
        vfs.clearCache();
        self.postMessage({ id, ok: true, vfsStats: vfs.getStats() });
        return;
      }
      if (op === 'clearSaves') {
        vfs.clearSaves();
        self.postMessage({ id, ok: true, vfsStats: vfs.getStats() });
        return;
      }
      if (op === 'exportSaves') {
        self.postMessage({ id, ok: true, bundle: vfs.exportSaves(), vfsStats: vfs.getStats() });
        return;
      }
      if (op === 'importSaves') {
        const imported = vfs.importSaves(opts.bundle);
        self.postMessage({ id, ok: true, imported, vfsStats: vfs.getStats() });
        return;
      }
      self.postMessage({ id, ok: false, error: `unknown vfsOp: ${op}` });
      return;
    }

    if (cmd === 'closeHandles') {
      vfs?.closeHandles();
      self.postMessage({ id, ok: true });
      return;
    }

    self.postMessage({ id, ok: false, error: `unknown command: ${cmd}` });
  } catch (err) {
    try { vfs?.closeHandles(); } catch {}
    self.postMessage({ id, ok: false, error: String(err && err.stack ? err.stack : err) });
  }
};
