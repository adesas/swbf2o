// Bring-Your-Own-Copy (BYOC) local GameData folder loader & IndexedDB handle store.
//
// Lets players point the browser at their own legal Star Wars Battlefront II (2005)
// installation (Steam, GOG, or retail CD) — at the game root, GameData/, GameData/data/,
// or GameData/data/_lvl_pc/ — without uploading or hosting any copyrighted game files.
//
// Files remain 100% local on the user's device. Only lightweight File handles are passed
// to the Dedicated Worker, which reads exact 256 KiB slices on demand via FileReaderSync.

const IDB_NAME = 'swbf2_byoc_v1';
const IDB_STORE = 'handles';
const IDB_KEY = 'gamedataDir';

// Core files required to boot the SWBF2 engine shell and missions.
export const REQUIRED_CORE_FILES = [
  'core.lvl',
  'common.lvl',
  'shell.lvl',
  'mission.lvl',
  'ingame.lvl',
];

// Canonical 24 retail planet/space .lvl files (used purely to report how many maps were found).
const KNOWN_MAP_PREFIXES = [
  'cor/cor1.lvl', 'dag/dag1.lvl', 'dea/dea1.lvl', 'end/end1.lvl',
  'fel/fel1.lvl', 'geo/geo1.lvl', 'hot/hot1.lvl', 'kam/kam1.lvl',
  'kas/kas2.lvl', 'mus/mus1.lvl', 'myg/myg1.lvl', 'nab/nab2.lvl',
  'pol/pol1.lvl', 'tat/tat2.lvl', 'tat/tat3.lvl', 'tan/tan1.lvl',
  'uta/uta1.lvl', 'yav/yav1.lvl',
  'spa/spa1.lvl', 'spa/spa3.lvl', 'spa/spa6.lvl',
  'spa/spa7.lvl', 'spa/spa8.lvl', 'spa/spa9.lvl',
];

const GAME_FILE_EXTS = /\.(lvl|bnk|mvs|def|ini|lua|txt|cfg)$/i;
const INSTALLER_HINT_RE = /(?:^|\/)(?:setup_.*\.(?:exe|bin)|goggame-.*\.exe|autorun\.exe)$/i;
const ARCHIVE_HINT_RE = /\.(?:rar|zip|7z|iso|tar|gz)$/i;
const SKIP_SCAN_DIRS = new Set([
  'savegames', 'docs', 'directx', 'install', '__support', 'tmp',
  'node_modules', '.git', '$recycle.bin', 'system volume information',
]);

export function supportsDirectoryPicker() {
  return typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';
}

function openIdb() {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === 'undefined') {
      resolve(null);
      return;
    }
    const req = indexedDB.open(IDB_NAME, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(IDB_STORE)) {
        db.createObjectStore(IDB_STORE);
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error || new Error('IndexedDB open failed'));
  });
}

export async function saveDirectoryHandle(dirHandle) {
  try {
    const db = await openIdb();
    if (!db) return false;
    await new Promise((resolve, reject) => {
      const tx = db.transaction(IDB_STORE, 'readwrite');
      tx.objectStore(IDB_STORE).put(dirHandle, IDB_KEY);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
    db.close();
    return true;
  } catch {
    return false;
  }
}

export async function loadSavedDirectoryHandle() {
  try {
    const db = await openIdb();
    if (!db) return null;
    const handle = await new Promise((resolve, reject) => {
      const tx = db.transaction(IDB_STORE, 'readonly');
      const req = tx.objectStore(IDB_STORE).get(IDB_KEY);
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => reject(req.error);
    });
    db.close();
    return handle || null;
  } catch {
    return null;
  }
}

export async function clearSavedDirectoryHandle() {
  try {
    const db = await openIdb();
    if (!db) return;
    await new Promise((resolve, reject) => {
      const tx = db.transaction(IDB_STORE, 'readwrite');
      tx.objectStore(IDB_STORE).delete(IDB_KEY);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
    db.close();
  } catch {}
}

// Normalize an arbitrary relative path from a picked folder into the engine's canonical
// _lvl_pc-relative lowercase path (e.g. "tat/tat2.lvl", "core.lvl", "sound/common.bnk").
function normalizeToLvlPcRel(rawRelPath) {
  let p = String(rawRelPath || '').replace(/\\/g, '/').replace(/^\/+/, '');
  const low = p.toLowerCase();

  // Case 1: Path contains "_lvl_pc/" anywhere (user picked SWBF2 root, GameData/, or data/)
  const marker = '_lvl_pc/';
  const idx = low.indexOf(marker);
  if (idx >= 0) {
    return {
      rel: p.slice(idx + marker.length),
      lower: low.slice(idx + marker.length),
      matchedLvlPc: true,
    };
  }

  // Case 2: User picked _lvl_pc directly (files sit directly under root folder name in webkitRelativePath)
  return {
    rel: p,
    lower: low,
    matchedLvlPc: false,
  };
}

// Build a validated BYOC file index from an array of { relPath, file } entries.
export function buildByocIndex(entries, rootName = 'GameData', hints = null) {
  const lvlPcFiles = new Map(); // lower -> { path, file }
  const directFiles = new Map();
  const strippedTopFiles = new Map();
  const allGameEntries = []; // { rel, lower, file }
  const corePrefixes = new Set();

  let detectedInstaller = !!(hints && hints.detectedInstaller);
  let detectedArchive = !!(hints && hints.detectedArchive);
  let scannedEntries = (hints && hints.scannedEntries) || 0;

  for (const { relPath, file } of entries) {
    scannedEntries++;
    const rawNorm = String(relPath || '').replace(/\\/g, '/').replace(/^\/+/, '');
    const rawLow = rawNorm.toLowerCase();
    if (INSTALLER_HINT_RE.test(rawLow)) detectedInstaller = true;
    if (ARCHIVE_HINT_RE.test(rawLow)) detectedArchive = true;

    if (!file || typeof file.size !== 'number') continue;
    const norm = normalizeToLvlPcRel(rawNorm);
    if (!norm.lower || !GAME_FILE_EXTS.test(norm.lower)) continue;

    allGameEntries.push({ rel: rawNorm, lower: rawLow, file });
    if (rawLow === 'core.lvl' || rawLow.endsWith('/core.lvl')) {
      corePrefixes.add(rawLow.slice(0, rawLow.length - 'core.lvl'.length));
    }

    if (norm.matchedLvlPc) {
      lvlPcFiles.set(norm.lower, { path: norm.rel, file });
      continue;
    }

    directFiles.set(norm.lower, { path: norm.rel, file });
    const slash = norm.rel.indexOf('/');
    if (slash > 0) {
      const subRel = norm.rel.slice(slash + 1);
      const subLow = subRel.toLowerCase();
      if (subLow) strippedTopFiles.set(subLow, { path: subRel, file });
    }
  }

  // Dynamic prefix candidates anchored on any discovered core.lvl at arbitrary depth/folder name
  // (handles GOG innoextract `app/GameData/data/_lvl_pc/`, nested parent folders, or renamed dirs).
  const dynamicCandidates = [];
  for (const prefix of corePrefixes) {
    const m = new Map();
    for (const item of allGameEntries) {
      if (!item.lower.startsWith(prefix)) continue;
      const subRel = item.rel.slice(prefix.length);
      const subLow = item.lower.slice(prefix.length);
      if (subLow) m.set(subLow, { path: subRel, file: item.file });
    }
    if (m.size > 0) dynamicCandidates.push(m);
  }

  // Choose the candidate map that contains the required core.lvl / common.lvl files.
  const scoreMap = (m) => {
    let score = 0;
    for (const req of REQUIRED_CORE_FILES) {
      if (m.has(req)) score++;
    }
    return score;
  };

  let chosen = lvlPcFiles;
  let bestScore = scoreMap(lvlPcFiles);
  for (const cand of [...dynamicCandidates, directFiles, strippedTopFiles]) {
    const s = scoreMap(cand);
    if (s > bestScore || (s === bestScore && cand.size > chosen.size)) {
      chosen = cand;
      bestScore = s;
    }
  }

  const missingCore = REQUIRED_CORE_FILES.filter((k) => !chosen.has(k));
  let mapCount = 0;
  for (const mk of KNOWN_MAP_PREFIXES) {
    if (chosen.has(mk)) mapCount++;
  }

  let totalBytes = 0;
  const files = new Map(); // lower -> File
  const pathNames = new Map(); // lower -> original relative path
  for (const [lower, rec] of chosen.entries()) {
    files.set(lower, rec.file);
    pathNames.set(lower, rec.path);
    totalBytes += rec.file.size;
  }

  return {
    valid: missingCore.length === 0,
    rootName: rootName || 'GameData',
    fileCount: files.size,
    totalBytes,
    mapCount,
    missingCore,
    detectedInstaller,
    detectedArchive,
    scannedEntries,
    files,
    pathNames,
  };
}

// Safely list entries of a FileSystemDirectoryHandle without aborting on protected/unreadable dirs.
async function safeListDir(handle) {
  const out = [];
  try {
    for await (const [name, child] of handle.entries()) {
      out.push([name, child]);
    }
  } catch (err) {
    console.warn('[byoc] skipping unreadable directory:', handle?.name, err);
  }
  return out;
}

// Recursively search up to depth 4 for _lvl_pc (or a directory containing core.lvl) so we
// handle Steam, GOG, GOG+innoextract (`app/GameData/data/_lvl_pc`), CD, and nested folders
// without wasting time scanning unrelated sibling directories.
async function resolveBestSubdir(dirHandle) {
  const trySub = async (h, names) => {
    let cur = h;
    for (const target of names) {
      let hit = null;
      for (const [name, child] of await safeListDir(cur)) {
        if (child.kind === 'directory' && name.toLowerCase() === target) {
          hit = child;
          break;
        }
      }
      if (!hit) return null;
      cur = hit;
    }
    return cur;
  };

  // Fast path 1: core.lvl sits right at the picked root
  for (const [name, child] of await safeListDir(dirHandle)) {
    if (child.kind === 'file' && name.toLowerCase() === 'core.lvl') {
      return dirHandle;
    }
  }

  // Fast path 2: standard Steam / GOG / innoextract / retail install chains
  const candidates = [
    ['gamedata', 'data', '_lvl_pc'],
    ['app', 'gamedata', 'data', '_lvl_pc'],
    ['data', '_lvl_pc'],
    ['_lvl_pc'],
  ];
  for (const chain of candidates) {
    const found = await trySub(dirHandle, chain);
    if (found) return found;
  }

  // Fallback BFS up to depth 4 to find any nested _lvl_pc or directory containing core.lvl
  const queue = [{ handle: dirHandle, depth: 0 }];
  let visitedDirs = 0;
  const MAX_BFS_DIRS = 120;
  while (queue.length > 0 && visitedDirs < MAX_BFS_DIRS) {
    const { handle, depth } = queue.shift();
    visitedDirs++;
    const items = await safeListDir(handle);
    for (const [name, child] of items) {
      const low = name.toLowerCase();
      if (child.kind === 'file' && low === 'core.lvl') {
        return handle;
      }
      if (child.kind === 'directory' && low === '_lvl_pc') {
        return child;
      }
    }
    if (depth >= 4) continue;
    for (const [name, child] of items) {
      if (child.kind !== 'directory') continue;
      const low = name.toLowerCase();
      if (SKIP_SCAN_DIRS.has(low)) continue;
      queue.push({ handle: child, depth: depth + 1 });
    }
  }
  return dirHandle;
}

export async function scanDirectoryHandle(dirHandle, { onScanProgress = null } = {}) {
  const targetDir = await resolveBestSubdir(dirHandle);
  const entries = [];
  let scanned = 0;
  const hints = { detectedInstaller: false, detectedArchive: false, scannedEntries: 0 };

  async function walk(handle, prefix, depth) {
    if (depth > 8) return;
    for (const [name, child] of await safeListDir(handle)) {
      const rel = prefix ? `${prefix}/${name}` : name;
      const lowName = name.toLowerCase();
      if (child.kind === 'directory') {
        if (!prefix && SKIP_SCAN_DIRS.has(lowName)) {
          continue;
        }
        await walk(child, rel, depth + 1);
      } else if (child.kind === 'file') {
        hints.scannedEntries++;
        if (INSTALLER_HINT_RE.test(lowName)) hints.detectedInstaller = true;
        if (ARCHIVE_HINT_RE.test(lowName)) hints.detectedArchive = true;
        if (!GAME_FILE_EXTS.test(name)) continue;
        try {
          const file = await child.getFile();
          entries.push({ relPath: rel, file });
          scanned++;
          if (onScanProgress && (scanned % 25 === 0)) {
            onScanProgress(scanned);
          }
        } catch (err) {
          console.warn('[byoc] skipping unreadable file:', rel, err);
        }
      }
    }
  }

  await walk(targetDir, '', 0);
  if (onScanProgress) onScanProgress(scanned);
  return buildByocIndex(entries, dirHandle.name || targetDir.name || 'GameData', hints);
}

export function scanFileList(fileList) {
  const list = Array.from(fileList || []);
  const entries = [];
  let rootName = 'GameData';
  for (const file of list) {
    const rel = file.webkitRelativePath || file.name;
    if (rootName === 'GameData' && rel.includes('/')) {
      rootName = rel.split('/')[0] || 'GameData';
    }
    entries.push({ relPath: rel, file });
  }
  return buildByocIndex(entries, rootName);
}

export async function pickLocalGameDataDirectory({ onScanProgress = null } = {}) {
  if (!supportsDirectoryPicker()) {
    throw new Error('Directory picker API is not supported in this browser.');
  }
  const dirHandle = await window.showDirectoryPicker({
    id: 'swbf2-gamedata',
    mode: 'read',
  });
  const index = await scanDirectoryHandle(dirHandle, { onScanProgress });
  if (index.valid) {
    await saveDirectoryHandle(dirHandle);
  }
  return { dirHandle, index };
}

export async function checkSavedDirectoryState() {
  const handle = await loadSavedDirectoryHandle();
  if (!handle) return { status: 'none', handle: null, name: '' };
  try {
    const perm = await handle.queryPermission({ mode: 'read' });
    return {
      status: perm === 'granted' ? 'granted' : 'prompt',
      handle,
      name: handle.name || 'GameData',
    };
  } catch {
    return { status: 'none', handle: null, name: '' };
  }
}

export async function reconnectSavedDirectory(handle, { onScanProgress = null } = {}) {
  if (!handle) throw new Error('No saved GameData folder handle.');
  let perm = await handle.queryPermission({ mode: 'read' });
  if (perm !== 'granted') {
    perm = await handle.requestPermission({ mode: 'read' });
  }
  if (perm !== 'granted') {
    throw new Error('Read permission was not granted for your GameData folder.');
  }
  const index = await scanDirectoryHandle(handle, { onScanProgress });
  return { dirHandle: handle, index };
}
