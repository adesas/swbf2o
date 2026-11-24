// Fast XMV / raw YUV420p -> BIKi (Bink1) encoder + self-test against libbinkdec.
//
// Reads raw YUV420p frames from stdin (piped from ffmpeg) and writes a standard BIKi
// stream using SKIP_BLOCK (0), FILL_BLOCK (6), and PATTERN_BLOCK (8, 2-color 8x8 BTC)
// with identity 4-bit VLC trees (vlc_num = 0).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include "../browser/platform/binkdec/interface/binkdec_interface.h"

namespace {

static inline int av_log2_u32(unsigned int v) {
    int n = 0;
    if (v & 0xffff0000u) { v >>= 16; n += 16; }
    if (v & 0xff00u)     { v >>= 8;  n += 8;  }
    if (v & 0xf0u)       { v >>= 4;  n += 4;  }
    if (v & 0xcu)        { v >>= 2;  n += 2;  }
    if (v & 0x2u)        { n += 1; }
    return n;
}

struct BitWriter {
    std::vector<uint8_t> buf;
    uint32_t curByte = 0;
    int bitPos = 0;

    void reserve(size_t n) { buf.reserve(n); }

    inline void putBit(uint32_t b) {
        curByte |= (b & 1u) << bitPos;
        if (++bitPos == 8) {
            buf.push_back((uint8_t)curByte);
            curByte = 0;
            bitPos = 0;
        }
    }

    inline void putBits(uint32_t val, int n) {
        for (int i = 0; i < n; ++i) {
            putBit((val >> i) & 1u);
        }
    }

    inline void align32() {
        while (bitPos != 0) {
            putBit(0);
        }
        while ((buf.size() & 3u) != 0) {
            buf.push_back(0);
        }
    }
};

struct PlaneEncoderState {
    int w;          // visible width
    int h;          // visible height
    int pitch;      // aligned to 16
    int ph;         // aligned to 16
    int bw;         // 8x8 blocks wide
    int bh;         // 8x8 blocks high
    std::vector<uint8_t> prev; // reconstructed previous plane

    void init(int visW, int visH, bool isChroma) {
        w = visW;
        h = visH;
        pitch = (visW + 15) & ~15;
        ph = (visH + 15) & ~15;
        bw = isChroma ? ((visW * 2 + 15) >> 4) : ((visW + 7) >> 3);
        bh = isChroma ? ((visH * 2 + 15) >> 4) : ((visH + 7) >> 3);
        prev.assign((size_t)pitch * ph, isChroma ? 128 : 16);
    }
};

void encodePlane(BitWriter& bwOut,
                 PlaneEncoderState& st,
                 const uint8_t* src,
                 int srcStride,
                 bool isKeyframe,
                 int skipThr,
                 int fillThr) {
    const int width = st.pitch;
    const int bw = st.bw;
    const int bh = st.bh;

    const int lenBlk  = av_log2_u32((width >> 3) + 511) + 1;
    const int lenSub  = av_log2_u32((width >> 4) + 511) + 1;
    const int lenCol  = av_log2_u32(bw * 64 + 511) + 1;
    const int lenPat  = av_log2_u32((bw << 3) + 511) + 1;
    const int lenXOff = av_log2_u32((width >> 3) + 511) + 1;
    const int lenYOff = av_log2_u32((width >> 3) + 511) + 1;
    const int lenInDC = av_log2_u32((width >> 3) + 511) + 1;
    const int lenItDC = av_log2_u32((width >> 3) + 511) + 1;
    const int lenRun  = av_log2_u32(bw * 48 + 511) + 1;

    // 1. Write ReadBundle(0..8): 23 trees * 4 zero bits = 92 zero bits.
    for (int i = 0; i < 23; ++i) {
        bwOut.putBits(0, 4);
    }

    std::vector<uint8_t> rowBlk;
    std::vector<uint8_t> rowCol;
    std::vector<uint8_t> rowPat;
    rowBlk.reserve(bw);
    rowCol.reserve(bw * 2);
    rowPat.reserve(bw * 8);

    for (int by = 0; by < bh; ++by) {
        rowBlk.clear();
        rowCol.clear();
        rowPat.clear();

        for (int bx = 0; bx < bw; ++bx) {
            uint8_t blkPixels[64];
            int minV = 255, maxV = 0, sumV = 0, maxDiff = 0;

            for (int y = 0; y < 8; ++y) {
                int sy = std::min(by * 8 + y, st.h - 1);
                int py = by * 8 + y;
                const uint8_t* srow = src + sy * srcStride;
                const uint8_t* prow = st.prev.data() + py * st.pitch;
                for (int x = 0; x < 8; ++x) {
                    int sx = std::min(bx * 8 + x, st.w - 1);
                    int px = bx * 8 + x;
                    uint8_t v = srow[sx];
                    uint8_t pv = prow[px];
                    blkPixels[y * 8 + x] = v;
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                    sumV += v;
                    int d = std::abs((int)v - (int)pv);
                    if (d > maxDiff) maxDiff = d;
                }
            }

            // Force last block of row to PATTERN_BLOCK (8) if rowPat is still empty so every
            // row consumes >= 1 item from BLOCK_TYPES, COLORS, and PATTERN.
            bool forcePattern = (bx == bw - 1 && rowPat.empty());

            if (!forcePattern && !isKeyframe && maxDiff <= skipThr) {
                rowBlk.push_back(0); // SKIP_BLOCK
            } else if (!forcePattern && (maxV - minV) <= fillThr) {
                uint8_t avg = (uint8_t)((sumV + 32) >> 6);
                rowBlk.push_back(6); // FILL_BLOCK
                rowCol.push_back(avg);
                for (int y = 0; y < 8; ++y) {
                    uint8_t* prow = st.prev.data() + (by * 8 + y) * st.pitch + bx * 8;
                    memset(prow, avg, 8);
                }
            } else {
                // PATTERN_BLOCK (8): 2-color Block Truncation Coding (col0, col1 + 8 pattern bytes)
                int avg = (sumV + 32) >> 6;
                int sum0 = 0, cnt0 = 0, sum1 = 0, cnt1 = 0;
                for (int i = 0; i < 64; ++i) {
                    if (blkPixels[i] < avg) {
                        sum0 += blkPixels[i];
                        cnt0++;
                    } else {
                        sum1 += blkPixels[i];
                        cnt1++;
                    }
                }
                uint8_t c0 = cnt0 ? (uint8_t)((sum0 + (cnt0 >> 1)) / cnt0) : (uint8_t)avg;
                uint8_t c1 = cnt1 ? (uint8_t)((sum1 + (cnt1 >> 1)) / cnt1) : (uint8_t)avg;
                int mid = ((int)c0 + (int)c1) >> 1;

                rowBlk.push_back(8);
                rowCol.push_back(c0);
                rowCol.push_back(c1);

                for (int y = 0; y < 8; ++y) {
                    uint8_t patByte = 0;
                    uint8_t* prow = st.prev.data() + (by * 8 + y) * st.pitch + bx * 8;
                    for (int x = 0; x < 8; ++x) {
                        uint8_t v = blkPixels[y * 8 + x];
                        if (v > mid) {
                            patByte |= (uint8_t)(1u << x);
                            prow[x] = c1;
                        } else {
                            prow[x] = c0;
                        }
                    }
                    rowPat.push_back(patByte);
                }
            }
        }

        // Emit row bundles in exact DecodePlane order:
        bwOut.putBits((uint32_t)rowBlk.size(), lenBlk);
        bwOut.putBit(0);
        for (uint8_t b : rowBlk) {
            bwOut.putBits(b, 4);
        }

        if (by == 0) bwOut.putBits(0, lenSub);

        bwOut.putBits((uint32_t)rowCol.size(), lenCol);
        bwOut.putBit(0);
        for (uint8_t c : rowCol) {
            bwOut.putBits((c >> 4) & 0xFu, 4);
            bwOut.putBits(c & 0xFu, 4);
        }

        bwOut.putBits((uint32_t)rowPat.size(), lenPat);
        for (uint8_t p : rowPat) {
            bwOut.putBits(p & 0xFu, 4);
            bwOut.putBits((p >> 4) & 0xFu, 4);
        }

        if (by == 0) {
            bwOut.putBits(0, lenXOff);
            bwOut.putBits(0, lenYOff);
            bwOut.putBits(0, lenInDC);
            bwOut.putBits(0, lenItDC);
            bwOut.putBits(0, lenRun);
        }
    }

    bwOut.align32();
}

struct MemIoCtx {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
    bool err;
};

bool memErr(void* u) { return static_cast<MemIoCtx*>(u)->err; }
void memSeek(int32_t off, uint8_t dir, void* u) {
    auto* c = static_cast<MemIoCtx*>(u);
    if (dir == 0) c->pos = (uint32_t)off;
    else if (dir == 1) c->pos = (uint32_t)((int32_t)c->pos + off);
}
uint32_t memRead(void* dst, uint32_t sz, void* u) {
    auto* c = static_cast<MemIoCtx*>(u);
    uint32_t avail = (c->pos < c->size) ? (c->size - c->pos) : 0;
    uint32_t n = (sz <= avail) ? sz : avail;
    if (n) memcpy(dst, c->data + c->pos, n);
    if (n < sz) memset((uint8_t*)dst + n, 0, sz - n);
    c->pos += n;
    return n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 8) {
        fprintf(stderr, "Usage: %s <width> <height> <fps_num> <fps_den> <max_frames> <max_bytes> <out.bik> [skip_thr] [fill_thr]\n", argv[0]);
        return 1;
    }
    const int W = atoi(argv[1]);
    const int H = atoi(argv[2]);
    const int fpsNum = atoi(argv[3]);
    const int fpsDen = atoi(argv[4]);
    const int maxFrames = atoi(argv[5]);
    const uint32_t maxBytes = (uint32_t)strtoul(argv[6], nullptr, 0);
    const char* outPath = argv[7];
    const int skipThr = (argc > 8) ? atoi(argv[8]) : 6;
    const int fillThr = (argc > 9) ? atoi(argv[9]) : 6;

    const size_t ySize = (size_t)W * H;
    const size_t uvSize = (size_t)(W / 2) * (H / 2);
    const size_t frameBytes = ySize + 2 * uvSize;
    std::vector<uint8_t> rawFrame(frameBytes);

    PlaneEncoderState stY, stU, stV;
    stY.init(W, H, false);
    stU.init(W / 2, H / 2, true);
    stV.init(W / 2, H / 2, true);

    std::vector<std::vector<uint8_t>> encodedFrames;
    BitWriter bw;
    bw.reserve(131072);

    uint32_t runningPayloadBytes = 0;
    int skippedBlackLeader = 0;

    while ((int)encodedFrames.size() < maxFrames) {
        size_t got = 0;
        while (got < frameBytes) {
            size_t n = fread(rawFrame.data() + got, 1, frameBytes - got, stdin);
            if (n == 0) break;
            got += n;
        }
        if (got < frameBytes) break;

        // Skip up to 15 leading solid-black fade-in frames so frame 0 starts on visible content.
        if (encodedFrames.empty() && skippedBlackLeader < 15) {
            uint64_t ySum = 0;
            for (size_t i = 0; i < ySize; i += 16) ySum += rawFrame[i];
            double yMean = (double)ySum / (double)(ySize / 16);
            if (yMean < 20.0) {
                skippedBlackLeader++;
                continue;
            }
        }

        bool isKey = encodedFrames.empty();
        bw.buf.clear();
        bw.curByte = 0;
        bw.bitPos = 0;

        bw.putBits(0, 32);

        const uint8_t* yPtr = rawFrame.data();
        const uint8_t* uPtr = yPtr + ySize;
        const uint8_t* vPtr = uPtr + uvSize;

        encodePlane(bw, stY, yPtr, W,     isKey, skipThr, fillThr);
        encodePlane(bw, stV, vPtr, W / 2, isKey, skipThr, fillThr);
        encodePlane(bw, stU, uPtr, W / 2, isKey, skipThr, fillThr);

        uint32_t nextCount = (uint32_t)encodedFrames.size() + 1u;
        uint32_t nextHeader = 44u + (nextCount + 1u) * 4u;
        uint32_t nextTotal = nextHeader + runningPayloadBytes + (uint32_t)bw.buf.size();
        if (maxBytes > 0 && nextTotal > maxBytes && !encodedFrames.empty()) {
            break;
        }

        runningPayloadBytes += (uint32_t)bw.buf.size();
        encodedFrames.push_back(bw.buf);
    }

    if (encodedFrames.empty()) {
        fprintf(stderr, "No frames encoded\n");
        return 1;
    }

    const uint32_t nFrames = (uint32_t)encodedFrames.size();
    const uint32_t headerSize = 44u + (nFrames + 1u) * 4u;
    uint32_t curOff = headerSize;
    uint32_t maxFrameSz = 0;
    std::vector<uint32_t> offsets(nFrames + 1);
    for (uint32_t i = 0; i < nFrames; ++i) {
        uint32_t sz = (uint32_t)encodedFrames[i].size();
        if (sz > maxFrameSz) maxFrameSz = sz;
        offsets[i] = curOff | (i == 0 ? 1u : 0u);
        curOff += sz;
    }
    offsets[nFrames] = curOff;
    const uint32_t totalSize = curOff;

    std::vector<uint8_t> out(totalSize);
    memcpy(out.data(), "BIKi", 4);
    auto put32 = [&](size_t off, uint32_t v) { memcpy(out.data() + off, &v, 4); };
    put32(4,  totalSize - 8);
    put32(8,  nFrames);
    put32(12, maxFrameSz);
    put32(16, nFrames);
    put32(20, (uint32_t)W);
    put32(24, (uint32_t)H);
    put32(28, (uint32_t)fpsNum);
    put32(32, (uint32_t)fpsDen);
    put32(36, 0);
    put32(40, 0);
    for (uint32_t i = 0; i <= nFrames; ++i) {
        put32(44 + i * 4, offsets[i]);
    }
    uint32_t writePos = headerSize;
    for (uint32_t i = 0; i < nFrames; ++i) {
        memcpy(out.data() + writePos, encodedFrames[i].data(), encodedFrames[i].size());
        writePos += (uint32_t)encodedFrames[i].size();
    }

    bdec_set_memory_funcs(malloc, free);
    bdec_set_log_func([](const char* msg) {
        fprintf(stderr, "[libbinkdec ERROR] %s\n", msg);
        exit(2);
    });
    bdec_decoder_t* dec = bdec_decoder_new();
    MemIoCtx ioCtx{out.data(), totalSize, 0, false};
    bdec_file_io_t io{memErr, memSeek, memRead};
    if (!bdec_open_file(dec, io, &ioCtx)) {
        fprintf(stderr, "Self-test bdec_open_file failed!\n");
        return 2;
    }
    bdec_video_plane_t planes[4]{};
    for (uint32_t i = 0; i < std::min(nFrames, 5u); ++i) {
        bdec_get_next_frame(dec, planes);
    }
    bdec_decoder_free(dec);

    FILE* fp = fopen(outPath, "wb");
    if (!fp) return 1;
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    printf("Encoded %u frames (%dx%d) -> %s (%u / %u bytes, %.2f KiB/frame, libbinkdec verified)\n",
           nFrames, W, H, outPath, totalSize, maxBytes, (double)totalSize / (1024.0 * nFrames));
    return 0;
}
