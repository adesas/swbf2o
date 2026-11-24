// On-demand GameData streaming VFS backed by Origin Private File System (OPFS).
//
// Runs inside a Dedicated Worker where both FileSystemSyncAccessHandle and synchronous
// XMLHttpRequest (with responseType='arraybuffer') are natively supported. This gives C++
// PblFile::Open / Seek / ReadData / WriteData 100% synchronous POSIX-like semantics with ZERO
// whole-file MEMFS copies and ZERO -sASYNCIFY overhead.
//
// Storage layout under navigator.storage.getDirectory() -> "swbf2_opfs_v1/":
//   - blocks.bin : pooled 256 KiB slot store holding cached slices of any of the 196 GameData files.
//   - index.bin  : append-only binary journal mapping (fileIndex, blockIndex) -> (slotIndex, fpHash).
//   - saves.bin  : synchronous read-write store for persistent /saves/* profiles and configs.
//
// Opening just 3 SyncAccessHandles once during async init() guarantees every subsequent file open
// (across all 196 .lvl / .bnk / .mvs / .def / .ini files and any save file) is 100% synchronous
// and never hits browser per-origin SyncAccessHandle limits.

const BLOCK_SIZE = 262144; // 256 KiB
const MAX_COALESCE_BLOCKS = 8; // Up to 2 MiB per HTTP Range request on sequential misses (cuts WAN RTTs 8x)
const PREFETCH_AHEAD_BLOCKS = 8; // Up to 2 MiB async non-blocking lookahead for audio/movie streams
const MAX_CONCURRENT_PREFETCH = 2;
const RAM_CACHE_MAX_BLOCKS = 256; // 64 MiB in-worker LRU RAM slab cache
const DEFAULT_MAX_OPFS_BLOCKS = 4096; // 1 GiB OPFS block budget (LRU recyclable)

const INDEX_MAGIC = 0x53324958; // 'S2IX'
const INDEX_VERSION = 1;
const INDEX_HEADER_BYTES = 16;
const INDEX_RECORD_BYTES = 24;
const INDEX_REC_MAGIC = 0x424C4B31; // 'BLK1'

const SAVES_MAGIC = 0x53325356; // 'S2SV'

function fnv1a64(str) {
  let lo = 0x84222325 >>> 0;
  let hi = 0xcbf29ce4 >>> 0;
  for (let i = 0; i < str.length; ++i) {
    lo ^= str.charCodeAt(i) & 0xff;
    // Multiply 64-bit (hi, lo) by 0x00000100_000001B3 (FNV prime) mod 2^64.
    const l0 = lo & 0xffff;
    const l1 = lo >>> 16;
    const h0 = hi & 0xffff;
    const h1 = hi >>> 16;
    const p0 = l0 * 0x01b3;
    const p1 = l1 * 0x01b3 + (p0 >>> 16);
    const p2 = h0 * 0x01b3 + l0 * 0x0100 + (p1 >>> 16);
    const p3 = h1 * 0x01b3 + l1 * 0x0100 + (p2 >>> 16);
    lo = ((p1 & 0xffff) << 16) | (p0 & 0xffff);
    hi = ((p3 & 0xffff) << 16) | (p2 & 0xffff);
  }
  return { lo: lo >>> 0, hi: hi >>> 0 };
}

export class OpfsVfs {
  constructor({ manifestUrl = '/browser/gamedata-manifest.json', baseUrl = null, localFiles = null, maxOpfsBlocks = DEFAULT_MAX_OPFS_BLOCKS, onProgress = null } = {}) {
    this.manifestUrl = manifestUrl;
    this.baseUrlOverride = baseUrl;
    this.maxOpfsBlocks = maxOpfsBlocks;
    this.onProgress = typeof onProgress === 'function' ? onProgress : null;
    this._lastProgressEmitMs = 0;
    this._lastProgressPath = '';

    this.mode = localFiles ? 'local' : 'http'; // 'local' (BYOC FileReaderSync) | 'http' | 'saves-only'
    this.localFiles = null; // Map<lowerRelPath, File>
    this.readerSync = typeof FileReaderSync !== 'undefined' ? new FileReaderSync() : null;

    this.manifest = null;
    this.baseUrl = '/GameData/';
    this.blockSize = BLOCK_SIZE;

    // Canonical lookup tables:
    //   byLower: "tat/tat2.lvl" -> FileMeta
    //   byFlat:  "tat_tat2.lvl" -> FileMeta (backwards compat with /data/tat_tat2.lvl callers)
    //   byId:    fileId (1..N)  -> FileMeta
    this.byLower = new Map();
    this.byFlat = new Map();
    this.byId = [];

    // OPFS handles (opened once asynchronously during init())
    this.opfsDir = null;
    this.blocksHandle = null;
    this.indexHandle = null;
    this.savesHandle = null;
    this.opfsPersistent = false;

    // Block allocation state:
    //   key: `${fileId}:${blockIdx}` -> { slot, size }
    this.blockMap = new Map();
    this.slotOwner = new Array(maxOpfsBlocks).fill(null); // slot -> key
    this.nextSlot = 0;
    this.indexWriteOffset = INDEX_HEADER_BYTES;

    // In-memory LRU slab cache: key -> Uint8Array (most recently used at end of Map)
    this.ramCache = new Map();

    // Persistent /saves/* virtual files: lowerPath -> Uint8Array
    this.saves = new Map();

    // Open file descriptors (fd >= 1)
    this.nextFd = 1;
    this.fds = new Map();

    // Async background prefetch state (keeps streaming music/movies from blocking gameplay over WAN)
    this.musicStreaming = true;
    this._cacheEpoch = 0;
    this._activePrefetches = 0;
    this._inflightBlocks = new Set(); // `${fileId}:${blockIdx}`
    this._failedBlocks = new Map(); // `${fileId}:${blockIdx}` -> failTimestampMs
    this._seqTracker = new Map(); // fileId -> { lastBlock, seqHits }

    // Telemetry
    this.stats = {
      mode: this.mode,
      httpRequests: 0,
      httpBytes: 0,
      localReads: 0,
      localBytes: 0,
      opfsHits: 0,
      opfsHitBytes: 0,
      memHits: 0,
      memHitBytes: 0,
      opfsWrites: 0,
      opfsEvictions: 0,
      savesFlushes: 0,
      perFile: {}, // relPath -> { size, httpBytes, httpRequests, opfsHits, memHits }
    };

    if (localFiles) {
      this._indexLocalFiles(localFiles);
    }
  }

  _indexLocalFiles(localFiles) {
    this.mode = 'local';
    this.stats.mode = 'local';
    this.localFiles = new Map();
    this.byLower.clear();
    this.byFlat.clear();
    this.byId = [null];
    this.ramCache.clear();
    this._seqTracker.clear();
    this._cacheEpoch++;

    const entries = localFiles instanceof Map
      ? Array.from(localFiles.entries())
      : Array.isArray(localFiles)
        ? localFiles
        : Object.entries(localFiles || {});

    entries.sort((a, b) => String(a[0]).localeCompare(String(b[0])));
    for (const [rawKey, file] of entries) {
      if (!file || typeof file.size !== 'number') continue;
      const lower = String(rawKey).replace(/\\/g, '/').replace(/^\/+/, '').toLowerCase();
      if (!lower) continue;
      const dot = lower.lastIndexOf('.');
      const ext = dot >= 0 ? lower.slice(dot) : '';
      const fp = fnv1a64(`${lower}|${file.size}|${file.lastModified || 0}`);
      const id = this.byId.length;
      const meta = {
        id,
        lower,
        path: lower,
        size: file.size,
        ext,
        fingerprint: `local:${file.size}:${file.lastModified || 0}`,
        fpLo: fp.lo,
        fpHi: fp.hi,
        numBlocks: Math.max(1, Math.ceil(file.size / this.blockSize)),
        localFile: file,
      };
      this.localFiles.set(lower, file);
      this.byId.push(meta);
      this.byLower.set(lower, meta);
      this.byFlat.set(lower.replace(/\//g, '_'), meta);
    }
  }

  async init({ savesOnly = false } = {}) {
    if (this.mode === 'local' && this.localFiles && this.localFiles.size > 0) {
      if (!this.savesHandle) {
        await this._openOpfs();
        this._loadSavesFromOpfs();
      }
      return;
    }

    if (savesOnly) {
      this.mode = 'saves-only';
      this.stats.mode = 'saves-only';
      if (!this.savesHandle) {
        await this._openOpfs();
        this._loadSavesFromOpfs();
      }
      return;
    }

    const res = await fetch(this.manifestUrl, { cache: 'no-store' });
    if (!res.ok) {
      throw new Error(`No local GameData folder selected and server GameData is unavailable (HTTP ${res.status}). Select your local Star Wars Battlefront II GameData folder to play.`);
    }
    this.mode = 'http';
    this.stats.mode = 'http';
    this.manifest = await res.json();
    this.baseUrl = this.baseUrlOverride || this.manifest.baseUrl || '/GameData/';
    if (!this.baseUrl.endsWith('/')) this.baseUrl += '/';
    this.blockSize = this.manifest.blockSize || BLOCK_SIZE;

    this.byLower.clear();
    this.byFlat.clear();
    this.byId = [null]; // 1-based fileId

    const sortedKeys = Object.keys(this.manifest.files).sort();
    for (const lower of sortedKeys) {
      const entry = this.manifest.files[lower];
      const fp = fnv1a64(`${lower}|${entry.size}|${entry.fingerprint || ''}`);
      const id = this.byId.length;
      const meta = {
        id,
        lower,
        path: entry.path,
        size: entry.size,
        ext: entry.ext || '',
        fingerprint: entry.fingerprint || '',
        fpLo: fp.lo,
        fpHi: fp.hi,
        numBlocks: Math.ceil(entry.size / this.blockSize),
      };
      this.byId.push(meta);
      this.byLower.set(lower, meta);
      this.byFlat.set(lower.replace(/\//g, '_'), meta);
    }

    await this._openOpfs();
    this._loadIndexFromOpfs();
    this._loadSavesFromOpfs();
  }

  async setLocalFiles(localFiles) {
    this._indexLocalFiles(localFiles);
    if (!this.savesHandle) {
      await this._openOpfs();
      this._loadSavesFromOpfs();
    }
  }

  async _openOpfs() {
    if (!navigator.storage || typeof navigator.storage.getDirectory !== 'function') {
      this.opfsPersistent = false;
      return;
    }
    try {
      const root = await navigator.storage.getDirectory();
      this.opfsDir = await root.getDirectoryHandle('swbf2_opfs_v1', { create: true });
      const blocksFile = await this.opfsDir.getFileHandle('blocks.bin', { create: true });
      const indexFile = await this.opfsDir.getFileHandle('index.bin', { create: true });
      const savesFile = await this.opfsDir.getFileHandle('saves.bin', { create: true });
      this.blocksHandle = await blocksFile.createSyncAccessHandle();
      this.indexHandle = await indexFile.createSyncAccessHandle();
      this.savesHandle = await savesFile.createSyncAccessHandle();
      this.opfsPersistent = true;
    } catch (err) {
      console.warn('[vfs_opfs] OPFS SyncAccessHandle unavailable, using RAM-only cache:', err);
      this.opfsPersistent = false;
    }
  }

  _writeFreshIndexHeader() {
    if (!this.indexHandle) return;
    const hdr = new ArrayBuffer(INDEX_HEADER_BYTES);
    const dv = new DataView(hdr);
    dv.setUint32(0, INDEX_MAGIC, true);
    dv.setUint32(4, INDEX_VERSION, true);
    dv.setUint32(8, this.blockSize, true);
    dv.setUint32(12, this.maxOpfsBlocks, true);
    this.indexHandle.truncate(INDEX_HEADER_BYTES);
    this.indexHandle.write(new Uint8Array(hdr), { at: 0 });
    this.indexHandle.flush();
    this.indexWriteOffset = INDEX_HEADER_BYTES;
  }

  _loadIndexFromOpfs() {
    this.blockMap.clear();
    this.slotOwner.fill(null);
    this.nextSlot = 0;

    if (!this.indexHandle || !this.blocksHandle) return;
    const size = this.indexHandle.getSize();
    if (size < INDEX_HEADER_BYTES) {
      this._writeFreshIndexHeader();
      return;
    }

    const raw = new Uint8Array(size);
    this.indexHandle.read(raw, { at: 0 });
    const dv = new DataView(raw.buffer);
    if (
      dv.getUint32(0, true) !== INDEX_MAGIC ||
      dv.getUint32(4, true) !== INDEX_VERSION ||
      dv.getUint32(8, true) !== this.blockSize
    ) {
      this.blocksHandle.truncate(0);
      this._writeFreshIndexHeader();
      return;
    }

    // Build lookup by (fpLo, fpHi) so manifest additions/reordering never invalidate cached files!
    const byFp = new Map();
    for (let i = 1; i < this.byId.length; ++i) {
      const m = this.byId[i];
      byFp.set(`${m.fpLo}:${m.fpHi}`, m);
    }

    const blocksFileBytes = this.blocksHandle.getSize();
    let maxUsedSlot = -1;
    let offset = INDEX_HEADER_BYTES;
    while (offset + INDEX_RECORD_BYTES <= size) {
      const magic = dv.getUint32(offset + 0, true);
      if (magic !== INDEX_REC_MAGIC) break;
      const fpLo = dv.getUint32(offset + 4, true);
      const fpHi = dv.getUint32(offset + 8, true);
      const blockIdx = dv.getUint32(offset + 12, true);
      const slot = dv.getUint32(offset + 16, true);
      const byteLen = dv.getUint32(offset + 20, true);
      offset += INDEX_RECORD_BYTES;

      if (slot >= this.maxOpfsBlocks) continue;
      if ((slot + 1) * this.blockSize > blocksFileBytes && (slot * this.blockSize + byteLen) > blocksFileBytes) {
        continue;
      }

      const meta = byFp.get(`${fpLo}:${fpHi}`);
      if (!meta || blockIdx >= meta.numBlocks) continue;

      const key = `${meta.id}:${blockIdx}`;
      const prevOwner = this.slotOwner[slot];
      if (prevOwner && prevOwner !== key) {
        this.blockMap.delete(prevOwner);
      }
      this.blockMap.set(key, { slot, len: byteLen });
      this.slotOwner[slot] = key;
      if (slot > maxUsedSlot) maxUsedSlot = slot;
    }

    this.nextSlot = (maxUsedSlot + 1) % this.maxOpfsBlocks;
    this.indexWriteOffset = offset;
  }

  _loadSavesFromOpfs() {
    this.saves.clear();
    if (!this.savesHandle) return;
    const size = this.savesHandle.getSize();
    if (size < 8) return;
    const raw = new Uint8Array(size);
    this.savesHandle.read(raw, { at: 0 });
    const dv = new DataView(raw.buffer);
    if (dv.getUint32(0, true) !== SAVES_MAGIC) return;
    const count = dv.getUint32(4, true);
    let pos = 8;
    const dec = new TextDecoder();
    for (let i = 0; i < count; ++i) {
      if (pos + 8 > size) break;
      const nameLen = dv.getUint32(pos, true);
      const dataLen = dv.getUint32(pos + 4, true);
      pos += 8;
      if (pos + nameLen + dataLen > size) break;
      const name = dec.decode(raw.subarray(pos, pos + nameLen));
      pos += nameLen;
      const data = raw.slice(pos, pos + dataLen);
      pos += dataLen;
      this.saves.set(name, data);
    }
  }

  _flushSavesToOpfs() {
    if (!this.savesHandle) return;
    const enc = new TextEncoder();
    const entries = [];
    let total = 8;
    for (const [name, data] of this.saves.entries()) {
      const nameBytes = enc.encode(name);
      entries.push([nameBytes, data]);
      total += 8 + nameBytes.byteLength + data.byteLength;
    }
    const out = new Uint8Array(total);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, SAVES_MAGIC, true);
    dv.setUint32(4, entries.length, true);
    let pos = 8;
    for (const [nameBytes, data] of entries) {
      dv.setUint32(pos, nameBytes.byteLength, true);
      dv.setUint32(pos + 4, data.byteLength, true);
      pos += 8;
      out.set(nameBytes, pos);
      pos += nameBytes.byteLength;
      out.set(data, pos);
      pos += data.byteLength;
    }
    this.savesHandle.truncate(total);
    this.savesHandle.write(out, { at: 0 });
    this.savesHandle.flush();
    this.stats.savesFlushes++;
  }

  clearCache() {
    this._cacheEpoch++;
    this._inflightBlocks.clear();
    this._failedBlocks.clear();
    this._seqTracker.clear();
    this.ramCache.clear();
    this.blockMap.clear();
    this.slotOwner.fill(null);
    this.nextSlot = 0;
    if (this.blocksHandle) {
      this.blocksHandle.truncate(0);
      this.blocksHandle.flush();
    }
    if (this.indexHandle) {
      this._writeFreshIndexHeader();
    }
    this.resetStats();
  }

  clearSaves() {
    this.saves.clear();
    this._flushSavesToOpfs();
  }

  exportSaves() {
    const files = [];
    for (const [name, data] of this.saves.entries()) {
      let bin = '';
      for (let i = 0; i < data.length; ++i) bin += String.fromCharCode(data[i]);
      files.push({ name, bytes: data.length, dataBase64: btoa(bin) });
    }
    return { version: 1, exportedAt: new Date().toISOString(), files };
  }

  importSaves(bundle) {
    if (!bundle || !Array.isArray(bundle.files)) {
      throw new Error('Invalid save backup format (expected { files: [...] })');
    }
    let count = 0;
    for (const entry of bundle.files) {
      if (!entry || typeof entry.name !== 'string') continue;
      let bytes = null;
      if (entry.bytes instanceof Uint8Array) {
        bytes = new Uint8Array(entry.bytes);
      } else if (ArrayBuffer.isView(entry.bytes)) {
        bytes = new Uint8Array(entry.bytes.buffer, entry.bytes.byteOffset, entry.bytes.byteLength).slice();
      } else if (entry.bytes instanceof ArrayBuffer) {
        bytes = new Uint8Array(entry.bytes).slice();
      } else if (typeof entry.dataBase64 === 'string') {
        const bin = atob(entry.dataBase64);
        bytes = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; ++i) bytes[i] = bin.charCodeAt(i);
      }
      if (!bytes) continue;
      let key = entry.name.replace(/\\/g, '/').replace(/^\/+/, '').toLowerCase();
      if (key.startsWith('saves/')) key = key.slice(6);
      // Place bare native SWBF2 save files (.profile, .gc, .rote, .sav) into savegames/ so
      // SaveDevice::StartRefreshFileList() discovers them immediately.
      if (!key.includes('/') && /\.(profile|gc|rote|sav)$/i.test(key)) {
        key = `savegames/${key}`;
      }
      if (!key || key.includes('..')) continue;
      this.saves.set(key, bytes);
      ++count;
    }
    if (count > 0) {
      this._flushSavesToOpfs();
    }
    return count;
  }

  closeHandles() {
    this._cacheEpoch++;
    this._inflightBlocks.clear();
    this._failedBlocks.clear();
    try { this.blocksHandle?.flush(); this.blocksHandle?.close(); } catch {}
    try { this.indexHandle?.flush(); this.indexHandle?.close(); } catch {}
    try { this.savesHandle?.flush(); this.savesHandle?.close(); } catch {}
    this.blocksHandle = null;
    this.indexHandle = null;
    this.savesHandle = null;
  }

  resetStats() {
    this.stats.mode = this.mode;
    this.stats.httpRequests = 0;
    this.stats.httpBytes = 0;
    this.stats.localReads = 0;
    this.stats.localBytes = 0;
    this.stats.opfsHits = 0;
    this.stats.opfsHitBytes = 0;
    this.stats.memHits = 0;
    this.stats.memHitBytes = 0;
    this.stats.opfsWrites = 0;
    this.stats.opfsEvictions = 0;
    this.stats.perFile = {};
  }

  getStats() {
    return {
      mode: this.mode,
      localFileCount: this.localFiles ? this.localFiles.size : 0,
      opfsPersistent: this.opfsPersistent,
      cachedBlocks: this.blockMap.size,
      cachedBytes: this.blockMap.size * this.blockSize,
      ramBlocks: this.ramCache.size,
      saveFiles: Array.from(this.saves.keys()),
      ...this.stats,
      perFile: { ...this.stats.perFile },
    };
  }

  // Normalize Windows/engine/MEMFS path into either:
  //   { kind: 'save', key: 'profile.sav' }
  //   { kind: 'game', meta: FileMeta }
  //   null
  _resolve(rawPath, forWrite = false) {
    if (!rawPath) return null;
    let p = rawPath.replace(/\\/g, '/').trim();
    while (p.startsWith('./')) p = p.slice(2);
    const low = p.toLowerCase();

    // Persistent save/profile paths
    if (
      forWrite ||
      low.startsWith('/saves/') ||
      low.startsWith('saves/') ||
      low.startsWith('savegames/') ||
      low.endsWith('.profile') ||
      low.endsWith('.sav')
    ) {
      let key = low.replace(/^\/+/, '');
      if (key.startsWith('saves/')) key = key.slice(6);
      return { kind: 'save', key };
    }

    // Strip engine prefixes: "data/_lvl_pc/", "/data/", "/gamedata/"
    let rel = low;
    if (rel.startsWith('data/_lvl_pc/')) rel = rel.slice('data/_lvl_pc/'.length);
    else if (rel.startsWith('/data/_lvl_pc/')) rel = rel.slice('/data/_lvl_pc/'.length);
    else if (rel.startsWith('/gamedata/')) rel = rel.slice('/gamedata/'.length);
    else if (rel.startsWith('/data/')) rel = rel.slice('/data/'.length);
    else if (rel.startsWith('/')) rel = rel.slice(1);

    let meta = this.byLower.get(rel);
    if (!meta) meta = this.byFlat.get(rel);
    if (meta) return { kind: 'game', meta };

    // Also allow reading an existing save file without explicit write flag
    if (this.saves.has(rel)) {
      return { kind: 'save', key: rel };
    }
    return null;
  }

  _fileStats(meta) {
    let s = this.stats.perFile[meta.path];
    if (!s) {
      s = { size: meta.size, httpBytes: 0, httpRequests: 0, opfsHits: 0, memHits: 0 };
      this.stats.perFile[meta.path] = s;
    }
    return s;
  }

  _ramGet(key) {
    const buf = this.ramCache.get(key);
    if (!buf) return null;
    // Refresh LRU position
    this.ramCache.delete(key);
    this.ramCache.set(key, buf);
    return buf;
  }

  _ramPut(key, buf) {
    if (this.ramCache.has(key)) {
      this.ramCache.delete(key);
    } else if (this.ramCache.size >= RAM_CACHE_MAX_BLOCKS) {
      const oldest = this.ramCache.keys().next().value;
      this.ramCache.delete(oldest);
    }
    this.ramCache.set(key, buf);
  }

  _storeBlockInOpfs(meta, blockIdx, bytes) {
    const key = `${meta.id}:${blockIdx}`;
    this._ramPut(key, bytes);
    if (!this.blocksHandle || !this.indexHandle) return;

    let entry = this.blockMap.get(key);
    let slot;
    if (entry) {
      slot = entry.slot;
    } else {
      slot = this.nextSlot;
      this.nextSlot = (this.nextSlot + 1) % this.maxOpfsBlocks;
      const prevOwner = this.slotOwner[slot];
      if (prevOwner && prevOwner !== key) {
        this.blockMap.delete(prevOwner);
        this.stats.opfsEvictions++;
      }
      this.slotOwner[slot] = key;
      this.blockMap.set(key, { slot, len: bytes.byteLength });
    }

    this.blocksHandle.write(bytes, { at: slot * this.blockSize });
    this.stats.opfsWrites++;

    const rec = new ArrayBuffer(INDEX_RECORD_BYTES);
    const dv = new DataView(rec);
    dv.setUint32(0, INDEX_REC_MAGIC, true);
    dv.setUint32(4, meta.fpLo, true);
    dv.setUint32(8, meta.fpHi, true);
    dv.setUint32(12, blockIdx, true);
    dv.setUint32(16, slot, true);
    dv.setUint32(20, bytes.byteLength, true);
    this.indexHandle.write(new Uint8Array(rec), { at: this.indexWriteOffset });
    this.indexWriteOffset += INDEX_RECORD_BYTES;
  }

  setOnProgress(fn) {
    this.onProgress = typeof fn === 'function' ? fn : null;
  }

  _emitProgress(meta, stage, force = false) {
    if (!this.onProgress || !meta) return;
    const now = performance.now();
    const pathChanged = meta.path !== this._lastProgressPath;
    if (!force && !pathChanged && (now - this._lastProgressEmitMs) < 80) return;
    this._lastProgressEmitMs = now;
    this._lastProgressPath = meta.path;
    const fs = this._fileStats(meta);
    this.onProgress({
      stage,
      file: meta.path,
      fileSize: meta.size,
      fileHttpBytes: fs.httpBytes,
      totalHttpBytes: this.stats.httpBytes,
      httpRequests: this.stats.httpRequests,
      cachedBytes: this.blockMap.size * this.blockSize,
    });
  }

  _fetchBlocksHttp(meta, startBlock, count) {
    const lo = startBlock * this.blockSize;
    const hi = Math.min(meta.size - 1, (startBlock + count) * this.blockSize - 1);
    const expectedLen = hi - lo + 1;
    const url = this.baseUrl + meta.path;

    // Notify main thread immediately before synchronous XHR blocks so WAN users
    // see which asset is actively downloading right away.
    this._emitProgress(meta, 'fetching', meta.path !== this._lastProgressPath);

    const xhr = new XMLHttpRequest();
    xhr.open('GET', url, false); // synchronous XHR is legal and zero-overhead in Dedicated Workers
    xhr.responseType = 'arraybuffer';
    xhr.setRequestHeader('Range', `bytes=${lo}-${hi}`);
    xhr.send();

    if (xhr.status !== 206 && !(xhr.status === 200 && lo === 0 && expectedLen === meta.size)) {
      throw new Error(`HTTP Range fetch failed for ${url} (${lo}-${hi}): status=${xhr.status}`);
    }

    if (xhr.status === 206) {
      const cr = xhr.getResponseHeader('Content-Range') || '';
      const m = /^bytes\s+(\d+)-(\d+)\/(\d+)$/i.exec(cr.trim());
      if (!m || Number(m[1]) !== lo || Number(m[2]) !== hi || Number(m[3]) !== meta.size) {
        throw new Error(`Invalid Content-Range for ${url}: got "${cr}", expected "bytes ${lo}-${hi}/${meta.size}"`);
      }
    }

    const buf = new Uint8Array(xhr.response);
    if (buf.byteLength !== expectedLen) {
      throw new Error(`Short Range read for ${url}: got ${buf.byteLength}, expected ${expectedLen}`);
    }

    this.stats.httpRequests++;
    this.stats.httpBytes += buf.byteLength;
    const fs = this._fileStats(meta);
    fs.httpRequests++;
    fs.httpBytes += buf.byteLength;

    for (let i = 0; i < count; ++i) {
      const sliceStart = i * this.blockSize;
      const sliceEnd = Math.min(buf.byteLength, sliceStart + this.blockSize);
      if (sliceStart >= buf.byteLength) break;
      const blk = buf.slice(sliceStart, sliceEnd);
      this._storeBlockInOpfs(meta, startBlock + i, blk);
    }

    this._emitProgress(meta, 'fetched', false);
  }

  setMusicStreaming(enabled) {
    this.musicStreaming = !!enabled;
  }

  _isStreamingMedia(meta) {
    return meta.lower.startsWith('sound/') || meta.lower.startsWith('movies/');
  }

  _schedulePrefetch(meta, fromBlock) {
    if (!this.musicStreaming && meta.lower.startsWith('sound/')) return;
    if (this._activePrefetches >= MAX_CONCURRENT_PREFETCH) return;
    if (fromBlock >= meta.numBlocks) return;

    // Find the first uncached, non-inflight block within the next 4 blocks ahead.
    let start = -1;
    const scanLimit = Math.min(meta.numBlocks, fromBlock + 4);
    for (let b = fromBlock; b < scanLimit; ++b) {
      const k = `${meta.id}:${b}`;
      if (!this.ramCache.has(k) && !this.blockMap.has(k) && !this._inflightBlocks.has(k)) {
        start = b;
        break;
      }
    }
    if (start < 0) return;

    let count = 0;
    const maxCount = Math.min(PREFETCH_AHEAD_BLOCKS, meta.numBlocks - start);
    while (count < maxCount) {
      const k = `${meta.id}:${start + count}`;
      if (this.ramCache.has(k) || this.blockMap.has(k) || this._inflightBlocks.has(k)) break;
      count++;
    }
    if (count <= 0) return;

    const lo = start * this.blockSize;
    const hi = Math.min(meta.size - 1, (start + count) * this.blockSize - 1);
    const expectedLen = hi - lo + 1;
    const url = this.baseUrl + meta.path;
    const epoch = this._cacheEpoch;
    const keys = [];
    for (let i = 0; i < count; ++i) {
      const k = `${meta.id}:${start + i}`;
      this._inflightBlocks.add(k);
      keys.push(k);
    }
    this._activePrefetches++;

    fetch(url, { headers: { Range: `bytes=${lo}-${hi}` } })
      .then((res) => {
        if (res.status !== 206 && !(res.status === 200 && lo === 0 && expectedLen === meta.size)) {
          throw new Error(`prefetch HTTP ${res.status}`);
        }
        return res.arrayBuffer();
      })
      .then((ab) => {
        if (epoch !== this._cacheEpoch) return;
        const buf = new Uint8Array(ab);
        if (buf.byteLength !== expectedLen) return;
        this.stats.httpRequests++;
        this.stats.httpBytes += buf.byteLength;
        const fs = this._fileStats(meta);
        fs.httpRequests++;
        fs.httpBytes += buf.byteLength;
        for (let i = 0; i < count; ++i) {
          const bIdx = start + i;
          const k = `${meta.id}:${bIdx}`;
          if (this.ramCache.has(k) || this.blockMap.has(k)) continue;
          const sliceStart = i * this.blockSize;
          const sliceEnd = Math.min(buf.byteLength, sliceStart + this.blockSize);
          if (sliceStart >= buf.byteLength) break;
          this._storeBlockInOpfs(meta, bIdx, buf.slice(sliceStart, sliceEnd));
        }
      })
      .catch(() => {})
      .finally(() => {
        this._activePrefetches = Math.max(0, this._activePrefetches - 1);
        for (const k of keys) this._inflightBlocks.delete(k);
      });
  }

  _readBlocksLocal(meta, startBlock, count) {
    const file = meta.localFile || (this.localFiles && this.localFiles.get(meta.lower));
    if (!file) {
      throw new Error(`Local GameData file missing: ${meta.path}`);
    }
    if (!this.readerSync) {
      throw new Error('FileReaderSync is unavailable in this Worker environment.');
    }
    const lo = startBlock * this.blockSize;
    const hi = Math.min(meta.size - 1, (startBlock + count) * this.blockSize - 1);
    const expectedLen = hi - lo + 1;
    const slice = file.slice(lo, hi + 1);
    const buf = new Uint8Array(this.readerSync.readAsArrayBuffer(slice));
    if (buf.byteLength !== expectedLen) {
      throw new Error(`Short local read for ${meta.path}: got ${buf.byteLength}, expected ${expectedLen}`);
    }

    this.stats.localReads++;
    this.stats.localBytes += buf.byteLength;
    const fs = this._fileStats(meta);
    fs.localReads = (fs.localReads || 0) + 1;
    fs.localBytes = (fs.localBytes || 0) + buf.byteLength;

    for (let i = 0; i < count; ++i) {
      const sliceStart = i * this.blockSize;
      const sliceEnd = Math.min(buf.byteLength, sliceStart + this.blockSize);
      if (sliceStart >= buf.byteLength) break;
      const blk = buf.slice(sliceStart, sliceEnd);
      this._ramPut(`${meta.id}:${startBlock + i}`, blk);
    }
  }

  _getBlock(meta, blockIdx, neededEndBlock) {
    const key = `${meta.id}:${blockIdx}`;
    const fs = this._fileStats(meta);
    const isMedia = this._isStreamingMedia(meta);

    // Track sequential block access per file so small C++ ReadData() calls still coalesce
    // into large 1-2 MiB reads instead of serial 256 KiB round-trips.
    let seq = this._seqTracker.get(meta.id);
    if (!seq) {
      seq = { lastBlock: -1, seqHits: 0 };
      this._seqTracker.set(meta.id, seq);
    }
    if (blockIdx === seq.lastBlock + 1) {
      seq.seqHits = Math.min(16, seq.seqHits + 1);
    } else if (blockIdx !== seq.lastBlock) {
      seq.seqHits = 0;
    }
    seq.lastBlock = blockIdx;

    // 1. RAM LRU hit
    const ram = this._ramGet(key);
    if (ram) {
      this.stats.memHits++;
      this.stats.memHitBytes += ram.byteLength;
      fs.memHits++;
      if (this.mode === 'http' && isMedia && (meta.lower.startsWith('movies/') || (this.musicStreaming && seq.seqHits >= 1))) {
        this._schedulePrefetch(meta, blockIdx + 1);
      }
      return ram;
    }

    // 1b. Local BYOC mode -> synchronous FileReaderSync directly from user's local GameData
    if (this.mode === 'local') {
      const aheadBlocks = seq.seqHits >= 2 ? 8 : (seq.seqHits >= 1 ? 4 : 2);
      let span = 1;
      const maxSpan = Math.min(MAX_COALESCE_BLOCKS, meta.numBlocks - blockIdx);
      const wantEnd = Math.min(meta.numBlocks - 1, Math.max(neededEndBlock, blockIdx + aheadBlocks - 1));
      while (span < maxSpan && (blockIdx + span) <= wantEnd) {
        if (this.ramCache.has(`${meta.id}:${blockIdx + span}`)) break;
        span++;
      }
      this._readBlocksLocal(meta, blockIdx, span);
      return this._ramGet(key);
    }

    // 2. OPFS SyncAccessHandle hit (HTTP streaming mode)
    const opfsEntry = this.blockMap.get(key);
    if (opfsEntry && this.blocksHandle) {
      const blk = new Uint8Array(opfsEntry.len);
      const n = this.blocksHandle.read(blk, { at: opfsEntry.slot * this.blockSize });
      if (n === opfsEntry.len) {
        this._ramPut(key, blk);
        this.stats.opfsHits++;
        this.stats.opfsHitBytes += n;
        fs.opfsHits++;
        if (isMedia && (meta.lower.startsWith('movies/') || (this.musicStreaming && seq.seqHits >= 1))) {
          this._schedulePrefetch(meta, blockIdx + 1);
        }
        return blk;
      }
    }

    // 3. Cache miss -> adaptive sequential read-ahead coalescing (up to MAX_COALESCE_BLOCKS = 2 MiB).
    // Voiceover clips inside sound/*.lvl are short (15-40 KiB) random seeks, so keep single-block
    // 256 KiB fetches on non-sequential sound reads to minimize WAN bandwidth while ramping up to
    // 1-2 MiB coalescing + async background prefetch only for continuous sequential music/movies.
    let aheadBlocks = 1;
    if (isMedia) {
      if (!this.musicStreaming && meta.lower.startsWith('sound/')) {
        aheadBlocks = 1;
      } else {
        aheadBlocks = seq.seqHits >= 2 ? 8 : (seq.seqHits >= 1 ? 4 : 1);
      }
    } else if (seq.seqHits >= 2) {
      aheadBlocks = 8;
    } else if (seq.seqHits >= 1) {
      aheadBlocks = 4;
    } else if (meta.size >= this.blockSize * 2) {
      aheadBlocks = 2;
    }

    let span = 1;
    const maxSpan = Math.min(MAX_COALESCE_BLOCKS, meta.numBlocks - blockIdx);
    const wantEnd = Math.min(meta.numBlocks - 1, Math.max(neededEndBlock, blockIdx + aheadBlocks - 1));
    while (span < maxSpan && (blockIdx + span) <= wantEnd) {
      const kNext = `${meta.id}:${blockIdx + span}`;
      if (this.ramCache.has(kNext) || this.blockMap.has(kNext)) break;
      span++;
    }
    this._fetchBlocksHttp(meta, blockIdx, span);
    if (isMedia && (meta.lower.startsWith('movies/') || (this.musicStreaming && seq.seqHits >= 1))) {
      this._schedulePrefetch(meta, blockIdx + span);
    }
    return this._ramGet(key);
  }

  // ---- Synchronous C++ PblFile bridge methods ------------------------------

  open(rawPath, modeFlags) {
    // modeFlags: 0 = read-only, 1 = create/truncate rw, 2 = read-write (create if missing)
    const writable = modeFlags !== 0;
    const target = this._resolve(rawPath, writable);
    if (!target) return -1;

    if (target.kind === 'save') {
      let data = this.saves.get(target.key);
      if (modeFlags === 1) {
        data = new Uint8Array(0);
        this.saves.set(target.key, data);
      } else if (!data) {
        if (modeFlags === 2) {
          data = new Uint8Array(0);
          this.saves.set(target.key, data);
        } else {
          return -1;
        }
      }
      const fd = this.nextFd++;
      this.fds.set(fd, {
        kind: 'save',
        key: target.key,
        writable,
        dirty: modeFlags === 1,
        buf: data.slice(),
        size: data.byteLength,
      });
      return fd;
    }

    if (writable) return -1; // GameData mirror files are strictly read-only
    const fd = this.nextFd++;
    this.fds.set(fd, { kind: 'game', meta: target.meta, size: target.meta.size });
    return fd;
  }

  getSizeByFd(fd) {
    const h = this.fds.get(fd);
    return h ? h.size : -1;
  }

  getSizeByPath(rawPath) {
    const target = this._resolve(rawPath, false);
    if (!target) return -1;
    if (target.kind === 'save') {
      const d = this.saves.get(target.key);
      return d ? d.byteLength : -1;
    }
    return target.meta.size;
  }

  exists(rawPath) {
    return this.getSizeByPath(rawPath) >= 0 ? 1 : 0;
  }

  deletePath(rawPath) {
    const target = this._resolve(rawPath, true);
    if (!target || target.kind !== 'save') return 0;
    if (this.saves.delete(target.key)) {
      this._flushSavesToOpfs();
      return 1;
    }
    return 0;
  }

  readInto(fd, offset, dstUint8, length) {
    const h = this.fds.get(fd);
    if (!h || offset < 0 || offset >= h.size || length <= 0) return 0;
    const toRead = Math.min(length, h.size - offset);

    if (h.kind === 'save') {
      dstUint8.set(h.buf.subarray(offset, offset + toRead), 0);
      return toRead;
    }

    const meta = h.meta;
    const endBlock = Math.floor((offset + toRead - 1) / this.blockSize);
    let copied = 0;
    let cur = offset;
    while (copied < toRead) {
      const blockIdx = Math.floor(cur / this.blockSize);
      const inBlock = cur - blockIdx * this.blockSize;
      const blk = this._getBlock(meta, blockIdx, endBlock);
      if (!blk) break;
      const take = Math.min(toRead - copied, blk.byteLength - inBlock);
      if (take <= 0) break;
      dstUint8.set(blk.subarray(inBlock, inBlock + take), copied);
      copied += take;
      cur += take;
    }
    return copied;
  }

  writeFrom(fd, offset, srcUint8) {
    const h = this.fds.get(fd);
    if (!h || h.kind !== 'save' || !h.writable || offset < 0) return 0;
    const end = offset + srcUint8.byteLength;
    if (end > h.buf.byteLength) {
      const next = new Uint8Array(Math.max(end, h.buf.byteLength * 2, 4096));
      next.set(h.buf, 0);
      h.buf = next;
    }
    h.buf.set(srcUint8, offset);
    if (end > h.size) h.size = end;
    h.dirty = true;
    return srcUint8.byteLength;
  }

  flush(fd) {
    const h = this.fds.get(fd);
    if (!h) return;
    if (h.kind === 'save' && h.dirty) {
      this.saves.set(h.key, h.buf.slice(0, h.size));
      this._flushSavesToOpfs();
      h.dirty = false;
    }
  }

  close(fd) {
    const h = this.fds.get(fd);
    if (!h) return;
    if (h.kind === 'save' && h.dirty) {
      this.saves.set(h.key, h.buf.slice(0, h.size));
      this._flushSavesToOpfs();
      h.dirty = false;
    }
    this.fds.delete(fd);
  }

  listSaves(prefix = '') {
    let norm = String(prefix || '').replace(/\\/g, '/').toLowerCase().replace(/^\/+/, '');
    if (norm.startsWith('saves/')) norm = norm.slice(6);
    const out = [];
    for (const key of this.saves.keys()) {
      if (!norm || key.startsWith(norm)) {
        out.push(key);
      }
    }
    out.sort();
    return out;
  }

  // Non-blocking cache probe for asynchronous Snd::File::Read() stream I/O.
  // Returns 1 if every block in [offset, offset + length) is already in RAM or OPFS,
  // 0 if one or more blocks still need async HTTP fetch, or -1 on error / failed fetch.
  isRangeCached(fd, offset, length) {
    const h = this.fds.get(fd);
    if (!h) return -1;
    if (h.kind === 'save' || this.mode === 'local' || length <= 0 || offset < 0 || offset >= h.size) return 1;
    const toCheck = Math.min(length, h.size - offset);
    if (toCheck <= 0) return 1;
    const meta = h.meta;
    const startBlock = Math.floor(offset / this.blockSize);
    const endBlock = Math.floor((offset + toCheck - 1) / this.blockSize);
    const now = performance.now();
    for (let b = startBlock; b <= endBlock; ++b) {
      const k = `${meta.id}:${b}`;
      if (this.ramCache.has(k) || this.blockMap.has(k)) continue;
      const failTs = this._failedBlocks.get(k);
      if (failTs !== undefined && (now - failTs) < 5000) return -1;
      return 0;
    }
    return 1;
  }

  // Non-blocking async fetch for an exact range needed by Snd::File::Read().
  // Never blocks the worker render/sim thread; completes asynchronously into RAM + OPFS.
  prefetchRangeAsync(fd, offset, length) {
    if (this.mode === 'local') return;
    const h = this.fds.get(fd);
    if (!h || h.kind !== 'game' || length <= 0 || offset < 0 || offset >= h.size) return;
    const toFetch = Math.min(length, h.size - offset);
    if (toFetch <= 0) return;
    const meta = h.meta;
    const startBlock = Math.floor(offset / this.blockSize);
    const endBlock = Math.floor((offset + toFetch - 1) / this.blockSize);

    // Collect contiguous runs of missing, non-inflight blocks.
    let b = startBlock;
    while (b <= endBlock) {
      const k = `${meta.id}:${b}`;
      if (this.ramCache.has(k) || this.blockMap.has(k) || this._inflightBlocks.has(k)) {
        ++b;
        continue;
      }
      const runStart = b;
      let runCount = 0;
      const maxRun = Math.min(MAX_COALESCE_BLOCKS, meta.numBlocks - runStart);
      while (runCount < maxRun && (runStart + runCount) <= endBlock) {
        const kr = `${meta.id}:${runStart + runCount}`;
        if (this.ramCache.has(kr) || this.blockMap.has(kr) || this._inflightBlocks.has(kr)) break;
        this._failedBlocks.delete(kr);
        runCount++;
      }
      if (runCount > 0) {
        const lo = runStart * this.blockSize;
        const hi = Math.min(meta.size - 1, (runStart + runCount) * this.blockSize - 1);
        const expectedLen = hi - lo + 1;
        const url = this.baseUrl + meta.path;
        const epoch = this._cacheEpoch;
        const keys = [];
        for (let i = 0; i < runCount; ++i) {
          const kr = `${meta.id}:${runStart + i}`;
          this._inflightBlocks.add(kr);
          keys.push(kr);
        }
        fetch(url, { headers: { Range: `bytes=${lo}-${hi}` } })
          .then((res) => {
            if (res.status !== 206 && !(res.status === 200 && lo === 0 && expectedLen === meta.size)) {
              throw new Error(`async range HTTP ${res.status}`);
            }
            return res.arrayBuffer();
          })
          .then((ab) => {
            if (epoch !== this._cacheEpoch) return;
            const buf = new Uint8Array(ab);
            if (buf.byteLength !== expectedLen) throw new Error('short async range read');
            this.stats.httpRequests++;
            this.stats.httpBytes += buf.byteLength;
            const fs = this._fileStats(meta);
            fs.httpRequests++;
            fs.httpBytes += buf.byteLength;
            for (let i = 0; i < runCount; ++i) {
              const bIdx = runStart + i;
              const kr = `${meta.id}:${bIdx}`;
              if (this.ramCache.has(kr) || this.blockMap.has(kr)) continue;
              const sliceStart = i * this.blockSize;
              const sliceEnd = Math.min(buf.byteLength, sliceStart + this.blockSize);
              if (sliceStart >= buf.byteLength) break;
              this._storeBlockInOpfs(meta, bIdx, buf.slice(sliceStart, sliceEnd));
            }
          })
          .catch(() => {
            const failAt = performance.now();
            for (const kr of keys) this._failedBlocks.set(kr, failAt);
          })
          .finally(() => {
            for (const kr of keys) this._inflightBlocks.delete(kr);
          });
      }
      b = runStart + Math.max(1, runCount);
    }
  }

  attachToModule(Module) {
    Module.vfsOpen = (path, mode) => this.open(path, mode);
    Module.vfsClose = (fd) => this.close(fd);
    Module.vfsGetSizeFd = (fd) => this.getSizeByFd(fd);
    Module.vfsGetSizePath = (path) => this.getSizeByPath(path);
    Module.vfsExists = (path) => this.exists(path);
    Module.vfsDelete = (path) => this.deletePath(path);
    Module.vfsFlush = (fd) => this.flush(fd);
    Module.vfsIsRangeCached = (fd, offset, len) => this.isRangeCached(fd, offset, len);
    Module.vfsPrefetchRange = (fd, offset, len) => this.prefetchRangeAsync(fd, offset, len);
    Module.vfsListSaves = (prefix, dstPtr, maxLen) => {
      const joined = this.listSaves(prefix).join('\n');
      const enc = new TextEncoder().encode(joined);
      const n = Math.min(enc.byteLength, Math.max(0, maxLen - 1));
      if (n > 0) Module.HEAPU8.set(enc.subarray(0, n), dstPtr);
      if (maxLen > 0) Module.HEAPU8[dstPtr + n] = 0;
      return n;
    };
    Module.vfsRead = (fd, offset, dstPtr, len) => {
      const dst = Module.HEAPU8.subarray(dstPtr, dstPtr + len);
      return this.readInto(fd, offset, dst, len);
    };
    Module.vfsWrite = (fd, offset, srcPtr, len) => {
      const src = Module.HEAPU8.subarray(srcPtr, srcPtr + len);
      return this.writeFrom(fd, offset, src);
    };
  }
}
