/* Browser Bink1 (BIKf/BIKg/BIKh/BIKi) video layer backed by libbinkdec.
 *
 * SWBF2's RedMoviePlayer (RedEngineFL/movie/pc/pcRedMoviePlayer.cpp) parses retail .mvs
 * containers (ucfb/info/Data + SegmentInfo), streams raw BIKi bytes from OPFS via PblFile
 * into its ring buffer, and calls the RAD Bink SDK C API using BinkSetIO(BinkFileOpen).
 *
 * This module implements the Bink API on top of libbinkdec (LGPL-2.1, in browser/platform/binkdec):
 *   - All decoder allocations run under RedSetCurrentHeap(-2) (system malloc/free) so they
 *     stay isolated from GameMemory::RunTimeHeap across ShellState <-> MissionState transitions.
 *   - Each video frame packet is read in a single BinkFileReadFrame call into a reusable system
 *     buffer, keeping streamPos aligned with the frame offset table and eliminating per-byte
 *     callback overhead in BitReader.
 *   - YUV420p planes (plus optional alpha) are converted with BT.601 integer math directly into
 *     the D3D9 D3DFMT_X8R8G8B8 / BGRA32 texture surface locked by RedMoviePlayer::Update().
 */

#include "RedEngineFL/movie/pc/pcRedMoviePlayer.h"
#include "RedEngineFL/Memory/RedMemory.h"
#include "binkdec/interface/binkdec_interface.h"

#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern "C"
{
    typedef long(__stdcall* BINKIOOPEN)(BINKIO* io, const char* name, unsigned long flags);
    typedef void*(__stdcall* BINKMEMALLOC)(unsigned int bytes);
    typedef void(__stdcall* BINKMEMFREE)(void* ptr);
    typedef void*(__stdcall* BINKSNDSYSOPEN)(unsigned long param);

    int __stdcall BinkSetIOSize(unsigned int iosize);
    BINKIOOPEN __stdcall BinkSetIO(BINKIOOPEN io);
    void __stdcall BinkSetMemory(BINKMEMALLOC a, BINKMEMFREE f);
    void* __stdcall BinkOpenDirectSound(unsigned long param);
    int __stdcall BinkSetSoundSystem(BINKSNDSYSOPEN open, unsigned long param);
    BINK* __stdcall BinkOpen(const char* name, unsigned int flags);
    void __stdcall BinkClose(BINK* bink);
    char* __stdcall BinkGetError();
    int __stdcall BinkWait(BINK* bink);
    int __stdcall BinkDoFrame(BINK* bink);
    void __stdcall BinkNextFrame(BINK* bink);
    int __stdcall BinkCopyToBuffer(BINK* bink, void* dest, int destpitch, unsigned int destheight,
                                   unsigned int destx, unsigned int desty, unsigned int flags);
    int __stdcall BinkDX9SurfaceType(IDirect3DSurface9* surface);
    void __stdcall BinkSetVolume(BINK* bink, unsigned int trackid, int volume);
}

extern void Log(const char* fmt, ...);

namespace
{

struct BinkSysHeapGuard
{
    int prevHeap;
    BinkSysHeapGuard() : prevHeap(RedSetCurrentHeap(-2)) {}
    ~BinkSysHeapGuard() { RedSetCurrentHeap(prevHeap); }
};

BINKIOOPEN g_ioOpen = nullptr;
BINKMEMALLOC g_memAlloc = nullptr;
BINKMEMFREE g_memFree = nullptr;
char g_lastError[256] = "No error";

double nowMs()
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return 0.0;
#endif
}

void binkLogCallback(const char* msg)
{
    if (msg)
    {
        strncpy(g_lastError, msg, sizeof(g_lastError) - 1);
        g_lastError[sizeof(g_lastError) - 1] = '\0';
        Log("binkdec: %s", msg);
    }
}

struct BrowserBink
{
    // MUST remain at offset 0 so (BINK*)this matches pcRedMoviePlayer.h layout.
    BINK header;

    BINKIO bio;
    bdec_decoder_t* dec;
    bdec_video_plane_t planes[4];
    bool transparent;
    bool hasDecodedFrame;
    bool ioError;
    bool inHeader;

    // Sequential stream tracking on top of RedMoviePlayer's BINKIO ring buffer.
    uint32_t streamPos;
    uint32_t pos;

    // Single-frame packet buffer filled once per BinkDoFrame via bio.ReadFrame.
    uint8_t* frameBuf;
    uint32_t frameBufCap;
    uint32_t bufBase;
    uint32_t bufLen;

    double lastFrameMs;
};

static constexpr uint32_t kBioReadChunk = 16384u;

static uint32_t readFromBio(BrowserBink* bb, void* dst, uint32_t size)
{
    if (!bb || size == 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint32_t total = 0;

    while (total < size)
    {
        uint32_t remain = size - total;
        uint32_t req = remain > kBioReadChunk ? kBioReadChunk : remain;
        unsigned long got = 0;
        if (bb->inHeader && bb->bio.ReadHeader)
        {
            got = bb->bio.ReadHeader(&bb->bio, (long)(bb->streamPos + total), out + total, req);
        }
        if (got == 0 && bb->bio.ReadFrame)
        {
            got = bb->bio.ReadFrame(&bb->bio, 0, (long)(bb->streamPos + total), out + total, req);
        }
        if (got == 0)
        {
            bb->ioError = true;
            break;
        }
        total += (uint32_t)got;
    }
    bb->streamPos += total;
    return total;
}

static bool skipFromBio(BrowserBink* bb, uint32_t bytes)
{
    uint8_t scratch[4096];
    uint32_t left = bytes;
    while (left > 0)
    {
        uint32_t chunk = left > sizeof(scratch) ? sizeof(scratch) : left;
        uint32_t got = readFromBio(bb, scratch, chunk);
        if (got < chunk) return false;
        left -= got;
    }
    return true;
}

static bool bdecIoError(void* usr)
{
    BrowserBink* bb = static_cast<BrowserBink*>(usr);
    return !bb || bb->ioError;
}

static void bdecIoSeek(int32_t offset, uint8_t seekDir, void* usr)
{
    BrowserBink* bb = static_cast<BrowserBink*>(usr);
    if (!bb) return;

    uint32_t target = bb->pos;
    if (seekDir == 0 /* kSeekStart */)
    {
        target = (offset >= 0) ? (uint32_t)offset : 0u;
    }
    else if (seekDir == 1 /* kSeekCurrent */)
    {
        int64_t next = (int64_t)bb->pos + (int64_t)offset;
        target = (next >= 0) ? (uint32_t)next : 0u;
    }

    // Fast path: seek inside the prefetched frame buffer.
    if (bb->bufLen > 0 && target >= bb->bufBase && target <= bb->bufBase + bb->bufLen)
    {
        bb->pos = target;
        return;
    }

    // Sequential forward skip on the underlying BINKIO stream (e.g. Open() advancing to frame 0).
    if (target > bb->streamPos)
    {
        if (!skipFromBio(bb, target - bb->streamPos))
        {
            bb->ioError = true;
            return;
        }
        bb->pos = bb->streamPos;
    }
    else if (target == bb->streamPos)
    {
        bb->pos = target;
    }
    else
    {
        // Backward seek outside buffered frame is not supported by sequential BINKIO.
        bb->ioError = true;
    }
}

static uint32_t bdecIoRead(void* dest, uint32_t destSize, void* usr)
{
    BrowserBink* bb = static_cast<BrowserBink*>(usr);
    if (!bb || !dest || destSize == 0) return 0;

    // Fast path: serve directly from prefetched frame packet buffer.
    if (bb->bufLen > 0 && bb->pos >= bb->bufBase && bb->pos < bb->bufBase + bb->bufLen)
    {
        uint32_t avail = (bb->bufBase + bb->bufLen) - bb->pos;
        uint32_t toCopy = destSize <= avail ? destSize : avail;
        memcpy(dest, bb->frameBuf + (bb->pos - bb->bufBase), toCopy);
        bb->pos += toCopy;
        if (toCopy < destSize)
        {
            memset(static_cast<uint8_t*>(dest) + toCopy, 0, destSize - toCopy);
        }
        return toCopy;
    }

    if (bb->pos > bb->streamPos)
    {
        if (!skipFromBio(bb, bb->pos - bb->streamPos))
        {
            memset(dest, 0, destSize);
            return 0;
        }
    }

    uint32_t got = readFromBio(bb, dest, destSize);
    bb->pos = bb->streamPos;
    if (got < destSize)
    {
        memset(static_cast<uint8_t*>(dest) + got, 0, destSize - got);
    }
    return got;
}

static inline uint8_t clampU8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

}  // namespace

extern "C" int __stdcall BinkSetIOSize(unsigned int iosize)
{
    (void)iosize;
    return 0;
}

extern "C" BINKIOOPEN __stdcall BinkSetIO(BINKIOOPEN io)
{
    BINKIOOPEN previous = g_ioOpen;
    g_ioOpen = io;
    return previous;
}

extern "C" void __stdcall BinkSetMemory(BINKMEMALLOC a, BINKMEMFREE f)
{
    g_memAlloc = a;
    g_memFree = f;
}

extern "C" void* __stdcall BinkOpenDirectSound(unsigned long param)
{
    (void)param;
    return nullptr;
}

extern "C" int __stdcall BinkSetSoundSystem(BINKSNDSYSOPEN open, unsigned long param)
{
    (void)open;
    (void)param;
    return 1;
}

extern "C" BINK* __stdcall BinkOpen(const char* name, unsigned int flags)
{
    if (!g_ioOpen)
    {
        strncpy(g_lastError, "BinkOpen called before BinkSetIO", sizeof(g_lastError) - 1);
        return nullptr;
    }

    BinkSysHeapGuard sysHeap;
    bdec_set_memory_funcs(malloc, free);
    bdec_set_log_func(binkLogCallback);

    BrowserBink* bb = static_cast<BrowserBink*>(calloc(1, sizeof(BrowserBink)));
    if (!bb)
    {
        strncpy(g_lastError, "Out of memory allocating BrowserBink", sizeof(g_lastError) - 1);
        return nullptr;
    }

    if (!g_ioOpen(&bb->bio, name ? name : "", flags))
    {
        strncpy(g_lastError, "BinkFileOpen failed", sizeof(g_lastError) - 1);
        free(bb);
        return nullptr;
    }
    bb->bio.bink = &bb->header;
    bb->inHeader = true;

    bb->dec = bdec_decoder_new();
    if (!bb->dec)
    {
        if (bb->bio.Close) bb->bio.Close(&bb->bio);
        free(bb);
        return nullptr;
    }

    bdec_file_io_t io{};
    io.error = bdecIoError;
    io.seek = bdecIoSeek;
    io.read = bdecIoRead;

    if (!bdec_open_file(bb->dec, io, bb) || bb->ioError)
    {
        bdec_decoder_free(bb->dec);
        if (bb->bio.Close) bb->bio.Close(&bb->bio);
        free(bb);
        return nullptr;
    }

    bb->inHeader = false;

    uint32_t w = 0, h = 0;
    bool transparent = false;
    bdec_get_info(bb->dec, &w, &h, &transparent);
    uint32_t fpsNum = 30, fpsDen = 1;
    bdec_get_fps(bb->dec, &fpsNum, &fpsDen);
    if (fpsNum == 0) fpsNum = 30;
    if (fpsDen == 0) fpsDen = 1;

    bb->transparent = transparent;
    bb->header.Width = w;
    bb->header.Height = h;
    bb->header.Frames = bdec_get_frame_count(bb->dec);
    bb->header.FrameNum = 1;
    bb->header.LastFrameNum = 0;
    bb->header.FrameRate = fpsNum;
    bb->header.FrameRateDiv = fpsDen;
    bb->header.NumTracks = (long)bdec_get_audio_track_count(bb->dec);
    bb->lastFrameMs = 0.0;

    Log("bink: opened BIK stream %lux%lu, %lu frames @ %lu/%lu fps, audioTracks=%ld",
        bb->header.Width, bb->header.Height, bb->header.Frames,
        bb->header.FrameRate, bb->header.FrameRateDiv, bb->header.NumTracks);

    return &bb->header;
}

extern "C" void __stdcall BinkClose(BINK* bink)
{
    if (!bink) return;
    BinkSysHeapGuard sysHeap;
    BrowserBink* bb = reinterpret_cast<BrowserBink*>(bink);
    if (bb->dec)
    {
        bdec_decoder_free(bb->dec);
        bb->dec = nullptr;
    }
    if (bb->bio.Close)
    {
        bb->bio.Close(&bb->bio);
    }
    if (bb->frameBuf)
    {
        free(bb->frameBuf);
        bb->frameBuf = nullptr;
    }
    free(bb);
}

extern "C" char* __stdcall BinkGetError()
{
    return g_lastError;
}

extern "C" int __stdcall BinkWait(BINK* bink)
{
    if (!bink) return 0;
    BrowserBink* bb = reinterpret_cast<BrowserBink*>(bink);
    if (!bb->dec) return 0;

    // Decode the first frame immediately after open so the texture populates right away.
    if (!bb->hasDecodedFrame) return 0;

    double frameMs = 1000.0 * (double)(bb->header.FrameRateDiv ? bb->header.FrameRateDiv : 1) /
                     (double)(bb->header.FrameRate ? bb->header.FrameRate : 30);
    double now = nowMs();
    if (now > 0.0 && bb->lastFrameMs > 0.0 && (now - bb->lastFrameMs) < frameMs)
    {
        return 1;  // Wait until next frame interval.
    }
    return 0;  // Ready to decode next frame.
}

extern "C" int __stdcall BinkDoFrame(BINK* bink)
{
    if (!bink) return 1;
    BrowserBink* bb = reinterpret_cast<BrowserBink*>(bink);
    if (!bb->dec || bb->header.Frames == 0) return 1;

    uint32_t frameIdx = (bb->header.FrameNum > 0) ? (uint32_t)(bb->header.FrameNum - 1) : 0u;
    uint32_t frameOff = 0, frameSize = 0;
    if (!bdec_get_frame_info(bb->dec, frameIdx, &frameOff, &frameSize) ||
        frameSize == 0 || frameSize > 16u * 1024u * 1024u)
    {
        return 1;
    }

    BinkSysHeapGuard sysHeap;

    if (frameOff < bb->streamPos)
    {
        Log("bink: stream position mismatch on frame %lu (frameOff=%lu < streamPos=%lu); skipping frame",
            (unsigned long)frameIdx, (unsigned long)frameOff, (unsigned long)bb->streamPos);
        return 1;
    }

    if (frameOff > bb->streamPos)
    {
        if (!skipFromBio(bb, frameOff - bb->streamPos))
        {
            return 1;
        }
    }

    if (frameSize > bb->frameBufCap)
    {
        uint8_t* grown = static_cast<uint8_t*>(realloc(bb->frameBuf, frameSize));
        if (!grown) return 1;
        bb->frameBuf = grown;
        bb->frameBufCap = frameSize;
    }

    // Read the frame packet in <= 16 KiB chunks so RedMoviePlayer::BinkFileReadFrame never
    // deadlocks waiting for a single contiguous chunk larger than mBuffer's free space.
    uint32_t got = 0;
    if (bb->bio.ReadFrame)
    {
        while (got < frameSize)
        {
            uint32_t remain = frameSize - got;
            uint32_t req = remain > kBioReadChunk ? kBioReadChunk : remain;
            unsigned long n = bb->bio.ReadFrame(&bb->bio, frameIdx, (long)(frameOff + got),
                                                bb->frameBuf + got, req);
            if (n == 0) break;
            got += (uint32_t)n;
        }
    }
    if (got < frameSize)
    {
        // End of segment or discontinuity flagged by RedMoviePlayer::BinkFileReadFrame.
        return 1;
    }

    bb->streamPos += got;
    bb->bufBase = frameOff;
    bb->bufLen = (uint32_t)got;
    bb->pos = frameOff;
    bb->ioError = false;

    bdec_goto_frame(bb->dec, frameIdx);
    bdec_get_next_frame(bb->dec, bb->planes);

    bb->header.LastFrameNum = bb->header.FrameNum;
    bb->hasDecodedFrame = true;
    bb->lastFrameMs = nowMs();
    return 0;  // 0 = frame decoded successfully.
}

extern "C" void __stdcall BinkNextFrame(BINK* bink)
{
    if (!bink) return;
    if (bink->FrameNum < bink->Frames)
    {
        bink->FrameNum++;
    }
}

extern "C" int __stdcall BinkCopyToBuffer(BINK* bink, void* dest, int destpitch,
                                          unsigned int destheight, unsigned int destx,
                                          unsigned int desty, unsigned int flags)
{
    (void)flags;
    if (!bink || !dest || destpitch <= 0) return 1;
    BrowserBink* bb = reinterpret_cast<BrowserBink*>(bink);
    if (!bb->hasDecodedFrame || !bb->planes[0].data || !bb->planes[1].data || !bb->planes[2].data)
    {
        return 1;
    }

    const uint32_t w = (uint32_t)bb->header.Width;
    const uint32_t h = (uint32_t)bb->header.Height;
    const uint32_t copyH = (desty < destheight)
                               ? ((h < destheight - desty) ? h : (destheight - desty))
                               : 0u;
    const uint32_t maxCols = (destpitch > 0) ? ((uint32_t)destpitch / 4u) : 0u;
    const uint32_t copyW = (destx < maxCols) ? ((w < maxCols - destx) ? w : (maxCols - destx)) : 0u;

    const uint8_t* yPlane = bb->planes[0].data;
    const uint8_t* uPlane = bb->planes[1].data;
    const uint8_t* vPlane = bb->planes[2].data;
    const uint8_t* aPlane = (bb->transparent && bb->planes[3].data) ? bb->planes[3].data : nullptr;

    const uint32_t yPitch = bb->planes[0].pitch;
    const uint32_t uPitch = bb->planes[1].pitch;
    const uint32_t vPitch = bb->planes[2].pitch;
    const uint32_t aPitch = aPlane ? bb->planes[3].pitch : 0u;

    uint8_t* dstBase = static_cast<uint8_t*>(dest);

    for (uint32_t y = 0; y < copyH; ++y)
    {
        const uint8_t* yRow = yPlane + y * yPitch;
        const uint8_t* uRow = uPlane + (y >> 1) * uPitch;
        const uint8_t* vRow = vPlane + (y >> 1) * vPitch;
        const uint8_t* aRow = aPlane ? (aPlane + y * aPitch) : nullptr;
        uint8_t* d = dstBase + (desty + y) * (uint32_t)destpitch + destx * 4u;

        for (uint32_t x = 0; x < copyW; ++x)
        {
            const uint32_t cx = x >> 1;
            const int c = (int)yRow[x] - 16;
            const int dd = (int)uRow[cx] - 128;
            const int ee = (int)vRow[cx] - 128;

            const int r = (298 * c + 409 * ee + 128) >> 8;
            const int g = (298 * c - 100 * dd - 208 * ee + 128) >> 8;
            const int b = (298 * c + 516 * dd + 128) >> 8;

            // D3DFMT_X8R8G8B8 / A8R8G8B8 in little-endian memory is [B, G, R, A].
            d[0] = clampU8(b);
            d[1] = clampU8(g);
            d[2] = clampU8(r);
            d[3] = aRow ? aRow[x] : 255u;
            d += 4;
        }
    }

    return 0;  // 0 = copied cleanly.
}

extern "C" int __stdcall BinkDX9SurfaceType(IDirect3DSurface9* surface)
{
    (void)surface;
    return 3;  // BINKSURFACE32 (32-bit BGRA / X8R8G8B8).
}

extern "C" void __stdcall BinkSetVolume(BINK* bink, unsigned int trackid, int volume)
{
    (void)bink;
    (void)trackid;
    (void)volume;
}
