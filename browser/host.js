// Browser host: owns startup, streams GameData into the wasm VFS, drives the engine's fixed
// 30 Hz logic cadence from requestAnimationFrame, and mirrors the engine's journal into the
// page. The engine never touches the DOM and the page never touches wasm memory directly -
// everything crosses through the small swbf2_* surface in browser/main.cpp.
(() => {
  const trace = (where) => {
    (window.__demoSteps ||= []).push(where + '@' + Date.now());
    console.log('[demo] ' + where);
  };
  const $ = (id) => document.getElementById(id);
  const canvas = $('canvas');
  const caption = $('caption');
  const progress = $('progress');
  const note = $('note');
  const logEl = $('log');

  const say = (msg) => { caption.textContent = msg; };
  // HUD slots are only present for the view that created them, so a readout must not assume one
  // exists: the frame loop runs against whatever is currently on screen.
  const setHud = (id, text) => { const el = $(id); if (el) el.textContent = text; };
  const setProgress = (p) => { progress.style.width = `${Math.round(p * 100)}%`; };

  function fitCanvas() {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.max(64, Math.round(canvas.clientWidth * dpr));
    const h = Math.max(64, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
      if (M && missionReady) M.ccall('swbf2_set_viewport', 'number', ['number', 'number'], [w, h]);
    }
  }

  let M = null;            // wasm module
  let logSeen = 0;         // engine log lines already mirrored into the DOM
  let current = null;      // staged container path inside the wasm VFS
  let currentLabel = '';
  let texIndex = 0;
  let texMax = 0;
  let files = [];

  // ---- engine log mirror ---------------------------------------------------
  function pumpLog() {
    if (!M) return;
    for (;;) {
      const line = M.ccall('swbf2_log_line', 'string', ['number'], [logSeen]);
      if (!line) break;
      const div = document.createElement('div');
      const low = line.toLowerCase();
      div.className = low.includes('fail') || low.includes('error') || low.includes('not present')
        ? 'e'
        : low.includes('warn') ? 'w' : '';
      div.textContent = line;
      logEl.appendChild(div);
      logEl.scrollTop = logEl.scrollHeight;
      logSeen++;
      if (logSeen > 4000) break;
    }
  }

  // ---- module bootstrap ----------------------------------------------------
  async function boot() {
    say('starting wasm…');
    M = await SWBF2({
      canvas,
      locateFile: (f) => 'build/browser/' + f,
      onAbort: (why) => { say('engine aborted: ' + why); },
    });
    M.FS.mkdir('/data');
    // Match the drawing buffer to the CSS box (device pixel ratio capped at 2): a canvas left
    // at its 300x150 default stretches into a blur, and the engine sizes its viewport from
    // the drawing buffer.
    fitCanvas();
    addEventListener('resize', fitCanvas);

    if (params.get('probe') === '1') {
      M.ccall('swbf2_set_probe', null, ['number'], [3]);  // log the chunk layout of 3 textures
    }
    M.ccall('swbf2_init', 'number', ['number'], [1]);
    window.__swbf2 = M;

    // The engine uploads DXT/S3TC straight away; in WebGL an extension is only *enabled*
    // once someone asks for it on that context, and the engine deliberately stays off the
    // DOM. Asking here is the host's job - it is the half that knows about the browser.
    const shared = canvas.getContext('webgl2');
    const s3tcEnabled = shared &&
      (shared.getExtension('WEBGL_compressed_texture_s3tc') ||
       shared.getExtension('WEBGL_compressed_texture_s3tc_srgb'));
    trace('host: s3tc extension ' + (s3tcEnabled ? 'enabled' : 'UNAVAILABLE'));

    // debugging/automation surface: the headless check pages through textures the same way
    // a user does rather than reaching into engine internals
    window.__showTexture = showTexture;
    window.__probe = probe;
    window.__stage = stage;
    window.__stageClass = stageClass;
    window.__loadClass = loadClass;
    window.__buildMontage = buildMontage;
    window.__scene3D = scene3D;
    window.__buildModels = buildModels;
    window.__modelStats = () => ({
      models: M.ccall('swbf2_model_models', 'number'),
      segments: M.ccall('swbf2_model_segments', 'number'),
      vertices: M.ccall('swbf2_model_vertices', 'number'),
      indices: M.ccall('swbf2_model_indices', 'number'),
      drawDeclared: M.ccall('swbf2_model_draw_declared', 'number'),
      primsDrawn: M.ccall('swbf2_model_prims_drawn', 'number'),
      indicesDrawn: M.ccall('swbf2_model_indices_drawn', 'number'),
      oddPrimTypes: M.ccall('swbf2_model_odd_prim_types', 'number'),
      instances: M.ccall('swbf2_model_instances', 'number'),
      unresolvedInstances: M.ccall('swbf2_model_unresolved_instances', 'number'),
      placedSegments: M.ccall('swbf2_model_placed_segments', 'number'),
      placedPrimTotal: M.ccall('swbf2_model_placed_prim_total', 'number'),
      placedIndexTotal: M.ccall('swbf2_model_placed_index_total', 'number'),
      visited: M.ccall('swbf2_model_visited', 'number'),
      drawn: M.ccall('swbf2_model_drawn', 'number'),
      culled: M.ccall('swbf2_model_culled', 'number'),
      texturesResolved: M.ccall('swbf2_model_textures_resolved', 'number'),
      truncated: M.ccall('swbf2_model_truncated', 'number'),
      geometryOn: M.ccall('swbf2_geometry_on', 'number'),
    });
    window.__currentContainer = () => currentLabel;
    window.__modelSelfCheck = (limit) => {
      if (!current || !M) return null;
      const bad = M.ccall('swbf2_model_selfcheck', 'number', ['string', 'number'], [current, limit || 200]);
      return { bad, report: M.ccall('swbf2_last_report', 'string') };
    };
    window.__orbit = orbit;
    window.__startMission = startMission;
    window.__missionAudioState = () => missionAudio ? missionAudio.stats() : { started: false };
    window.__missionState = () => {
      if (!missionReady) return { ready: false };
      M.ccall('swbf2_trooper_state', 'number', ['number', 'number', 'number', 'number'],
        [missionStateFloat, 24, missionStateInt, 6]);
      const f = (i) => M.getValue(missionStateFloat + i * 4, 'float');
      return {
        ready: true,
        x: f(0), y: f(1), z: f(2),
        health: f(13),
        camX: f(14), camY: f(15), camZ: f(16),
        camDirX: f(17), camDirY: f(18), camDirZ: f(19),
        dirX: f(3), dirY: f(4), dirZ: f(5),
        fov: f(20),
        modelSegments: missionModelSegments,
        engineCamera: M.ccall('swbf2_engine_camera_on', 'number'),
        instances: M.ccall('swbf2_model_instances', 'number'),
        drawn: M.ccall('swbf2_model_drawn', 'number'),
        trooperDrawn: M.ccall('swbf2_trooper_drawn', 'number'),
      };
    };
    window.__missionDrive = (code, down) => {
      if (!missionInput) return false;
      if (down) missionInput.state.keys.add(code); else missionInput.state.keys.delete(code);
      return true;
    };
    window.__sceneStats = () => ({
      drawn: M.ccall('swbf2_scene_drawn', 'number'),
      culled: M.ccall('swbf2_scene_culled', 'number'),
      on: M.ccall('swbf2_is_3d', 'number'),
    });
    // The proof that perspective and the depth buffer are wired up: two known-colour quads at
    // two depths, nearer one first vs nearer one second. The two centre colours must differ and
    // each must be the nearer quad's colour.
    window.__depthOrderTest = () => ({
      redNear: M.ccall('swbf2_depth_order_test', 'number', ['number'], [1]),
      greenNear: M.ccall('swbf2_depth_order_test', 'number', ['number'], [0]),
    });
    // When the offscreen checks fail, the engine's own diagnosis of why (readable string).
    window.__fboProbe = () => ({
      code: M.ccall('swbf2_fbo_probe', 'number'),
      report: M.ccall('swbf2_last_report', 'string'),
    });
    window.__gpuStats = () => ({
      luma: M.ccall('swbf2_gpu_luma', 'number'),
      litPermille: M.ccall('swbf2_gpu_lit', 'number'),
      colors: M.ccall('swbf2_gpu_distinct', 'number'),
      center: M.ccall('swbf2_gpu_center_packed', 'number'),
      frames: M.ccall('swbf2_frames', 'number'),
      tex: M.ccall('swbf2_texture_packed', 'number'),
    });
    $('ver').textContent = M.ccall('swbf2_version', 'string');

    const gl = document.createElement('canvas');
    const s3tc = (() => {
      try {
        const g = gl.getContext('webgl2');
        return !!g && !!g.getExtension('WEBGL_compressed_texture_s3tc');
      } catch (e) { return false; }
    })();

    // wasmMemory is deliberately not exported - the page has no business poking engine
    // memory. The status line reports what the module actually is instead.
    setStat(0, `${(M.getExceptionMessage ? '' : '')}wasm module ready`);
    setStat(1, M.ccall('swbf2_engine_build', 'string'));
    setStat(2, 'WebGL2 ' + (s3tc ? '+ S3TC' : 'without S3TC'));
    say('engine ready — pick a .lvl on the right');
    pumpLog();
  }

  function setStat(i, text) {
    const cell = $('status').children[1 + i * 2];
    if (cell) cell.textContent = text;
  }

  // ---- game data streaming -------------------------------------------------
  async function loadManifest() {
    const res = await fetch('/browser/manifest.json');
    files = await res.json();
    const list = $('files');
    list.innerHTML = '';
    for (const f of files) {
      const row = document.createElement('div');
      row.className = 'row';
      row.innerHTML = `<span>${f.path}</span><span class="sz">${(f.size / 1048576).toFixed(1)} MB</span>`;
      row.onclick = () => stage(f);
      list.appendChild(row);
    }
    note.textContent = `${files.length} containers available; nothing is preloaded — files stream in on demand.`;
  }

  async function stage(f) {
    if (!M) return;
    say(`fetching ${f.path}…`);
    setProgress(0.02);
    // Extracted class records carry their own URL and VFS name. The VFS name is the original
    // side file's name on purpose: the engine still asks for "side/cis.lvl;cis_inf_rifleman",
    // but only that class's record has been downloaded.
    const url = f.url || '/GameData/' + f.path.split('/').map(encodeURIComponent).join('/');
    const res = await fetch(url);
    trace(`stage:fetch ${url} -> ${res.status}`);
    if (!res.ok) { say('fetch failed: ' + res.status); return; }
    const total = +res.headers.get('x-total-size') || f.size;
    const reader = res.body.getReader();
    const chunks = [];
    let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      got += value.length;
      setProgress(Math.min(0.98, got / total));
      say(`fetching ${f.path}… ${(got / 1048576).toFixed(1)} / ${(total / 1048576).toFixed(1)} MB`);
    }
    const buf = new Uint8Array(got);
    let off = 0;
    for (const c of chunks) { buf.set(c, off); off += c.length; }

    const fsPath = f.fsPath || '/data/' + f.path.replace(/\//g, '_');
    say(`writing ${f.path} to the engine filesystem…`);
    M.FS.writeFile(fsPath, buf);
    trace(`stage:wrote ${fsPath} ${buf.length} bytes -> ${JSON.stringify(M.FS.readdir('/data').slice(2))}`);
    current = fsPath;
    currentLabel = f.classFile || f.path;
    texIndex = 0;
    texMax = 0;
    setProgress(1);
    say(`${f.path} staged (${(got / 1048576).toFixed(1)} MB) — engine can open it`);
    note.innerHTML = `staged <b>${f.path}</b> at <code>${fsPath}</code>`;
    pumpLog();
    return fsPath;
  }

  // Stage exactly one class record, without pulling the rest of the faction's side file.
  // The manifest is generated by tools/gen_manifest.py from build/class_data.
  async function stageClass(classFile) {
    const f = files.find((entry) => entry.kind === 'class' && entry.classFile === classFile);
    if (!f) {
      say(`no extracted record for ${classFile}; run tools/extract_class_level.py`);
      return null;
    }
    return stage(f);
  }

  async function loadClass(classFile) {
    const [source] = classFile.split(';');
    // Faction classes use the small extracted record. Shared files such as ingame.lvl are
    // ordinary containers and are staged whole; if a class has no extracted record, the original
    // container is the honest fallback rather than a fabricated success.
    const staged = (await stageClass(classFile)) ||
      (await stage(files.find((entry) => entry.kind !== 'class' && entry.path === source) ||
                  { path: source, size: 0 }));
    if (!staged) return 0;
    const rc = M.ccall('swbf2_load_class', 'number', ['string'], [classFile]);
    say(`${classFile}: ${rc === 1 ? 'class loaded' : 'class load failed'}`);
    pumpLog();
    return rc;
  }

  // ---- actions -------------------------------------------------------------
  function probe() {
    if (!current) { say('stage a container first'); return; }
    const rc = M.ccall('swbf2_probe_file', 'number', ['string'], [current]);
    const rep = M.ccall('swbf2_last_report', 'string');
    note.innerHTML = rc === 0 ? `<b>engine walk:</b> ${rep}` : `<b style="color:#ff8a7a">${rep}</b>`;
    const m = rep.match(/(\d+) tex_/);
    if (m) texMax = +m[1];
    pumpLog();
  }

  function showTexture(dir) {
    if (!current) { say('stage a container first'); return; }
    // Asking for a texture means the model view is not what the user wants on screen; the two share
    // the same camera, so the mode has to be left explicitly rather than left running.
    if (M && M.ccall('swbf2_geometry_on', 'number')) M.ccall('swbf2_set_geometry', null, ['number'], [0]);
    if (!texMax) probe();
    if (!texMax) { say('no tex_ chunks in this container'); return; }
    // skip forward past encodings the port refuses, rather than showing a blank canvas and
    // making the user walk into them one keypress at a time
    M.ccall('swbf2_montage', null, ['number'], [0]);  // paging implies single view
    let tries = 0, rc = 0, rep = '';
    do {
      texIndex = (texIndex + dir + texMax) % texMax;
      rc = M.ccall('swbf2_show_texture', 'number', ['string', 'number'], [current, texIndex]);
      rep = M.ccall('swbf2_last_report', 'string');
    } while (rc !== 0 && ++tries < texMax);

    const enc = M.ccall('swbf2_texture_format_name', 'string') || '';
    if (rc === 0) {
      const packed = M.ccall('swbf2_texture_packed', 'number');
      const w = packed >> 16, h = packed & 0xffff;
      $('hTex').textContent = `${w}x${h} ${enc}`;
      note.innerHTML = `<b>texture ${texIndex + 1}/${texMax}</b> &middot; ${w}&times;${h} ${enc} ` +
        `&middot; read by the engine's own PblFile/PblChunk, uploaded to WebGL2`;
    } else {
      note.innerHTML = `<b style="color:#ff8a7a">texture ${texIndex + 1}/${texMax}:</b> ${rep}`;
    }
    pumpLog();
  }

  // Pull as many textures out of the staged container as the resident set holds, then let the
  // engine draw them all in one pass. This is the decoder's breadth on screen: DXT1, DXT3,
  // V8U8 bump maps, 32-bit art, all read through the engine's own file layer.
  function buildMontage() {
    // The flat views and the model view share the camera, so asking for a montage leaves the model
    // view: the mode has to be left explicitly rather than left running.
    if (M && M.ccall('swbf2_geometry_on', 'number')) M.ccall('swbf2_set_geometry', null, ['number'], [0]);
    if (!current) { say('stage a container first'); return; }
    if (!texMax) probe();
    if (!texMax) { say('no tex_ chunks in this container'); return; }
    M.ccall('swbf2_clear_cache', null);
    const encodes = new Set();
    let staged = 0, scanned = 0;
    for (let i = 0; i < texMax && staged < 24; ++i) {
      const rc = M.ccall('swbf2_stage_texture', 'number', ['string', 'number'], [current, i]);
      scanned++;
      if (rc >= 0) {
        staged++;
        const enc = M.ccall('swbf2_texture_format_name', 'string') || '?';
        encodes.add(enc);
      }
      if (scanned % 6 === 0) pumpLog();
    }
    M.ccall('swbf2_montage', null, ['number'], [1]);
    const shown = M.ccall('swbf2_montage_count', 'number');
    $('hTex').textContent = shown ? `${shown} resident` : '';
    note.innerHTML = `<b>montage</b> &middot; ${shown} textures from ${currentLabel} drawn at once ` +
      `&middot; encodings on screen: ${[...encodes].join(', ')}`;
    say(`${shown} real textures on screen — ← → returns to paging one at a time`);
    pumpLog();
  }

  function singleView() {
    if (!M) return;
    M.ccall('swbf2_montage', null, ['number'], [0]);
    if (M.ccall('swbf2_is_3d', 'number')) M.ccall('swbf2_set_3d', null, ['number'], [0]);
    const packed = M.ccall('swbf2_texture_packed', 'number');
    $('hTex').textContent = packed ? `${packed >> 16}x${packed & 0xffff} ${M.ccall('swbf2_texture_format_name', 'string') || ''}` : '';
    say('single texture view');
  }

  // ---- 3D view --------------------------------------------------------------
  // The same decoded textures, placed in world space and drawn through the engine's RedCamera.
  // The host's only job here is camera input: yaw/pitch/distance go into the engine as numbers,
  // and the engine answers with the counts its own frustum test produced.

  // Real model geometry: `modl`/`segm` decoded by the host's reader, drawn through the same camera.
  // Kept as a separate action from the texture gallery so the two can be compared, and so the
  // headless check can assert on the reader's own numbers rather than on a picture.
  function buildModels() {
    if (!M) return;
    if (!current) { say('stage a container first'); return; }
    const segments = M.ccall('swbf2_build_models', 'number', ['string'], [current]);
    const rep = M.ccall('swbf2_last_report', 'string');
    if (segments <= 0) { note.innerHTML = `<b style="color:#ff8a7a">${rep}</b>`; pumpLog(); return; }
    M.ccall('swbf2_set_3d', null, ['number'], [1]);
    M.ccall('swbf2_set_geometry', null, ['number'], [1]);
    note.innerHTML = `<b>model geometry</b> &middot; ${rep}<br>` +
      `<span id="hModel">drawn ${M.ccall('swbf2_model_drawn', 'number')}, ` +
      `culled ${M.ccall('swbf2_model_culled', 'number')}</span> &middot; ` +
      `drawn through the same RedCamera path as the texture view; key G toggles real geometry ` +
      `against the resident textures`;
    say('model geometry on');
    pumpLog();
  }

  function scene3D(on) {
    if (!M) return;
    M.ccall('swbf2_set_3d', null, ['number'], [on ? 1 : 0]);
    if (on) {
      const drawn = M.ccall('swbf2_scene_drawn', 'number');
      const culled = M.ccall('swbf2_scene_culled', 'number');
      note.innerHTML = `<b>3D view</b> &middot; resident textures placed in world space, transformed and ` +
        `culled by the engine's own RedCamera &middot; <span id="hScene">drawn ${drawn}, culled ${culled}</span><br>` +
        `drag to orbit, wheel to zoom, arrows to move, Esc for the flat view`;
      say('3D scene on');
    } else {
      singleView();
    }
  }

  function orbit(dYaw, dPitch, dDist) {
    if (!M) return;
    M.ccall('swbf2_scene_orbit', null, ['number', 'number', 'number'], [dYaw, dPitch, dDist]);
  }

  function runScripts() {
    if (!current) { say('stage a container first'); return; }
    const ran = M.ccall('swbf2_run_scripts_from', 'number', ['string', 'number'], [current, 40]);
    note.innerHTML = `<b>LuaFL:</b> executed ${ran} mission script(s) from ${currentLabel}`;
    pumpLog();
  }

  // ---- playable mission ----------------------------------------------------
  // The single-player slice: one Imperial stormtrooper on tat2, spawned by the engine's own
  // factory and driven by the engine's own controller and chase camera. The level's geometry
  // is built before the class files are staged, because the geometry reader uses `current`.
  let missionReady = false;
  let missionModelSegments = 0;
  let missionInput = null;
  let missionStateFloat = 0;
  let missionStateInt = 0;
  let missionAudio = null;
  let missionAudioStart = null;
  let missionAudioError = false;
  let missionFireWasDown = false;

  async function startMissionAudio() {
    if (missionAudio || missionAudioStart) return missionAudioStart;
    missionAudioStart = (async () => {
      const { createWebAudioSink } = await import('/browser/webaudio.mjs');
      missionAudio = createWebAudioSink({ module: M });
      await missionAudio.start();
      return missionAudio;
    })();
    return missionAudioStart;
  }

  function armMissionAudioGesture() {
    const start = () => {
      startMissionAudio().catch((error) => {
        if (!missionAudioError) {
          missionAudioError = true;
          console.warn('WebAudio could not start:', error);
        }
      });
    };
    // AudioContext creation/resume belongs to a real user gesture, not the module's async boot.
    document.addEventListener('pointerdown', start, { once: true });
    document.addEventListener('keydown', start, { once: true });
  }

  function playMissionFireSound() {
    startMissionAudio().then(() => {
      const status = M.ccall('audio_browser_load_sample', 'number', ['string', 'number'],
        ['/data/sound_common.bnk', 0x2eb95650]);
      if (status !== 1 && !missionAudioError) {
        missionAudioError = true;
        console.warn('the fire sample did not load, status', status);
      }
    }).catch((error) => {
      if (!missionAudioError) {
        missionAudioError = true;
        console.warn('the fire sample could not play:', error);
      }
    });
  }

  async function startMission() {
    if (!M || missionReady) return missionReady;
    say('mission: staging core data…');
    await stage(files.find((f) => f.kind !== 'class' && f.path === 'core.lvl'));
    await stage(files.find((f) => f.kind !== 'class' && f.path === 'tat/tat2.lvl'));
    if (M.ccall('swbf2_load_level', 'number', ['string'], ['tat/tat2']) !== 1) {
      say('mission: the engine could not load tat/tat2');
      return false;
    }
    buildModels();

    // The chase camera computes its aspect ratio when it is attached. Give it the real canvas
    // dimensions before combat setup, not after: otherwise it stores 0/0 as its FOV.
    M.ccall('swbf2_set_viewport', 'number', ['number', 'number'], [canvas.width, canvas.height]);

    say('mission: loading the shared and faction classes…');
    if ((await loadClass('ingame.lvl;com_inf_default')) !== 1) return false;
    if ((await loadClass('side/imp.lvl;imp_inf_rifleman')) !== 1) return false;
    if ((await loadClass('side/cis.lvl;cis_inf_rifleman')) !== 1) return false;

    // The engine's own teams, player Character and four target Characters. This is the same
    // setup the combat oracle uses; the host's WASD/mouse input is not replaced.
    if (M.ccall('swbf2_combat_setup', 'number',
                ['string', 'string', 'string', 'string', 'number'],
                ['imp_inf_rifleman', 'side/imp.lvl;imp_inf_rifleman',
                 'cis_inf_rifleman', 'side/cis.lvl;cis_inf_rifleman', 4]) !== 1) {
      say('mission: the engine could not create the player and targets');
      return false;
    }
    const modelSegments = M.ccall('swbf2_append_combat_models', 'number', ['string', 'string'],
      ['/data/side_imp.lvl', '/data/side_cis.lvl']);
    missionModelSegments = modelSegments;
    if (modelSegments <= 0) {
      say('mission: the combat models could not be appended');
      return false;
    }

    await stage(files.find((f) => f.kind === 'sound' && f.path === 'sound/common.bnk;sample_2eb95650'));
    armMissionAudioGesture();

    M.ccall('swbf2_use_engine_camera', null, ['number'], [1]);
    M.ccall('swbf2_combat_set_ai_fire', null, ['number'], [1]);
    missionStateFloat = M._malloc(24 * 4);
    missionStateInt = M._malloc(6 * 4);
    missionAmmoBuf = M._malloc(8);
    $('reticle').style.display = 'block';
    $('combatHud').style.display = 'block';
    $('hWeapon').textContent = 'E-11 Blaster Rifle';
    $('hAmmo').textContent = '50 / 250';
    missionReady = true;
    say('stormtrooper ready — WASD to move, mouse to look, Space to jump, Ctrl to roll, LMB to fire, 1-3 switch weapons');
    const { attachInputCamera } = await import('/browser/input_driver.mjs');
    missionInput = attachInputCamera(M, { canvas });
    pumpLog();
    return true;
  }

  let missionAmmoBuf = 0;
  let missionWave = 1;
  let prevTargetsAlive = 4;

  function triggerHitMarker() {
    const ret = $('reticle');
    if (ret) {
      ret.classList.add('hit');
      setTimeout(() => ret.classList.remove('hit'), 150);
    }
  }

  window.addEventListener('keydown', (ev) => {
    if (!missionReady || !M) return;
    if (ev.code === 'KeyR' || (ev.code === 'Space' && M.ccall('swbf2_combat_unit_alive', 'number', ['number'], [0]) === 0)) {
      const alive = M.ccall('swbf2_combat_unit_alive', 'number', ['number'], [0]);
      const health = M.ccall('swbf2_combat_unit_health', 'number', ['number'], [0]);
      const unitCount = M.ccall('swbf2_combat_unit_count', 'number');
      let targetsAlive = 0;
      for (let i = 1; i < unitCount; ++i) {
        if (M.ccall('swbf2_combat_unit_alive', 'number', ['number'], [i]) === 1) targetsAlive++;
      }
      if (alive === 0 || health <= 0) {
        M.ccall('swbf2_combat_respawn_player');
        say('respawned stormtrooper at forward post');
      } else if (targetsAlive === 0) {
        missionWave++;
        M.ccall('swbf2_combat_respawn_targets');
        prevTargetsAlive = unitCount - 1;
        say(`wave ${missionWave} incoming — CIS droids deployed`);
      }
    } else if (ev.code === 'Digit1') {
      M.ccall('swbf2_combat_select_weapon', 'number', ['number', 'number', 'number'], [0, 0, 0]);
      $('hWeapon').textContent = 'E-11 Blaster Rifle';
    } else if (ev.code === 'Digit2') {
      M.ccall('swbf2_combat_select_weapon', 'number', ['number', 'number', 'number'], [0, 0, 1]);
      $('hWeapon').textContent = 'SE-14r Blaster Pistol';
    } else if (ev.code === 'Digit3') {
      M.ccall('swbf2_combat_select_weapon', 'number', ['number', 'number', 'number'], [0, 0, 2]);
      $('hWeapon').textContent = 'Thermal Detonator';
    }
  });

  function updateCombatHud() {
    if (!missionReady) return;
    if (M.ccall('swbf2_trooper_state', 'number', ['number', 'number', 'number', 'number'],
                [missionStateFloat, 24, missionStateInt, 6]) !== 1) return;
    
    const health = M.ccall('swbf2_combat_unit_health', 'number', ['number'], [0]);
    const alive = M.ccall('swbf2_combat_unit_alive', 'number', ['number'], [0]);
    const maxHealth = 400;
    const pct = Math.max(0, Math.min(100, Math.round((health / maxHealth) * 100)));
    $('hHealth').textContent = `${Math.max(0, Math.round(health))}`;
    if ($('hHealthBar')) $('hHealthBar').style.width = `${pct}%`;

    if (missionAmmoBuf && M.ccall('swbf2_combat_unit_ammo', 'number', ['number', 'number', 'number'], [0, missionAmmoBuf, missionAmmoBuf + 4]) === 1) {
      const cur = Math.round(M.getValue(missionAmmoBuf, 'float'));
      const reserve = Math.round(M.getValue(missionAmmoBuf + 4, 'float'));
      $('hAmmo').textContent = reserve >= 0 ? `${cur} / ${reserve}` : `${cur}`;
    }

    const unitCount = M.ccall('swbf2_combat_unit_count', 'number');
    let targetsAlive = 0;
    for (let i = 1; i < unitCount; ++i) {
      if (M.ccall('swbf2_combat_unit_alive', 'number', ['number'], [i]) === 1) {
        targetsAlive++;
      }
    }
    if (targetsAlive < prevTargetsAlive) {
      triggerHitMarker();
      prevTargetsAlive = targetsAlive;
    }
    if ($('hTargets')) $('hTargets').textContent = `${targetsAlive} / ${Math.max(0, unitCount - 1)}`;

    const banner = $('bannerOverlay');
    if (banner) {
      if (alive === 0 || health <= 0) {
        banner.style.display = 'flex';
        banner.className = 'defeated';
        banner.innerHTML = '<div class="banner-title">DEFEATED</div><div class="banner-sub">Press [Space] or [R] to Respawn</div>';
      } else if (targetsAlive === 0 && unitCount > 1) {
        banner.style.display = 'flex';
        banner.className = 'victory';
        banner.innerHTML = `<div class="banner-title">VICTORY</div><div class="banner-sub">Wave ${missionWave} Cleared! Press [R] for Next Wave</div>`;
      } else {
        banner.style.display = 'none';
      }
    }

    const fireDown = M.getValue(missionStateInt + 2 * 4, 'i32') !== 0;
    if (fireDown && !missionFireWasDown) playMissionFireSound();
    missionFireWasDown = fireDown;
  }

  // ---- self-demo -----------------------------------------------------------
  // Opening the page with no arguments should show something real rather than an empty
  // canvas and a list to click through, so boot stages a default container and runs the
  // whole pipeline once. ?file=<path> overrides which container, ?quiet=1 skips it.
  const params = new URLSearchParams(location.search);
  const DEFAULT_CONTAINER = 'tat/tat2.lvl';  // 60 real level textures

  async function selfDemo() {
    trace('selfDemo:enter files=' + files.length);
    if (params.get('quiet') === '1') return;
    if (params.get('mission') === '1') {
      await startMission();
      window.__demoDone = true;
      return;
    }
    const want = params.get('file') || DEFAULT_CONTAINER;
    const pick = files.find((f) => f.path === want) ||
      files.find((f) => f.path === want && f.size < 40 * 1048576) ||
      files.find((f) => f.size > 1048576 && f.size < 12 * 1048576);
    if (!pick) { say('no container under 12 MB to demo with'); window.__demoDone = true; return; }
    trace('selfDemo:staging ' + pick.path);
    await stage(pick);
    trace('selfDemo:staged, probing');
    probe();
    trace('selfDemo:texture');
    showTexture(1);
    trace('selfDemo:scripts');
    runScripts();
    trace('selfDemo:models');
    buildModels();
    trace('selfDemo:done');
    const ms = window.__modelStats();
    say(`showing real model geometry from ${pick.path} — ${ms.models} models, ${ms.segments} segments, ` +
      `${ms.vertices} vertices through ${ms.instances} placed instances; ` +
      `G switches between the model view and the resident textures, ← → pages textures, L reruns scripts`);
    // the headless check (browser/cdp_check.mjs) waits on this before reading the page
    window.__demoDone = true;
  }

  // ---- frame loop ----------------------------------------------------------
  let last = performance.now();
  let fpsAt = last, fpsFrames = 0;

  function frame(now) {
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;
    if (M) {
      try {
        if (missionInput) missionInput.send(dt);
        M.ccall('swbf2_frame', null, ['number'], [dt]);
        updateCombatHud();
      } catch (e) {
        // a throw here must not stop the frame loop or the log pump - it would look like the
        // engine froze when only the readout is broken
        if (!window.__frameErrShown) { console.error('frame:', e); window.__frameErrShown = 1; }
      }
      $('hFrames').textContent = M.ccall('swbf2_frames', 'number');
      // The engine's own clock, not a host accumulator: swbf2_sim_time is the engine's time base,
      // swbf2_sim_delta is the delta the game logic was handed this frame, and swbf2_sim_ticks is
      // the integer tick count underneath it (the shim's clock ticks at 1 MHz / 128).
      const sim = M.ccall('swbf2_sim_time', 'number');
      const delta = M.ccall('swbf2_sim_delta', 'number');
      const ticks = M.ccall('swbf2_sim_ticks', 'number');
      $('hSim').textContent = `${sim.toFixed(3)}s`;
      if ($('hClock')) {
        $('hClock').textContent = `Δ ${(delta * 1000).toFixed(2)}ms · ${ticks} ticks`;
      }
      $('hLua').textContent = M.ccall('swbf2_lua_calls', 'number');
      const g = window.__gpuStats();
      $('hGpu').textContent = g.tex
        ? `${g.tex >> 16}x${g.tex & 0xffff} tex · luma ${g.luma} · ${(g.litPermille / 10).toFixed(0)}% lit · ${g.colors} colours`
        : `luma ${g.luma}`;
      if (M.ccall('swbf2_is_3d', 'number')) {
        if (M.ccall('swbf2_geometry_on', 'number')) {
          setHud('hModel', `drawn ${M.ccall('swbf2_model_drawn', 'number')}, culled ${M.ccall('swbf2_model_culled', 'number')}`);
        } else {
          const sc = window.__sceneStats();
          setHud('hScene', `drawn ${sc.drawn}, culled ${sc.culled}`);
          setHud('hTex', `${sc.drawn + sc.culled} resident`);
        }
      }
      pumpLog();
    }
    fpsFrames++;
    if (now - fpsAt > 500) {
      $('hFps').textContent = Math.round((fpsFrames * 1000) / (now - fpsAt));
      fpsAt = now; fpsFrames = 0;
    }
    requestAnimationFrame(frame);
  }

  // ---- wiring --------------------------------------------------------------
  $('btnProbe').onclick = probe;
  $('btnMontage').onclick = buildMontage;
  $('btnSingle').onclick = singleView;
  $('btnTex').onclick = () => showTexture(1);
  $('btnNext').onclick = () => showTexture(1);
  $('btnPrev').onclick = () => showTexture(-1);
  $('btnLua').onclick = runScripts;
  $('btn3D').onclick = () => scene3D(true);
  $('btnFlat').onclick = singleView;

  // Camera input. Drag orbits, wheel zooms; the canvas is the only thing that should swallow
  // these, so the page keeps scrolling normally outside it.
  let dragging = false, lastX = 0, lastY = 0;
  canvas.addEventListener('mousedown', (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; });
  addEventListener('mouseup', () => { dragging = false; });
  addEventListener('mousemove', (e) => {
    if (!dragging) return;
    orbit((e.clientX - lastX) * 0.006, (e.clientY - lastY) * 0.006, 0);
    lastX = e.clientX; lastY = e.clientY;
  });
  canvas.addEventListener('wheel', (e) => { e.preventDefault(); orbit(0, 0, e.deltaY * 0.02); }, { passive: false });

  addEventListener('keydown', (e) => {
    if (!M) return;
    if (e.key === 'ArrowRight' || e.key.toLowerCase() === 'd') showTexture(1);
    else if (e.key === 'ArrowLeft' || e.key.toLowerCase() === 'a') showTexture(-1);
    else if (e.key.toLowerCase() === 'l') runScripts();
    else if (e.key.toLowerCase() === 'p') probe();
    else if (e.key.toLowerCase() === 'm') buildMontage();
    else if (e.key.toLowerCase() === 'g') { M.ccall('swbf2_set_geometry', null, ['number'], [M.ccall('swbf2_geometry_on', 'number') ? 0 : 1]); say('geometry ' + (M.ccall('swbf2_geometry_on','number') ? 'on' : 'off (resident textures stand in)')); }
    else if (e.key === '3') scene3D(true);
    else if (e.key === 'Escape') singleView();
    // In 3D the arrows drive the camera; in the flat views they page textures. One key set,
    // two meanings, chosen by the mode that is actually on screen.
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'ArrowLeft' || e.key.toLowerCase() === 'a')) orbit(-0.12, 0, 0);
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'ArrowRight' || e.key.toLowerCase() === 'd')) orbit(0.12, 0, 0);
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'ArrowUp' || e.key.toLowerCase() === 'w')) orbit(0, 0.1, 0);
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'ArrowDown' || e.key.toLowerCase() === 's')) orbit(0, -0.1, 0);
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'q' || e.key === '+')) orbit(0, 0, -2);
    else if (M.ccall('swbf2_is_3d', 'number') && (e.key === 'z' || e.key === '-')) orbit(0, 0, 2);
  });

  // Strict sequence: the wasm module must be up before the manifest, and both before the
  // self-demo, whose actions all guard on `M` being live. (Kicking them off in parallel made
  // every engine call a silent no-op.)
  boot()
    .then(loadManifest)
    .then(selfDemo)
    .catch((e) => {
      trace('selfDemo:rejected ' + e);
      note.textContent = 'manifest/self-demo failed: ' + e;
      window.__demoDone = true;
    });
  boot().catch((e) => { say('boot failed: ' + e); note.textContent = String(e); });
  requestAnimationFrame(frame);
})();
