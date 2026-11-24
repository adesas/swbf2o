// Browser & native implementation of PebbleFL's PblFile.
//
// Supports two backends transparently:
//   1. OPFS On-Demand Streaming VFS (Dedicated Worker): when the JS host attaches Module.vfsOpen
//      (see browser/vfs_opfs.mjs), PblFile streams 256 KiB slices on demand from the GameData
//      mirror over HTTP Range + OPFS SyncAccessHandle, with a 64 KiB in-WASM read-ahead window so
//      thousands of tiny Read8/16/32/Float chunk reads stay inside WASM linear memory without
//      crossing the JS boundary. Persistent /saves/* files write synchronously to OPFS saves.bin.
//   2. Standard POSIX FILE* fallback (native i386 oracles, Node.js tests, legacy MEMFS demo):
//      when Module.vfsOpen is not attached, PblFile opens files through stdio fopen/fread/fseek.
//
// The PC build parks its platform handle in `_Buffer`; we store a heap-allocated BrowserFileHandle*
// there while `_bIsMemFile == false`, preserving the exact PblFile class layout from PblFile.h.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "PebbleFL/Common/PblFile.h"

void(__cdecl* PblFile::_blockCallbackFunc)(void) = 0;

int g_traceFile = 0;
#define TRACE(...) do { if (g_traceFile) { fprintf(stderr, __VA_ARGS__); } } while (0)

extern void Log(const char* fmt, ...);
extern "C" __attribute__((weak)) void swbf2_wgpu_arm_load_banner_capture(int mode);

// Returns: 0 = not a load screen LVL, 1 = load/<map>.lvl planet banner, 2 = load/load.lvl UI assets
static int ClassifyLoadingLvlPath(const char* p)
{
    if (!p || !p[0]) return 0;
    char low[256];
    size_t i = 0;
    for (; p[i] && i + 1 < sizeof(low); ++i)
    {
        char c = p[i];
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        low[i] = c;
    }
    low[i] = '\0';
    const char* hit = strstr(low, "load/");
    if (!hit) return 0;
    if (hit != low && hit[-1] != '/') return 0;
    const char* fname = hit + 5;
    if (strchr(fname, '/')) return 0;
    size_t flen = strlen(fname);
    if (flen < 5 || strcmp(fname + flen - 4, ".lvl") != 0) return 0;
    if (strcmp(fname, "shell.lvl") == 0) return 0;
    if (strcmp(fname, "load.lvl") == 0) return 2;
    return 1;
}

static bool g_saidNoDestination = false;

namespace
{
struct TraceFromEnvironment
{
    TraceFromEnvironment()
    {
        const char* p = getenv("SWBF2_TRACE");
        g_traceFile = (p && p[0] && p[0] != '0') ? 1 : 0;
    }
} g_traceFromEnvironment;

struct BrowserFileHandle
{
    static constexpr uint32_t kWindowCap = 65536;

    bool isVfs;
    bool writable;
    int vfsFd;
    FILE* fp;
    uint32_t pos;
    uint32_t size;

    // In-WASM 64 KiB read-ahead window for read-only VFS handles: avoids crossing WASM->JS on
    // every 4-byte chunk header / scalar read while never fetching past a Seek().
    uint32_t winStart;
    uint32_t winLen;
    uint8_t win[kWindowCap];
};

static inline BrowserFileHandle* HandleOf(const PblFile* f)
{
    return (BrowserFileHandle*)(void*)f->_Buffer;
}

static inline void SetHandle(PblFile* f, BrowserFileHandle* h)
{
    f->_Buffer = (char*)(void*)h;
}

static inline long FileSize(FILE* h)
{
    long here = ftell(h);
    fseek(h, 0, SEEK_END);
    long size = ftell(h);
    fseek(h, here, SEEK_SET);
    return size;
}
}  // namespace

#ifdef __EMSCRIPTEN__
EM_JS(int, js_vfs_active, (), {
  return (typeof Module !== 'undefined' && typeof Module.vfsOpen === 'function') ? 1 : 0;
});
EM_JS(int, js_vfs_open, (const char* path, int mode), {
  return Module.vfsOpen(UTF8ToString(path), mode);
});
EM_JS(void, js_vfs_close, (int fd), {
  Module.vfsClose(fd);
});
EM_JS(int, js_vfs_size_fd, (int fd), {
  return Module.vfsGetSizeFd(fd);
});
EM_JS(int, js_vfs_size_path, (const char* path), {
  return Module.vfsGetSizePath(UTF8ToString(path));
});
EM_JS(int, js_vfs_exists, (const char* path), {
  return Module.vfsExists(UTF8ToString(path));
});
EM_JS(void, js_vfs_delete, (const char* path), {
  Module.vfsDelete(UTF8ToString(path));
});
EM_JS(void, js_vfs_flush, (int fd), {
  Module.vfsFlush(fd);
});
EM_JS(int, js_vfs_read, (int fd, uint32_t offset, void* dst, int len), {
  return Module.vfsRead(fd, offset >>> 0, dst >>> 0, len);
});
EM_JS(int, js_vfs_write, (int fd, uint32_t offset, const void* src, int len), {
  return Module.vfsWrite(fd, offset >>> 0, src >>> 0, len);
});
EM_JS(int, js_vfs_is_range_cached, (int fd, uint32_t offset, int len), {
  return (typeof Module !== 'undefined' && typeof Module.vfsIsRangeCached === 'function')
    ? Module.vfsIsRangeCached(fd, offset >>> 0, len) : 1;
});
EM_JS(void, js_vfs_prefetch_range, (int fd, uint32_t offset, int len), {
  if (typeof Module !== 'undefined' && typeof Module.vfsPrefetchRange === 'function') {
    Module.vfsPrefetchRange(fd, offset >>> 0, len);
  }
});
#else
static inline int js_vfs_active() { return 0; }
static inline int js_vfs_open(const char*, int) { return -1; }
static inline int js_vfs_is_range_cached(int, uint32_t, int) { return 1; }
static inline void js_vfs_prefetch_range(int, uint32_t, int) {}
static inline void js_vfs_close(int) {}
static inline int js_vfs_size_fd(int) { return -1; }
static inline int js_vfs_size_path(const char*) { return -1; }
static inline int js_vfs_exists(const char*) { return 0; }
static inline void js_vfs_delete(const char*) {}
static inline void js_vfs_flush(int) {}
static inline int js_vfs_read(int, uint32_t, void*, int) { return 0; }
static inline int js_vfs_write(int, uint32_t, const void*, int) { return 0; }
#endif

#define ENGINE_DATA_ROOT "data\\_lvl_pc\\"
#define ENGINE_DATA_ROOT_LEN (sizeof(ENGINE_DATA_ROOT) - 1)

static const char* DataRoot()
{
    const char* p = getenv("SWBF2_DATA_ROOT");
    if (p && p[0])
    {
        static char acRoot[512];
        snprintf(acRoot, sizeof(acRoot), "%s", p);
        const size_t n = strlen(acRoot);
        if (n > 0 && acRoot[n - 1] != '/')
        {
            acRoot[n] = '/';
            acRoot[n + 1] = '\0';
        }
        return acRoot;
    }
    return "/data/";
}

static const char* TranslatePath(const char* pcFileName, char* pBuffer, int iBufferSize,
                                 bool bTree)
{
    if (!pcFileName || !pBuffer || iBufferSize <= 0)
    {
        return 0;
    }

    const char* pRel = pcFileName;
    if (strncmp(pRel, ENGINE_DATA_ROOT, ENGINE_DATA_ROOT_LEN) == 0)
    {
        pRel += ENGINE_DATA_ROOT_LEN;
    }
    else if (strncmp(pRel, "data\\_lvl_pc/", ENGINE_DATA_ROOT_LEN - 1) == 0)
    {
        pRel += ENGINE_DATA_ROOT_LEN - 1;
    }

    int i = 0;
    if (pRel[0] == '/')
    {
        for (const char* p = pRel; *p && i + 1 < iBufferSize; ++p)
        {
            pBuffer[i++] = *p;
        }
        pBuffer[i] = '\0';
        return pBuffer;
    }

    const char* pcRoot = DataRoot();
    const int iRootLen = (int)strlen(pcRoot);
    if (i + iRootLen < iBufferSize)
    {
        memcpy(pBuffer, pcRoot, (size_t)iRootLen);
        i += iRootLen;
    }

    for (const char* p = pRel; *p && i + 1 < iBufferSize; ++p)
    {
        if (*p == '\\' || *p == '/')
        {
            pBuffer[i++] = bTree ? '/' : '_';
        }
        else
        {
            pBuffer[i++] = *p;
        }
    }
    pBuffer[i] = '\0';
    return pBuffer;
}

static const char* ResolveCaseInsensitive(const char* pcPath, char* pBuffer, int iBufferSize)
{
    if (!pcPath || !pBuffer || iBufferSize <= 0)
    {
        return 0;
    }

    const char* pSlash = strrchr(pcPath, '/');
    if (!pSlash)
    {
        return 0;
    }

    const int iDirLen = (int)(pSlash - pcPath);
    const char* pBase = pSlash + 1;
    if (!pBase[0])
    {
        return 0;
    }

    char acDir[256];
    if (iDirLen <= 0)
    {
        snprintf(acDir, sizeof(acDir), "/");
    }
    else if (iDirLen < (int)sizeof(acDir))
    {
        memcpy(acDir, pcPath, iDirLen);
        acDir[iDirLen] = '\0';
    }
    else
    {
        return 0;
    }

    DIR* pDir = opendir(acDir);
    if (!pDir)
    {
        return 0;
    }

    const char* pFound = 0;
    struct dirent* pEntry;
    while ((pEntry = readdir(pDir)) != 0)
    {
        if (strcasecmp(pEntry->d_name, pBase) == 0)
        {
            snprintf(pBuffer, iBufferSize, "%s/%s", acDir, pEntry->d_name);
            pFound = pBuffer;
            break;
        }
    }
    closedir(pDir);
    return pFound;
}

static FILE* OpenDataFile(const char* pcFileName, const char* pcMode, char* pPathOut,
                          int iPathOutSize)
{
    char acTree[512];
    char acFlat[512];
    const char* pcTree = TranslatePath(pcFileName, acTree, (int)sizeof(acTree), true);
    const char* pcFlat = TranslatePath(pcFileName, acFlat, (int)sizeof(acFlat), false);
    const char* apCandidates[2] = {pcTree, pcFlat};

    for (int i = 0; i < 2; ++i)
    {
        FILE* h = fopen(apCandidates[i], pcMode);
        if (h)
        {
            snprintf(pPathOut, iPathOutSize, "%s", apCandidates[i]);
            return h;
        }
        char acFound[512];
        const char* pcFound = ResolveCaseInsensitive(apCandidates[i], acFound, (int)sizeof(acFound));
        if (pcFound)
        {
            h = fopen(pcFound, pcMode);
            if (h)
            {
                snprintf(pPathOut, iPathOutSize, "%s", pcFound);
                return h;
            }
        }
    }
    return 0;
}

static bool OpenCommon(PblFile* self, const char* pcFileName, int vfsMode, const char* stdioMode,
                       bool bOSBuffer, bool bBuffer)
{
    if (self->_bIsOpen)
    {
        self->Close();
    }

    if (js_vfs_active())
    {
        int fd = js_vfs_open(pcFileName, vfsMode);
        if (fd >= 0)
        {
            BrowserFileHandle* h = (BrowserFileHandle*)calloc(1, sizeof(BrowserFileHandle));
            if (!h)
            {
                js_vfs_close(fd);
                return false;
            }
            int sz = js_vfs_size_fd(fd);
            h->isVfs = true;
            h->writable = (vfsMode != 0);
            h->vfsFd = fd;
            h->pos = 0;
            h->size = sz > 0 ? (uint32_t)sz : 0u;
            h->winStart = 0;
            h->winLen = 0;
            TRACE("vfs open %s fd=%d size=%u mode=%d\n", pcFileName, fd, h->size, vfsMode);
            if (vfsMode == 0 && swbf2_wgpu_arm_load_banner_capture)
            {
                int loadKind = ClassifyLoadingLvlPath(pcFileName);
                if (loadKind > 0) swbf2_wgpu_arm_load_banner_capture(loadKind);
            }
            SetHandle(self, h);
            self->_bIsMemFile = false;
            self->_bAsynchronous = false;
            self->_bOSBuffer = bOSBuffer;
            self->_bBuffer = bBuffer;
            self->_iBufferSize = h->size;
            self->_iPos = 0;
            self->_bIsOpen = true;
            return true;
        }
        // Fall through to stdio only if the file might exist in MEMFS (e.g., hybrid test)
    }

    char acPath[512];
    FILE* fp = OpenDataFile(pcFileName, stdioMode, acPath, (int)sizeof(acPath));
    if (!fp && vfsMode == 2)
    {
        fp = OpenDataFile(pcFileName, "w+b", acPath, (int)sizeof(acPath));
    }
    if (!fp)
    {
        return false;
    }

    BrowserFileHandle* h = (BrowserFileHandle*)calloc(1, sizeof(BrowserFileHandle));
    if (!h)
    {
        fclose(fp);
        return false;
    }
    long sz = FileSize(fp);
    h->isVfs = false;
    h->writable = (vfsMode != 0);
    h->vfsFd = -1;
    h->fp = fp;
    h->pos = 0;
    h->size = sz > 0 ? (uint32_t)sz : 0u;
    TRACE("open %s size %ld\n", acPath, sz);
    SetHandle(self, h);
    self->_bIsMemFile = false;
    self->_bAsynchronous = false;
    self->_bOSBuffer = bOSBuffer;
    self->_bBuffer = bBuffer;
    self->_iBufferSize = h->size;
    self->_iPos = 0;
    self->_bIsOpen = true;
    return true;
}

PblFile::PblFile()
{
    _Buffer = 0;
    _iBufferSize = 0;
    _iPos = 0;
    _bIsOpen = false;
    _bAsynchronous = false;
    _bOSBuffer = false;
    _bBuffer = false;
    _bIsMemFile = false;
}

PblFile::~PblFile()
{
    if (_bIsOpen)
    {
        Close();
    }
}

bool PblFile::Open(const char* pcFileName, bool bAsynchronous, bool bOSBuffer, bool bBuffer)
{
    (void)bAsynchronous;
    return OpenCommon(this, pcFileName, 0, "rb", bOSBuffer, bBuffer);
}

bool PblFile::OpenCreate(const char* pcFileName, bool bAsynchronous, bool bOSBuffer, bool bBuffer)
{
    (void)bAsynchronous;
    return OpenCommon(this, pcFileName, 1, "w+b", bOSBuffer, bBuffer);
}

bool PblFile::OpenReadWrite(const char* pcFileName, bool bAsynchronous, bool bOSBuffer, bool bBuffer)
{
    (void)bAsynchronous;
    return OpenCommon(this, pcFileName, 2, "r+b", bOSBuffer, bBuffer);
}

void PblFile::Close()
{
    if (!_bIsOpen)
    {
        return;
    }

    if (_bIsMemFile)
    {
        MemCloseFile();
    }
    else
    {
        BrowserFileHandle* h = HandleOf(this);
        if (h)
        {
            if (h->isVfs)
            {
                if (h->vfsFd >= 0)
                {
                    js_vfs_close(h->vfsFd);
                }
            }
            else if (h->fp)
            {
                fclose(h->fp);
            }
            free(h);
        }
        _Buffer = 0;
    }

    _bIsOpen = false;
    _bIsMemFile = false;
    _iBufferSize = 0;
    _iPos = 0;
}

void PblFile::Delete(const char* pcFileName)
{
    if (js_vfs_active())
    {
        js_vfs_delete(pcFileName);
        return;
    }
    remove(pcFileName);
}

bool PblFile::Exists(const char* pcFileName)
{
    if (js_vfs_active() && js_vfs_exists(pcFileName))
    {
        return true;
    }
    struct stat st;
    return stat(pcFileName, &st) == 0;
}

int PblFile::GetSize()
{
    if (_bIsMemFile)
    {
        return (int)MemGetSize();
    }

    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return 0;
    }
    if (h->isVfs)
    {
        return (int)h->size;
    }
    return h->fp ? (int)FileSize(h->fp) : 0;
}

int __cdecl PblFile::GetSize(const char* pcFileName)
{
    if (js_vfs_active())
    {
        int sz = js_vfs_size_path(pcFileName);
        if (sz >= 0)
        {
            return sz;
        }
    }
    struct stat st;
    if (stat(pcFileName, &st) != 0)
    {
        return -1;
    }
    return (int)st.st_size;
}

int PblFile::Tell() const
{
    if (_bIsMemFile)
    {
        return (int)MemTell();
    }

    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return 0;
    }
    if (h->isVfs)
    {
        return (int)h->pos;
    }
    return h->fp ? (int)ftell(h->fp) : 0;
}

void PblFile::Seek(int iOffset, SeekType eFrom)
{
    if (_bIsMemFile)
    {
        MemSeek(iOffset, eFrom);
        return;
    }

    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return;
    }

    if (h->isVfs)
    {
        int64_t next = 0;
        if (eFrom == SeekFromStart)
        {
            next = (int64_t)iOffset;
        }
        else if (eFrom == SeekFromHere)
        {
            next = (int64_t)h->pos + (int64_t)iOffset;
        }
        else if (eFrom == SeekFromEnd)
        {
            next = (int64_t)h->size + (int64_t)iOffset;
        }
        if (next < 0)
        {
            next = 0;
        }
        if (next > (int64_t)h->size && !h->writable)
        {
            next = (int64_t)h->size;
        }
        TRACE("vfs seek %d from %d -> %lld\n", iOffset, (int)eFrom, (long long)next);
        h->pos = (uint32_t)next;
        return;
    }

    if (!h->fp)
    {
        return;
    }
    int origin = SEEK_SET;
    if (eFrom == SeekFromHere)
    {
        origin = SEEK_CUR;
    }
    else if (eFrom == SeekFromEnd)
    {
        origin = SEEK_END;
    }
    TRACE("seek %d from %d at %ld\n", iOffset, (int)eFrom, ftell(h->fp));
    fseek(h->fp, (long)iOffset, origin);
}

char PblFile::Read8()
{
    char c = 0;
    ReadData(&c, 1);
    return c;
}

short PblFile::Read16()
{
    unsigned char b[2] = {0, 0};
    ReadData(b, 2);
    return (short)(b[0] | (b[1] << 8));
}

int PblFile::Read32()
{
    unsigned char b[4] = {0, 0, 0, 0};
    ReadData(b, 4);
    return (int)((unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16) |
                 ((unsigned int)b[3] << 24));
}

float PblFile::ReadFloat()
{
    int iBits = Read32();
    float f;
    memcpy(&f, &iBits, 4);
    return f;
}

void PblFile::ReadData(void* pBuffer, int iNumBytes)
{
    if (iNumBytes <= 0)
    {
        return;
    }

    if (_bIsMemFile)
    {
        MemRead(pBuffer, iNumBytes);
        return;
    }

    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return;
    }

    if (!pBuffer)
    {
        if (!g_saidNoDestination)
        {
            g_saidNoDestination = true;
            Log("gl: the engine asked for a D3D9 buffer that this build does not have; "
                "the bytes it wanted are skipped (said once)");
        }
        if (h->isVfs)
        {
            uint32_t next = h->pos + (uint32_t)iNumBytes;
            h->pos = next <= h->size ? next : h->size;
        }
        else if (h->fp)
        {
            fseek(h->fp, (long)iNumBytes, SEEK_CUR);
        }
        return;
    }

    if (h->isVfs)
    {
        const uint32_t want = (uint32_t)iNumBytes;
        if (h->pos >= h->size)
        {
            return;
        }
        const uint32_t avail = h->size - h->pos;
        const uint32_t toRead = want < avail ? want : avail;

        // Fast path: satisfied completely out of our 64 KiB in-WASM read-ahead window (0 JS calls!)
        if (!h->writable && h->winLen > 0 && h->pos >= h->winStart &&
            (h->pos - h->winStart) + toRead <= h->winLen)
        {
            memcpy(pBuffer, h->win + (h->pos - h->winStart), toRead);
            h->pos += toRead;
            return;
        }

        // Large or writable read: read directly into destination (zero double-copy).
        if (h->writable || toRead >= (BrowserFileHandle::kWindowCap / 2))
        {
            int got = js_vfs_read(h->vfsFd, h->pos, pBuffer, (int)toRead);
            if (got > 0)
            {
                h->pos += (uint32_t)got;
            }
            return;
        }

        // Small miss on read-only stream: refill 64 KiB window at current position.
        const uint32_t refill = avail < BrowserFileHandle::kWindowCap ? avail : BrowserFileHandle::kWindowCap;
        int got = js_vfs_read(h->vfsFd, h->pos, h->win, (int)refill);
        if (got > 0)
        {
            h->winStart = h->pos;
            h->winLen = (uint32_t)got;
            const uint32_t take = toRead < h->winLen ? toRead : h->winLen;
            memcpy(pBuffer, h->win, take);
            h->pos += take;
        }
        return;
    }

    TRACE("read %d at %ld\n", iNumBytes, h->fp ? ftell(h->fp) : -1L);
    if (h->fp)
    {
        fread(pBuffer, 1, (size_t)iNumBytes, h->fp);
    }
}

void PblFile::WriteData(void* pBuffer, int iNumBytes)
{
    if (iNumBytes <= 0 || !pBuffer)
    {
        return;
    }

    if (_bIsMemFile)
    {
        MemWrite(pBuffer, iNumBytes);
        return;
    }

    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return;
    }

    if (h->isVfs)
    {
        int wrote = js_vfs_write(h->vfsFd, h->pos, pBuffer, iNumBytes);
        if (wrote > 0)
        {
            h->pos += (uint32_t)wrote;
            if (h->pos > h->size)
            {
                h->size = h->pos;
                _iBufferSize = h->size;
            }
            h->winLen = 0;
        }
        return;
    }

    if (h->fp)
    {
        fwrite(pBuffer, 1, (size_t)iNumBytes, h->fp);
    }
}

void PblFile::WriteString(const char* pcText)
{
    int iLen = (int)strlen(pcText) + 1;
    WriteData((void*)pcText, iLen);
}

void PblFile::Write8(char cValue)
{
    WriteData(&cValue, 1);
}

void PblFile::Write16(short sValue)
{
    WriteData(&sValue, 2);
}

void PblFile::Write32(int iValue)
{
    WriteData(&iValue, 4);
}

void PblFile::WriteFloat(float fValue)
{
    WriteData(&fValue, 4);
}

void PblFile::Flush()
{
    BrowserFileHandle* h = HandleOf(this);
    if (!h)
    {
        return;
    }
    if (h->isVfs)
    {
        js_vfs_flush(h->vfsFd);
    }
    else if (h->fp)
    {
        fflush(h->fp);
    }
}

void PblFile::Suspend()
{
}

void PblFile::Resume(const char* pcFileName)
{
    if (pcFileName)
    {
        Open(pcFileName, false, _bOSBuffer, _bBuffer);
    }
}

bool PblFile::MemOpenFile(void* pBuffer, unsigned int uiSize)
{
    _Buffer = (char*)pBuffer;
    _iBufferSize = uiSize;
    _iPos = 0;
    _bIsMemFile = true;
    _bIsOpen = true;
    _bAsynchronous = false;
    return true;
}

void PblFile::MemCloseFile()
{
    _Buffer = 0;
    _iBufferSize = 0;
    _iPos = 0;
    _bIsMemFile = false;
}

bool PblFile::OpenMemFile(void* pBuffer, unsigned int uiSize)
{
    if (_bIsOpen)
    {
        Close();
    }
    return MemOpenFile(pBuffer, uiSize);
}

void PblFile::CloseMemFile()
{
    MemCloseFile();
    _bIsOpen = false;
}

// Note: SeekAlign, MemGetSize, MemTell, MemRead, MemWrite, MemSeek are defined inline in
// vendor/openswbf2's PebbleFL/Common/PblFile.h.

void __cdecl PblFile::SetBlockCallback(void(__cdecl* pFunc)(void))
{
    _blockCallbackFunc = pFunc;
}

extern "C" int swbf2_pblfile_is_range_cached(const PblFile* f, uint32_t offset, uint32_t length)
{
    if (!f || f->_bIsMemFile) return 1;
    BrowserFileHandle* h = HandleOf(f);
    if (!h || !h->isVfs || h->vfsFd < 0) return 1;
    if (offset >= h->size || length == 0) return 1;
    // Include PblFile's 64 KiB read-ahead window so subsequent ReadData() hits 100% cache!
    uint32_t checkLen = (length < BrowserFileHandle::kWindowCap) ? BrowserFileHandle::kWindowCap : length;
    if (checkLen > h->size - offset) checkLen = h->size - offset;
    return js_vfs_is_range_cached(h->vfsFd, offset, (int)checkLen);
}

extern "C" void swbf2_pblfile_prefetch_range(const PblFile* f, uint32_t offset, uint32_t length)
{
    if (!f || f->_bIsMemFile) return;
    BrowserFileHandle* h = HandleOf(f);
    if (!h || !h->isVfs || h->vfsFd < 0) return;
    if (offset >= h->size || length == 0) return;
    uint32_t fetchLen = (length < BrowserFileHandle::kWindowCap) ? BrowserFileHandle::kWindowCap : length;
    if (fetchLen > h->size - offset) fetchLen = h->size - offset;
    js_vfs_prefetch_range(h->vfsFd, offset, (int)fetchLen);
}

#ifdef __EMSCRIPTEN__
// Diagnostic helper for Phase 1 OPFS persistent /saves verification through real PblFile calls.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_vfs_write_save(const char* pcPath, const char* pcText)
{
    if (!pcPath || !pcText)
    {
        return 0;
    }
    PblFile f;
    if (!f.OpenCreate(pcPath, false, false, false))
    {
        return 0;
    }
    int len = (int)strlen(pcText);
    f.WriteData((void*)pcText, len);
    f.Flush();
    f.Close();
    return len;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_vfs_read_save(const char* pcPath)
{
    static char sBuf[1024];
    sBuf[0] = '\0';
    if (!pcPath)
    {
        return sBuf;
    }
    PblFile f;
    if (!f.Open(pcPath, false, false, false))
    {
        return sBuf;
    }
    int sz = f.GetSize();
    if (sz > (int)sizeof(sBuf) - 1)
    {
        sz = (int)sizeof(sBuf) - 1;
    }
    if (sz > 0)
    {
        f.ReadData(sBuf, sz);
        sBuf[sz] = '\0';
    }
    f.Close();
    return sBuf;
}
#endif
