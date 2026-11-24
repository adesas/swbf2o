// swbf2-browser host: the Win32 entry point the decompilation no longer owns.
//
// pcMain.cpp (excluded from the build) used to do: allocate the engine, create the
// D3D9 device, seed PblRandom, run the message pump. Here the same job is done against
// the browser: a canvas + WebGL2 context, a requestAnimationFrame loop driving the
// engine's fixed 30 Hz logic cadence, and the engine's own file layer reading game
// data out of the VFS.
//
// Everything below deliberately runs through real engine code where engine code exists
// (PblFile -> PblFileChunk -> LuaFL -> Zlib), so the demo exercises the ported engine
// rather than a reimplementation of it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>

#include "PebbleFL/Common/PblFile.h"
#include "PebbleFL/Common/PblChunk.h"
#include "PebbleFL/Common/PblHashTable.h"
#include "RedEngineFL/Graphics/RedCamera.h"

// The engine's own clock. GameState::UpdateTime() is the function the game paces itself with, and
// the three globals below are GameState.cpp's own; it does not declare them in a header, so the host
// declares them the way the engine does - global scope, C++ linkage, same types.
/* The world's half-extent in metres (GameState.cpp:292). The mission's Lua script sets it
   (LuaCallbacks_Mission.cpp:2621); the browser's harness has no mission script, so the engine's
   own default is what it gets. */
extern int WORLD_EXTENTS;

#include "../spike/level_harness.h"
#include "../spike/combat_harness.h"
#include "RedEngineFL/Application/RedTerrain.h"
#include "Battlefront2/Source/GameState.h"
#include "Battlefront2/Source/NetSetup.h"
#include "Battlefront2/Source/LoadUtil.h"
#include "Battlefront2/Source/CollisionMesh.h"
#include "Battlefront2/Source/GameMemory.h"
#include "Battlefront2/Source/CollisionManager.h"
#include "Battlefront2/Source/Chunk.h"
#include "Battlefront2/Source/TriggerRegionManager.h"
#include "Battlefront2/Source/DamageRegion.h"
#include "Battlefront2/Source/CommandPost.h"
#include "Battlefront2/Source/ControllerManager.h"
#include "Battlefront2/Source/AIUtil.h"
#include "Battlefront2/Source/ConnectivityGraph.h"
#include "Battlefront2/Source/CommandPostManager.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Interface/RedInterfaceScreen.h"
#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Application/pc/pcFLRender.h"
#include "RedEngineFL/Util/RedWarning.h"

// browser/platform/redwarning_browser.cpp: the engine's log callback setter (the decompilation
// never produced a body for it) and the format fix that makes the callback carry the message.
extern void RedWarningBrowserInit(void(__cdecl* pCallback)(char const*, ...));
extern float sDeltaTime, sSystemDeltaTime;
extern PblTimeBase::Stamp AppTimeStamp, SystemTimeStamp;

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
static_assert(sizeof(lua_Number) == 4, "SWBF2 retail precompiled Lua bytecode requires 4-byte float lua_Number");

// Build-time facts about the engine archive this host is linked against. tools/build_browser.sh
// derives them from build/core.a and build/results.tsv; the fallbacks keep a bare compile honest
// rather than flattering.
#ifndef SWBF2_ENGINE_OBJECTS
#define SWBF2_ENGINE_OBJECTS 0
#endif
#ifndef SWBF2_ENGINE_SCOPE
#define SWBF2_ENGINE_SCOPE 0
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#ifdef __EMSCRIPTEN__
// A small resident set of decoded game textures, so the screen can show the breadth of what
// the reader handles (DXT1, DXT3, V8U8, A8R8G8B8, ...) instead of one image at a time.
#define kMaxCachedTextures 24
struct CachedTexture
{
    GLuint glId;
    int width;
    int height;
    int glInternal;
    char name[64];
};
static CachedTexture g_cache[kMaxCachedTextures];
static int g_cacheCount = 0;
static int g_montage = 0;

static GLint g_uScale = -1;
static GLint g_uOffset = -1;
static GLint g_uHasTex = -1;
static GLint g_uTint = -1;
#endif

// ---------------------------------------------------------------------------
// fourcc helpers
// ---------------------------------------------------------------------------
static unsigned int FourCC(const char* p)
{
    return (unsigned char)p[0] | ((unsigned char)p[1] << 8) | ((unsigned char)p[2] << 16) |
           ((unsigned char)p[3] << 24);
}

static void FourToStr(unsigned int id, char* out)
{
    out[0] = (char)(id & 0xff);
    out[1] = (char)((id >> 8) & 0xff);
    out[2] = (char)((id >> 16) & 0xff);
    out[3] = (char)((id >> 24) & 0xff);
    out[4] = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (out[i] < 32 || out[i] > 126)
        {
            out[i] = '.';
        }
    }
}

static bool IsFourCC(unsigned int id, const char* s)
{
    return id == FourCC(s);
}

// ---------------------------------------------------------------------------
// log ring, shown by the HTML shell
// ---------------------------------------------------------------------------
#define LOG_LINES 64
static char g_log[LOG_LINES][256];
static int g_logHead = 0;
static int g_logCount = 0;

// Shared with the browser platform layer (browser/platform/*.cpp) so engine-boundary files can
// report what they were asked for into the same ring buffer the page shows.
void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char* line = g_log[g_logHead];
    vsnprintf(line, sizeof(g_log[0]), fmt, ap);
    va_end(ap);

    g_logCount++;
    g_logHead = (g_logHead + 1) % LOG_LINES;

    fflush(stdout);
    fputc('\n', stdout);
    fputs(line, stdout);
    fflush(stdout);
}

/* The engine's own log destination. RedWarning::SetLogCallback takes a printf-style callback
 * (RedWarning.h:67), but the engine calls it with one argument - the message it has already
 * formatted (RedWarning.cpp:342) - so the text is copied verbatim rather than interpreted as a
 * format string. The browser installs this so the engine's warnings and errors reach the page's
 * log: without it they go to the console destination (a RedConsole instance a browser does not
 * have) or to a log file that is not there, and a fatal message is lost. */
static void __cdecl EngineLogCallback(char const* fmt, ...)
{
    Log("engine: %s", fmt ? fmt : "");
}

// One-line result of the last host command, so the JS host never has to pass an output
// buffer around (which would mean exporting malloc/stackAlloc just to call this).
// Sized for the fullest diagnostic line (see swbf2_fbo_probe): a truncated report is a
// misleading one, so the buffer is generous rather than tuned to the shortest message.
static char g_report[1600];

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_last_report(void)
{
    return g_report;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_log_line(int i)
{
    if (i < 0 || i >= LOG_LINES)
    {
        return "";
    }
    int idx = (g_logHead + i) % LOG_LINES;
    if (i >= g_logCount)
    {
        return "";
    }
    return g_log[idx];
}

// ---------------------------------------------------------------------------
// engine file layer
// ---------------------------------------------------------------------------
static const char* g_dataRoot = "/data";

static void ResolvePath(char* out, int cap, const char* rel)
{
    if (rel[0] == '/')
    {
        snprintf(out, cap, "%s", rel);
    }
    else
    {
        snprintf(out, cap, "%s/%s", g_dataRoot, rel);
    }
}

// Walk a ucfb container with the engine's own PblFileChunk, counting chunks and
// pulling out the interesting ones. Returns the number of chunks visited.
struct WalkStats
{
    int chunks;
    int textures;
    int scripts;
    int models;
    int levels;
    long long bytes;
};

// How deep a generic walk descends into a container. Deeper would find more, but a chunk id
// read out of a texture's pixel data is indistinguishable from a real nested chunk, so the
// depth is one shared number rather than a per-callsite guess - and every host routine that
// needs to agree with another one (probe counts vs. texture/script index lookups) uses it.
// How deep a generic walk descends, and how many chunk headers it may read in total. Both are
// ceilings against pathological files, not format limits.
static const int kChunkWalkDepth = 4;
static const int kMaxChunkHits = 1024;     // collected per call; containers hold far fewer
static const int kMaxChunkVisits = 400000;  // headers read per walk before giving up

// Only ids the format actually uses as parents get descended into. Descending into an arbitrary
// chunk means reading a texture's pixel data as chunk headers, and a 60 MB container will
// happily yield millions of plausible-looking ones - the walk then never terminates.
static int IsContainerId(unsigned int id)
{
    return IsFourCC(id, "ucfb") || IsFourCC(id, "lvl_");
}

// Collect every chunk with a given fourcc, in file order, descending to kChunkWalkDepth.
// Callers collect first and act afterwards: the engine's PblFileChunk tracks its own read
// cursor, so acting on a hit mid-walk would move the cursor under the sibling iteration.
struct ChunkHit
{
    unsigned int offset;  // payload start
    int size;
};

// Class records are only read by the append path. Ordinary level reads must not descend into
// the alternate lvl_ world variants, or the model counts include the same level twice.
static bool g_classRecordRead = false;

static int CollectChunksRec(PblFile& file, PblFileChunk& parent, unsigned int want, int depth,
                            ChunkHit* hits, int max, int* ioTotal, int* ioVisits)
{
    int got = 0;
    while (!parent.IsDone() && *ioVisits < kMaxChunkVisits)
    {
        PblFileChunk child = parent.ReadNextChild();
        (*ioVisits)++;
        if (child._iId == want)
        {
            if (*ioTotal < max)
            {
                hits[*ioTotal].offset = (unsigned int)child._iNext;
                hits[*ioTotal].size = child._iSize;
            }
            (*ioTotal)++;
            got++;
        }
        // A side file's lvl_ record starts with the class hash and the nested size. The child
        // that holds the class's chunks therefore has the class hash as its id, not "lvl_".
        // Descend through it too, or an extracted class record opens but yields no modl chunks.
        if (depth < kChunkWalkDepth &&
            (IsContainerId(child._iId) || (g_classRecordRead && IsFourCC(parent._iId, "lvl_"))))
        {
            got += CollectChunksRec(file, child, want, depth + 1, hits, max, ioTotal, ioVisits);
            if (got >= max || *ioVisits >= kMaxChunkVisits)
            {
                break;
            }
        }
    }
    return got;
}

// Returns how many matching chunks exist (capped at `max` collected into hits).
static int CollectChunks(PblFile& file, const char* fourCC, ChunkHit* hits, int max, int* ioTotal)
{
    file.Seek(0, PblFile::SeekFromStart);  // the ucfb header is the parent chunk
    PblFileChunk root(&file);
    unsigned int want = 0;
    for (int i = 0; i < 4; ++i)
    {
        want |= ((unsigned int)(unsigned char)fourCC[i]) << (8 * i);
    }
    int visits = 0;
    return CollectChunksRec(file, root, want, 0, hits, max, ioTotal, &visits);
}

static void WalkChunks(PblFileChunk& parent, WalkStats& stats, int depth, int maxDepth,
                       const char* filterFourCC, unsigned int* outFound, int maxFound,
                       int* ioFound, const char* nameFilter, char* outName, int nameCap)
{
    while (!parent.IsDone())
    {
        PblFileChunk child = parent.ReadNextChild();

        char id[8];
        FourToStr(child._iId, id);
        stats.chunks++;
        stats.bytes += child._iSize;

        if (IsFourCC(child._iId, "tex_"))
            stats.textures++;
        else if (IsFourCC(child._iId, "scr_"))
            stats.scripts++;
        else if (IsFourCC(child._iId, "modl"))
            stats.models++;
        else if (IsFourCC(child._iId, "lvl_"))
            stats.levels++;

        if (filterFourCC && IsFourCC(child._iId, filterFourCC))
        {
            if (*ioFound < maxFound)
            {
                outFound[*ioFound] = (unsigned int)child._iNext;  // file offset of payload
            }
            (*ioFound)++;
            if (nameFilter && outName && nameCap > 0 && *ioFound == 1)
            {
                // remember where this chunk's body begins so the caller can read it
                snprintf(outName, nameCap, "%d", (int)child._iNext);
            }
        }

        if (depth < maxDepth && IsContainerId(child._iId))
        {
            // PblFileChunk tracks its own cursor: descending is safe because the sibling walk
            // resumes from _iNext, which we saved above.
            WalkChunks(child, stats, depth + 1, maxDepth, filterFourCC, outFound, maxFound,
                       ioFound, nameFilter, outName, nameCap);
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_probe_file(const char* path)
{
    char* outReport = g_report;
    const int reportCap = (int)sizeof(g_report);
    outReport[0] = 0;

    char full[512];
    ResolvePath(full, sizeof(full), path);

    struct stat st;
    if (stat(full, &st) != 0)
    {
        snprintf(outReport, reportCap, "missing: %s", full);
        return -1;
    }

    PblFile file;
    if (!file.Open(full, false, false, false))
    {
        snprintf(outReport, reportCap, "open failed: %s", full);
        return -1;
    }

    int size = file.GetSize();
    unsigned int magic = (unsigned int)file.Read32();
    int declared = file.Read32();

    char magicStr[8];
    FourToStr(magic, magicStr);

    file.Seek(0, PblFile::SeekFromStart);  // the ucfb header is the parent chunk
    PblFileChunk root(&file);
    WalkStats stats = {0, 0, 0, 0, 0, 0};
    WalkChunks(root, stats, 0, kChunkWalkDepth, 0, 0, 0, 0, 0, 0, 0);
    file.Close();

    snprintf(outReport, reportCap,
             "%s: %s container, %d bytes (header says %d), %d chunks visited, "
             "%d tex_, %d scr_, %d modl, %d lvl_",
             magicStr, full, size, declared, stats.chunks, stats.textures, stats.scripts,
             stats.models, stats.levels);

    Log("%s", outReport);
    return 0;
}

// ---------------------------------------------------------------------------
static int g_texProbe = 0;  // number of textures whose chunk layout gets logged

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_set_probe(int count) { g_texProbe = count < 0 ? 0 : count; }

// Texture extraction.
//
// A tex_ chunk holds a NAME, an INFO, and one or more FMT_ blocks - alternative encodings of
// the same image, which the PC build kept around for different hardware. Each FMT_ carries an
// INFO naming its encoding and giving its dimensions, then a FACE per cube face / volume
// slice; each FACE wraps a nested "LVL_" container whose INFO/BODY pair holds one mip level
// of pixels, largest first.
//
// The encoding id in FMT_/INFO is a Direct3D 9 format value - on PC it goes straight into
// D3DX. The browser has to name its own encodings, so the mapping is spelled out here instead
// of being inferred from payload sizes.
enum
{
    kD3DR8G8B8 = 20,
    kD3DA8R8G8B8 = 21,
    kD3DX8R8G8B8 = 22,
    kD3DR5G6B5 = 23,
    kD3DA8R3G3B2 = 24,
    kD3DX4R4G4B4 = 25,
    kD3DA1R5G5B5 = 26,
    kD3DA4R4G4B4 = 27,
    kD3DA8 = 28,
    kD3DA8L8 = 29,
    kD3DL8 = 50,
    kD3DV8U8 = 60,   // signed two-channel bump data - confirmed against water_bumpmap_* in
    kD3DL6V5U5 = 61, // yav1.lvl: id 60 at 128x128 stores exactly 2 bytes per texel
    kD3DDXT1 = 0x31545844,  // 'DXT1' as a little-endian fourcc, which is what D3DFMT_DXT1 is
    kD3DDXT2 = 0x32545844,
    kD3DDXT3 = 0x33545844,
    kD3DDXT4 = 0x34545844,
    kD3DDXT5 = 0x35545844,
};

// GL internal formats used for upload. DXT1 keeps its 1-bit alpha by taking the RGBA variant.
enum
{
    kGlRGBA8 = 0x8058,           // GL_RGBA8
    kGlRGB8 = 0x8051,            // GL_RGB8
    kGlDXT1RGBA = 0x83f1,        // COMPRESSED_RGBA_S3TC_DXT1_EXT
    kGlDXT3 = 0x83f2,            // COMPRESSED_RGBA_S3TC_DXT3_EXT
    kGlDXT5 = 0x83f3,            // COMPRESSED_RGBA_S3TC_DXT5_EXT
};

static char g_texFormatName[24] = { 0 };  // engine encoding of the last decoded texture
static int g_texFailDescribed = 0;        // 1 once a specific failure reason was written

struct TextureData
{
    char name[64];
    char formatName[16];  // engine-side encoding, for the UI
    int width;
    int height;
    int d3dFormat;        // id as stored in the file
    int glInternal;       // upload target, 0 when the encoding is not supported here
    int bytes;            // length of pixels
    int compressed;       // 1 when pixels are already block-compressed for glInternal
    unsigned char* pixels;  // upload-ready base mip, malloc'd
};

static int DxtBlockSize(int glInternal)
{
    switch (glInternal)
    {
    case kGlDXT1RGBA:
        return 8;
    case kGlDXT3:
    case kGlDXT5:
        return 16;
    default:
        return 0;
    }
}

// Bytes per texel for the uncompressed encodings the host converts; 0 when unsupported.
static int D3DBytesPerPixel(int d3d)
{
    switch (d3d)
    {
    case kD3DA8R8G8B8:
    case kD3DX8R8G8B8:
        return 4;
    case kD3DR8G8B8:
        return 3;
    case kD3DR5G6B5:
    case kD3DA1R5G5B5:
    case kD3DA4R4G4B4:
    case kD3DX4R4G4B4:
    case kD3DA8L8:
    case kD3DV8U8:
    case kD3DL6V5U5:
        return 2;
    case kD3DL8:
    case kD3DA8:
    case kD3DA8R3G3B2:
        return 1;
    default:
        return 0;
    }
}

static void D3DFormatName(int d3d, char* out, int cap)
{
    char fourcc[5] = { 0, 0, 0, 0, 0 };
    FourToStr((unsigned int)d3d, fourcc);
    // Fourcc-encoded formats are the usual spelling (D3DFMT_DXT3 is literally 'DXT3'), and the
    // digits in DXT1..DXT5 are part of the name, so the test has to allow alphanumerics.
    int ascii = 1;
    for (int i = 0; i < 4; ++i)
    {
        const char c = fourcc[i];
        const int alphaNum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9');
        if (!alphaNum)
        {
            ascii = 0;
        }
    }
    if (ascii)
    {
        snprintf(out, cap, "%s", fourcc);
        return;
    }
    switch (d3d)
    {
    case kD3DA8R8G8B8: snprintf(out, cap, "A8R8G8B8"); break;
    case kD3DX8R8G8B8: snprintf(out, cap, "X8R8G8B8"); break;
    case kD3DR5G6B5:   snprintf(out, cap, "R5G6B5"); break;
    case kD3DR8G8B8:   snprintf(out, cap, "R8G8B8"); break;
    case kD3DL8:       snprintf(out, cap, "L8"); break;
    case kD3DA8:       snprintf(out, cap, "A8"); break;
    case kD3DA8L8:     snprintf(out, cap, "A8L8"); break;
    case kD3DV8U8:     snprintf(out, cap, "V8U8"); break;
    case kD3DL6V5U5:   snprintf(out, cap, "L6V5U5"); break;
    case kD3DA1R5G5B5: snprintf(out, cap, "A1R5G5B5"); break;
    case kD3DA4R4G4B4: snprintf(out, cap, "A4R4G4B4"); break;
    default:           snprintf(out, cap, "fmt#%d", d3d); break;
    }
}

// Engine encoding -> GL encoding. S3TC stays block-compressed all the way to the GPU; the
// uncompressed encodings get converted to RGBA8 here, which keeps the upload layer free of
// per-format packing rules.
//
// Only ids confirmed from real containers are listed. A wrong id is worse than a missing one:
// a missing encoding shows up as an honest "no browser-side encoding (file offers: …)" in the
// audit, a wrong one silently renders the wrong picture.
static int GLForD3DFormat(int d3d)
{
    switch (d3d)
    {
    case kD3DDXT1:
        return kGlDXT1RGBA;
    case kD3DDXT2:
    case kD3DDXT3:
        return kGlDXT3;
    case kD3DDXT4:
    case kD3DDXT5:
        return kGlDXT5;
    case kD3DA8R8G8B8:
    case kD3DX8R8G8B8:
    case kD3DR8G8B8:
    case kD3DR5G6B5:
    case kD3DA1R5G5B5:
    case kD3DA4R4G4B4:
    case kD3DX4R4G4B4:
    case kD3DL8:
    case kD3DA8:
    case kD3DA8L8:
    case kD3DV8U8:
    case kD3DL6V5U5:
    case kD3DA8R3G3B2:
        return kGlRGBA8;
    default:
        return 0;
    }
}

static unsigned short Load16(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }
static unsigned int Load32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

// One texel -> RGBA. D3DFMT_A8R8G8B8 is BGRA in memory, which is the single most common way
// for game art to come out the wrong colour when a port assumes GL order.
static void StoreTexel(unsigned char* dst, const unsigned char* src, int d3d)
{
    switch (d3d)
    {
    case kD3DA8R8G8B8:  // B,G,R,A
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
        break;
    case kD3DX8R8G8B8:
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
        break;
    case kD3DR8G8B8:
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
        break;
    case kD3DR5G6B5:
    {
        const unsigned short v = Load16(src);
        dst[0] = (unsigned char)(((v >> 11) & 0x1f) * 255 / 31);
        dst[1] = (unsigned char)(((v >> 5) & 0x3f) * 255 / 63);
        dst[2] = (unsigned char)((v & 0x1f) * 255 / 31);
        dst[3] = 255;
        break;
    }
    case kD3DA1R5G5B5:
    {
        const unsigned short v = Load16(src);
        dst[0] = (unsigned char)(((v >> 10) & 0x1f) * 255 / 31);
        dst[1] = (unsigned char)(((v >> 5) & 0x1f) * 255 / 31);
        dst[2] = (unsigned char)((v & 0x1f) * 255 / 31);
        dst[3] = (v & 0x8000) ? 255 : 0;
        break;
    }
    case kD3DA4R4G4B4:
    case kD3DX4R4G4B4:
    {
        const unsigned short v = Load16(src);
        dst[0] = (unsigned char)(((v >> 8) & 0xf) * 17);
        dst[1] = (unsigned char)(((v >> 4) & 0xf) * 17);
        dst[2] = (unsigned char)((v & 0xf) * 17);
        dst[3] = (d3d == kD3DX4R4G4B4) ? 255 : (unsigned char)(((v >> 12) & 0xf) * 17);
        break;
    }
    case kD3DA8L8:
        dst[0] = src[1]; dst[1] = src[1]; dst[2] = src[1]; dst[3] = src[0];
        break;
    case kD3DL8:
        dst[0] = src[0]; dst[1] = src[0]; dst[2] = src[0]; dst[3] = 255;
        break;
    case kD3DA8:
        dst[0] = 0; dst[1] = 0; dst[2] = 0; dst[3] = src[0];
        break;
    case kD3DV8U8:
    case kD3DL6V5U5:
    {
        // Tangent-space bump data: the engine stores signed U/V and reconstructs Z the same
        // way a shader would, so the port unpacks it the same way rather than showing two
        // channels as a flat green image.
        int u, v;
        float scale;
        if (d3d == kD3DV8U8)
        {
            u = (int)(signed char)src[0];
            v = (int)(signed char)src[1];
            scale = 127.5f;  // 8-bit signed channels
        }
        else
        {
            const unsigned short t = Load16(src);
            u = (int)(t & 0x1f) - 16;
            v = (int)((t >> 5) & 0x1f) - 16;
            scale = 15.5f;  // 5-bit signed channels
        }
        const float fu = (float)u / scale;  // both land in [-1,1] without clipping
        const float fv = (float)v / scale;
        const float inner = 1.f - fu * fu - fv * fv;
        const float fz = inner > 0.f ? sqrtf(inner) : 0.f;
        dst[0] = (unsigned char)((fu * 0.5f + 0.5f) * 255.f);
        dst[1] = (unsigned char)((fv * 0.5f + 0.5f) * 255.f);
        dst[2] = (unsigned char)((fz * 0.5f + 0.5f) * 255.f);
        dst[3] = 255;
        break;
    }
    case kD3DA8R3G3B2:
        dst[0] = (unsigned char)((src[0] & 0x1c) << 3);
        dst[1] = (unsigned char)((src[0] & 0x3) << 6);
        dst[2] = 0;
        dst[3] = src[0] ? 255 : 0;
        break;
    default:
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        break;
    }
}

static int ReadChunkHeader(PblFile& f, unsigned int* id, int* size, int* body)
{
    int here = f.Tell();
    int next = here + ((-here) & 3);
    if (next != here)
    {
        f.Seek(next, PblFile::SeekFromStart);
    }
    int end = f.GetSize();
    if (f.Tell() + 8 > end)
    {
        return 0;
    }
    *id = (unsigned int)f.Read32();
    *size = f.Read32();
    *body = f.Tell();
    if (*size < 0 || *body + *size > end)
    {
        return 0;
    }
    return 1;
}

// Read the pixel bytes of one FACE (its nested LVL_ container's first BODY) into dst.
// *outBodySize reports the BODY's real length, which is how a caller can tell a 2D image from
// a volume/array payload that happens to start with the same bytes.
static int ReadFaceBody(PblFile& f, int faceBody, int faceSize, unsigned char* dst, int want,
                        int* outBodySize)
{
    *outBodySize = 0;
    f.Seek(faceBody, PblFile::SeekFromStart);
    int faceEnd = faceBody + faceSize;
    while (f.Tell() < faceEnd)
    {
        unsigned int id;
        int size, body;
        const int before = f.Tell();
        if (!ReadChunkHeader(f, &id, &size, &body))
        {
            return 0;
        }
        if (IsFourCC(id, "LVL_"))
        {
            // descend the nested container looking for BODY
            f.Seek(body, PblFile::SeekFromStart);
            const int nestEnd = body + size;
            while (f.Tell() < nestEnd)
            {
                unsigned int nid;
                int nsize, nbody;
                const int nbefore = f.Tell();
                if (!ReadChunkHeader(f, &nid, &nsize, &nbody))
                {
                    return 0;
                }
                if (IsFourCC(nid, "BODY"))
                {
                    *outBodySize = nsize;
                    const int take = nsize < want ? nsize : want;
                    f.Seek(nbody, PblFile::SeekFromStart);
                    f.ReadData(dst, take);
                    return take == want;
                }
                f.Seek(nbody + ((nsize + 3) & ~3), PblFile::SeekFromStart);
                if (f.Tell() <= nbefore)
                {
                    break;
                }
            }
            return 0;
        }
        f.Seek(body + ((size + 3) & ~3), PblFile::SeekFromStart);
        if (f.Tell() <= before)
        {
            break;
        }
    }
    return 0;
}

// Decode one tex_ chunk. Returns 1 and fills *out (pixels malloc'd) when an encoding this
// browser can upload was found.
static bool ExtractTexture(PblFile& f, int texBody, int texSize, TextureData* out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "(unnamed)");

    const int texEnd = texBody + texSize;
    f.Seek(texBody, PblFile::SeekFromStart);

    int bestGl = 0;
    int bestD3D = 0;
    int bestW = 0, bestH = 0;
    int bestFaceBody = 0, bestFaceSize = 0;
    int bestRank = -1;  // S3TC beats uncompressed, and 32-bit beats 8-bit when it does not
    char pendingName[64] = { 0 };
    int seenIds[6];  // encodings this tex_ offered, so a failure can name what was rejected
    int seenCount = 0;
    int bestBodySize = 0;

    while (f.Tell() < texEnd)
    {
        unsigned int id;
        int size, body;
        const int before = f.Tell();
        if (!ReadChunkHeader(f, &id, &size, &body))
        {
            break;
        }

        if (IsFourCC(id, "NAME"))
        {
            f.Seek(body, PblFile::SeekFromStart);
            const int len = size < (int)sizeof(pendingName) - 1 ? size : (int)sizeof(pendingName) - 1;
            f.ReadData(pendingName, len);
            pendingName[len] = 0;
        }
        else if (IsFourCC(id, "FMT_"))
        {
            int d3d = 0, w = 0, h = 0;
            int faceBody = 0, faceSize = 0;

            f.Seek(body, PblFile::SeekFromStart);
            const int fmtEnd = body + size;
            while (f.Tell() < fmtEnd)
            {
                unsigned int fid;
                int fsize, fbody;
                const int fbefore = f.Tell();
                if (!ReadChunkHeader(f, &fid, &fsize, &fbody))
                {
                    break;
                }
                if (IsFourCC(fid, "INFO"))
                {
                    f.Seek(fbody, PblFile::SeekFromStart);
                    d3d = f.Read32();
                    const unsigned int dims = (unsigned int)f.Read32();
                    w = (int)(dims & 0xffff);
                    h = (int)((dims >> 16) & 0xffff);
                }
                else if (IsFourCC(fid, "FACE") && !faceBody)
                {
                    faceBody = fbody;  // base mip of the first face
                    faceSize = fsize;
                }
                f.Seek(fbody + ((fsize + 3) & ~3), PblFile::SeekFromStart);
                if (f.Tell() <= fbefore)
                {
                    break;
                }
            }

            const int gl = GLForD3DFormat(d3d);
            const int compressed = DxtBlockSize(gl);
            const int bpp = D3DBytesPerPixel(d3d);
            if (seenCount < (int)(sizeof(seenIds) / sizeof(seenIds[0])))
            {
                seenIds[seenCount++] = d3d;
            }
            if (gl && w > 0 && h > 0 && w <= 4096 && h <= 4096 && faceBody)
            {
                // A volume texture stores every slice in one BODY; the 2D viewer cannot use it,
                // and pretending the first slice is the whole image would lie about the size.
                const int need = compressed ? ((w + 3) / 4) * ((h + 3) / 4) * compressed : w * h * bpp;
                int rank = (compressed ? 20 : 0) + bpp;
                if (need > 0 && rank > bestRank)
                {
                    bestRank = rank;
                    bestGl = gl;
                    bestD3D = d3d;
                    bestW = w;
                    bestH = h;
                    bestFaceBody = faceBody;
                    bestFaceSize = faceSize;
                }
            }
        }

        f.Seek(body + ((size + 3) & ~3), PblFile::SeekFromStart);
        if (f.Tell() <= before)
        {
            break;
        }
    }

    if (!bestGl)
    {
        char list[96] = { 0 };
        int at = 0;
        for (int i = 0; i < seenCount; ++i)
        {
            char nm[16];
            D3DFormatName(seenIds[i], nm, sizeof(nm));
            at += snprintf(list + at, sizeof(list) - (size_t)at, "%s%s", i ? ", " : "", nm);
        }
        snprintf(g_report, sizeof(g_report), "no browser-side encoding (file offers: %s)",
                 seenCount ? list : "nothing readable");
        g_texFailDescribed = 1;
        return false;
    }

    const int compressed = DxtBlockSize(bestGl);
    const int bpp = D3DBytesPerPixel(bestD3D);
    const int rawBytes = compressed ? ((bestW + 3) / 4) * ((bestH + 3) / 4) * compressed
                                    : bestW * bestH * bpp;

    // Uncompressed encodings are converted to RGBA8; DXT goes to the GPU untouched.
    const int outBytes = compressed ? rawBytes : bestW * bestH * 4;
    unsigned char* raw = (unsigned char*)malloc((size_t)rawBytes);
    unsigned char* pixels = (unsigned char*)malloc((size_t)outBytes);
    if (!raw || !pixels)
    {
        free(raw);
        free(pixels);
        snprintf(g_report, sizeof(g_report), "out of memory decoding a texture");
        g_texFailDescribed = 1;
        return false;
    }

    int bodySize = 0;
    if (!ReadFaceBody(f, bestFaceBody, bestFaceSize, raw, rawBytes, &bodySize))
    {
        free(raw);
        free(pixels);
        char enc[16];
        D3DFormatName(bestD3D, enc, sizeof(enc));
        snprintf(g_report, sizeof(g_report),
                 "'%s' %dx%d %s: BODY holds %d bytes, less than the %d the encoding needs",
                 pendingName, bestW, bestH, enc, bodySize, rawBytes);
        g_texFailDescribed = 1;
        if (g_texProbe)
        {
            Log("tex %s: BODY %d bytes is short for %dx%d", pendingName, bodySize, bestW, bestH);
        }
        return false;
    }

    if (bodySize > rawBytes * 2)
    {
        // The BODY carries far more than one image's worth of pixels: a volume or texture
        // array. Upload its first slice and the picture is a lie, so it is refused here and
        // named in the audit instead.
        free(raw);
        free(pixels);
        D3DFormatName(bestD3D, g_texFormatName, sizeof(g_texFormatName));
        snprintf(g_report, sizeof(g_report), "'%s' %dx%d %s: multi-slice payload (%d bytes, 2D "
                 "would be %d) - volume/array not supported",
                 pendingName, bestW, bestH, g_texFormatName, bodySize, rawBytes);
        g_texFailDescribed = 1;
        return false;
    }

    if (compressed)
    {
        memcpy(pixels, raw, (size_t)rawBytes);
    }
    else
    {
        for (int i = 0; i < bestW * bestH; ++i)
        {
            StoreTexel(pixels + i * 4, raw + i * bpp, bestD3D);
        }
    }
    free(raw);

    snprintf(out->name, sizeof(out->name), "%s", pendingName);
    D3DFormatName(bestD3D, out->formatName, sizeof(out->formatName));
    out->width = bestW;
    out->height = bestH;
    out->d3dFormat = bestD3D;
    out->glInternal = bestGl;
    out->bytes = outBytes;
    out->compressed = compressed ? 1 : 0;
    out->pixels = pixels;

    if (g_texProbe)
    {
        Log("tex '%s': %dx%d %s -> GL 0x%x (%d bytes%s)", out->name, out->width, out->height,
            out->formatName, out->glInternal, out->bytes, out->compressed ? ", compressed" : ", RGBA8");
        --g_texProbe;
    }
    return true;
}

// Find the n-th texture in a container and decode it.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_load_texture(const char* path, int index,
                                                       int* outWidth, int* outHeight,
                                                       int* outFormat, char* outName,
                                                       int nameCap)
{
    char full[512];
    ResolvePath(full, sizeof(full), path);

    PblFile file;
    if (!file.Open(full, false, false, false))
    {
        Log("texture: cannot open %s", full);
        snprintf(g_report, sizeof(g_report), "cannot open %s", full);
        return -1;
    }

    // Collect first, then pick the index - textures live inside lvl_ groups in most containers,
    // so a top-level-only scan would disagree with what swbf2_probe_file() advertises.
    static ChunkHit hits[kMaxChunkHits];
    int total = 0;
    CollectChunks(file, "tex_", hits, kMaxChunkHits, &total);

    int foundBody = -1;
    int foundSize = 0;
    if (index >= 0 && index < total && index < kMaxChunkHits)
    {
        foundBody = (int)hits[index].offset;
        foundSize = hits[index].size;
    }

    if (foundBody < 0)
    {
        Log("texture: index %d not present in %s (%d textures found)", index, full, total);
        snprintf(g_report, sizeof(g_report), "index %d out of range (%d textures in file)", index,
                 total);
        file.Close();
        return -1;
    }

    TextureData tex;
    memset(&tex, 0, sizeof(tex));
    g_texFailDescribed = 0;
    snprintf(tex.name, sizeof(tex.name), "?");
    bool ok = ExtractTexture(file, foundBody, foundSize, &tex);
    file.Close();

    if (!ok)
    {
        Log("texture: index %d in %s has no encoding this browser can upload: %s", index, full,
            g_texFailDescribed ? g_report : "(no reason recorded)");
        if (!g_texFailDescribed)
        {
            snprintf(g_report, sizeof(g_report),
                     "no browser-side encoding for this texture (see log)");
        }
        return -2;
    }

    *outWidth = tex.width;
    *outHeight = tex.height;
    *outFormat = tex.glInternal;
    if (outName && nameCap > 0)
    {
        snprintf(outName, nameCap, "%s", tex.name);
    }

    Log("texture[%d]: '%s' %dx%d %s -> GL 0x%x (%d bytes) from %s", index, tex.name, tex.width,
        tex.height, tex.formatName, tex.glInternal, tex.bytes, full);
    snprintf(g_texFormatName, sizeof(g_texFormatName), "%s", tex.formatName);

    // hand the bytes to the caller through a static buffer; the GL upload happens
    // immediately afterwards in swbf2_tex_upload_bytes()
    extern unsigned char* g_texScratch;
    extern int g_texScratchLen;
    free(g_texScratch);
    g_texScratch = tex.pixels;
    g_texScratchLen = tex.bytes;
    return tex.bytes;
}

// defined further down with the GL context; declared here so the host's one-call surface
// above can upload straight after decoding.
extern "C" int swbf2_tex_upload(int w, int h, int fmt, int len);

// One-call surface for the host: decode texture `index` from `path`, upload it, and leave a
// one-line description in swbf2_last_report(). The renderer draws whatever is currently
// uploaded, so the host does not have to marshal pixel data or out-parameters across the
// wasm boundary - which is why malloc/free are not exported either.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_show_texture(const char* path, int index)
{
    int w = 0, h = 0, fmt = 0;
    char name[128];
    name[0] = 0;

    int bytes = swbf2_load_texture(path, index, &w, &h, &fmt, name, (int)sizeof(name));
    if (bytes <= 0)
    {
        snprintf(g_report, sizeof(g_report),
                 "texture %d of %s: not decodable (see log)", index, path);
        return bytes;
    }

#ifdef __EMSCRIPTEN__
    int up = swbf2_tex_upload(w, h, fmt, bytes);
    if (up != 0)
    {
        snprintf(g_report, sizeof(g_report),
                 "texture '%s' %dx%d: upload failed (gl error %d, S3TC unsupported?)", name, w, h,
                 up);
        return -10 - up;
    }
#endif
    snprintf(g_report, sizeof(g_report),
             "texture '%s' %dx%d %s -> GL 0x%x, %d bytes - now on screen", name, w, h,
             g_texFormatName, fmt, bytes);
    return 0;
}

unsigned char* g_texScratch = 0;
int g_texScratchLen = 0;


extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_texture_format_name(void)
{
    return g_texFormatName;
}

// Decode without uploading: the headless audit (tools/audit_textures.mjs) runs where there is
// no GL context, and it still needs to know which engine encodings the port can read.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_decode_texture(const char* path, int index)
{
    int w = 0, h = 0, fmt = 0;
    char name[128];
    name[0] = 0;
    const int bytes = swbf2_load_texture(path, index, &w, &h, &fmt, name, (int)sizeof(name));
    if (bytes > 0)
    {
        snprintf(g_report, sizeof(g_report), "'%s' %dx%d %s, %d bytes", name, w, h,
                 g_texFormatName, bytes);
    }
    // On failure the loader already left the reason in g_report - overwriting it with an empty
    // description is what made the audit useless.
    return bytes;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_tex_scratch_len()
{
    return g_texScratchLen;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_tex_scratch_into(unsigned char* dst, int cap)
{
    if (!g_texScratch || g_texScratchLen > cap)
    {
        return -1;
    }
    memcpy(dst, g_texScratch, (size_t)g_texScratchLen);
    return g_texScratchLen;
}

// ---------------------------------------------------------------------------
// LuaFL: run a real mission script out of a scr_ chunk
// ---------------------------------------------------------------------------
static lua_State* g_L = 0;
static int g_luaCalls = 0;

static int l_print(lua_State* L)
{
    int n = lua_gettop(L);
    char line[512];
    int off = 0;
    for (int i = 1; i <= n; ++i)
    {
        const char* s = lua_tostring(L, i);
        int w = snprintf(line + off, sizeof(line) - off, "%s%s", i > 1 ? " " : "",
                         s ? s : (lua_isnumber(L, i) ? "" : "?"));
        if (w > 0)
        {
            off += w;
        }
    }
    Log("lua> %s", line);
    return 0;
}

// LuaFL is Lua 5.0.2: it has no luaL_newstate and no luaL_openlibs (the game opens the
// standard libraries itself), so we bootstrap the way linit/lauxlib did in 5.0.
typedef int (*LuaOpenFn)(lua_State*);

static void OpenLuaLib(lua_State* L, LuaOpenFn fn, const char* name)
{
    lua_pushcfunction(L, fn);
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 0, 0) != 0)
    {
        Log("lua: opening '%s' failed: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void OpenLuaLibs(lua_State* L)
{
    struct
    {
        const char* name;
        LuaOpenFn fn;
    } libs[] = {
        {"base", luaopen_base},   {"table", luaopen_table}, {"string", luaopen_string},
        {"math", luaopen_math},   {"debug", luaopen_debug}, {"io", luaopen_io},
    };
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); ++i)
    {
        OpenLuaLib(L, libs[i].fn, libs[i].name);
    }
}

static void EnsureLua(void)
{
    if (g_L)
    {
        return;
    }
    g_L = lua_open();
    OpenLuaLibs(g_L);
    lua_register(g_L, "print", l_print);

    // The game's scripts call thousands of engine bindings that only exist once the C++
    // side of the port grows. Returning 0 for them is not enough: scripts write
    // `SomeClass.someField.other`, and indexing the 0 errors out. So unknown names resolve
    // to an `any` node - a table that can be indexed forever, called, and stringified -
    // which lets the real control flow of the mission script execute.
    static const char* stub =
        "local any\n"
        "any = setmetatable({}, {\n"
        "  __index = function(self, k) return any end,\n"
        "  __call = function(self, ...) return any end,\n"
        "  __tostring = function(self) return '<stub>' end,\n"
        "  __concat = function(self, o) return tostring(self)..tostring(o) end,\n"
        "  __eq = function(a, b) return a == b end,\n"
        "  __lt = function(a, b) return false end,\n"
        "  __le = function(a, b) return false end,\n"
        "  __add = function(a, b) return any end,\n"
        "  __sub = function(a, b) return any end,\n"
        "  __mul = function(a, b) return any end,\n"
        "  __div = function(a, b) return any end,\n"
        "  __pow = function(a, b) return any end,\n"
        "  __unm = function(a) return any end,\n"
        "})\n"
        "setmetatable(_G, { __index = function(t, k) if k == nil then return nil end "
        "  rawset(t, k, any) return any end })\n";
    if (luaL_loadbuffer(g_L, stub, strlen(stub), "host-bootstrap") == 0)
    {
        if (lua_pcall(g_L, 0, 0, 0) != 0)
        {
            Log("lua: stub bootstrap failed: %s", lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    }
    Log("lua: LuaFL %s state ready", LUA_VERSION);
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_run_script(const char* code, int len)
{
    EnsureLua();
    int top = lua_gettop(g_L);
    if (luaL_loadbuffer(g_L, code, (size_t)len, "host") != 0)
    {
        Log("lua compile: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 2);
        return -1;
    }
    if (lua_pcall(g_L, 0, LUA_MULTRET, 0) != 0)
    {
        Log("lua run: %s", lua_tostring(g_L, -1));
        lua_settop(g_L, top);
        return -2;
    }
    g_luaCalls++;
    lua_settop(g_L, top);
    return 0;
}

// Pull the first (or n-th) scr_ chunk out of a container and execute its bytecode.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_run_scripts_from(const char* path, int maxScripts)
{
    char full[512];
    ResolvePath(full, sizeof(full), path);

    PblFile file;
    if (!file.Open(full, false, false, false))
    {
        Log("scripts: cannot open %s", full);
        return -1;
    }

    static ChunkHit hits[kMaxChunkHits];
    int total = 0;
    CollectChunks(file, "scr_", hits, kMaxChunkHits, &total);

    int ran = 0;
    const int want = total < maxScripts ? total : maxScripts;
    for (int iscript = 0; iscript < want && ran < maxScripts; ++iscript)
    {
        const int chunkBody = (int)hits[iscript].offset;

        // scr_ payload: NAME + INFO + BODY. BODY is Lua 5.0 pre-compiled bytecode - the
        // container's own signature is \x1bLua'P' (0x50, i.e. Lua 5.0 exactly), which is why
        // LuaFL is the right VM and Fengari (5.3) would not load a single mission script.
        int end = chunkBody + hits[iscript].size;
        file.Seek(chunkBody, PblFile::SeekFromStart);

        char scriptName[64] = "?";
        int codeBody = -1;
        int codeSize = 0;

        while (file.Tell() < end)
        {
            unsigned int id;
            int size, body;
            int before = file.Tell();
            if (!ReadChunkHeader(file, &id, &size, &body))
            {
                break;
            }
            if (IsFourCC(id, "NAME"))
            {
                file.Seek(body, PblFile::SeekFromStart);
                int n = size < (int)sizeof(scriptName) - 1 ? size : (int)sizeof(scriptName) - 1;
                file.ReadData(scriptName, n);
                scriptName[n] = 0;
            }
            else if (IsFourCC(id, "BODY") || IsFourCC(id, "CODE"))
            {
                codeBody = body;
                codeSize = size;
            }
            file.Seek(body + ((size + 3) & ~3), PblFile::SeekFromStart);
            if (file.Tell() <= before)
            {
                break;
            }
        }

        if (codeBody >= 0 && codeSize > 0)
        {
            char* buf = (char*)malloc((size_t)codeSize + 1);
            if (buf)
            {
                file.Seek(codeBody, PblFile::SeekFromStart);
                file.ReadData(buf, codeSize);
                buf[codeSize] = 0;

                EnsureLua();
                int top = lua_gettop(g_L);
                if (luaL_loadbuffer(g_L, buf, (size_t)codeSize, scriptName) == 0)
                {
                    if (lua_pcall(g_L, 0, 0, 0) == 0)
                    {
                        ran++;
                        g_luaCalls++;
                    }
                    else
                    {
                        Log("script %s: runtime %s", scriptName, lua_tostring(g_L, -1));
                        lua_pop(g_L, 1);
                    }
                }
                else
                {
                    Log("script %s: load %s", scriptName, lua_tostring(g_L, -1));
                    lua_pop(g_L, 1);
                }
                lua_settop(g_L, top);
                free(buf);
            }
        }
    }

    file.Close();
    Log("scripts: %s -> %d scr_ chunks seen, %d executed", full, total, ran);
    return ran;
}

// ---------------------------------------------------------------------------
// render + main loop
// ---------------------------------------------------------------------------
static int g_frames = 0;
static double g_simTime = 0.0;
static float g_simDelta = 0.0f;
static int g_renderReady = 0;
static float g_camZ = 3.0f;

/* The engine's frame-pacing clock, driven from the browser.
 *
 * The host used to keep its own accumulator and call it "sim time". That was a host float that
 * happened to be called at 30 Hz, and it disagreed with the engine in a way nobody could see: the
 * engine's own time base is an integer tick counter, so the delta the game receives is quantised
 * (0.128 ms at the shim's 1 MHz) and never exactly 1/30 s. Now the host asks the engine for the
 * time instead of counting it.
 *
 * Two decisions, both measured in docs/frame-pacing.md:
 *
 *  - The fixed-rate branch (g_fFixedRate > 0) is the one the browser uses. The variable branch
 *    spins on QueryPerformanceCounter until 12.5 ms have passed, and the shim's Sleep is a no-op,
 *    so in a browser that spin is a busy-wait on the main thread.
 *  - The host still decides *when* to tick (30 Hz, from requestAnimationFrame), but the engine
 *    decides *how much* time passed. That is the same split the original game has: the message
 *    pump decides when a frame happens, GameState::UpdateTime decides what the frame is worth. */
static bool g_clockReady = false;

static void EngineClockInit(void)
{
    if (g_clockReady)
    {
        return;
    }
    g_clockReady = true;

    g_fFixedRate = 30.0f;

    /* The engine marks the stamps against the time bases when a state is entered
       (GameState::MissionState::Enter). Until the mission state exists, the host does it, or
       GetElapsed() has no base to measure from and the game delta is zero. */
    AppTimeStamp.Mark(&GameState::_TimeBase);
    SystemTimeStamp.Mark(&GameState::_SystemTimeBase);

    Log("clock: engine GameState::UpdateTime at %g Hz (fixed-rate branch)", (double)g_fFixedRate);
}

#ifdef __EMSCRIPTEN__
static const char* VS =
    "#version 300 es\n"
    "in vec2 aPos; in vec2 aUV;\n"
    "out vec2 vUV;\n"
    // uScale/uOffset place one quad inside its own cell, which is all the montage needs - the
    // geometry stays a single unit quad shared by every cell.
    "uniform vec2 uScale; uniform vec2 uOffset;\n"
    // uUse3D switches between the two display paths the engine drives: screen-space placement
    // (single image and montage) and a real camera transform. Both share one unit quad.
    "uniform int uUse3D; uniform mat4 uMVP;\n"
    "void main(){\n"
    "  vUV = aUV;\n"
    "  if(uUse3D != 0){ gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }\n"
    "  else { gl_Position = vec4(aPos * uScale + uOffset, 0.0, 1.0); }\n"
    "}\n";

static const char* FS =
    "#version 300 es\n"
    "precision mediump float; in vec2 vUV; out vec4 fragColor;\n"
    "uniform sampler2D uTex; uniform int uHasTex; uniform vec4 uTint;\n"
    "void main(){\n"
    "  vec4 c = uHasTex != 0 ? texture(uTex, vUV) : uTint;\n"
    "  fragColor = vec4(c.rgb, c.a);\n"
    "}\n";

static GLuint g_program = 0;
static GLuint g_vao = 0;
static GLuint g_texture = 0;

// --- 3D view: the engine's own camera, rasterised by WebGL2 -------------------------------
//
// The camera is a real RedCamera (RedEngineFL/Graphics/RedCamera.cpp) - not an approximation -
// so the view matrix, the projection matrix and the frustum culling all come from the ported
// engine. Two conventions were read out of that source rather than assumed, because getting
// either one wrong produces a picture that looks plausible and is subtly broken:
//
//   * RedCamera::_Matrix is the camera's WORLD matrix (GetPos reads its translation row, and
//     GetViewTrans hands back _MatrixInverse). So the host authors camera-to-world and the
//     engine computes world-to-view with its own inverse.
//   * ComputeProjMatrix maps the near plane to z_ndc 0 and PROJECTION_FAR_PLANE (10000) to 1,
//     with w taken straight from a +z-forward view space. That is already monotonic and inside
//     GL's clip range, so no depth remap is needed - the whole transform is just the engine's,
//     transposed into GL's column-major order.
static int g_mode3D = 0;
static RedCamera g_cam;          // the engine's camera: view, projection and frustum all live here
static float g_camYaw = 0.0f;
static float g_camPitch = 0.18f;
static float g_camDist = 26.0f;
static float g_camFov = 1.1517f;  // 66 degrees in radians: SetFOV/SetPerspective take radians
static int g_sceneDrawn = 0;
static int g_sceneCulled = 0;
static GLint g_uMVP = -1;
static GLint g_uUse3D = -1;

/* The camera the 3D scene is drawn with: the engine's own chase camera - the one CameraManager
 * places behind the trooper the engine is controlling - when it is in use, and the host's own
 * orbit camera otherwise. The engine's camera is the engine's: its world matrix, its field of view
 * and its frustum all come from the engine's CameraManager::Update, and the host only reads them. */
static int g_engineCamera = 0;

static RedCamera* SceneCamera()
{
    if (g_engineCamera)
    {
        if (RedCamera* pEngine = LevelHarnessCamera())
        {
            return pEngine;
        }
    }
    return &g_cam;
}

// The camera this module renders the 3D scene with, offered to the renderer device layer
// (browser/platform/pcRedRenderer_browser.cpp). Engine code that asks the renderer which camera
// is in use gets the one actually drawing, rather than a null it would have to work around.
RedCamera* BrowserSceneCamera()
{
    return SceneCamera();
}

static int g_gpuLuma = 0;      // 0..255 mean luma of the sampled framebuffer
static int g_gpuLit = 0;       // per-mille of samples brighter than black
static int g_gpuDistinct = 0;  // distinct 6-bit-per-channel colour buckets seen
static int g_gpuCenter[3] = { 0, 0, 0 };

// Display-mode globals owned by the browser platform layer (browser/platform/pc_globals.cpp).
// The engine's settings code reads them, so the host keeps them equal to the real canvas size.
extern int pcWidth;
extern int pcHeight;

static int g_texW = 0;
static int g_texH = 0;
static int g_texFmt = 0;

static GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char msg[512];
        glGetShaderInfoLog(s, sizeof(msg), 0, msg);
        Log("gl compile: %s", msg);
    }
    return s;
}

#ifdef __EMSCRIPTEN__
// glGetError returns the OLDEST unreported error, so "an error appeared" is not a diagnosis.
// Tagging the calls turns it into one.
#define GLCHK(tag)                            \
    do {                                      \
        const GLint e_ = glGetError();        \
        if (e_ != GL_NO_ERROR)                \
        {                                     \
            Log("glerr@%s: %d", tag, (int)e_); \
        }                                     \
    } while (0)
#endif

static int InitGL(void)
{
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.alpha = 0;
    attrs.depth = 1;
    attrs.stencil = 0;
    attrs.antialias = 1;
    attrs.premultipliedAlpha = 0;
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attrs);
    if ((int)ctx <= 0)
    {
        ctx = emscripten_webgl_create_context(0, &attrs);
    }
    if ((int)ctx <= 0)
    {
        Log("gl: no WebGL context (code %d) - running logic only", (int)ctx);
        return 0;
    }
    emscripten_webgl_make_context_current(ctx);
    Log("gl: context %d current", (int)ctx);

    GLuint vs = Compile(GL_VERTEX_SHADER, VS);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, FS);
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glBindAttribLocation(g_program, 0, "aPos");
    glBindAttribLocation(g_program, 1, "aUV");
    glLinkProgram(g_program);
    glUseProgram(g_program);
    GLCHK("useProgram");
    glUniform1i(glGetUniformLocation(g_program, "uTex"), 0);
    GLCHK("samplerUniform");

    float verts[] = {
        -1.f, -1.f, 0.f, 1.f,   // fullscreen quad, UV flipped for GL origin
        1.f,  -1.f, 1.f, 1.f,
        1.f,  1.f,  1.f, 0.f,
        -1.f, -1.f, 0.f, 1.f,
        1.f,  1.f,  1.f, 0.f,
        -1.f, 1.f,  0.f, 0.f,
    };
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    // a unit quad; every cell of the montage is placed by scaling/offsetting in the vertex
    // shader, so this buffer is uploaded once for the lifetime of the context
    g_uScale = glGetUniformLocation(g_program, "uScale");
    g_uOffset = glGetUniformLocation(g_program, "uOffset");
    g_uHasTex = glGetUniformLocation(g_program, "uHasTex");
    g_uTint = glGetUniformLocation(g_program, "uTint");
    g_uMVP = glGetUniformLocation(g_program, "uMVP");
    g_uUse3D = glGetUniformLocation(g_program, "uUse3D");
    glUniform2f(g_uScale, 1.f, 1.f);
    glUniform2f(g_uOffset, 0.f, 0.f);

    const GLubyte* exts = glGetString(GL_EXTENSIONS);
    if (exts && strstr((const char*)exts, "WEBGL_compressed_texture_s3tc"))
    {
        Log("gl: S3TC/DXT available");
    }
    else
    {
        Log("gl: WARNING no S3TC - game textures cannot be uploaded directly");
    }

    return 1;
}
#endif

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// 3D view: real game textures placed in world space and drawn through the engine's camera
//
// This is the renderer seam. WebGL2 does the rasterising, but every transform and every culling
// decision comes from RedCamera in the ported engine, so the code path is the one a real renderer
// backend will have to drive: author camera-to-world, ask the engine for world-to-view and the
// projection, cull against the engine's frustum, upload one matrix per object.
//
// It is a gallery of the textures the reader actually decoded - not a level. Real terrain and
// models need the `modl`/`segm` vertex layout, which the decompilation does not contain.
// ---------------------------------------------------------------------------

// An engine matrix is row-major with its basis vectors in the rows and its translation in row 3,
// and the engine transforms with row vectors: clip = v * M. GL transforms with column vectors:
// clip = M * v. So what GL wants is M transpose - and a row-major float[4][4] held in memory is
// byte for byte the column-major layout of its own transpose. The correct conversion is therefore
// a copy, not a shuffle.
//
// Transposing here silently drops the translation: with the basis rows going where the columns
// were, row 3 becomes column 3, whose bottom element is the homogeneous 1 - so the view matrix
// claims the camera sits at the origin, the whole world lands behind the near plane, and every
// culling and depth test downstream reports an empty scene.
static void PblToGL(float* dst, const PblMatrix& m)
{
    const float* src = &m.m[0][0];
    for (int i = 0; i < 16; ++i)
    {
        dst[i] = src[i];
    }
}

// Column-major multiply as GL consumes it: out = a * b, so a point is transformed by b first.
static void Mat4Mul(float* out, const float* a, const float* b)
{
    float tmp[16];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            tmp[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] +
                                 a[8 + row] * b[col * 4 + 2] + a[12 + row] * b[col * 4 + 3];
        }
    }
    for (int i = 0; i < 16; ++i)
    {
        out[i] = tmp[i];
    }
}

// A GL perspective built from the engine's own frustum dimensions.
//
// RedCamera::ComputeProjMatrix cannot be uploaded to GL directly: it is written for D3D9 in the
// decompiler's row-major layout, takes clip w straight from view-space z, and maps the near plane
// to z_ndc 0. GL wants w = -z and near at z_ndc -1. What is *not* re-derived here is the view
// volume: near, far and the half-extents come from the engine's GetFrustum, so the field of view
// and clipping planes stay the engine's - only the matrix form changes, which is exactly the
// device layer's job.
static void GLPerspectiveFromFrustum(float* out, float fNear, float fFar, float fHalfW, float fHalfH)
{
    for (int i = 0; i < 16; ++i)
    {
        out[i] = 0.f;
    }
    out[0] = fNear / fHalfW;                                    // m00
    out[5] = fNear / fHalfH;                                    // m11
    out[10] = -(fFar + fNear) / (fFar - fNear);                 // m22
    out[11] = -1.f;                                             // w = -z
    out[14] = -(2.f * fFar * fNear) / (fFar - fNear);           // m32
}

// Engine view (world -> camera) composed with a GL projection, ready for one glUniformMatrix4fv.
static void BuildGLViewProj(float* out)
{
    float fNear, fFar, fWidth, fHeight;
    SceneCamera()->GetFrustum(fNear, fFar, fWidth, fHeight);   // the engine's own frustum

    float proj[16];
    GLPerspectiveFromFrustum(proj, fNear, fFar, 0.5f * fWidth, 0.5f * fHeight);

    float view[16];
    PblToGL(view, SceneCamera()->GetMatrixInverse());  // world -> view, from the engine's inverse

    Mat4Mul(out, proj, view);
}

static void PblSetRow(PblMatrix& m, int row, float x, float y, float z)
{
    m.m[row][0] = x;
    m.m[row][1] = y;
    m.m[row][2] = z;
    m.m[row][3] = (row == 3) ? 1.0f : 0.0f;
}

static float VecLen(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void VecCross(float* out, const float* a, const float* b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Deterministic placement for the resident textures: a golden-angle spiral of rings at varied
// depth, so quads overlap from some viewing angles (which is what makes the depth test provable)
// and fall off the frustum from others (which is what makes the culling provable).
static void ScenePlacement(int i, float outPos[3])
{
    const float golden = 2.39996323f;
    const float ang = (float)i * golden;
    const float rad = 4.5f + 1.45f * sqrtf((float)i);
    outPos[0] = cosf(ang) * rad;
    outPos[1] = sinf(ang) * rad * 0.6f;
    outPos[2] = (float)((i % 5) - 2) * 3.0f;
}

// Camera size for a texture: aspect-aware so the image is not stretched, roughly constant area.
static void SceneSize(const CachedTexture& t, float& halfW, float& halfH)
{
    const float aspect = (float)t.width / (float)(t.height > 0 ? t.height : 1);
    const float base = 2.6f;
    if (aspect >= 1.f)
    {
        halfW = base * 0.5f * sqrtf(aspect);
        halfH = halfW / aspect;
    }
    else
    {
        halfH = base * 0.5f * sqrtf(1.f / aspect);
        halfW = halfH * aspect;
    }
    halfW *= 1.7f;
    halfH *= 1.7f;
}

// Author camera-to-world and hand it to the engine.
//
// The engine's camera Z axis points AWAY from what the camera looks at: RedCamera::ClassifySphere
// computes fZ = -(Zrow . (point - eye)) and treats fZ as forward depth, so a point in front is
// only "in front" when Zrow opposes the look direction. Setting Z to the look direction instead
// makes every object fall behind the near plane and the engine's own culling rejects the whole
// scene - which is exactly the bug this comment exists to prevent someone re-introducing.
static void BuildCamera(float aspect)
{
    const float cp = cosf(g_camPitch);
    const float sp = sinf(g_camPitch);
    float eye[3];
    eye[0] = g_camDist * cp * sinf(g_camYaw);
    eye[1] = g_camDist * sp;
    eye[2] = g_camDist * cp * cosf(g_camYaw);

    // Camera Z points from the target back towards the eye (the engine's forward is -Z).
    float back[3] = { eye[0], eye[1], eye[2] };   // looking at the origin
    const float len = VecLen(back);
    if (len < 1e-4f)
    {
        return;
    }
    back[0] /= len;
    back[1] /= len;
    back[2] /= len;

    // X = up x Z, Y = Z x X: the right-handed basis a camera uses when its Z runs backwards,
    // verified against the obvious case (eye on +Z looking at the origin gives X=+1 right,
    // Y=+1 up). Getting this order backwards mirrors the scene, which reads as a texture bug.
    float up0[3] = { 0.f, 1.f, 0.f };
    float right[3];
    VecCross(right, up0, back);
    float rlen = VecLen(right);
    if (rlen < 1e-4f)  // looking straight down the up axis: pick any perpendicular
    {
        right[0] = 1.f;
        right[1] = 0.f;
        right[2] = 0.f;
        rlen = 1.f;
    }
    right[0] /= rlen;
    right[1] /= rlen;
    right[2] /= rlen;
    float up[3];
    VecCross(up, back, right);

    PblMatrix world;
    world.Reset();
    PblSetRow(world, 0, right[0], right[1], right[2]);
    PblSetRow(world, 1, up[0], up[1], up[2]);
    PblSetRow(world, 2, back[0], back[1], back[2]);
    PblSetRow(world, 3, eye[0], eye[1], eye[2]);

    g_cam.SetMatrix(world);                                // camera -> world
    g_cam.SetPerspective(0.5f, 900.f, g_camFov, aspect);   // the frustum comes from the engine
}

// One billboarded quad: basis rows taken from the camera so the image always faces the viewer,
// scaled by the texture's own aspect.
static void DrawBillboard(const float pos[3], float halfW, float halfH,
                          const float* viewProj, const float right[3], const float up[3],
                          const float back[3])
{
    // Reset() first: only the basis columns of each row are written below, so without an identity
    // starting point the fourth column and m[3][3] stay uninitialised stack values.
    // GetInverseFast transposes the 3x3 and repairs only m[3][3], so that garbage survives into
    // the view matrix - which is precisely how a correct-looking camera produced a degenerate MVP.
    PblMatrix world;
    world.Reset();
    PblSetRow(world, 0, right[0] * halfW, right[1] * halfW, right[2] * halfW);
    PblSetRow(world, 1, up[0] * halfH, up[1] * halfH, up[2] * halfH);
    PblSetRow(world, 2, back[0], back[1], back[2]);
    PblSetRow(world, 3, pos[0], pos[1], pos[2]);

    float obj[16];
    float mvp[16];
    PblToGL(obj, world);
    Mat4Mul(mvp, viewProj, obj);
    glUniformMatrix4fv(g_uMVP, 1, GL_FALSE, mvp);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Upload a decoded texture to its own GL texture object. Used by the resident set (the montage)
// and by the model path, so there is one upload rule rather than two that can drift.
static GLuint UploadDecodedTexture(const TextureData& t)
{
    if (!g_renderReady || !t.pixels || t.bytes <= 0 || t.glInternal <= 0) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int blockSize = DxtBlockSize(t.glInternal);
    if (blockSize)
    {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, (GLenum)t.glInternal, t.width, t.height, 0,
                               t.bytes, t.pixels);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLenum)t.glInternal, t.width, t.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, t.pixels);
    }
    if (glGetError() != GL_NO_ERROR)
    {
        glDeleteTextures(1, &id);
        return 0;
    }
    return id;
}

// ---------------------------------------------------------------------------
// Model geometry: `modl` and `segm`
//
// The decompiled tree has no model-file reader, so the vertex layout has to come from the data.
// Everything below is transcribed from the two functions that do define it -
// pcRedSegment.cpp::RedSegment::Read (which dispatches on child chunk id) and
// pcRedVertexFormat.cpp::CreateVertexDeclaration (which defines the RVF bits and therefore the
// stride) - and every decode rule was then checked against real GameData rather than assumed:
//
//   * Positions stored as shorts are quantised against the MODEL box, not the segment box.
//     pcRedSegment.cpp sets CompressedPos::s_vMin and s_vScale from m_bbox; m_bbox is
//     _GetModelBox(pModel), i.e. RedModel::_BoundingBox, which RedModel.cpp fills from the box
//     inside modl's INFO chunk. The decode is the inverse of the encode every effect uses
//     (short((v - s_vMin) * s_vScaleInv - 32767.5)), so
//         world = min + (short + 32767.5) * extent / 65535.
//     tools/modl_decompress_check.py tests it against the box each segment records: 14,804 of
//     14,819 stream records over all 185 containers reproduce it. tools/modl_packed_pair_check.py
//     compares the packed stream against the uncompressed copy of the same vertices that the same
//     segment stores - the file answering itself - and names the model box for 7,174 segments
//     against 1,486 for the segment box.
//   * Packed components are D3DCOLORs, whose bytes in memory are B,G,R,A, so a packed normal
//     reads z,y,x. Verified against the file's own uncompressed copy for 7,267 segments.
//   * Packed texcoords are fixed point at 1/2048: 7,235 of 7,236 comparable segments reproduce
//     their own float UVs within one quantisation step. 1,188 vertices have UVs a 12-bit fixed
//     point cannot hold at all, so those are reported as not checkable rather than counted as wrong.
//   * prim type 5 is D3DPT_TRIANGLESTRIP, not a triangle list. The arithmetic confirms it on every
//     segment in every container tested: primCount == numIndices - 2, which is exactly the primitive
//     count of a strip of numIndices indices. Drawing that index buffer as a triangle list gives a
//     different mesh from the same data and still looks like a picture, so the host draws strips.
//   * segm INFO's primCount is the number of primitives to draw; numIndices is the even-padded
//     allocation, so drawing numIndices reads padding.
//   * Model placement is the instance: wrld/inst names a model through its TYPE string and its
//     XFRM is read exactly as LoadUtil::ReadTransform does - four rows of three floats, the
//     fourth being the translation. In tat2, 662 of the 712 instances the documented walk policy
//     reaches name a model in the same container; the other 50 resolve through gmod/BASE/PRNT, which
//     this reader does not follow yet, so they are counted and reported rather than guessed at. Two
//     further instances sit inside a group id that policy does not recognise, and the oracle counts
//     them separately for that reason.
//
// The caps are the measured maxima across all 185 containers that hold segments - 157 models
// (uta1.lvl), 413 segments (dea1.lvl), 118,765 vertices and 198,005 indices (tan1.lvl), 964
// instances (end1.lvl), 136 texture names (ingame.lvl) - rounded up, so a container is never
// silently truncated: when a cap is reached the host says so.
static const int kMaxModels = 512;
static const int kMaxSegments = 512;
static const int kMaxInstances = 1024;
static const int kMaxTexNames = 1536;
static const int kMaxModelVertices = 130000;
static const int kMaxModelIndices = 200000;

// The vertex the host uploads, and the byte offsets the model shader's attributes read. Written
// as explicit offsets rather than a struct so the stride cannot drift from the attribute setup.
static const int kVertexSize = 36;
static const int kVertexPosOff = 0;      // 3 floats
static const int kVertexUvOff = 12;      // 2 floats
static const int kVertexColorOff = 20;   // 4 bytes, normalised
static const int kVertexNormalOff = 24;  // 3 floats

struct VertexLayout
{
    int size;
    int posOff, posComponents;
    int normalOff;          // -1 when the format has no normal group
    int uvOff, uvSets;      // -1 when it has no texcoord group
    int colorOff;           // -1 when it has no diffuse group
    int packedPos, packedNormal, packedUv;
};

struct ModelInfo
{
    char name[64];
    float box[6];           // the quantisation reference for packed positions
    int firstSegment, segmentCount;
};

struct SegmentInfo
{
    int model;
    int primType;             // 5 = D3DPT_TRIANGLESTRIP in every segment tested
    int vertexBase, indexBase;
    int primCount;            // INFO: primitives to draw
    int indexCount;           // IBUF: indices the segment holds
    int vertexCount;
    int texName;              // index into g_texNames, -1 when the segment names nothing
    unsigned int glTex;       // upload for that name, 0 when there is no GL context or no name
    int unlit;                // material flag bit 0 is the "lit" bit in the engine's own terms
    unsigned char diffuse[4];
    float box[6];             // the segment's own recorded box, used for culling
};

struct InstanceInfo { int model; float matrix[16]; int unit; };  // unit < 0: a static level instance

static ModelInfo g_models[kMaxModels];
static int g_modelCount = 0;
static SegmentInfo g_segments[kMaxSegments];
static int g_segmentCount = 0;
static InstanceInfo g_instances[kMaxInstances];
static int g_instanceCount = 0;
static unsigned char* g_vertexData = 0;      // kMaxModelVertices * kVertexSize
static unsigned short g_indexData[kMaxModelIndices];
static int g_vertexTotal = 0, g_indexTotal = 0;

struct TexNameRef { char name[64]; int body; int size; };
static TexNameRef g_texNames[kMaxTexNames];
static int g_texNameCount = 0;

struct ResolvedTex { char name[64]; GLuint gl; int w, h, fmt; };
static ResolvedTex g_resolvedTex[128];
static int g_resolvedCount = 0;

static int g_geometryOn = 0;
// The instance whose transform comes from the spawned trooper, or -1 before a class model is
// appended. All the other instances keep the level's own placement.
static int g_trooperInstance = -1;
static int g_trooperDrawn = 0;   // segments of that instance drawn on the last frame
static int g_modelDrawn = 0, g_modelCulled = 0;
static int g_modelPrims = 0;         // primitives the last draw actually drew
static int g_modelIndicesDrawn = 0;  // indices the last draw actually read
static int g_modelVisited = 0;       // segments the last draw actually looked at
static int g_placedSegments = 0;     // segments reachable through placed instances, counted at build
static int g_placedPrimTotal = 0;    // and the primitive counts those segments declare
static int g_placedIndexTotal = 0;   // and the index counts they hold
static int g_oddPrimTypes = 0;       // prim types other than 5 seen while reading
static int g_truncated = 0;          // a cap was reached for the last build
static int g_instancesUnresolved = 0;
// Self-check counters, one per decoded category: 0 position, 1 normal, 2 texcoord, 3 diffuse.
// A category is only counted for a segment that actually has that group.
static long long g_selfcheckWorst[4] = { 0, 0, 0, 0 };
static int g_selfcheckOk[4] = { 0, 0, 0, 0 };
static int g_selfcheckBad[4] = { 0, 0, 0, 0 };
static int g_selfcheckPairs = 0, g_selfcheckUvOutOfRange = 0;

static GLuint g_modelProgram = 0, g_modelVAO = 0, g_modelVbo = 0, g_modelIbo = 0;
static GLint g_muMVP = -1, g_muModel = -1, g_muTex = -1, g_muHasTex = -1, g_muTint = -1;
static GLint g_muLightDir = -1, g_muAmbient = -1;
static const float kModelAmbient = 0.35f;
static const float kModelLight[3] = { 0.35f, 0.75f, 0.56f };   // the host's, not the engine's:
// there is no light list in this data path yet, so shading is one fixed direction plus ambient.

static short ShortLE(const unsigned char* p) { unsigned short v = (unsigned short)(p[0] | (p[1] << 8)); return (short)v; }
static float FloatLE(const unsigned char* p) { float f; memcpy(&f, p, 4); return f; }

// The RVF bits, in the order CreateVertexDeclaration lays elements out. A group only occupies
// bytes when its bit is set - getting that wrong shifts everything after it, which is how a
// texcoord decode can look plausible and read garbage.
static void VertexLayoutFor(int fmt, int size, VertexLayout* out)
{
    memset(out, 0, sizeof(*out));
    out->size = size;
    out->posOff = -1; out->normalOff = -1; out->uvOff = -1; out->colorOff = -1;
    int off = 0;
    if (fmt & 3)
    {
        out->posOff = off;
        out->posComponents = (fmt & 3) + 1;
        out->packedPos = (fmt & 0x1000) ? 1 : 0;
        off += out->packedPos ? (out->posComponents > 2 ? 8 : 4) : out->posComponents * 4;
    }
    if (fmt & 0x1C)
    {
        int w = (fmt >> 2) & 7;
        if (w > 1) off += (fmt & 0x2000) ? 4 : w * 4 - 4;
        off += 4;
    }
    if (fmt & 0x20)
    {
        out->normalOff = off;
        out->packedNormal = (fmt & 0x4000) ? 1 : 0;
        off += out->packedNormal ? 4 : 12;
    }
    if (fmt & 0x40)
    {
        off += (fmt & 0x4000) ? 8 : 24;
    }
    if (fmt & 0x80)
    {
        out->colorOff = off;
        off += 4;
    }
    if (fmt & 0x100) off += 4;
    if (fmt & 0x600)
    {
        out->uvOff = off;
        out->uvSets = (fmt >> 9) & 3;
        out->packedUv = (fmt & 0x8000) ? 1 : 0;
    }
}

// One vertex, decoded through the rules above. A zero normal marks "this format has no normal
// group", which the model shader answers with the flat normal from the screen-space derivatives
// rather than with an invented vertex normal.
static void DecodeVertex(const unsigned char* v, const VertexLayout& L, const float modelBox[6],
                        unsigned char* out)
{
    float p[3] = { 0.f, 0.f, 0.f };
    float uv[2] = { 0.f, 0.f };
    float n[3] = { 0.f, 0.f, 0.f };
    unsigned char rgba[4] = { 255, 255, 255, 255 };

    if (L.posOff >= 0)
    {
        for (int i = 0; i < L.posComponents && i < 3; ++i)
        {
            const unsigned char* q = v + L.posOff + (L.packedPos ? i * 2 : i * 4);
            if (L.packedPos)
            {
                const float extent = modelBox[3 + i] - modelBox[i];
                p[i] = modelBox[i] + ((float)ShortLE(q) + 32767.5f) * (extent / 65535.0f);
            }
            else
            {
                p[i] = FloatLE(q);
            }
        }
    }
    if (L.normalOff >= 0)
    {
        const unsigned char* q = v + L.normalOff;
        if (L.packedNormal)
        {
            // D3DCOLOR: the bytes in memory are B,G,R, so component order is z,y,x.
            n[0] = (q[2] / 255.0f) * 2.0f - 1.0f;
            n[1] = (q[1] / 255.0f) * 2.0f - 1.0f;
            n[2] = (q[0] / 255.0f) * 2.0f - 1.0f;
        }
        else
        {
            for (int i = 0; i < 3; ++i) n[i] = FloatLE(q + i * 4);
        }
    }
    if (L.uvOff >= 0 && L.uvSets > 0)
    {
        const unsigned char* q = v + L.uvOff;
        for (int i = 0; i < 2; ++i)
        {
            uv[i] = L.packedUv ? (float)ShortLE(q + i * 2) / 2048.0f : FloatLE(q + i * 4);
        }
    }
    if (L.colorOff >= 0)
    {
        const unsigned char* q = v + L.colorOff;
        rgba[0] = q[2]; rgba[1] = q[1]; rgba[2] = q[0]; rgba[3] = q[3];
    }

    memcpy(out + kVertexPosOff, p, 12);
    memcpy(out + kVertexUvOff, uv, 8);
    memcpy(out + kVertexColorOff, rgba, 4);
    memcpy(out + kVertexNormalOff, n, 12);
}

struct ChildRef { unsigned int id; int body; int size; };

// Children of one chunk, read with the host's header helper, which follows the same alignment
// rule as PblChunk's _ReadHeader / Realign. Descending here is deliberate and bounded: modl,
// segm, wrld and inst are the ids the model reader itself treats as parents, and nothing else.
static int ReadChildren(PblFile& f, int body, int size, ChildRef* out, int max)
{
    int n = 0;
    f.Seek(body, PblFile::SeekFromStart);
    const int end = body + size;
    while (f.Tell() < end && n < max)
    {
        unsigned int id; int sz, b;
        const int before = f.Tell();
        if (!ReadChunkHeader(f, &id, &sz, &b)) break;
        out[n].id = id; out[n].body = b; out[n].size = sz;
        ++n;
        f.Seek(b + ((sz + 3) & ~3), PblFile::SeekFromStart);
        if (f.Tell() <= before) break;
    }
    return n;
}

static void ReadStringAt(PblFile& f, int pos, int len, char* out, int cap)
{
    f.Seek(pos, PblFile::SeekFromStart);
    const int n = len < cap - 1 ? len : cap - 1;
    out[0] = 0;
    if (n > 0) f.ReadData(out, n);
    // chunk names are not always NUL-terminated in the payload, so terminate at the length read
    out[n] = 0;
    for (int i = 0; i < n; ++i)
    {
        if (out[i] == 0) break;
    }
}

static void ReadNameAt(PblFile& f, const ChildRef& c, char* out, int cap)
{
    ReadStringAt(f, c.body, c.size, out, cap);
}

// Every segment names its textures through TNAM, so the name has to resolve to a tex_ chunk. The
// index is built once per build from the container's own texture walk.
static int BuildTextureNameIndex(PblFile& file, bool append)
{
    static ChunkHit hits[kMaxChunkHits];
    int total = 0;
    const int found = CollectChunks(file, "tex_", hits, kMaxChunkHits, &total);
    // Appending a class record must not renumber the level's texture-name indices: the level's
    // segments still point into this table. A fresh build starts at zero; an append continues.
    if (!append) g_texNameCount = 0;
    for (int i = 0; i < found && g_texNameCount < kMaxTexNames; ++i)
    {
        ChildRef kids[8];
        const int n = ReadChildren(file, (int)hits[i].offset, hits[i].size, kids, 8);
        for (int k = 0; k < n; ++k)
        {
            if (!IsFourCC(kids[k].id, "NAME")) continue;
            TexNameRef& ref = g_texNames[g_texNameCount++];
            ref.body = (int)hits[i].offset;
            ref.size = hits[i].size;
            ReadNameAt(file, kids[k], ref.name, (int)sizeof(ref.name));
            break;
        }
    }
    return g_texNameCount;
}

static int FindTexName(const char* name)
{
    for (int i = 0; i < g_texNameCount; ++i)
    {
        if (strcmp(g_texNames[i].name, name) == 0) return i;
    }
    return -1;
}

static GLuint ResolveTexture(PblFile& file, int nameIdx)
{
    if (nameIdx < 0 || !g_renderReady) return 0;
    for (int i = 0; i < g_resolvedCount; ++i)
    {
        if (strcmp(g_resolvedTex[i].name, g_texNames[nameIdx].name) == 0) return g_resolvedTex[i].gl;
    }
    if (g_resolvedCount >= (int)(sizeof(g_resolvedTex) / sizeof(g_resolvedTex[0]))) return 0;

    TextureData tex;
    if (!ExtractTexture(file, g_texNames[nameIdx].body, g_texNames[nameIdx].size, &tex)) return 0;
    const GLuint id = UploadDecodedTexture(tex);   // defined with the GL section
    if (!id) { free(tex.pixels); return 0; }
    ResolvedTex& slot = g_resolvedTex[g_resolvedCount++];
    snprintf(slot.name, sizeof(slot.name), "%s", tex.name);
    slot.gl = id;
    slot.w = tex.width;
    slot.h = tex.height;
    slot.fmt = tex.glInternal;
    free(tex.pixels);
    return id;
}

// The model shader. Segments with no normal group are flat-shaded from the derivative pair, which
// is exact for a triangle and needs no invented vertex data.
static const char* MODEL_VS =
    "#version 300 es\n"
    "in vec3 aPos; in vec2 aUV; in vec4 aColor; in vec3 aNormal;\n"
    "uniform mat4 uMVP; uniform mat4 uModel;\n"
    "out vec2 vUV; out vec4 vColor; out vec3 vNormal; out vec3 vWorld;\n"
    "void main(){ gl_Position = uMVP * vec4(aPos, 1.0); vWorld = (uModel * vec4(aPos, 1.0)).xyz;\n"
    "  vUV = aUV; vColor = aColor; vNormal = aNormal; }\n";

static const char* MODEL_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vUV; in vec4 vColor; in vec3 vNormal; in vec3 vWorld; out vec4 fragColor;\n"
    "uniform sampler2D uTex; uniform int uHasTex; uniform vec3 uTint; uniform vec3 uLightDir; uniform float uAmbient;\n"
    "void main(){\n"
    "  vec3 n = normalize(vNormal);\n"
    "  if (dot(vNormal, vNormal) < 1e-6) n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));\n"
    "  float lambert = max(0.0, dot(n, normalize(uLightDir)));\n"
    "  float light = uAmbient + (1.0 - uAmbient) * lambert;\n"
    "  vec4 tex = uHasTex != 0 ? texture(uTex, vUV) : vec4(1.0, 1.0, 1.0, 1.0);\n"
    "  fragColor = vec4(tex.rgb * uTint * light, tex.a);\n"
    "}\n";

static int EnsureModelGL(void)
{
    if (g_modelProgram) return 1;
    GLuint vs = Compile(GL_VERTEX_SHADER, MODEL_VS);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, MODEL_FS);
    g_modelProgram = glCreateProgram();
    glAttachShader(g_modelProgram, vs);
    glAttachShader(g_modelProgram, fs);
    glBindAttribLocation(g_modelProgram, 0, "aPos");
    glBindAttribLocation(g_modelProgram, 1, "aUV");
    glBindAttribLocation(g_modelProgram, 2, "aColor");
    glBindAttribLocation(g_modelProgram, 3, "aNormal");
    glLinkProgram(g_modelProgram);
    GLint ok = 0;
    glGetProgramiv(g_modelProgram, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char msg[256];
        glGetProgramInfoLog(g_modelProgram, sizeof(msg), 0, msg);
        Log("model program: %s", msg);
        glDeleteProgram(g_modelProgram);
        g_modelProgram = 0;
        return 0;
    }
    g_muMVP = glGetUniformLocation(g_modelProgram, "uMVP");
    g_muModel = glGetUniformLocation(g_modelProgram, "uModel");
    g_muTex = glGetUniformLocation(g_modelProgram, "uTex");
    g_muHasTex = glGetUniformLocation(g_modelProgram, "uHasTex");
    g_muTint = glGetUniformLocation(g_modelProgram, "uTint");
    g_muLightDir = glGetUniformLocation(g_modelProgram, "uLightDir");
    g_muAmbient = glGetUniformLocation(g_modelProgram, "uAmbient");

    glGenVertexArrays(1, &g_modelVAO);
    glGenBuffers(1, &g_modelVbo);
    glGenBuffers(1, &g_modelIbo);
    glBindVertexArray(g_modelVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_modelVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_modelIbo);
    // The offsets here are the constants above; the stride is kVertexSize, so the attribute setup
    // and the bytes written by DecodeVertex are one agreement, not two.
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kVertexSize, (const void*)kVertexPosOff);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kVertexSize, (const void*)kVertexUvOff);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, kVertexSize, (const void*)kVertexColorOff);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, kVertexSize, (const void*)kVertexNormalOff);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    return 1;
}

static void ClearModelData(void)
{
    for (int i = 0; i < g_resolvedCount; ++i)
    {
        if (g_resolvedTex[i].gl) glDeleteTextures(1, &g_resolvedTex[i].gl);
    }
    g_resolvedCount = 0;
    g_trooperInstance = -1;
    g_modelCount = 0; g_segmentCount = 0; g_instanceCount = 0;
    g_vertexTotal = 0; g_indexTotal = 0;
    g_texNameCount = 0;
    g_truncated = 0; g_instancesUnresolved = 0;
    g_modelVisited = 0; g_oddPrimTypes = 0;
    g_placedSegments = 0; g_placedPrimTotal = 0; g_placedIndexTotal = 0;
}

// Decode every segment of every model in a container into one vertex and one index buffer, then
// read the instance placement. Indices are offset by each segment's vertex base because all
// segments share one buffer.
static int BuildModelGeometry(const char* path, bool append = false)
{
    char full[512];
    ResolvePath(full, sizeof(full), path);
    PblFile file;
    if (!file.Open(full, false, false, false))
    {
        snprintf(g_report, sizeof(g_report), "models: cannot open %s", full);
        return -1;
    }
    if (!g_vertexData) g_vertexData = (unsigned char*)malloc((size_t)kMaxModelVertices * kVertexSize);
    if (!g_vertexData)
    {
        snprintf(g_report, sizeof(g_report), "models: no room for the vertex data");
        file.Close();
        return -2;
    }
    g_classRecordRead = append;
    if (!append) ClearModelData();
    BuildTextureNameIndex(file, append);

    static ChunkHit hits[kMaxChunkHits];
    int total = 0;
    const int models = CollectChunks(file, "modl", hits, kMaxChunkHits, &total);
    ChildRef walkKids[1024];   // level 1: modl and wrld children
    ChildRef segKids[128];     // level 2: segm children
    ChildRef instKids[32];     // level 3: inst and INFO children

    for (int m = 0; m < models && g_modelCount < kMaxModels; ++m)
    {
        const int n = ReadChildren(file, (int)hits[m].offset, hits[m].size, walkKids, 1024);
        ModelInfo& model = g_models[g_modelCount];
        memset(&model, 0, sizeof(model));
        model.firstSegment = g_segmentCount;
        for (int k = 0; k < n; ++k)
        {
            if (IsFourCC(walkKids[k].id, "NAME")) ReadNameAt(file, walkKids[k], model.name, (int)sizeof(model.name));
            // modl INFO is the model box: skin, proj flag, segment count, pad, then the box that
            // RedModel stores as _BoundingBox - the reference every packed position is read against.
            if (IsFourCC(walkKids[k].id, "INFO") && walkKids[k].size >= 68)
            {
                file.Seek(walkKids[k].body, PblFile::SeekFromStart);
                file.Read32(); file.Read32(); file.Read32(); file.Read32();
                for (int i = 0; i < 6; ++i) model.box[i] = file.ReadFloat();
            }
        }
        ++g_modelCount;

        for (int k = 0; k < n; ++k)
        {
            if (!IsFourCC(walkKids[k].id, "segm")) continue;
            if (g_segmentCount >= kMaxSegments) { g_truncated = 1; continue; }
            const int sn = ReadChildren(file, walkKids[k].body, walkKids[k].size, segKids, 128);
            SegmentInfo seg;
            memset(&seg, 0, sizeof(seg));
            seg.model = g_modelCount - 1;
            seg.texName = -1;
            seg.unlit = 1;                       // the engine's own reading: material flag bit 0 is the lit bit
            for (int i = 0; i < 4; ++i) seg.diffuse[i] = 200;
            int vcount = 0, vsize = 0, vfmt = 0, vpayload = -1;
            int icount = 0, ipayload = -1;

            for (int s = 0; s < sn; ++s)
            {
                const ChildRef& c = segKids[s];
                file.Seek(c.body, PblFile::SeekFromStart);
                if (IsFourCC(c.id, "INFO") && c.size >= 12)
                {
                    const unsigned int primType = (unsigned int)file.Read32();
                    file.Read32();                  // numVertices
                    seg.primCount = file.Read32();  // primCount
                    // prim type 5 is D3DPT_TRIANGLESTRIP, not a triangle list. That is not a guess:
                    // for every segment in every container tested, primCount == numIndices - 2, which
                    // is exactly the primitive count of a strip of numIndices indices. Drawing those
                    // indices as a triangle list would draw a different mesh.
                    seg.primType = (int)primType;
                    if (primType != 5) g_oddPrimTypes++;
                }
                else if ((IsFourCC(c.id, "VBUF") || IsFourCC(c.id, "VDAT")) && c.size >= 12)
                {
                    const int cnt = file.Read32(), sz = file.Read32(), fmt = file.Read32();
                    // prefer the packed stream, which is the path the engine takes when the render
                    // caps report hardware geometry; the uncompressed copy is still read, by the
                    // self-check, as the file's own answer
                    if (cnt > 0 && sz > 0 && (vcount == 0 || (fmt & 0x1000)))
                    {
                        vcount = cnt; vsize = sz; vfmt = fmt; vpayload = c.body + 12;
                    }
                }
                else if (IsFourCC(c.id, "IBUF") && c.size >= 4)
                {
                    seg.indexCount = file.Read32();
                    ipayload = c.body + 4;
                }
                else if (IsFourCC(c.id, "BBOX") && c.size >= 24)
                {
                    for (int i = 0; i < 6; ++i) seg.box[i] = file.ReadFloat();
                }
                else if (IsFourCC(c.id, "MTRL") && c.size >= 16)
                {
                    const int flags = file.Read32();
                    seg.unlit = (flags & 1) ? 0 : 1;
                    const unsigned int colour = (unsigned int)file.Read32();
                    seg.diffuse[0] = (unsigned char)(colour & 0xff);
                    seg.diffuse[1] = (unsigned char)((colour >> 8) & 0xff);
                    seg.diffuse[2] = (unsigned char)((colour >> 16) & 0xff);
                    seg.diffuse[3] = (unsigned char)((colour >> 24) & 0xff);
                }
                else if (IsFourCC(c.id, "TNAM") && c.size >= 4)
                {
                    // TNAM is an index followed by the name: reading the payload as a string would
                    // keep the four index bytes and match nothing.
                    const int idx = file.Read32();
                    if (idx == 0 && seg.texName < 0)
                    {
                        char nm[64];
                        ReadStringAt(file, c.body + 4, c.size - 4, nm, (int)sizeof(nm));
                        seg.texName = FindTexName(nm);
                    }
                }
            }
            if (vcount <= 0 || vsize <= 0 || vpayload < 0) continue;
            if (g_vertexTotal + vcount > kMaxModelVertices) { g_truncated = 1; continue; }
            if (seg.indexCount > 0 && g_indexTotal + seg.indexCount > kMaxModelIndices) { g_truncated = 1; continue; }

            VertexLayout L;
            VertexLayoutFor(vfmt, vsize, &L);
            seg.vertexCount = vcount;
            seg.vertexBase = g_vertexTotal;
            seg.indexBase = g_indexTotal;
            int slot = g_segmentCount;
            g_segments[slot] = seg;
            ++g_segmentCount;

            file.Seek(vpayload, PblFile::SeekFromStart);
            static unsigned char* streamBuf = 0;
            static size_t streamCap = 0;
            if (streamCap < (size_t)vcount * vsize)
            {
                free(streamBuf);
                streamBuf = (unsigned char*)malloc((size_t)vcount * vsize);
                streamCap = streamBuf ? (size_t)vcount * vsize : 0;
            }
            if (!streamBuf) { g_truncated = 1; continue; }
            file.ReadData(streamBuf, (size_t)vcount * vsize);
            for (int v = 0; v < vcount; ++v)
            {
                DecodeVertex(streamBuf + (size_t)v * vsize, L, g_models[seg.model].box,
                             g_vertexData + ((size_t)g_vertexTotal + v) * kVertexSize);
            }
            g_vertexTotal += vcount;

            if (seg.indexCount > 0 && ipayload >= 0)
            {
                file.Seek(ipayload, PblFile::SeekFromStart);
                for (int i = 0; i < seg.indexCount; ++i)
                {
                    g_indexData[g_indexTotal + i] = (unsigned short)(seg.vertexBase + file.Read16());
                }
                g_indexTotal += seg.indexCount;
            }
            // Upload what the segment asks for, once per name. Without a GL context this is the
            // decode-only path the node check uses, and it still reports the same counts.
            g_segments[slot].glTex = ResolveTexture(file, seg.texName);
        }
        model.segmentCount = g_segmentCount - model.firstSegment;
    }

    // Instances: TYPE names a model, XFRM is the transform. Read the way LoadUtil::ReadTransform
    // does it - four rows of three floats, the fourth row being the translation.
    total = 0;
    const int worlds = CollectChunks(file, "wrld", hits, kMaxChunkHits, &total);
    for (int w = 0; w < worlds; ++w)
    {
        const int wn = ReadChildren(file, (int)hits[w].offset, hits[w].size, walkKids, 1024);
        for (int k = 0; k < wn; ++k)
        {
            if (!IsFourCC(walkKids[k].id, "inst")) continue;
            const int in = ReadChildren(file, walkKids[k].body, walkKids[k].size, segKids, 128);
            char type[64] = { 0 };
            ChildRef xfrm = { 0, -1, 0 };
            for (int i = 0; i < in; ++i)
            {
                if (!IsFourCC(segKids[i].id, "INFO")) continue;
                const int ii = ReadChildren(file, segKids[i].body, segKids[i].size, instKids, 32);
                for (int j = 0; j < ii; ++j)
                {
                    if (IsFourCC(instKids[j].id, "TYPE")) ReadNameAt(file, instKids[j], type, (int)sizeof(type));
                    else if (IsFourCC(instKids[j].id, "XFRM")) xfrm = instKids[j];
                }
            }
            int modelIdx = -1;
            for (int m = 0; m < g_modelCount; ++m)
            {
                if (strcmp(g_models[m].name, type) == 0) { modelIdx = m; break; }
            }
            if (modelIdx < 0 || xfrm.body < 0 || xfrm.size < 48) { ++g_instancesUnresolved; continue; }
            if (g_instanceCount >= kMaxInstances) { g_truncated = 1; continue; }
            InstanceInfo& inst = g_instances[g_instanceCount++];
            inst.model = modelIdx;
            inst.unit = -1;
            // What the draw path is expected to reach through this instance, recorded at build time,
            // so that "the draw path reached exactly what the reader decoded" is an assertion rather
            // than a tautology. A model placed by several instances is reached once per instance.
            g_placedSegments += g_models[modelIdx].segmentCount;
            for (int s = 0; s < g_models[modelIdx].segmentCount; ++s)
            {
                const SegmentInfo& seg = g_segments[g_models[modelIdx].firstSegment + s];
                g_placedPrimTotal += seg.primCount;
                g_placedIndexTotal += seg.indexCount;
            }
            for (int i = 0; i < 16; ++i) inst.matrix[i] = 0.f;
            file.Seek(xfrm.body, PblFile::SeekFromStart);
            PblMatrix m;
            m.Reset();
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 3; ++col) m.m[row][col] = file.ReadFloat();
                m.m[row][3] = (row == 3) ? 1.0f : 0.0f;
            }
            PblToGL(inst.matrix, m);
        }
    }
    file.Close();
    return 0;
}

// The same decode run twice: once on the packed stream, once on the uncompressed copy of the
// same vertices that the same segment stores. The file answers itself, and this validates the C
// decoder rather than the Python one - a wrong offset or a wrong sign shows up here as a number.
static void RunModelSelfCheck(const char* path, int limit)
{
    char full[512];
    ResolvePath(full, sizeof(full), path);
    PblFile file;
    if (!file.Open(full, false, false, false)) return;
    g_selfcheckOk[0] = g_selfcheckOk[1] = g_selfcheckOk[2] = g_selfcheckOk[3] = 0;
    g_selfcheckBad[0] = g_selfcheckBad[1] = g_selfcheckBad[2] = g_selfcheckBad[3] = 0;
    g_selfcheckWorst[0] = g_selfcheckWorst[1] = g_selfcheckWorst[2] = g_selfcheckWorst[3] = 0;
    g_selfcheckPairs = 0;
    g_selfcheckUvOutOfRange = 0;
    ChildRef walkKids[1024], segKids[128];
    static ChunkHit hits[kMaxChunkHits];
    int total = 0;
    const int models = CollectChunks(file, "modl", hits, kMaxChunkHits, &total);
    long long worstPosScaled = 0;
    static unsigned char* bufA = 0;
    static unsigned char* bufB = 0;
    static size_t capA = 0, capB = 0;

    for (int m = 0; m < models && g_selfcheckPairs < limit; ++m)
    {
        const int n = ReadChildren(file, (int)hits[m].offset, hits[m].size, walkKids, 1024);
        float modelBox[6] = { 0, 0, 0, 0, 0, 0 };
        for (int k = 0; k < n; ++k)
        {
            if (IsFourCC(walkKids[k].id, "INFO") && walkKids[k].size >= 68)
            {
                file.Seek(walkKids[k].body, PblFile::SeekFromStart);
                file.Read32(); file.Read32(); file.Read32(); file.Read32();
                for (int i = 0; i < 6; ++i) modelBox[i] = file.ReadFloat();
                break;
            }
        }
        for (int k = 0; k < n && g_selfcheckPairs < limit; ++k)
        {
            if (!IsFourCC(walkKids[k].id, "segm")) continue;
            const int sn = ReadChildren(file, walkKids[k].body, walkKids[k].size, segKids, 128);
            ChildRef packed = { 0, -1, 0 }, plain = { 0, -1, 0 };
            for (int s = 0; s < sn; ++s)
            {
                if (!(IsFourCC(segKids[s].id, "VBUF") || IsFourCC(segKids[s].id, "VDAT")) || segKids[s].size < 12) continue;
                file.Seek(segKids[s].body, PblFile::SeekFromStart);
                const int cnt = file.Read32(), sz = file.Read32(), fmt = file.Read32();
                if (cnt <= 0) continue;
                if ((fmt & 0x1000) && packed.body < 0) packed = segKids[s];
                else if (!(fmt & 0x1000) && plain.body < 0) plain = segKids[s];
            }
            if (packed.body < 0 || plain.body < 0) continue;
            file.Seek(packed.body, PblFile::SeekFromStart);
            const int cnt = file.Read32(), psz = file.Read32(), pfmt = file.Read32();
            file.Seek(plain.body, PblFile::SeekFromStart);
            const int fcnt = file.Read32(), fsz = file.Read32(), ffmt = file.Read32();
            if (cnt != fcnt || cnt <= 0 || cnt > kMaxModelVertices) continue;
            if (capA < (size_t)cnt * psz) { free(bufA); bufA = (unsigned char*)malloc((size_t)cnt * psz); capA = bufA ? (size_t)cnt * psz : 0; }
            if (capB < (size_t)cnt * fsz) { free(bufB); bufB = (unsigned char*)malloc((size_t)cnt * fsz); capB = bufB ? (size_t)cnt * fsz : 0; }
            if (!bufA || !bufB) continue;
            file.Seek(packed.body + 12, PblFile::SeekFromStart);
            file.ReadData(bufA, (size_t)cnt * psz);
            file.Seek(plain.body + 12, PblFile::SeekFromStart);
            file.ReadData(bufB, (size_t)cnt * fsz);

            VertexLayout pl, fl;
            VertexLayoutFor(pfmt, psz, &pl);
            VertexLayoutFor(ffmt, fsz, &fl);
            if (pl.posComponents != fl.posComponents) continue;

            // The file answers itself: the packed stream and the uncompressed copy of the same
            // vertices must agree to the resolution the format can carry. Measured in units of one
            // quantisation step per category, so "agrees" means "the decoded value is the value the
            // file states", not "the picture looks right".
            long long worst[4] = { 0, 0, 0, 0 };   // position, normal, uv, colour
            int uvOutOfRange = 0;
            const double uvStep = 1.0 / 2048.0, byteStep = 2.0 / 255.0;   // one byte of a signed range
            for (int v = 0; v < cnt; ++v)
            {
                unsigned char a[36], b[36];
                DecodeVertex(bufA + (size_t)v * psz, pl, modelBox, a);
                DecodeVertex(bufB + (size_t)v * fsz, fl, modelBox, b);
                for (int i = 0; i < 3; ++i)
                {
                    if (pl.posOff < 0 || fl.posOff < 0) break;
                    // one quantisation step is per axis: the model box extent divided by 65535.
                    // Normalising by 1/65535 instead measures distance in world units and calls every
                    // correctly decoded vertex "bad" whenever the box is wider than one unit - which is
                    // exactly what this check got wrong the first time it ran.
                    const double ext = (double)modelBox[3 + i] - (double)modelBox[i];
                    if (ext <= 1e-9) continue;
                    const double step = ext / 65535.0;
                    const double d = fabs((double)FloatLE(a + kVertexPosOff + i * 4) -
                                          (double)FloatLE(b + kVertexPosOff + i * 4));
                    const long long steps = (long long)llround(d / step);
                    if (steps > worst[0]) worst[0] = steps;
                }
                if (pl.normalOff >= 0 && fl.normalOff >= 0)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        const double d = fabs((double)FloatLE(a + kVertexNormalOff + i * 4) -
                                             (double)FloatLE(b + kVertexNormalOff + i * 4));
                        const long long steps = (long long)llround(d / byteStep);
                        if (steps > worst[1]) worst[1] = steps;
                    }
                }
                if (pl.uvOff >= 0 && fl.uvOff >= 0 && pl.uvSets > 0 && fl.uvSets > 0)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        const double ref = (double)FloatLE(b + kVertexUvOff + i * 4);
                        if (fabs(ref) > 16.0) { ++uvOutOfRange; continue; }   // 12-bit fixed point cannot hold it
                        const double d = fabs((double)FloatLE(a + kVertexUvOff + i * 4) - ref);
                        const long long steps = (long long)llround(d / uvStep);
                        if (steps > worst[2]) worst[2] = steps;
                    }
                }
                for (int i = 0; i < 4; ++i)
                {
                    if (pl.colorOff < 0 || fl.colorOff < 0) break;
                    const long long d = llabs((long long)a[kVertexColorOff + i] - (long long)b[kVertexColorOff + i]);
                    if (d > worst[3]) worst[3] = d;
                }
            }
            ++g_selfcheckPairs;
            for (int c = 0; c < 4; ++c)
            {
                if (worst[c] == 0) continue;
                if (worst[c] > 1) ++g_selfcheckBad[c];
                else ++g_selfcheckOk[c];
                if (worst[c] > g_selfcheckWorst[c]) g_selfcheckWorst[c] = worst[c];
            }
            g_selfcheckUvOutOfRange += uvOutOfRange;
        }
    }
    free(bufA); free(bufB);
    capA = capB = 0;
    bufA = bufB = 0;
    file.Close();
}

// Transform a point the way the engine does: row vectors, v * M, with the translation in row 3.
static void TransformPointRow(const PblMatrix& m, const float in[3], float out[3])
{
    for (int k = 0; k < 3; ++k)
    {
        out[k] = in[0] * m.m[0][k] + in[1] * m.m[1][k] + in[2] * m.m[2][k] + m.m[3][k];
    }
}

static const char* BOLT_VS =
    "#version 300 es\n"
    "in vec3 aPos;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ gl_Position = uMVP * vec4(aPos, 1.0); }\n";

static const char* BOLT_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "out vec4 fragColor;\n"
    "void main(){ fragColor = uColor; }\n";

static GLuint g_boltProgram = 0;
static GLuint g_boltVao = 0;
static GLuint g_boltVbo = 0;
static GLint g_uBoltMVP = -1;
static GLint g_uBoltColor = -1;

static void EnsureBoltGL(void)
{
    if (g_boltProgram) return;
    GLuint vs = Compile(GL_VERTEX_SHADER, BOLT_VS);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, BOLT_FS);
    g_boltProgram = glCreateProgram();
    glAttachShader(g_boltProgram, vs);
    glAttachShader(g_boltProgram, fs);
    glBindAttribLocation(g_boltProgram, 0, "aPos");
    glLinkProgram(g_boltProgram);
    g_uBoltMVP = glGetUniformLocation(g_boltProgram, "uMVP");
    g_uBoltColor = glGetUniformLocation(g_boltProgram, "uColor");

    glGenVertexArrays(1, &g_boltVao);
    glBindVertexArray(g_boltVao);
    glGenBuffers(1, &g_boltVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_boltVbo);
    glBufferData(GL_ARRAY_BUFFER, 64 * 12 * 3 * sizeof(float), 0, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

static void DrawOrdnance(const float* viewProj)
{
    EnsureBoltGL();
    if (!g_boltProgram || !g_boltVao) return;
    float ord[64 * 7];
    int count = CombatHarnessGetLiveOrdnance(ord, 64);
    if (count <= 0) return;

    float teamVerts[2][64 * 12 * 3];
    int vCount[2] = { 0, 0 };

    for (int i = 0; i < count && i < 64; ++i)
    {
        float* p = &ord[i * 7];
        float px = p[0], py = p[1], pz = p[2];
        float vx = p[3], vy = p[4], vz = p[5];
        int team = (int)p[6] == 1 ? 0 : 1; // 0 = Player/Blue, 1 = CIS/Red

        float vlen = sqrtf(vx * vx + vy * vy + vz * vz);
        float dx = vlen > 0.1f ? vx / vlen : 0.0f;
        float dy = vlen > 0.1f ? vy / vlen : 0.0f;
        float dz = vlen > 0.1f ? vz / vlen : 1.0f;

        float len = 1.6f;
        float w = 0.07f;
        float hx = px, hy = py, hz = pz;
        float tx = px - dx * len, ty = py - dy * len, tz = pz - dz * len;

        // Quad 1: offset in Y
        float* out = &teamVerts[team][vCount[team] * 3];
        out[0] = tx; out[1] = ty - w; out[2] = tz;
        out[3] = hx; out[4] = hy - w; out[5] = hz;
        out[6] = hx; out[7] = hy + w; out[8] = hz;
        out[9] = tx; out[10] = ty - w; out[11] = tz;
        out[12] = hx; out[13] = hy + w; out[14] = hz;
        out[15] = tx; out[16] = ty + w; out[17] = tz;
        vCount[team] += 6;

        // Quad 2: offset perpendicular in X/Z
        float px_ = -dz * w;
        float pz_ = dx * w;
        out = &teamVerts[team][vCount[team] * 3];
        out[0] = tx - px_; out[1] = ty; out[2] = tz - pz_;
        out[3] = hx - px_; out[4] = hy; out[5] = hz - pz_;
        out[6] = hx + px_; out[7] = hy; out[8] = hz + pz_;
        out[9] = tx - px_; out[10] = ty; out[11] = tz - pz_;
        out[12] = hx + px_; out[13] = hy; out[14] = hz + pz_;
        out[15] = tx + px_; out[16] = ty; out[17] = tz + pz_;
        vCount[team] += 6;
    }

    glUseProgram(g_boltProgram);
    glBindVertexArray(g_boltVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_boltVbo);
    glUniformMatrix4fv(g_uBoltMVP, 1, GL_FALSE, viewProj);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    // Player/Republic/Empire (Blue)
    if (vCount[0] > 0)
    {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vCount[0] * 3 * sizeof(float), teamVerts[0]);
        glUniform4f(g_uBoltColor, 0.2f, 0.75f, 1.0f, 0.95f);
        glDrawArrays(GL_TRIANGLES, 0, vCount[0]);
    }

    // CIS / Droids (Red)
    if (vCount[1] > 0)
    {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vCount[1] * 3 * sizeof(float), teamVerts[1]);
        glUniform4f(g_uBoltColor, 1.0f, 0.2f, 0.1f, 0.95f);
        glDrawArrays(GL_TRIANGLES, 0, vCount[1]);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

static void DrawModelGeometry(const float* viewProj)
{
    if (!g_modelProgram || !g_segmentCount) return;
    glUseProgram(g_modelProgram);
    glBindVertexArray(g_modelVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_modelVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_modelIbo);
    glActiveTexture(GL_TEXTURE0);
    glUniform3f(g_muLightDir, kModelLight[0], kModelLight[1], kModelLight[2]);
    glUniform1f(g_muAmbient, kModelAmbient);
    glUniform1i(g_muTex, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Per-draw counters: the host reports what the last draw actually visited, so an assertion
    // about coverage is about the draw path, not about how many frames happened to run.
    g_modelDrawn = 0; g_modelCulled = 0; g_modelPrims = 0; g_modelIndicesDrawn = 0; g_modelVisited = 0;
    g_trooperDrawn = 0;

    for (int i = 0; i < g_instanceCount; ++i)
    {
        InstanceInfo& inst = g_instances[i];
        if (inst.model < 0 || inst.model >= g_modelCount) continue;
        if (inst.unit >= 0)
        {
            // A combat unit's instance follows its own GameObject, including jumps and rolls.
            // Read the engine's full matrix rather than rebuilding an orientation from a heading.
            float values[16];
            if (CombatHarnessUnitMatrix(inst.unit, values, 16))
            {
                PblMatrix world;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) world.m[r][c] = values[r * 4 + c];
                if (!CombatHarnessUnitAlive(inst.unit))
                {
                    // Fallen unit on ground: pitch 90 degrees back, lower to ground
                    float origUp[3] = { world.m[1][0], world.m[1][1], world.m[1][2] };
                    world.m[1][0] = -world.m[2][0];
                    world.m[1][1] = -world.m[2][1];
                    world.m[1][2] = -world.m[2][2];
                    world.m[2][0] = origUp[0];
                    world.m[2][1] = origUp[1];
                    world.m[2][2] = origUp[2];
                    world.m[3][1] = values[3 * 4 + 1] - 0.85f;
                }
                PblToGL(inst.matrix, world);
            }
        }
        const ModelInfo& model = g_models[inst.model];
        float mvp[16];
        Mat4Mul(mvp, viewProj, inst.matrix);
        glUniformMatrix4fv(g_muMVP, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(g_muModel, 1, GL_FALSE, inst.matrix);

        // The instance matrix is the engine's row-major transform (PblToGL is a byte copy: the
        // row-vector layout is also what GL expects for a column-vector transform). Use that same
        // layout for the centre transform. Transposing it here moves the translation out of the
        // bottom row, so a moving trooper is culled as if it were still at the origin.
        PblMatrix instM;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) instM.m[r][c] = inst.matrix[r * 4 + c];

        for (int s = 0; s < model.segmentCount; ++s)
        {
            const SegmentInfo& seg = g_segments[model.firstSegment + s];
            const float centre[3] = { (seg.box[0] + seg.box[3]) * 0.5f,
                                      (seg.box[1] + seg.box[4]) * 0.5f,
                                      (seg.box[2] + seg.box[5]) * 0.5f };
            float worldCentre[3];
            TransformPointRow(instM, centre, worldCentre);
            const float ext = sqrtf((seg.box[3] - seg.box[0]) * (seg.box[3] - seg.box[0]) +
                                    (seg.box[4] - seg.box[1]) * (seg.box[4] - seg.box[1]) +
                                    (seg.box[5] - seg.box[2]) * (seg.box[5] - seg.box[2])) * 0.5f;
            g_modelVisited++;
            if (SceneCamera()->ClassifySphere(PblVector3(worldCentre[0], worldCentre[1], worldCentre[2]),
                                    ext > 0.05f ? ext : 0.05f, 900.f) == RedCamera::VISIBILITY_NONE)
            {
                ++g_modelCulled;
                continue;
            }
            GLuint tex = seg.glTex;
            if (tex)
            {
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(g_muHasTex, 1);
            }
            else
            {
                glUniform1i(g_muHasTex, 0);
            }
            glUniform3f(g_muTint, seg.unlit ? 1.0f : seg.diffuse[0] / 255.0f,
                        seg.unlit ? 1.0f : seg.diffuse[1] / 255.0f,
                        seg.unlit ? 1.0f : seg.diffuse[2] / 255.0f);
            if (seg.primType == 5)
            {
                // Drawn as the strip the segment says it is. The same index buffer drawn as a triangle
                // list gives a different mesh from the same data, and both look like a picture.
                glDrawElements(GL_TRIANGLE_STRIP, seg.indexCount, GL_UNSIGNED_SHORT,
                               (const void*)(size_t)((long long)seg.indexBase * 2));
            }
            else
            {
                glDrawElements(GL_TRIANGLES, seg.primCount * 3, GL_UNSIGNED_SHORT,
                               (const void*)(size_t)((long long)seg.indexBase * 2));
            }
            ++g_modelDrawn;
            if (i == g_trooperInstance) ++g_trooperDrawn;
            g_modelPrims += seg.primCount;
            g_modelIndicesDrawn += seg.indexCount;
        }
    }
    DrawOrdnance(viewProj);
    glBindVertexArray(g_vao);
    GLCHK("drawModels");
}

// Upload the combined level + class geometry. An append replaces the GPU buffer contents, not
// the level's models or texture-name table.
static void UploadModelBuffers()
{
    if (!g_renderReady || !g_vertexTotal || !g_indexTotal) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_modelVbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)g_vertexTotal * kVertexSize, g_vertexData, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_modelIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)g_indexTotal * 2, g_indexData, GL_STATIC_DRAW);
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_build_models(const char* path)
{
#ifdef __EMSCRIPTEN__
    // The reader itself needs no renderer: parsing, decoding and the self-check work in node too.
    // Only the GL object and the buffer upload are skipped when there is no context, so the same
    // numbers can be checked with and without a browser.
    if (g_renderReady && !EnsureModelGL()) return -2;
    const int rc = BuildModelGeometry(path);
    if (rc != 0) { Log("models: %s", g_report); return rc; }
    UploadModelBuffers();
    snprintf(g_report, sizeof(g_report),
             "models: %d models, %d segments, %d vertices, %d indices from %s; instances placed: %d "
             "(%d could not be resolved), segments reachable through placed instances: %d, textures "
             "named: %d. Segments are triangle strips (prim type 5), drawn as strips. Culling and drawing "
             "through the engine's camera; shading is the host's single fixed light plus ambient because "
             "no light list is in this data path.",
             g_modelCount, g_segmentCount, g_vertexTotal, g_indexTotal, path, g_instanceCount,
             g_instancesUnresolved, g_placedSegments, g_texNameCount);
    if (g_truncated) strcat(g_report, " | CAP REACHED: the container is larger than these caps");
    Log("models: %s", g_report);
    g_geometryOn = 1;
    return g_segmentCount;
#else
    (void)path;
    return -1;
#endif
}

/* Appends one unit's model to the level. The class record has no wrld/inst placement of its
 * own; the unit's transform comes from its GameObject every frame. Parse the record only when
 * bReadFile is true, so four targets of one faction share one parse and one set of GL buffers. */
static int AppendUnitModel(const char* path, int iSlot, bool bReadFile)
{
    const unsigned int uiWanted = CombatHarnessUnitModelHash(iSlot);
    if (!uiWanted) return 0;
    const int iFirstNewModel = bReadFile ? g_modelCount : 0;
    if (bReadFile)
    {
        const int rc = BuildModelGeometry(path, true);
        if (rc != 0) { Log("models: %s", g_report); return rc; }
    }

    int iModel = -1;
    for (int m = iFirstNewModel; m < g_modelCount; ++m)
    {
        if (PblHash(g_models[m].name) == uiWanted)
        {
            iModel = m;
            break;
        }
    }
    if (iModel < 0 || g_instanceCount >= kMaxInstances)
    {
        Log("models: no model with hash %08x for combat unit %d in %s", uiWanted, iSlot, path);
        return 0;
    }

    const int iInstance = g_instanceCount++;
    InstanceInfo& inst = g_instances[iInstance];
    inst.model = iModel;
    inst.unit = iSlot;
    PblMatrix world;
    world.Reset();
    PblToGL(inst.matrix, world);
    if (iSlot == 0) g_trooperInstance = iInstance;

    const ModelInfo& model = g_models[iModel];
    g_placedSegments += model.segmentCount;
    for (int s = 0; s < model.segmentCount; ++s)
    {
        const SegmentInfo& seg = g_segments[model.firstSegment + s];
        g_placedPrimTotal += seg.primCount;
        g_placedIndexTotal += seg.indexCount;
    }
    Log("models: unit %d uses %s with %d segments", iSlot, model.name, model.segmentCount);
    return model.segmentCount;
}

/* Appends the player model and every target's model, with one parse per faction. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_append_combat_models(const char* playerPath,
                                                                const char* targetPath)
{
#ifdef __EMSCRIPTEN__
    if (g_renderReady && !EnsureModelGL()) return -2;
    int iTotal = AppendUnitModel(playerPath, 0, true);
    const int iUnits = CombatHarnessUnitCount();
    for (int i = 1; i < iUnits; ++i)
    {
        iTotal += AppendUnitModel(targetPath, i, i == 1);
    }
    UploadModelBuffers();
    g_geometryOn = 1;
    return iTotal;
#else
    (void)playerPath;
    (void)targetPath;
    return -1;
#endif
}

/* The single-trooper form, kept for the original mission check and for other callers. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_append_trooper_model(const char* path)
{
#ifdef __EMSCRIPTEN__
    if (g_renderReady && !EnsureModelGL()) return -2;
    const int iSegments = AppendUnitModel(path, 0, true);
    UploadModelBuffers();
    g_geometryOn = 1;
    return iSegments;
#else
    (void)path;
    return -1;
#endif
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_selfcheck(const char* path, int limit)
{
#ifdef __EMSCRIPTEN__
    RunModelSelfCheck(path, limit > 0 ? limit : 200);
    static const char* kCategoryNames[4] = { "position", "normal", "texcoord", "diffuse" };
    snprintf(g_report, sizeof(g_report),
             "model self-check on %s: %d segment pairs compared. The host decoded the packed stream "
             "and the file's own uncompressed copy of the same vertices with the same decoder, then "
             "asked whether they agree to the resolution the format can carry (one quantisation step "
             "per category): ",
             path, g_selfcheckPairs);
    for (int c = 0; c < 4; ++c)
    {
        char frag[128];
        snprintf(frag, sizeof(frag), "%s %d ok / %d bad (worst %d step%s)", kCategoryNames[c],
                 g_selfcheckOk[c], g_selfcheckBad[c], (int)g_selfcheckWorst[c],
                 g_selfcheckWorst[c] == 1 ? "" : "s");
        strcat(g_report, frag);
        strcat(g_report, c < 3 ? "; " : "");
    }
    char tail[256];
    snprintf(tail, sizeof(tail), "; texcoord values outside the 12-bit range and therefore not "
             "checkable: %d. The position rule is the model box from modl's INFO, not the "
             "segment's own recorded box.", g_selfcheckUvOutOfRange);
    strcat(g_report, tail);
    Log("models: %s", g_report);
    int bad = 0;
    for (int c = 0; c < 4; ++c) bad += g_selfcheckBad[c];
    return bad;
#else
    (void)path; (void)limit;
    return -1;
#endif
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_set_geometry(int enable) { g_geometryOn = enable ? 1 : 0; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_geometry_on(void) { return g_geometryOn; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_segments(void) { return g_segmentCount; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_models(void) { return g_modelCount; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_instances(void) { return g_instanceCount; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_drawn(void) { return g_modelDrawn; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_culled(void) { return g_modelCulled; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_vertices(void) { return g_vertexTotal; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_indices(void) { return g_indexTotal; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_prims_drawn(void) { return g_modelPrims; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_indices_drawn(void) { return g_modelIndicesDrawn; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_odd_prim_types(void) { return g_oddPrimTypes; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_truncated(void) { return g_truncated; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_textures_resolved(void) { return g_resolvedCount; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_unresolved_instances(void) { return g_instancesUnresolved; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_trooper_drawn(void) { return g_trooperDrawn; }
// Segments reached through a placed instance: drawn + culled must equal this, which is the
// statement that the host's culling path sees exactly what it decoded rather than a subset.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_visited(void) { return g_modelVisited; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_placed_segments(void) { return g_placedSegments; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_placed_prim_total(void) { return g_placedPrimTotal; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_placed_index_total(void) { return g_placedIndexTotal; }
// Sum of every decoded segment's draw count (INFO primCount). Compared against the Python oracle's
// independent tally of the same number, so the C reader's INFO handling is being checked, not
// assumed.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_model_draw_declared(void)
{
    int total = 0;
    for (int i = 0; i < g_segmentCount; ++i) total += g_segments[i].primCount;
    return total;
}

static void DrawScene3D(int w, int h)
{
    const float aspect = (float)w / (float)(h > 0 ? h : 1);
    BuildCamera(aspect);

    float viewProj[16];
    BuildGLViewProj(viewProj);

    glUseProgram(g_program);
    glUniform1i(g_uUse3D, 1);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);  // billboards must be visible from both sides

    const PblMatrix& cam = SceneCamera()->GetMatrix();
    const float* right = cam.m[0];
    const float* up = cam.m[1];
    const float* back = cam.m[2];

    g_sceneDrawn = 0;
    g_sceneCulled = 0;

    // Real geometry when the host has read any; otherwise the resident textures stand in, so the
    // same camera path can be demonstrated before the model reader has been run.
    if (g_geometryOn && g_segmentCount)
    {
        DrawModelGeometry(viewProj);
        return;
    }

    if (g_cacheCount == 0)
    {
        // no textures staged yet: one tinted quad at a known depth, so an empty cache cannot be
        // mistaken for a broken transform
        glUniform1i(g_uHasTex, 0);
        glUniform4f(g_uTint, 0.2f, 0.55f, 0.9f, 1.f);
        float pos[3] = { 0.f, 0.f, 0.f };
        DrawBillboard(pos, 4.f, 4.f, viewProj, right, up, back);
        ++g_sceneDrawn;
    }
    else
    {
        glUniform1i(g_uHasTex, 1);
        for (int i = 0; i < g_cacheCount; ++i)
        {
            const CachedTexture& t = g_cache[i];
            float pos[3];
            ScenePlacement(i, pos);
            float halfW = 1.f, halfH = 1.f;
            SceneSize(t, halfW, halfH);

            // the engine's own frustum test, per object, per frame
            const float radius = (halfW > halfH) ? halfW : halfH;
            if (SceneCamera()->ClassifySphere(PblVector3(pos[0], pos[1], pos[2]), radius, 900.f)
                == RedCamera::VISIBILITY_NONE)
            {
                ++g_sceneCulled;
                continue;
            }

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t.glId);
            DrawBillboard(pos, halfW, halfH, viewProj, right, up, back);
            ++g_sceneDrawn;
        }
    }

    glUniform1i(g_uUse3D, 0);
    GLCHK("draw3d");
}

// Proof that the transform and the depth buffer are actually wired up, independent of what the
// textures look like: render two overlapping tinted quads into an offscreen target, at two
// depths, with `redInFront` deciding which colour is nearer. The centre pixel must be the nearer
// colour, and must swap when the depths swap. A wrong matrix convention (mirrored, inverted z,
// or no depth test) shows up here as the wrong colour, not as a plausible picture.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_depth_order_test(int redInFront)
{
    if (!g_renderReady)
    {
        return -1;
    }

    const int kSize = 64;
    GLuint fbo = 0, colorRb = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &colorRb);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kSize, kSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb);
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, kSize, kSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        Log("gl: depth-order test has no complete framebuffer");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &depthRb);
        glDeleteRenderbuffers(1, &colorRb);
        glDeleteFramebuffers(1, &fbo);
        GLCHK("depthTestFBO");
        return -2;
    }

    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glViewport(0, 0, kSize, kSize);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepthf(1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_program);
    glUniform1i(g_uUse3D, 1);
    glUniform1i(g_uHasTex, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);

    BuildCamera(1.f);
    float viewProj[16];
    BuildGLViewProj(viewProj);

    const PblMatrix& cam = SceneCamera()->GetMatrix();
    const float* right = cam.m[0];
    const float* up = cam.m[1];
    const float* back = cam.m[2];
    const float* eye = cam.m[3];

    const float dMid[2] = { 6.f, 16.f };
    for (int pass = 0; pass < 2; ++pass)
    {
        // pass 0 always draws the nearer quad, so the depth test is also decided by draw order:
        // whichever colour is nearer must survive and the farther one must be rejected by depth.
        // Forward is -Z, because the engine's camera Z axis points backwards from what it sees.
        const float dist = (pass == 0) ? dMid[0] : dMid[1];
        const int red = (pass == 0) ? redInFront : (1 - redInFront);
        if (red)
        {
            glUniform4f(g_uTint, 1.f, 0.f, 0.f, 1.f);
        }
        else
        {
            glUniform4f(g_uTint, 0.f, 1.f, 0.f, 1.f);
        }
        float pos[3] = { eye[0] - back[0] * dist, eye[1] - back[1] * dist, eye[2] - back[2] * dist };
        DrawBillboard(pos, 2.2f, 2.2f, viewProj, right, up, back);
    }
    GLCHK("depthTestDraw");

    GLubyte px[4] = { 0, 0, 0, 0 };
    glReadPixels(kSize / 2, kSize / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLCHK("depthTestRead");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &depthRb);
    glDeleteRenderbuffers(1, &colorRb);
    glDeleteFramebuffers(1, &fbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDisable(GL_DEPTH_TEST);
    glUniform1i(g_uUse3D, 0);

    return ((int)px[0] << 16) | ((int)px[1] << 8) | (int)px[2];
}

// Diagnose the offscreen target, because "the depth test returns black" has two very different
// causes and they need different fixes: the FBO may not be drawable at all, or the transform may
// be putting geometry where the viewport never sees it. Separates them by drawing one plain
// screen-space quad first (a control that cannot depend on the camera) and then one quad straight
// ahead, and reports the frustum the transform was built from.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_fbo_probe(void)
{
    if (!g_renderReady)
    {
        snprintf(g_report, sizeof(g_report), "no GL state: no container staged yet?");
        return -1;
    }

    const int kProbeSize = 64;
    GLuint fbo = 0, colorRb = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &colorRb);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kProbeSize, kProbeSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb);
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, kProbeSize, kProbeSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    const bool complete = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glViewport(0, 0, kProbeSize, kProbeSize);
    glUseProgram(g_program);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClearDepthf(1.f);

    // control: the shared unit triangles already fill clip space, so with uUse3D off this must
    // cover the whole target. If this reads black, nothing about the camera is at fault.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glUniform1i(g_uUse3D, 0);
    glUniform1i(g_uHasTex, 0);
    glUniform4f(g_uTint, 1.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    GLubyte px[4] = { 0, 0, 0, 0 };
    glReadPixels(kProbeSize / 2, kProbeSize / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const int control = ((int)px[0] << 16) | ((int)px[1] << 8) | (int)px[2];

    // one quad, dead ahead, six units out, big enough to cover the middle of the frame
    BuildCamera(1.f);
    // Captured immediately, before any other camera call, so a difference between this and the
    // matrix read at the end of the probe says whether SetMatrix stored wrong or something later
    // disturbed the camera's own storage.
    const PblMatrix camEarly = SceneCamera()->GetMatrix();
    float viewProj[16];
    BuildGLViewProj(viewProj);
    const PblMatrix& cam = SceneCamera()->GetMatrix();
    const float* eye = cam.m[3];
    const float* back = cam.m[2];
    float pos[3] = { eye[0] - back[0] * 6.f, eye[1] - back[1] * 6.f, eye[2] - back[2] * 6.f };
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glUniform1i(g_uUse3D, 1);
    glUniform4f(g_uTint, 0.f, 1.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    DrawBillboard(pos, 2.2f, 2.2f, viewProj, cam.m[0], cam.m[1], cam.m[2]);
    glFinish();
    glReadPixels(kProbeSize / 2, kProbeSize / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const int ahead = ((int)px[0] << 16) | ((int)px[1] << 8) | (int)px[2];

    // How much of the target did the quad cover, and where? A quad that is present but off to one
    // side needs a different fix from one that produced no fragments at all, and the eye cannot
    // tell those apart from a single centre sample.
    static GLubyte full[kProbeSize * kProbeSize * 4];
    glReadPixels(0, 0, kProbeSize, kProbeSize, GL_RGBA, GL_UNSIGNED_BYTE, full);
    int lit = 0, minX = kProbeSize, maxX = -1, minY = kProbeSize, maxY = -1;
    for (int y = 0; y < kProbeSize; ++y)
    {
        for (int x = 0; x < kProbeSize; ++x)
        {
            const int o = (y * kProbeSize + x) * 4;
            if (full[o] + full[o + 1] + full[o + 2] > 24)
            {
                ++lit;
                if (x < minX) { minX = x; }
                if (x > maxX) { maxX = x; }
                if (y < minY) { minY = y; }
                if (y > maxY) { maxY = y; }
            }
        }
    }

    float fNear, fFar, fWidth, fHeight;
    SceneCamera()->GetFrustum(fNear, fFar, fWidth, fHeight);
    const GLenum err = glGetError();

    // Full state dump, because partial readings kept producing plausible-looking but wrong
    // theories. Every number the transform is built from, in one line.
    char mat[512];
    char inv[512];
    char vp[512];
    snprintf(mat, sizeof(mat), "late %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f early %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f",
             cam.m[0][0], cam.m[0][1], cam.m[0][2], cam.m[0][3], cam.m[1][0], cam.m[1][1], cam.m[1][2], cam.m[1][3],
             cam.m[2][0], cam.m[2][1], cam.m[2][2], cam.m[2][3], cam.m[3][0], cam.m[3][1], cam.m[3][2], cam.m[3][3],
             camEarly.m[0][0], camEarly.m[0][1], camEarly.m[0][2], camEarly.m[0][3], camEarly.m[1][0], camEarly.m[1][1], camEarly.m[1][2], camEarly.m[1][3],
             camEarly.m[2][0], camEarly.m[2][1], camEarly.m[2][2], camEarly.m[2][3], camEarly.m[3][0], camEarly.m[3][1], camEarly.m[3][2], camEarly.m[3][3]);
    const PblMatrix& i = SceneCamera()->GetMatrixInverse();
    snprintf(inv, sizeof(inv), "%.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f",
             i.m[0][0], i.m[0][1], i.m[0][2], i.m[0][3], i.m[1][0], i.m[1][1], i.m[1][2], i.m[1][3],
             i.m[2][0], i.m[2][1], i.m[2][2], i.m[2][3], i.m[3][0], i.m[3][1], i.m[3][2], i.m[3][3]);
    snprintf(vp, sizeof(vp), "%.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f | %.2f,%.2f,%.2f,%.2f",
             viewProj[0], viewProj[1], viewProj[2], viewProj[3], viewProj[4], viewProj[5], viewProj[6], viewProj[7],
             viewProj[8], viewProj[9], viewProj[10], viewProj[11], viewProj[12], viewProj[13], viewProj[14], viewProj[15]);

    snprintf(g_report, sizeof(g_report),
             "fbo %s %dx%d | control #%06x | ahead #%06x lit %d/%d box x[%d..%d] y[%d..%d] | "
             "frustum n %.2f f %.0f w %.3f h %.3f | cam y=%.3f p=%.3f d=%.1f | sizes cam=%d mat=%d _Matrix@%d | cam %s | inv %s | "
             "glvp %s | locs use3d=%d mvp=%d | glError 0x%04x",
             complete ? "complete" : "INCOMPLETE", kProbeSize, kProbeSize, control, ahead,
             lit, kProbeSize * kProbeSize, minX, maxX, minY, maxY,
             fNear, fFar, fWidth, fHeight, g_camYaw, g_camPitch, g_camDist,
             (int)sizeof(RedCamera), (int)sizeof(PblMatrix),
             (int)((const char*)&SceneCamera()->GetMatrix() - (const char*)SceneCamera()), mat, inv, vp,
             (int)g_uUse3D, (int)g_uMVP, (unsigned)err);

    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &depthRb);
    glDeleteRenderbuffers(1, &colorRb);
    glDeleteFramebuffers(1, &fbo);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glUniform1i(g_uUse3D, 0);
    GLCHK("fboProbe");

    return (control ? 1 : 0) + (ahead ? 2 : 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_set_3d(int enable)
{
    g_mode3D = enable ? 1 : 0;
    if (g_mode3D)
    {
        g_montage = 0;  // montage is a screen-space mode; the two cannot mean anything at once
    }
    Log("gl: 3D scene %s (%d resident textures)", g_mode3D ? "on" : "off", g_cacheCount);
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_is_3d(void)
{
    return g_mode3D;
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_scene_orbit(float dYaw, float dPitch, float dDist)
{
    g_camYaw += dYaw;
    g_camPitch += dPitch;
    if (g_camPitch > 1.45f)
    {
        g_camPitch = 1.45f;
    }
    if (g_camPitch < -1.45f)
    {
        g_camPitch = -1.45f;
    }
    g_camDist += dDist;
    if (g_camDist < 6.f)
    {
        g_camDist = 6.f;
    }
    if (g_camDist > 120.f)
    {
        g_camDist = 120.f;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_scene_fov(float radians)
{
    if (radians >= 0.2f && radians <= 2.4f)
    {
        g_camFov = radians;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_scene_drawn(void)
{
    return g_sceneDrawn;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_scene_culled(void)
{
    return g_sceneCulled;
}
#endif

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_tex_upload(int w, int h, int fmt, int len)
{
#ifdef __EMSCRIPTEN__
    if (!g_renderReady || !g_texScratch || len <= 0)
    {
        return -1;
    }
    if (!g_texture)
    {
        glGenTextures(1, &g_texture);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    GLCHK("bind");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLCHK("texparams");
    // S3TC goes to the GPU as blocks; everything else arrives already converted to RGBA8 by
    // the extractor, so the upload layer only has two cases instead of one per engine format.
    const int blockSize = DxtBlockSize(fmt);
    if (blockSize)
    {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, (GLenum)fmt, w, h, 0, len, g_texScratch);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLenum)fmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     g_texScratch);
    }
    {
        // INVALID_VALUE means the payload length does not match what (format, width, height)
        // implies - so the diagnosis has to carry all four.
        const GLint e_ = glGetError();
        if (e_ != GL_NO_ERROR)
        {
            const int blocksW = (w + 3) / 4, blocksH = (h + 3) / 4;
            const int expect = blockSize ? blocksW * blocksH * blockSize : w * h * 4;
            Log("glerr@upload %dx%d fmt 0x%x len %d (format implies %d) scratch=%p: %d", w, h, fmt,
                len, expect, (void*)g_texScratch, (int)e_);
        }
    }

    GLint err = glGetError();
    g_texW = w;
    g_texH = h;
    g_texFmt = fmt;
    Log("gl: uploaded %s texture %dx%d fmt 0x%x len %d%s", blockSize ? "S3TC" : "RGBA8", w, h,
        fmt, len, err ? " (glErr after upload above)" : "");
    return err == 0 ? 0 : -2;
#else
    (void)w;
    (void)h;
    (void)fmt;
    (void)len;
    return -1;
#endif
}

// Read the framebuffer back in the same turn as the draw. After compositing, the default
// buffer is gone, so this is the only reliable way to assert that real game texels reached
// the screen instead of a clear colour.

static void SampleFrame(void)
{
#ifdef __EMSCRIPTEN__
    GLint w = 0, h = 0;
    emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &w, &h);
    if (w <= 0 || h <= 0)
    {
        return;
    }
    // Plain buffers on purpose: the engine archive is built without exceptions, so the host
    // stays inside that discipline rather than pulling throwing containers (and the exception
    // runtime they drag in) across the boundary.
    static unsigned char* px = 0;
    static size_t pxCap = 0;
    static unsigned char buckets[1 << 15];  // 6 bits per channel, 32 KiB of presence bits
    const size_t want = (size_t)w * h * 4;
    if (pxCap < want)
    {
        free(px);
        px = (unsigned char*)malloc(want);
        pxCap = px ? want : 0;
        if (!px)
        {
            g_gpuLuma = g_gpuLit = g_gpuDistinct = 0;
            return;
        }
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLCHK("readPixels");

    const size_t stride = 4 * 331;  // ~0.3% of the pixels is plenty
    unsigned long sum = 0;
    long lit = 0, n = 0;
    memset(buckets, 0, sizeof(buckets));
    for (size_t i = 0; i + 3 < want; i += stride)
    {
        const int luma = (px[i] * 3 + px[i + 1] * 4 + px[i + 2]) / 8;
        sum += luma;
        lit += (luma > 16);
        n++;
        const unsigned bucket =
            (px[i] & 0x3f) | ((px[i + 1] & 0x3f) << 2) | ((px[i + 2] & 0x3f) << 4);
        buckets[bucket >> 3] |= (unsigned char)(1u << (bucket & 7));
    }
    g_gpuLuma = n ? (int)(sum / n) : 0;
    g_gpuLit = n ? (int)(lit * 1000 / n) : 0;

    int distinct = 0;
    for (size_t b = 0; b < sizeof(buckets); ++b)
    {
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            distinct += (buckets[b] >> bit) & 1;
        }
    }
    g_gpuDistinct = distinct;

    const size_t center = ((size_t)(h / 2) * w + (w / 2)) * 4;
    g_gpuCenter[0] = px[center];
    g_gpuCenter[1] = px[center + 1];
    g_gpuCenter[2] = px[center + 2];
#endif
}

/* One logic tick. The host no longer decides how much time passed: it asks the engine, and the
   engine's answer is the quantised delta its own time base can represent. `dt` is kept only so the
   caller can keep the host's 30 Hz cadence; the fixed-rate branch does not read a clock. */
static void Step(float dt)
{
    (void)dt;
    EngineClockInit();

    GameState::UpdateTime();

    /* The engine's own numbers, not a host float: _TimeBase is the game's clock, sDeltaTime is what
       the game logic would be handed this frame. */
    g_simTime = (double)GameState::_TimeBase.GetTime();
    g_simDelta = sDeltaTime;
}

static void Draw(void)
{
#ifdef __EMSCRIPTEN__
    if (!g_renderReady)
    {
        return;
    }
    int w = 0, h = 0;
    emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &w, &h);
    glViewport(0, 0, w, h);
    if (w != pcWidth || h != pcHeight)
    {
        pcWidth = w;
        pcHeight = h;
    }
    glClearColor(0.02f, 0.03f, 0.05f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g_mode3D)
    {
        DrawScene3D(w, h);
        return;
    }

    // The screen-space modes below place a quad directly in clip space, so depth testing has no
    // meaning for them - and leaving it on would let a stale depth value hide the image.
    glDisable(GL_DEPTH_TEST);

    // The geometry is one unit quad made in InitGL; each image is placed by scaling/offsetting
    // it, so no per-frame buffer uploads and no vertex data touching the engine's data at all.
    glUniform4f(g_uTint, 0.85f, 0.78f, 0.2f, 1.f);

    if (g_montage && g_cacheCount > 0)
    {
        int cols = 1;
        while (cols * cols < g_cacheCount)
        {
            ++cols;
        }
        const int rows = (g_cacheCount + cols - 1) / cols;
        const float cellW = 2.f / (float)cols;
        const float cellH = 2.f / (float)rows;

        for (int i = 0; i < g_cacheCount; ++i)
        {
            const CachedTexture& t = g_cache[i];
            const int col = i % cols;
            const int row = i / cols;
            const float cx = -1.f + cellW * (col + 0.5f);
            const float cy = 1.f - cellH * (row + 0.5f);

            // aspect-fit inside the cell, with a small gutter so neighbours do not bleed
            const float avail = 0.88f;
            const float aspect = (float)t.width / (float)(t.height > 0 ? t.height : 1);
            float sx = 1.f, sy = 1.f;
            if (aspect >= 1.f)
            {
                sx = avail;
                sy = avail / aspect * (cellH / cellW);
            }
            else
            {
                sy = avail;
                sx = avail * aspect * (cellW / cellH);
            }

            glUniform2f(g_uScale, sx * cellW, sy * cellH);
            glUniform2f(g_uOffset, cx, cy);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t.glId);
            glUniform1i(g_uHasTex, 1);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glUniform2f(g_uScale, 1.f, 1.f);
        glUniform2f(g_uOffset, 0.f, 0.f);
    }
    else if (g_texW)
    {
        const float aspect = (float)g_texW / (float)(g_texH > 0 ? g_texH : 1);
        const float s = 0.92f;
        glUniform2f(g_uScale, aspect >= 1.f ? s : s * aspect, aspect >= 1.f ? s / aspect : s);
        glUniform2f(g_uOffset, 0.f, 0.f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glUniform1i(g_uHasTex, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glUniform2f(g_uScale, 1.f, 1.f);
    }
    else
    {
        // nothing decoded yet: show the engine's own placeholder rather than a clear colour,
        // so a blank canvas is never mistaken for a working render
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(g_uHasTex, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    GLCHK("draw");
#endif
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_init(int renderEnabled)
{
    Log("swbf2-browser host: engine file layer + LuaFL + WebGL");
#ifdef __EMSCRIPTEN__
    if (renderEnabled)
    {
        g_renderReady = InitGL();
    }
#else
    (void)renderEnabled;
#endif
    struct stat st;
    if (stat(g_dataRoot, &st) == 0)
    {
        Log("vfs: %s mounted", g_dataRoot);
    }
    else
    {
        Log("vfs: %s not mounted yet", g_dataRoot);
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_frame(float dt)
{
    if (dt > 0.25f || dt <= 0.f)
    {
        dt = 1.f / 60.f;
    }

    // The engine runs logic at 30 Hz; the renderer is allowed to run faster.
    static double accum = 0.0;
    accum += dt;
    while (accum >= (1.0 / 30.0))
    {
        Step(1.0f / 30.0f);
        /* The trooper's turn, on the same clock as the rest of the engine: the host injects its
           input once per animation frame (into the engine's own input record) and this tick
           consumes it, through the engine's own player controller and then the engine's own
           camera update. Before the host attaches the trooper there is nothing to drive, and the
           frame behaves exactly as it did. */
        if (CombatHarnessUnitCount() > 0)
        {
            HostInput input;
            LevelHarnessGetInput(&input);
            CombatHarnessLiveTurn(&input);
        }
        else if (LevelHarnessControlReady())
        {
            LevelHarnessControlUpdate(1.0f / 30.0f);
            LevelHarnessCameraUpdate(1.0f / 30.0f);
        }
        accum -= 1.0 / 30.0f;
    }

    Draw();
    SampleFrame();
    g_frames++;
}

extern "C" EMSCRIPTEN_KEEPALIVE double swbf2_sim_time(void)
{
    return g_simTime;
}

/* The delta the engine handed the game this frame, in seconds. Not 1/30: the engine's time base is
   an integer tick counter, so the delta is quantised (0.128 ms at the shim's 1 MHz). */
extern "C" EMSCRIPTEN_KEEPALIVE double swbf2_sim_delta(void)
{
    return (double)g_simDelta;
}

/* The engine's own tick counter. Two hosts with the same tick count are in the same simulated
   instant, whatever their wall clocks say. */
extern "C" EMSCRIPTEN_KEEPALIVE double swbf2_sim_ticks(void)
{
    return (double)GameState::_TimeBase._iValue;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_frames(void)
{
    return g_frames;
}

/* Load a level through the engine's own loader.
 *
 * LoadUtil::ReadDataFile is the function the game itself uses: it opens the container and hands
 * every chunk to the engine's own reader - coll to CollisionMesh::Read, prim to
 * CollisionBodyPrimitive::Read, tern to RedTerrain::Read, entc to EntityClass::Read, and so on
 * (LoadUtil.cpp:196-400). The browser does not reimplement any of that: the harness names a
 * level and the engine's readers fill the engine's own structures.
 *
 * The name is the level's path relative to the data directory, with or without the extension:
 * "tat/tat2" and "tat/tat2.lvl" are the same level. The engine builds the Windows-shaped name
 * itself; the file layer translates it to the VFS path the host staged (see
 * browser/platform/pblfile_browser.cpp).
 *
 * Returns 1 when the engine's loader accepted the level, 0 when it did not.
 */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_load_level(const char* name)
{
    if (!name || !name[0])
    {
        return 0;
    }

    /* The engine's own game setup, once, before anything is loaded. The harness is shared with
       the native oracle build (spike/level_harness.cpp), so the browser and the oracle run the
       same start-up against the same data. */
    if (!LevelHarnessInit(EngineLogCallback))
    {
        return 0;
    }

    Log("level: loading %s through LoadUtil::ReadDataFile", name);
    const bool bOk = LevelHarnessLoad(name);
    Log("level: %s %s (collision meshes: %d)", name, bOk ? "loaded" : "failed",
        LevelHarnessCollisionMeshCount());
    return bOk ? 1 : 0;
}

/* Drops a capsule at a point of the loaded level and reports where it comes to rest. The
   surface is found by the engine's own collision query and the terrain's own height, so the
   number this returns is the engine's, not the host's. The native oracle build runs the same
   function (spike/level_harness.cpp) and the two are compared by
   tools/ground_oracle_check.py. */
extern "C" EMSCRIPTEN_KEEPALIVE float swbf2_ground_probe(float x, float z)
{
    GroundProbeResult result;
    if (!LevelHarnessDrop(x, z, 0.35f, 0.85f, &result))
    {
        return NAN;
    }
    return result.restY;
}

/* The whole result of the drop, for the oracle's report.
 *
 * The native oracle prints the terrain height, the ground height the ray found, the height the
 * capsule came to rest at and the number of steps it took. The browser has to print the same
 * four numbers for the comparison to mean anything, so it asks for them in one call: the drop
 * is the same work each time, and running it four times would be four chances for the two
 * builds to differ for no reason.
 */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_ground_probe_result(float x, float z, float* pTerrain,
                                                                float* pGround, float* pRest,
                                                                int* pSteps)
{
    GroundProbeResult result;
    const bool bOk = LevelHarnessDrop(x, z, 0.35f, 0.85f, &result);
    if (pTerrain)
    {
        *pTerrain = result.terrainY;
    }
    if (pGround)
    {
        *pGround = result.groundY;
    }
    if (pRest)
    {
        *pRest = result.restY;
    }
    if (pSteps)
    {
        *pSteps = result.steps;
    }
    return bOk ? 1 : 0;
}

/* The engine's own terrain height at a point, for the oracle's report. */
extern "C" EMSCRIPTEN_KEEPALIVE float swbf2_terrain_height(float x, float z)
{
    if (!RedTerrain::_bEnable)
    {
        return NAN;
    }
    return RedTerrain::GetInterpolatedHeight(x, z);
}

/* How many collision meshes the engine's own collision system now holds. CollisionMesh::Read
   puts every mesh it parses into the engine's hash table (CollisionMesh.h:127), so this counts
   the hulls the engine loaded - not the host's own reading of the file. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_collision_meshes(void)
{
    return CollisionMesh::_HashTable.GetNumEntries();
}

/* Loads a class the way the mission script loads one.
 *
 * The name is "file;class", the form the engine's own Lua callback uses
 * (LuaCallbacks_Mission.cpp:581-617: ReadDataFile("SIDE\\cis.lvl", "cis_inf_rifleman", ...)
 * becomes LoadUtil::ReadDataFile with the class's name hashed). The harness reads the class's
 * own nested level out of that file - the loader matches the nested level's id against the
 * hash of the class's name (LoadUtil.cpp:288-300) - so the megabytes of the other classes in
 * the file are skipped rather than parsed.
 *
 * Returns 1 when the engine's loader accepted the class, 0 when it did not. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_load_class(const char* name)
{
    if (!name || !name[0])
    {
        return 0;
    }
    if (!LevelHarnessInit(EngineLogCallback))
    {
        return 0;
    }
    Log("class: loading %s", name);
    const bool bOk = LevelHarnessLoadClass(name);
    Log("class: %s %s", name, bOk ? "loaded" : "failed");
    return bOk ? 1 : 0;
}

/* Spawns a soldier of a class and reports what the engine made of it.
 *
 * The spawn is the engine's own: EntityClass::Find hashes the class's name and finds the class
 * the engine read, EntitySoldierClass::Build allocates the soldier from the engine's memory
 * pool, adds it to the collision manager and activates its physics
 * (EntitySoldier.cpp:5830-5851), and the three steps Character::Spawn takes after the build
 * follow (Character.cpp:1034-1039).
 *
 * The numbers are the engine's: where the entity ended up, how much health it has, how many
 * weapons its class carries and how many of them the entity built, and how many collision
 * bodies the class gave it. The native oracle build runs the same function
 * (spike/level_harness.cpp) and the two are compared by tools/spawn_oracle_check.py.
 *
 * Returns 1 when the entity was built, 0 when it was not. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_spawn_soldier(const char* className, float x, float y,
                                                         float z, int playerId, float* pX,
                                                         float* pY, float* pZ, float* pHealth,
                                                         float* pMaxHealth, int* pClassWeapons,
                                                         int* pBuiltWeapons, int* pBodies)
{
    SpawnResult spawn;
    const bool bOk = LevelHarnessSpawn(className, x, y, z, playerId, &spawn);
    if (pX)
    {
        *pX = spawn.x;
    }
    if (pY)
    {
        *pY = spawn.y;
    }
    if (pZ)
    {
        *pZ = spawn.z;
    }
    if (pHealth)
    {
        *pHealth = spawn.health;
    }
    if (pMaxHealth)
    {
        *pMaxHealth = spawn.maxHealth;
    }
    if (pClassWeapons)
    {
        *pClassWeapons = spawn.classWeaponCount;
    }
    if (pBuiltWeapons)
    {
        *pBuiltWeapons = spawn.builtWeaponCount;
    }
    if (pBodies)
    {
        *pBodies = spawn.collisionBodyCount;
    }
    return bOk ? 1 : 0;
}

/* Runs the engine's own update loop for a while: Thread::UpdateAll advances the turn threads,
   which is where the soldier's physics runs (GameLoop.cpp:1299 is the game's own call). */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_update(float delta)
{
    return LevelHarnessUpdate(delta);
}

/* The combat setup is the same engine-native path the combat oracle uses: the teams, the
 * player's real Character, and 3-5 target Characters made with Team::CreateCharacter and
 * Character::Spawn. It does not run the scripted fight or replace the host's input. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_setup(const char* playerClass,
                                                        const char* playerClassFile,
                                                        const char* targetClass,
                                                        const char* targetClassFile,
                                                        int targetCount)
{
    if (!playerClass || !playerClassFile || !targetClass || !targetClassFile) return 0;
    CombatConfig config;
    CombatHarnessDefaultConfig(&config);
    config.pcPlayerClass = playerClass;
    config.pcPlayerClassFile = playerClassFile;
    config.pcTargetClass = targetClass;
    config.pcTargetClassFile = targetClassFile;
    config.iTargetCount = targetCount;
    // The playable slice asks for simple targets, not an AI firefight: the targets are alive
    // and can be shot, but they do not fire back. The combat oracle keeps its own default of
    // true so the two-way exchange is still measured.
    config.bTargetsFire = false;
    if (playerClass[0] == 'i' && playerClass[1] == 'm' && playerClass[2] == 'p')
    {
        config.iPlayerSide = 2; /* Team::side_Empire */
    }
    else if (playerClass[0] == 'r' && playerClass[1] == 'e' && playerClass[2] == 'p')
    {
        config.iPlayerSide = 3; /* Team::side_Republic */
    }
    else if (playerClass[0] == 'c' && playerClass[1] == 'i' && playerClass[2] == 's')
    {
        config.iPlayerSide = 4; /* Team::side_CIS */
    }
    return CombatHarnessSetupLive(&config, EngineLogCallback) ? 1 : 0;
}

/* One live turn. The host's current input is used unchanged; the targets' fire policy comes
   from the same config the oracle uses. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_turn_live(void)
{
    HostInput input;
    LevelHarnessGetInput(&input);
    return CombatHarnessLiveTurn(&input) ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_active(void)
{
    return CombatHarnessUnitCount() > 0 ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_combat_report(void)
{
    return CombatHarnessLiveReport();
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_unit_count(void)
{
    return CombatHarnessUnitCount();
}

extern "C" EMSCRIPTEN_KEEPALIVE unsigned int swbf2_combat_unit_model_hash(int slot)
{
    return CombatHarnessUnitModelHash(slot);
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_unit_matrix(int slot, float* pOut, int count)
{
    return CombatHarnessUnitMatrix(slot, pOut, count) ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_unit_alive(int slot)
{
    return CombatHarnessUnitAlive(slot) ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE float swbf2_combat_unit_health(int slot)
{
    return CombatHarnessUnitHealth(slot);
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_unit_ammo(int slot, float* pCurClip, float* pNumClips)
{
    return CombatHarnessUnitAmmo(slot, pCurClip, pNumClips) ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_select_weapon(int slot, int channel, int index)
{
    return CombatHarnessSelectWeapon(slot, channel, index) ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_respawn_player(void)
{
    return CombatHarnessRespawnPlayer() ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_respawn_targets(void)
{
    return CombatHarnessRespawnTargets() ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_combat_set_ai_fire(int enable)
{
    CombatHarnessSetAIFire(enable ? true : false);
}

/* ------------------------------------------------------------------------ host input and camera
 *
 * The host's input goes into the engine's own input record - the ProcessedInputs the engine's
 * player controller reads (PlayerController.cpp:157, 247-500) - and the trooper moves because the
 * engine's own code moves it. The camera is the engine's: CameraManager's chase camera, placed
 * behind the trooper by the engine's own track matrix (EntitySoldier::SetupCameraTrackMatrix) and
 * read back here.
 */

/* Gives the engine the size of the picture the host draws into, with the engine's own setters.
   The chase camera's field of view is computed from those numbers (ChaseCamera.cpp:306-309), so
   without them the field of view is 0/0. Returns 1 when the engine took the size. */
extern float GetDefaultFOV();
extern float GetWidescreenFOVAdjustment(float fFOV);

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_fov_inputs(float* pOut, int iCount)
{
    if (!pOut || iCount < 4) return 0;
    float afFOV[2];
    if (!LevelHarnessGetTrooperFOVInputs(afFOV)) return 0;
    pOut[0] = afFOV[0];
    pOut[1] = afFOV[1];
    pOut[2] = GetDefaultFOV();
    pOut[3] = GetWidescreenFOVAdjustment(afFOV[0]);
    return 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_viewport_state(float* pOut, int iCount)
{
    if (!pOut || iCount < 7) return 0;
    pOut[0] = (float)pcWidth;
    pOut[1] = (float)pcHeight;
    pOut[2] = (float)FLRenderer::GetViewWidth(0);
    pOut[3] = (float)FLRenderer::GetViewHeight(0);
    pOut[4] = (float)RedInterfaceScreen::GetScreenWidth();
    pOut[5] = (float)RedInterfaceScreen::GetScreenHeight();
    pOut[6] = RedRenderer::GetScreenAspectRatio();
    return 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_set_viewport(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    /* The platform layer's display mode: the renderer seam's screen aspect ratio is computed from
       it (pcRedRenderer_browser.cpp:814-821), and the chase camera multiplies that into its own
       ratio. The host owns the canvas, so the host sets it. */
    pcWidth = width;
    pcHeight = height;
    const bool bOk = LevelHarnessSetViewport((unsigned int)width, (unsigned int)height);
    if (bOk && LevelHarnessControlReady())
    {
        // Recompute immediately: a camera that was attached before the canvas had a size may
        // already have stored 0/0 as its FOV. Do not wait for the next turn to repair it.
        LevelHarnessCameraUpdate(1.0f / 30.0f);
    }
    return bOk ? 1 : 0;
}

/* Puts the engine's chase camera on the spawned trooper and finds the player controller the
   engine made for it. Returns 1 when the trooper is under the engine's own player control. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_control_attach(void)
{
    return LevelHarnessControlAttach() && LevelHarnessControlReady() ? 1 : 0;
}

/* True when the trooper is under the engine's own player control. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_control_ready(void)
{
    return LevelHarnessControlReady() ? 1 : 0;
}

/* Injects one turn of the host's input into the engine's own input record for the local player.
   The axes are the engine's four ProcessedInputs axes, in the engine's units: move and strafe are
   -1..1, and turn and pitch are the engine's mouse-mode rates (the engine clamps them to +-16 in
   mouse mode, +-1 outside it - PlayerController.cpp:276-300). The buttons are the engine's own
   bits (see HostInputButton in spike/level_harness.h). The mouse mode is the engine's own field:
   2 is what the engine's ProcessedInputs constructor puts there. */
extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_set_input(float move, float strafe, float turn,
                                                     float pitch, unsigned int buttons,
                                                     unsigned int mouseMode)
{
    HostInput input;
    input.fMove = move;
    input.fStrafe = strafe;
    input.fTurn = turn;
    input.fPitch = pitch;
    input.uiButtons = buttons;
    input.uiMouseMode = mouseMode;
    LevelHarnessSetInput(&input);
}

/* The input the host last injected, so the page can show what it has sent. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_get_input(float* pMove, float* pStrafe, float* pTurn,
                                                     float* pPitch, unsigned int* pButtons,
                                                     unsigned int* pMouseMode)
{
    HostInput input;
    LevelHarnessGetInput(&input);
    if (pMove) *pMove = input.fMove;
    if (pStrafe) *pStrafe = input.fStrafe;
    if (pTurn) *pTurn = input.fTurn;
    if (pPitch) *pPitch = input.fPitch;
    if (pButtons) *pButtons = input.uiButtons;
    if (pMouseMode) *pMouseMode = input.uiMouseMode;
    return 1;
}

/* One engine turn with the input live: the trooper's own player controller and then the engine's
   turn threads. Returns the number of turns the harness has run. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_control_update(float delta)
{
    return LevelHarnessControlUpdate(delta);
}

/* The engine's own camera update - the call the game makes once per frame (GameLoop.cpp:1837). */
extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_camera_update(float delta)
{
    LevelHarnessCameraUpdate(delta);
}

/* The trooper and the engine's camera, as the engine reports them.
 *
 * The floats, in order: x, y, z, the direction the entity faces (its matrix's z axis), its
 * velocity, the four control values the engine scaled, its health, the camera's position, the
 * camera's own z axis, the camera's field of view and zoom, the class's roll factor and the
 * trooper's energy.
 *
 * The ints, in order: the turn count, the soldier's own state, whether the fire and the
 * special-move triggers are down, and the engine's own input record (its button bits and its
 * mouse-mode field).
 *
 * Returns 1 when there is a trooper to report on. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_trooper_state(float* pFloats, int iFloatCount,
                                                        int* pInts, int iIntCount)
{
    TrooperState state;
    if (!LevelHarnessGetTrooperState(&state))
    {
        return 0;
    }
    const float afFloats[] = {
        state.x, state.y, state.z,
        state.dirX, state.dirY, state.dirZ,
        state.velX, state.velY, state.velZ,
        state.controlMove, state.controlStrafe, state.controlTurn, state.controlPitch,
        state.fHealth,
        state.camX, state.camY, state.camZ,
        state.camDirX, state.camDirY, state.camDirZ,
        state.camFov, state.camZoom,
        state.fRollSpeedFactor, state.fEnergy,
    };
    const int aiInts[] = {
        state.updates, state.iState, state.iFireDown, state.iSpecialMoveDown,
        (int)state.uiHostButtons, (int)state.uiHostMouseMode,
    };
    if (pFloats)
    {
        for (int i = 0; i < iFloatCount && i < (int)(sizeof(afFloats) / sizeof(afFloats[0])); ++i)
        {
            pFloats[i] = afFloats[i];
        }
    }
    if (pInts)
    {
        for (int i = 0; i < iIntCount && i < (int)(sizeof(aiInts) / sizeof(aiInts[0])); ++i)
        {
            pInts[i] = aiInts[i];
        }
    }
    return 1;
}

/* The engine camera's own world matrix, sixteen floats in the engine's own row-major order. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_camera_matrix(float* pOut, int iCount)
{
    RedCamera* pCamera = LevelHarnessCamera();
    if (!pCamera || !pOut)
    {
        return 0;
    }
    const PblMatrix& m = pCamera->GetMatrix();
    for (int i = 0; i < iCount && i < 16; ++i)
    {
        pOut[i] = m.m[i / 4][i % 4];
    }
    return 1;
}

/* Draws the 3D scene with the engine's chase camera instead of the host's own orbit camera. The
   engine's camera is the engine's: its world matrix, its field of view and its frustum all come
   from CameraManager::Update. */
extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_use_engine_camera(int enable)
{
    g_engineCamera = enable ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_engine_camera_on(void)
{
    return g_engineCamera;
}

/* The number of turns the movement script would record, and the script itself: the same fixed
   input sequence the native oracle runs, so a host can drive the trooper through it and compare
   the two builds. The script lives in the shared harness, so the two builds run the same input by
   construction. */
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_movement_script_length(void)
{
    return LevelHarnessMovementScriptLength();
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_movement_script(float* pFloats, int iFloatCount,
                                                          int* pInts, int iIntCount)
{
    const int iCapacity = LevelHarnessMovementScriptLength();
    if (iCapacity <= 0)
    {
        return 0;
    }
    TrooperState* pStates = (TrooperState*)malloc(sizeof(TrooperState) * (size_t)iCapacity);
    if (!pStates)
    {
        return 0;
    }
    int iTurns = 0;
    const bool bRan = LevelHarnessMovementScript(pStates, iCapacity, &iTurns);
    if (bRan && pFloats && pInts)
    {
        for (int i = 0; i < iTurns && i < iCapacity; ++i)
        {
            const TrooperState& s = pStates[i];
            const float afFloats[] = {
                s.x, s.y, s.z,
                s.dirX, s.dirY, s.dirZ,
                s.velX, s.velY, s.velZ,
                s.controlMove, s.controlStrafe, s.controlTurn, s.controlPitch,
                s.fHealth,
                s.camX, s.camY, s.camZ,
                s.camDirX, s.camDirY, s.camDirZ,
                s.camFov, s.camZoom,
                s.fRollSpeedFactor, s.fEnergy,
            };
            const int aiInts[] = {
                s.updates, s.iState, s.iFireDown, s.iSpecialMoveDown,
                (int)s.uiHostButtons, (int)s.uiHostMouseMode,
            };
            const int iFloatBase = i * iFloatCount;
            const int iIntBase = i * iIntCount;
            for (int f = 0; f < iFloatCount && f < (int)(sizeof(afFloats) / sizeof(afFloats[0]));
                 ++f)
            {
                pFloats[iFloatBase + f] = afFloats[f];
            }
            for (int n = 0; n < iIntCount && n < (int)(sizeof(aiInts) / sizeof(aiInts[0])); ++n)
            {
                pInts[iIntBase + n] = aiInts[n];
            }
        }
    }
    free(pStates);
    return bRan ? iTurns : 0;
}


extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_lua_calls(void)
{
    return g_luaCalls;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_texture_size(int* w, int* h)
{
    *w = g_texW;
    *h = g_texH;
    return g_texW ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_gpu_luma(void) { return g_gpuLuma; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_gpu_lit(void) { return g_gpuLit; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_gpu_distinct(void) { return g_gpuDistinct; }
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_gpu_center_packed(void)
{
    return (g_gpuCenter[0] << 16) | (g_gpuCenter[1] << 8) | g_gpuCenter[2];
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_texture_packed(void)
{
    return (g_texW << 16) | (g_texH & 0xffff);
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_engine_build(void)
{
    // Built from the actual archive at link time (tools/build_browser.sh), so the page cannot
    // claim more engine coverage than this module really carries.
    static char buf[96];
    snprintf(buf, sizeof(buf), "%d of %d engine TUs, no undefined symbols at link", 
             SWBF2_ENGINE_OBJECTS, SWBF2_ENGINE_SCOPE);
    return buf;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_version(void)
{
    static char buf[192];
    snprintf(buf, sizeof(buf),
             "swbf2-browser host 0.1 (%d of %d engine TUs; LuaFL Lua 5.0.2; WebGL2 + S3TC)",
             SWBF2_ENGINE_OBJECTS, SWBF2_ENGINE_SCOPE);
    return buf;
}

#ifdef __EMSCRIPTEN__
static void FrameCB(void*)
{
    swbf2_frame(1.f / 60.f);
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_start_loop(void)
{
    emscripten_set_main_loop_arg(FrameCB, 0, 0, EM_TRUE);
    emscripten_resume_main_loop();
}
#endif

// The JS host owns startup: it mounts GameData, then calls swbf2_init(render) and
// drives swbf2_frame(dt) from requestAnimationFrame (or a timer when headless).
int main(void)
{
    Log("%s", swbf2_version());
    return 0;
}

// ---------------------------------------------------------------------------
// Resident-set surface: assembled here rather than next to the decoder because it needs
// both the decoder and the GL context, which are defined further up this file.
extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_montage(int enable)
{
#ifdef __EMSCRIPTEN__
    g_montage = enable ? 1 : 0;
#else
    (void)enable;
#endif
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_montage_count(void)
{
    return g_cacheCount;
}

// Decode a texture and keep it resident as its own GL texture, so a montage can be assembled.
// Kept separate from swbf2_show_texture on purpose: that one is the "what is on screen now"
// path the keyboard pages through, this one is the archive view.
extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_stage_texture(const char* path, int index)
{
#ifdef __EMSCRIPTEN__
    if (!g_renderReady || g_cacheCount >= kMaxCachedTextures)
    {
        return -1;
    }

    int w = 0, h = 0, fmt = 0;
    char name[128];
    name[0] = 0;
    const int bytes = swbf2_load_texture(path, index, &w, &h, &fmt, name, (int)sizeof(name));
    if (bytes <= 0)
    {
        return bytes;
    }

    // One upload rule: the same helper the model path uses.
    TextureData t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name);
    t.width = w;
    t.height = h;
    t.glInternal = fmt;
    t.bytes = bytes;
    t.pixels = g_texScratch;
    const GLuint id = UploadDecodedTexture(t);
    if (!id)
    {
        Log("stage: upload of '%s' failed", name);
        return -3;
    }

    CachedTexture& slot = g_cache[g_cacheCount];
    slot.glId = id;
    slot.width = w;
    slot.height = h;
    slot.glInternal = fmt;
    snprintf(slot.name, sizeof(slot.name), "%s", name);
    ++g_cacheCount;

    snprintf(g_report, sizeof(g_report), "staged '%s' %dx%d %s (%d/%d resident)", name, w, h,
             g_texFormatName, g_cacheCount, kMaxCachedTextures);
    return g_cacheCount - 1;
#else
    (void)path;
    (void)index;
    return -1;
#endif
}

extern "C" EMSCRIPTEN_KEEPALIVE void swbf2_clear_cache(void)
{
#ifdef __EMSCRIPTEN__
    for (int i = 0; i < g_cacheCount; ++i)
    {
        if (g_cache[i].glId)
        {
            glDeleteTextures(1, &g_cache[i].glId);
        }
        g_cache[i] = CachedTexture();
    }
    g_cacheCount = 0;
    g_montage = 0;
    // The model path draws against textures it resolved from this set, so clearing the set drops
    // the geometry with it rather than leaving buffers pointing at deleted texture objects.
    ClearModelData();
#endif
}
