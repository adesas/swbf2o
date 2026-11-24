// Direct3D 9 resource objects backed by WebGPU (emdawnwebgpu):
//   - Textures (2D, Cube, Volume) with hardware BC1/2/3 + automatic software BC1/2/3 &
//     legacy 16-bit / luminance / BGRA -> RGBA8Unorm decoding
//   - Surfaces (backbuffer, depth-stencil, texture mip surface, offscreen plain)
//   - Static & Dynamic Vertex / Index Buffers (using per-frame streaming GPU rings)
//   - Vertex Declarations (D3DVERTEXELEMENT9[] + FVF)
//   - Vertex / Pixel Shader bytecode containers (for Phase 3 SM 1.1 -> WGSL)
//   - Queries

#include "d3d9_wgpu_internal.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

static uint64_t g_nextResourceId = 1;

// ---------------------------------------------------------------------------
// Per-frame dynamic ring buffer
// ---------------------------------------------------------------------------
bool WgpuDynRing::init(wgpu::Device& dev, uint32_t capBytes, wgpu::BufferUsage usage, const char* label) {
  WgpuSysHeapGuard _sysHeap;
  capacity = (capBytes + 255u) & ~255u;
  used = 0;
  cpu.assign(capacity, 0);
  wgpu::BufferDescriptor bd{};
  bd.label = label;
  bd.size = capacity;
  bd.usage = usage | wgpu::BufferUsage::CopyDst;
  gpu = dev.CreateBuffer(&bd);
  return (bool)gpu;
}

uint32_t WgpuDynRing::alloc(uint32_t bytes, uint32_t align, void** outPtr) {
  if (align < 4) align = 4;
  uint32_t aligned = (used + (align - 1u)) & ~(align - 1u);
  uint32_t allocBytes = (bytes + 3u) & ~3u;
  if (aligned + allocBytes > capacity) {
    // Wrap to 0 if a single frame ever exceeds the ring budget (unlikely with 16 MiB/4 MiB/8 MiB).
    aligned = 0;
    if (allocBytes > capacity) {
      if (outPtr) *outPtr = nullptr;
      return 0;
    }
  }
  used = aligned + allocBytes;
  if (outPtr) *outPtr = cpu.data() + aligned;
  return aligned;
}

void WgpuDynRing::upload(wgpu::Queue& queue) {
  if (used == 0 || !gpu) return;
  uint32_t sz = (used + 3u) & ~3u;
  if (sz > capacity) sz = capacity;
  queue.WriteBuffer(gpu, 0, cpu.data(), sz);
}

// ---------------------------------------------------------------------------
// Format sizing & BC1/BC2/BC3 + legacy format decoders -> RGBA8Unorm
// ---------------------------------------------------------------------------
bool IsCompressedD3DFormat(D3DFORMAT fmt) {
  return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
         fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

static uint32_t bytesPerPixelUncompressed(D3DFORMAT fmt) {
  switch (fmt) {
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
      return 8;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_X8V8U8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32:
      return 4;
    case D3DFMT_R8G8B8:
      return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_A8L8:
    case D3DFMT_L16:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:
    case D3DFMT_R16F:
      return 2;
    case D3DFMT_R3G3B2:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A4L4:
    case D3DFMT_P8:
      return 1;
    default:
      return 4;
  }
}

uint32_t CalcD3D9SurfacePitch(D3DFORMAT fmt, uint32_t w) {
  if (w == 0) w = 1;
  if (fmt == D3DFMT_DXT1) {
    return ((w + 3u) / 4u) * 8u;
  }
  if (IsCompressedD3DFormat(fmt)) {
    return ((w + 3u) / 4u) * 16u;
  }
  return w * bytesPerPixelUncompressed(fmt);
}

uint32_t CalcD3D9SurfaceByteSize(D3DFORMAT fmt, uint32_t w, uint32_t h) {
  if (w == 0) w = 1;
  if (h == 0) h = 1;
  if (IsCompressedD3DFormat(fmt)) {
    return CalcD3D9SurfacePitch(fmt, w) * ((h + 3u) / 4u);
  }
  return CalcD3D9SurfacePitch(fmt, w) * h;
}

static inline void expand565(uint16_t c, uint8_t out[4]) {
  uint8_t r = (uint8_t)((c >> 11) & 0x1F);
  uint8_t g = (uint8_t)((c >> 5) & 0x3F);
  uint8_t b = (uint8_t)(c & 0x1F);
  out[0] = (uint8_t)((r << 3) | (r >> 2));
  out[1] = (uint8_t)((g << 2) | (g >> 4));
  out[2] = (uint8_t)((b << 3) | (b >> 2));
  out[3] = 255;
}

static void decodeBC1Block(const uint8_t blk[8], uint8_t rgba[64], bool allowPunchthrough) {
  uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
  uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
  uint32_t bits = (uint32_t)blk[4] | ((uint32_t)blk[5] << 8) |
                  ((uint32_t)blk[6] << 16) | ((uint32_t)blk[7] << 24);
  uint8_t pal[4][4];
  expand565(c0, pal[0]);
  expand565(c1, pal[1]);
  if (c0 > c1 || !allowPunchthrough) {
    for (int ch = 0; ch < 3; ++ch) {
      pal[2][ch] = (uint8_t)((2 * pal[0][ch] + pal[1][ch] + 1) / 3);
      pal[3][ch] = (uint8_t)((pal[0][ch] + 2 * pal[1][ch] + 1) / 3);
    }
    pal[2][3] = 255;
    pal[3][3] = 255;
  } else {
    for (int ch = 0; ch < 3; ++ch) {
      pal[2][ch] = (uint8_t)((pal[0][ch] + pal[1][ch]) / 2);
      pal[3][ch] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  for (int i = 0; i < 16; ++i) {
    uint32_t idx = (bits >> (2 * i)) & 3u;
    memcpy(rgba + i * 4, pal[idx], 4);
  }
}

static void decodeBC2Block(const uint8_t blk[16], uint8_t rgba[64]) {
  decodeBC1Block(blk + 8, rgba, false);
  for (int i = 0; i < 16; ++i) {
    uint8_t b = blk[i >> 1];
    uint8_t a4 = (i & 1) ? (b >> 4) : (b & 0x0F);
    rgba[i * 4 + 3] = (uint8_t)((a4 << 4) | a4);
  }
}

static void decodeBC3Block(const uint8_t blk[16], uint8_t rgba[64]) {
  decodeBC1Block(blk + 8, rgba, false);
  uint8_t a0 = blk[0], a1 = blk[1];
  uint8_t apal[8];
  apal[0] = a0;
  apal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i <= 6; ++i) {
      apal[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
    }
  } else {
    for (int i = 1; i <= 4; ++i) {
      apal[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5);
    }
    apal[6] = 0;
    apal[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= ((uint64_t)blk[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) {
    rgba[i * 4 + 3] = apal[(abits >> (3 * i)) & 7u];
  }
}

void DecodeTextureLevelToRGBA8(D3DFORMAT fmt, uint32_t w, uint32_t h,
                               const uint8_t* src, uint32_t srcPitch,
                               uint8_t* dstRGBA8) {
  if (!src || !dstRGBA8 || w == 0 || h == 0) return;

  if (IsCompressedD3DFormat(fmt)) {
    uint32_t bxCount = (w + 3u) / 4u;
    uint32_t byCount = (h + 3u) / 4u;
    uint32_t blkSize = (fmt == D3DFMT_DXT1) ? 8u : 16u;
    uint8_t blkRgba[64];
    for (uint32_t by = 0; by < byCount; ++by) {
      const uint8_t* rowPtr = src + by * srcPitch;
      for (uint32_t bx = 0; bx < bxCount; ++bx) {
        const uint8_t* blk = rowPtr + bx * blkSize;
        if (fmt == D3DFMT_DXT1) {
          decodeBC1Block(blk, blkRgba, true);
        } else if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3) {
          decodeBC2Block(blk, blkRgba);
        } else {
          decodeBC3Block(blk, blkRgba);
        }
        for (uint32_t py = 0; py < 4; ++py) {
          uint32_t y = by * 4 + py;
          if (y >= h) break;
          for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = bx * 4 + px;
            if (x >= w) break;
            memcpy(dstRGBA8 + (y * w + x) * 4, blkRgba + (py * 4 + px) * 4, 4);
          }
        }
      }
    }
    return;
  }

  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = src + y * srcPitch;
    uint8_t* d = dstRGBA8 + y * w * 4;
    switch (fmt) {
      case D3DFMT_A8R8G8B8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
          s += 4; d += 4;
        }
        break;
      case D3DFMT_X8R8G8B8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
          s += 4; d += 4;
        }
        break;
      case D3DFMT_A8B8G8R8:
        memcpy(d, s, w * 4);
        break;
      case D3DFMT_X8B8G8R8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
          s += 4; d += 4;
        }
        break;
      case D3DFMT_R8G8B8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
          s += 3; d += 4;
        }
        break;
      case D3DFMT_R5G6B5:
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
          expand565(p, d);
          s += 2; d += 4;
        }
        break;
      case D3DFMT_A1R5G5B5:
      case D3DFMT_X1R5G5B5:
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
          uint8_t r = (uint8_t)((p >> 10) & 0x1F);
          uint8_t g = (uint8_t)((p >> 5) & 0x1F);
          uint8_t b = (uint8_t)(p & 0x1F);
          d[0] = (uint8_t)((r << 3) | (r >> 2));
          d[1] = (uint8_t)((g << 3) | (g >> 2));
          d[2] = (uint8_t)((b << 3) | (b >> 2));
          d[3] = (fmt == D3DFMT_A1R5G5B5) ? ((p & 0x8000) ? 255 : 0) : 255;
          s += 2; d += 4;
        }
        break;
      case D3DFMT_A4R4G4B4:
      case D3DFMT_X4R4G4B4:
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
          uint8_t a = (uint8_t)((p >> 12) & 0x0F);
          uint8_t r = (uint8_t)((p >> 8) & 0x0F);
          uint8_t g = (uint8_t)((p >> 4) & 0x0F);
          uint8_t b = (uint8_t)(p & 0x0F);
          d[0] = (uint8_t)((r << 4) | r);
          d[1] = (uint8_t)((g << 4) | g);
          d[2] = (uint8_t)((b << 4) | b);
          d[3] = (fmt == D3DFMT_A4R4G4B4) ? (uint8_t)((a << 4) | a) : 255;
          s += 2; d += 4;
        }
        break;
      case D3DFMT_A8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = 0; d[1] = 0; d[2] = 0; d[3] = s[0];
          s += 1; d += 4;
        }
        break;
      case D3DFMT_L8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[0]; d[1] = s[0]; d[2] = s[0]; d[3] = 255;
          s += 1; d += 4;
        }
        break;
      case D3DFMT_A8L8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = s[0]; d[1] = s[0]; d[2] = s[0]; d[3] = s[1];
          s += 2; d += 4;
        }
        break;
      case D3DFMT_A4L4:
        for (uint32_t x = 0; x < w; ++x) {
          uint8_t l = (uint8_t)(s[0] & 0x0F);
          uint8_t a = (uint8_t)((s[0] >> 4) & 0x0F);
          uint8_t l8 = (uint8_t)((l << 4) | l);
          d[0] = l8; d[1] = l8; d[2] = l8; d[3] = (uint8_t)((a << 4) | a);
          s += 1; d += 4;
        }
        break;
      case D3DFMT_L16:
        for (uint32_t x = 0; x < w; ++x) {
          uint8_t l = s[1];
          d[0] = l; d[1] = l; d[2] = l; d[3] = 255;
          s += 2; d += 4;
        }
        break;
      case D3DFMT_V8U8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = (uint8_t)((int8_t)s[0] + 128);
          d[1] = (uint8_t)((int8_t)s[1] + 128);
          d[2] = 255;
          d[3] = 255;
          s += 2; d += 4;
        }
        break;
      case D3DFMT_X8V8U8:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = (uint8_t)((int8_t)s[0] + 128);
          d[1] = (uint8_t)((int8_t)s[1] + 128);
          d[2] = s[2];
          d[3] = 255;
          s += 4; d += 4;
        }
        break;
      case D3DFMT_L6V5U5:
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
          int8_t u = (int8_t)((p & 0x1F) << 3);
          int8_t v = (int8_t)(((p >> 5) & 0x1F) << 3);
          uint8_t l = (uint8_t)(((p >> 10) & 0x3F) << 2);
          d[0] = (uint8_t)(u + 128);
          d[1] = (uint8_t)(v + 128);
          d[2] = l;
          d[3] = 255;
          s += 2; d += 4;
        }
        break;
      default:
        for (uint32_t x = 0; x < w; ++x) {
          d[0] = 255; d[1] = 255; d[2] = 255; d[3] = 255;
          d += 4;
        }
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// WgpuSurface
// ---------------------------------------------------------------------------
WgpuSurface::WgpuSurface(WgpuD3D9Device* dev, uint32_t w, uint32_t h, D3DFORMAT fmt,
                         D3DUSAGE u, D3DPOOL p, bool backbuf, bool depth)
    : device(dev), width(w), height(h), format(fmt), usage(u), pool(p),
      isBackbuffer(backbuf), isDepth(depth) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!backbuf && ctx.ready && w > 0 && h > 0) {
    wgpu::TextureDescriptor td{};
    td.size = {w, h, 1};
    td.mipLevelCount = 1;
    if (depth) {
      td.format = wgpu::TextureFormat::Depth24PlusStencil8;
      td.usage = wgpu::TextureUsage::RenderAttachment;
    } else {
      td.format = wgpu::TextureFormat::RGBA8Unorm;
      td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    }
    ownedTex = ctx.device.CreateTexture(&td);
    if (ownedTex) {
      ownedView = ownedTex.CreateView();
    }
  }
}

WgpuSurface::WgpuSurface(WgpuD3D9Device* dev, WgpuTexture2D* parent, uint32_t mip)
    : device(dev), parentTex(parent), parentMip(mip) {
  if (parentTex) {
    parentTex->AddRef();
    width = std::max(1u, parentTex->width >> mip);
    height = std::max(1u, parentTex->height >> mip);
    format = parentTex->d3dFormat;
    usage = parentTex->usage;
    pool = parentTex->pool;
  }
}

WgpuSurface::~WgpuSurface() {
  if (parentTex) {
    parentTex->Release();
    parentTex = nullptr;
  }
}

wgpu::Texture WgpuSurface::getTexture() const {
  if (isBackbuffer) {
    return isDepth ? GetWgpuContext().depthTex : GetWgpuContext().backbufferTex;
  }
  if (parentTex) return parentTex->gpuTex;
  return ownedTex;
}

wgpu::TextureView WgpuSurface::getView() const {
  if (isBackbuffer) {
    return isDepth ? GetWgpuContext().depthView : GetWgpuContext().backbufferView;
  }
  if (parentTex) {
    if (parentMip < parentTex->mipViews.size()) return parentTex->mipViews[parentMip];
    return parentTex->fullView;
  }
  return ownedView;
}

uint32_t WgpuSurface::getWidth() const {
  if (isBackbuffer) return GetWgpuContext().width;
  return width;
}

uint32_t WgpuSurface::getHeight() const {
  if (isBackbuffer) return GetWgpuContext().height;
  return height;
}

STDMETHODIMP WgpuSurface::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL;
  *ppvObj = this;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) WgpuSurface::AddRef() { return (ULONG)++refCount; }

STDMETHODIMP_(ULONG) WgpuSurface::Release() {
  int r = --refCount;
  if (r <= 0) delete this;
  return (ULONG)(r > 0 ? r : 0);
}

STDMETHODIMP WgpuSurface::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuSurface::GetDesc(D3DSURFACE_DESC* pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  *pDesc = D3DSURFACE_DESC{};
  pDesc->Format = format;
  pDesc->Type = D3DRTYPE_SURFACE;
  pDesc->Usage = usage;
  pDesc->Pool = pool;
  pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
  pDesc->MultiSampleQuality = 0;
  pDesc->Width = getWidth();
  pDesc->Height = getHeight();
  return D3D_OK;
}

STDMETHODIMP WgpuSurface::LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
  if (!pLockedRect) return D3DERR_INVALIDCALL;
  if (parentTex) {
    return parentTex->LockRect(parentMip, pLockedRect, pRect, Flags);
  }
  uint32_t w = getWidth(), h = getHeight();
  uint32_t pitch = CalcD3D9SurfacePitch(format, w);
  uint32_t total = CalcD3D9SurfaceByteSize(format, w, h);
  if (cpuShadow.size() < total) cpuShadow.assign(total, 0);
  pLockedRect->Pitch = (int)pitch;
  pLockedRect->pBits = cpuShadow.data();
  return D3D_OK;
}

STDMETHODIMP WgpuSurface::UnlockRect() {
  if (parentTex) {
    return parentTex->UnlockRect(parentMip);
  }
  if (!isDepth && !isBackbuffer && ownedTex && !cpuShadow.empty()) {
    auto& ctx = GetWgpuContext();
    uint32_t w = getWidth(), h = getHeight();
    ByteVec rgba(w * h * 4);
    DecodeTextureLevelToRGBA8(format, w, h, cpuShadow.data(), CalcD3D9SurfacePitch(format, w), rgba.data());
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = ownedTex;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = w * 4;
    layout.rowsPerImage = h;
    wgpu::Extent3D ext{w, h, 1};
    ctx.queue.WriteTexture(&dst, rgba.data(), rgba.size(), &layout, &ext);
  }
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuTexture2D
// ---------------------------------------------------------------------------
static uint32_t computeFullMipCount(uint32_t w, uint32_t h) {
  uint32_t m = 1;
  uint32_t s = w > h ? w : h;
  while (s > 1) { s >>= 1; ++m; }
  return m;
}

WgpuTexture2D::WgpuTexture2D(WgpuD3D9Device* dev, uint32_t w, uint32_t h, uint32_t lvls,
                             D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p)
    : device(dev), width(w ? w : 1), height(h ? h : 1),
      d3dFormat(fmt), usage(u), pool(p), uniqueId(g_nextResourceId++) {
  WgpuSysHeapGuard _sysHeap;
  uint32_t maxLvls = computeFullMipCount(width, height);
  levels = (lvls == 0 || lvls > maxLvls) ? maxLvls : lvls;

  auto& ctx = GetWgpuContext();
  const bool compressed = IsCompressedD3DFormat(d3dFormat);
  const bool mult4 = ((width & 3u) == 0) && ((height & 3u) == 0);
  // WebGPU requires BC textures to have base dimensions be multiples of 4 and not be RenderAttachment.
  if (compressed && ctx.hasBC && mult4 && !(usage & D3DUSAGE_RENDERTARGET)) {
    useHardwareBC = true;
    if (d3dFormat == D3DFMT_DXT1) {
      wgpuFormat = wgpu::TextureFormat::BC1RGBAUnorm;
    } else if (d3dFormat == D3DFMT_DXT2 || d3dFormat == D3DFMT_DXT3) {
      wgpuFormat = wgpu::TextureFormat::BC2RGBAUnorm;
    } else {
      wgpuFormat = wgpu::TextureFormat::BC3RGBAUnorm;
    }
  } else {
    useHardwareBC = false;
    wgpuFormat = wgpu::TextureFormat::RGBA8Unorm;
  }

  locks.resize(levels);

  if (ctx.ready) {
    wgpu::TextureDescriptor td{};
    td.size = {width, height, 1};
    td.mipLevelCount = levels;
    td.format = wgpuFormat;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    if (usage & D3DUSAGE_RENDERTARGET) {
      td.usage |= wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    }
    gpuTex = ctx.device.CreateTexture(&td);
    if (gpuTex) {
      fullView = gpuTex.CreateView();
      mipViews.resize(levels);
      for (uint32_t i = 0; i < levels; ++i) {
        wgpu::TextureViewDescriptor vd{};
        vd.format = wgpuFormat;
        vd.dimension = wgpu::TextureViewDimension::e2D;
        vd.baseMipLevel = i;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = 0;
        vd.arrayLayerCount = 1;
        mipViews[i] = gpuTex.CreateView(&vd);
      }
    }
  }
}

WgpuTexture2D::~WgpuTexture2D() = default;

STDMETHODIMP WgpuTexture2D::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL;
  *ppvObj = this;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) WgpuTexture2D::AddRef() { return (ULONG)++refCount; }

STDMETHODIMP_(ULONG) WgpuTexture2D::Release() {
  int r = --refCount;
  if (r <= 0) delete this;
  return (ULONG)(r > 0 ? r : 0);
}

STDMETHODIMP WgpuTexture2D::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuTexture2D::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) {
  if (!pDesc || Level >= levels) return D3DERR_INVALIDCALL;
  *pDesc = D3DSURFACE_DESC{};
  pDesc->Format = d3dFormat;
  pDesc->Type = D3DRTYPE_SURFACE;
  pDesc->Usage = usage;
  pDesc->Pool = pool;
  pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
  pDesc->MultiSampleQuality = 0;
  pDesc->Width = std::max(1u, width >> Level);
  pDesc->Height = std::max(1u, height >> Level);
  return D3D_OK;
}

STDMETHODIMP WgpuTexture2D::GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) {
  if (!ppSurfaceLevel || Level >= levels) return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = new WgpuSurface(device, this, Level);
  return D3D_OK;
}

STDMETHODIMP WgpuTexture2D::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                                     CONST RECT* pRect, DWORD Flags) {
  if (!pLockedRect || Level >= levels) return D3DERR_INVALIDCALL;
  uint32_t mw = std::max(1u, width >> Level);
  uint32_t mh = std::max(1u, height >> Level);
  uint32_t pitch = CalcD3D9SurfacePitch(d3dFormat, mw);
  uint32_t total = CalcD3D9SurfaceByteSize(d3dFormat, mw, mh);

  auto& lk = locks[Level];
  if (lk.data.size() < total) {
    lk.data.assign(total, 0);
  }
  lk.pitch = pitch;
  lk.locked = true;
  lk.readonly = (Flags & D3DLOCK_READONLY) != 0;

  uint8_t* ptr = lk.data.data();
  if (pRect && !IsCompressedD3DFormat(d3dFormat)) {
    uint32_t bpp = bytesPerPixelUncompressed(d3dFormat);
    uint32_t rx = pRect->left > 0 ? (uint32_t)pRect->left : 0;
    uint32_t ry = pRect->top > 0 ? (uint32_t)pRect->top : 0;
    size_t off = (size_t)ry * pitch + (size_t)rx * bpp;
    if (off < lk.data.size()) ptr += off;
  }
  pLockedRect->Pitch = (int)pitch;
  pLockedRect->pBits = ptr;
  return D3D_OK;
}

STDMETHODIMP WgpuTexture2D::UnlockRect(UINT Level) {
  if (Level >= levels) return D3DERR_INVALIDCALL;
  auto& lk = locks[Level];
  if (!lk.locked) return D3D_OK;
  lk.locked = false;
  if (!lk.readonly) {
    uploadMip(Level);
  }
  // Free CPU shadow for managed static textures to keep WASM heap lean; keep for dynamic/movie textures.
  if (pool == D3DPOOL_MANAGED && !(usage & D3DUSAGE_DYNAMIC)) {
    ByteVec().swap(lk.data);
  }
  return D3D_OK;
}

static int g_loadBannerCaptureMode = 0; // 0=off, 1=planet banner, 2=load.lvl UI assets
static bool g_sentTipsBoxAsset = false;
static bool g_sentLedIconAsset = false;

extern "C" void swbf2_wgpu_arm_load_banner_capture(int mode) {
  g_loadBannerCaptureMode = mode;
}

extern "C" __attribute__((weak)) void swbf2_gamestate_emit_load_banner(
    const uint8_t* rgba, uint32_t width, uint32_t height);
extern "C" __attribute__((weak)) void swbf2_gamestate_emit_load_asset(
    const char* kind, const uint8_t* rgba, uint32_t width, uint32_t height);

void WgpuTexture2D::uploadMip(uint32_t level) {
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !gpuTex || level >= levels) return;
  auto& lk = locks[level];
  if (lk.data.empty()) return;

  uint32_t mw = std::max(1u, width >> level);
  uint32_t mh = std::max(1u, height >> level);

  // Capture original retail UI textures from load/load.lvl (128x128 metallic tips box & 32x32 red
  // loading icon) once per session so the browser loading banner matches 2005 SWBF2 pixel-for-pixel.
  if (g_loadBannerCaptureMode == 2 && level == 0 && swbf2_gamestate_emit_load_asset) {
    if (!g_sentTipsBoxAsset && mw == 128 && mh == 128) {
      g_sentTipsBoxAsset = true;
      ByteVec rgba((size_t)mw * mh * 4);
      DecodeTextureLevelToRGBA8(d3dFormat, mw, mh, lk.data.data(), lk.pitch, rgba.data());
      swbf2_gamestate_emit_load_asset("tipsBox", rgba.data(), mw, mh);
    } else if (!g_sentLedIconAsset && mw == 32 && mh == 32) {
      g_sentLedIconAsset = true;
      ByteVec rgba((size_t)mw * mh * 4);
      DecodeTextureLevelToRGBA8(d3dFormat, mw, mh, lk.data.data(), lk.pitch, rgba.data());
      swbf2_gamestate_emit_load_asset("ledIcon", rgba.data(), mw, mh);
    }
    if (g_sentTipsBoxAsset && g_sentLedIconAsset) {
      g_loadBannerCaptureMode = 0;
    }
  }

  // If LoadDisplay just opened load/<map>.lvl, capture & decode the real retail planet loading
  // banner (1024x1024 DXT1) and immediately emit it to the main thread before ReadDataFile blocks
  // the worker on synchronous map streaming.
  if (g_loadBannerCaptureMode == 1 && level == 0 && mw >= 512 && mh >= 512 &&
      swbf2_gamestate_emit_load_banner) {
    g_loadBannerCaptureMode = 0;
    ByteVec bannerRgba((size_t)mw * mh * 4);
    DecodeTextureLevelToRGBA8(d3dFormat, mw, mh, lk.data.data(), lk.pitch, bannerRgba.data());
    swbf2_gamestate_emit_load_banner(bannerRgba.data(), mw, mh);
  }

  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = gpuTex;
  dst.mipLevel = level;
  dst.origin = {0, 0, 0};

  if (useHardwareBC) {
    uint32_t bw = (mw + 3u) / 4u;
    uint32_t bh = (mh + 3u) / 4u;
    uint32_t blkBytes = (d3dFormat == D3DFMT_DXT1) ? 8u : 16u;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = bw * blkBytes;
    layout.rowsPerImage = bh;
    // WebGPU BC WriteTexture extent must be the physical block-aligned size (multiple of 4).
    wgpu::Extent3D ext{bw * 4u, bh * 4u, 1};
    ctx.queue.WriteTexture(&dst, lk.data.data(), (size_t)layout.bytesPerRow * bh, &layout, &ext);
  } else {
    ByteVec rgba((size_t)mw * mh * 4);
    DecodeTextureLevelToRGBA8(d3dFormat, mw, mh, lk.data.data(), lk.pitch, rgba.data());
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = mw * 4u;
    layout.rowsPerImage = mh;
    wgpu::Extent3D ext{mw, mh, 1};
    ctx.queue.WriteTexture(&dst, rgba.data(), rgba.size(), &layout, &ext);
  }
}

// ---------------------------------------------------------------------------
// WgpuCubeTexture & WgpuVolumeTexture (CPU-lockable placeholders until Phase 3 SM 1.1)
// ---------------------------------------------------------------------------
WgpuCubeTexture::WgpuCubeTexture(WgpuD3D9Device* dev, uint32_t edgeLen, uint32_t lvls,
                                 D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p)
    : device(dev), edge(edgeLen ? edgeLen : 1), levels(lvls ? lvls : 1),
      d3dFormat(fmt), usage(u), pool(p), uniqueId(g_nextResourceId++) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (ctx.ready) {
    wgpu::TextureDescriptor td{};
    td.label = "swbf2_cubetex";
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {edge, edge, 6};
    td.mipLevelCount = levels;
    td.sampleCount = 1;
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    gpuTex = ctx.device.CreateTexture(&td);
    if (gpuTex) {
      wgpu::TextureViewDescriptor vd{};
      vd.format = wgpu::TextureFormat::RGBA8Unorm;
      vd.dimension = wgpu::TextureViewDimension::Cube;
      vd.baseMipLevel = 0;
      vd.mipLevelCount = levels;
      vd.baseArrayLayer = 0;
      vd.arrayLayerCount = 6;
      cubeView = gpuTex.CreateView(&vd);
    }
  }
}

WgpuCubeTexture::~WgpuCubeTexture() = default;

STDMETHODIMP WgpuCubeTexture::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuCubeTexture::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuCubeTexture::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuCubeTexture::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuCubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  *pDesc = D3DSURFACE_DESC{};
  pDesc->Format = d3dFormat; pDesc->Type = D3DRTYPE_SURFACE;
  pDesc->Usage = usage; pDesc->Pool = pool;
  pDesc->Width = std::max(1u, edge >> Level);
  pDesc->Height = std::max(1u, edge >> Level);
  return D3D_OK;
}
STDMETHODIMP WgpuCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES, UINT Level, IDirect3DSurface9** ppSurf) {
  if (!ppSurf) return D3DERR_INVALIDCALL;
  uint32_t s = std::max(1u, edge >> Level);
  *ppSurf = new WgpuSurface(device, s, s, d3dFormat, usage, pool, false, false);
  return D3D_OK;
}
STDMETHODIMP WgpuCubeTexture::LockRect(D3DCUBEMAP_FACES, UINT Level, D3DLOCKED_RECT* pLockedRect,
                                       CONST RECT*, DWORD) {
  if (!pLockedRect) return D3DERR_INVALIDCALL;
  uint32_t s = std::max(1u, edge >> Level);
  uint32_t pitch = CalcD3D9SurfacePitch(d3dFormat, s);
  uint32_t total = CalcD3D9SurfaceByteSize(d3dFormat, s, s);
  if (scratch.size() < total) scratch.resize(total);
  pLockedRect->Pitch = (int)pitch;
  pLockedRect->pBits = scratch.data();
  return D3D_OK;
}
STDMETHODIMP WgpuCubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !gpuTex || Level >= levels || (uint32_t)FaceType >= 6) return D3D_OK;
  uint32_t s = std::max(1u, edge >> Level);
  uint32_t pitch = CalcD3D9SurfacePitch(d3dFormat, s);
  ByteVec rgba((size_t)s * s * 4);
  DecodeTextureLevelToRGBA8(d3dFormat, s, s, scratch.data(), pitch, rgba.data());

  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = gpuTex;
  dst.mipLevel = Level;
  dst.origin = {0, 0, (uint32_t)FaceType};
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = s * 4;
  layout.rowsPerImage = s;
  wgpu::Extent3D ext{s, s, 1};
  ctx.queue.WriteTexture(&dst, rgba.data(), rgba.size(), &layout, &ext);
  return D3D_OK;
}

WgpuVolumeTexture::WgpuVolumeTexture(WgpuD3D9Device* dev, uint32_t w, uint32_t h, uint32_t d,
                                     uint32_t lvls, D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p)
    : device(dev), width(w ? w : 1), height(h ? h : 1), depth(d ? d : 1),
      levels(lvls ? lvls : 1), d3dFormat(fmt), usage(u), pool(p), uniqueId(g_nextResourceId++) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (ctx.ready) {
    wgpu::TextureDescriptor td{};
    td.label = "swbf2_voltex";
    td.dimension = wgpu::TextureDimension::e3D;
    td.size = {width, height, depth};
    td.mipLevelCount = levels;
    td.sampleCount = 1;
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    gpuTex = ctx.device.CreateTexture(&td);
    if (gpuTex) {
      wgpu::TextureViewDescriptor vd{};
      vd.format = wgpu::TextureFormat::RGBA8Unorm;
      vd.dimension = wgpu::TextureViewDimension::e3D;
      vd.baseMipLevel = 0;
      vd.mipLevelCount = levels;
      volView = gpuTex.CreateView(&vd);
    }
  }
}

WgpuVolumeTexture::~WgpuVolumeTexture() = default;

STDMETHODIMP WgpuVolumeTexture::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuVolumeTexture::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuVolumeTexture::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuVolumeTexture::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuVolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  *pDesc = D3DVOLUME_DESC{};
  pDesc->Format = d3dFormat; pDesc->Type = D3DRTYPE_VOLUMETEXTURE;
  pDesc->Usage = usage; pDesc->Pool = pool;
  pDesc->Width = std::max(1u, width >> Level);
  pDesc->Height = std::max(1u, height >> Level);
  pDesc->Depth = std::max(1u, depth >> Level);
  return D3D_OK;
}
STDMETHODIMP WgpuVolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX*, DWORD) {
  if (!pLockedVolume) return D3DERR_INVALIDCALL;
  uint32_t mw = std::max(1u, width >> Level);
  uint32_t mh = std::max(1u, height >> Level);
  uint32_t md = std::max(1u, depth >> Level);
  uint32_t rowPitch = CalcD3D9SurfacePitch(d3dFormat, mw);
  uint32_t slicePitch = CalcD3D9SurfaceByteSize(d3dFormat, mw, mh);
  uint32_t total = slicePitch * md;
  if (scratch.size() < total) scratch.resize(total);
  pLockedVolume->RowPitch = (int)rowPitch;
  pLockedVolume->SlicePitch = (int)slicePitch;
  pLockedVolume->pBits = scratch.data();
  return D3D_OK;
}
STDMETHODIMP WgpuVolumeTexture::UnlockBox(UINT Level) {
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !gpuTex || Level >= levels) return D3D_OK;
  uint32_t mw = std::max(1u, width >> Level);
  uint32_t mh = std::max(1u, height >> Level);
  uint32_t md = std::max(1u, depth >> Level);
  uint32_t rowPitch = CalcD3D9SurfacePitch(d3dFormat, mw);
  uint32_t slicePitch = CalcD3D9SurfaceByteSize(d3dFormat, mw, mh);
  ByteVec rgba((size_t)mw * mh * md * 4);
  for (uint32_t z = 0; z < md; ++z) {
    DecodeTextureLevelToRGBA8(d3dFormat, mw, mh,
                              scratch.data() + (size_t)z * slicePitch,
                              rowPitch,
                              rgba.data() + (size_t)z * mw * mh * 4);
  }
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = gpuTex;
  dst.mipLevel = Level;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = mw * 4;
  layout.rowsPerImage = mh;
  wgpu::Extent3D ext{mw, mh, md};
  ctx.queue.WriteTexture(&dst, rgba.data(), rgba.size(), &layout, &ext);
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuVertexBuffer
// ---------------------------------------------------------------------------
WgpuVertexBuffer::WgpuVertexBuffer(WgpuD3D9Device* dev, uint32_t len, D3DUSAGE u, DWORD fvfIn, D3DPOOL p)
    : device(dev), length(len ? len : 4), usage(u), fvf(fvfIn), pool(p),
      isDynamic((u & D3DUSAGE_DYNAMIC) != 0) {
  WgpuSysHeapGuard _sysHeap;
  uint32_t alignedLen = (length + 3u) & ~3u;
  if (!isDynamic) {
    cpuShadow.assign(alignedLen, 0);
    auto& ctx = GetWgpuContext();
    if (ctx.ready) {
      wgpu::BufferDescriptor bd{};
      bd.size = alignedLen;
      bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
      staticGpu = ctx.device.CreateBuffer(&bd);
    }
  }
}

WgpuVertexBuffer::~WgpuVertexBuffer() = default;

STDMETHODIMP WgpuVertexBuffer::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuVertexBuffer::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuVertexBuffer::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuVertexBuffer::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuVertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
  if (!ppbData) return D3DERR_INVALIDCALL;
  uint32_t sz = (SizeToLock == 0) ? (length > OffsetToLock ? length - OffsetToLock : 0) : SizeToLock;
  if (OffsetToLock + sz > length) {
    sz = (OffsetToLock < length) ? (length - OffsetToLock) : 0;
  }
  lockOffset = OffsetToLock;
  lockSize = sz;

  if (isDynamic) {
    auto& ctx = GetWgpuContext();
    bool needNewWindow = (!ringValid) || (ringFrame != ctx.frameCount) || ((Flags & D3DLOCK_DISCARD) != 0);
    if (needNewWindow) {
      void* basePtr = nullptr;
      ringBaseOffset = ctx.dynVertRing.alloc(length, 16, &basePtr);
      ringFrame = ctx.frameCount;
      ringValid = (basePtr != nullptr);
      if (!ringValid) {
        if (cpuShadow.size() < length) cpuShadow.assign(length, 0);
        *ppbData = cpuShadow.data() + OffsetToLock;
        return D3D_OK;
      }
    }
    *ppbData = ctx.dynVertRing.cpu.data() + ringBaseOffset + OffsetToLock;
    return D3D_OK;
  }

  if (cpuShadow.size() < ((length + 3u) & ~3u)) {
    cpuShadow.resize((length + 3u) & ~3u, 0);
  }
  *ppbData = cpuShadow.data() + OffsetToLock;
  return D3D_OK;
}

STDMETHODIMP WgpuVertexBuffer::Unlock() {
  if (isDynamic) {
    // Already written directly into ctx.dynVertRing.cpu; uploaded in one WriteBuffer at Present().
    return D3D_OK;
  }
  auto& ctx = GetWgpuContext();
  if (ctx.ready && staticGpu && lockSize > 0) {
    // WebGPU WriteBuffer requires 4-byte aligned offset and size.
    uint32_t alignedOff = lockOffset & ~3u;
    uint32_t end = ((lockOffset + lockSize) + 3u) & ~3u;
    if (end > cpuShadow.size()) end = (uint32_t)cpuShadow.size();
    if (end > alignedOff) {
      ctx.queue.WriteBuffer(staticGpu, alignedOff, cpuShadow.data() + alignedOff, end - alignedOff);
    }
  }
  lockSize = 0;
  return D3D_OK;
}

STDMETHODIMP WgpuVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC* pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  *pDesc = D3DVERTEXBUFFER_DESC{};
  pDesc->Format = D3DFMT_VERTEXDATA;
  pDesc->Type = D3DRTYPE_VERTEXBUFFER;
  pDesc->Usage = usage;
  pDesc->Pool = pool;
  pDesc->Size = length;
  pDesc->FVF = fvf;
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuIndexBuffer
// ---------------------------------------------------------------------------
WgpuIndexBuffer::WgpuIndexBuffer(WgpuD3D9Device* dev, uint32_t len, D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p)
    : device(dev), length(len ? len : 4), usage(u), format(fmt), pool(p),
      isDynamic((u & D3DUSAGE_DYNAMIC) != 0) {
  WgpuSysHeapGuard _sysHeap;
  uint32_t alignedLen = (length + 3u) & ~3u;
  if (!isDynamic) {
    cpuShadow.assign(alignedLen, 0);
    auto& ctx = GetWgpuContext();
    if (ctx.ready) {
      wgpu::BufferDescriptor bd{};
      bd.size = alignedLen;
      bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
      staticGpu = ctx.device.CreateBuffer(&bd);
    }
  }
}

WgpuIndexBuffer::~WgpuIndexBuffer() = default;

STDMETHODIMP WgpuIndexBuffer::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuIndexBuffer::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuIndexBuffer::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuIndexBuffer::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuIndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
  if (!ppbData) return D3DERR_INVALIDCALL;
  uint32_t sz = (SizeToLock == 0) ? (length > OffsetToLock ? length - OffsetToLock : 0) : SizeToLock;
  if (OffsetToLock + sz > length) {
    sz = (OffsetToLock < length) ? (length - OffsetToLock) : 0;
  }
  lockOffset = OffsetToLock;
  lockSize = sz;

  if (isDynamic) {
    auto& ctx = GetWgpuContext();
    bool needNewWindow = (!ringValid) || (ringFrame != ctx.frameCount) || ((Flags & D3DLOCK_DISCARD) != 0);
    if (needNewWindow) {
      void* basePtr = nullptr;
      ringBaseOffset = ctx.dynIdxRing.alloc(length, 4, &basePtr);
      ringFrame = ctx.frameCount;
      ringValid = (basePtr != nullptr);
      if (!ringValid) {
        if (cpuShadow.size() < length) cpuShadow.assign(length, 0);
        *ppbData = cpuShadow.data() + OffsetToLock;
        return D3D_OK;
      }
    }
    *ppbData = ctx.dynIdxRing.cpu.data() + ringBaseOffset + OffsetToLock;
    return D3D_OK;
  }

  if (cpuShadow.size() < ((length + 3u) & ~3u)) {
    cpuShadow.resize((length + 3u) & ~3u, 0);
  }
  *ppbData = cpuShadow.data() + OffsetToLock;
  return D3D_OK;
}

STDMETHODIMP WgpuIndexBuffer::Unlock() {
  if (isDynamic) return D3D_OK;
  auto& ctx = GetWgpuContext();
  if (ctx.ready && staticGpu && lockSize > 0) {
    uint32_t alignedOff = lockOffset & ~3u;
    uint32_t end = ((lockOffset + lockSize) + 3u) & ~3u;
    if (end > cpuShadow.size()) end = (uint32_t)cpuShadow.size();
    if (end > alignedOff) {
      ctx.queue.WriteBuffer(staticGpu, alignedOff, cpuShadow.data() + alignedOff, end - alignedOff);
    }
  }
  lockSize = 0;
  return D3D_OK;
}

STDMETHODIMP WgpuIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC* pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  *pDesc = D3DINDEXBUFFER_DESC{};
  pDesc->Format = format;
  pDesc->Type = D3DRTYPE_INDEXBUFFER;
  pDesc->Usage = usage;
  pDesc->Pool = pool;
  pDesc->Size = length;
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuVertexDecl
// ---------------------------------------------------------------------------
uint8_t MapD3DDeclTypeToVtxFmt(unsigned char t, uint16_t* outBytes) {
  switch (t) {
    case D3DDECLTYPE_FLOAT1:   *outBytes = 4;  return WGPU_VTX_FLOAT1;
    case D3DDECLTYPE_FLOAT2:   *outBytes = 8;  return WGPU_VTX_FLOAT2;
    case D3DDECLTYPE_FLOAT3:   *outBytes = 12; return WGPU_VTX_FLOAT3;
    case D3DDECLTYPE_FLOAT4:   *outBytes = 16; return WGPU_VTX_FLOAT4;
    case D3DDECLTYPE_D3DCOLOR: *outBytes = 4;  return WGPU_VTX_D3DCOLOR;
    case 5:                    *outBytes = 4;  return WGPU_VTX_UBYTE4; // D3DDECLTYPE_UBYTE4
    case D3DDECLTYPE_SHORT2:   *outBytes = 4;  return WGPU_VTX_SHORT2;
    case D3DDECLTYPE_SHORT4:   *outBytes = 8;  return WGPU_VTX_SHORT4;
    default:                   *outBytes = 4;  return WGPU_VTX_NONE;
  }
}

WgpuVertexDecl::WgpuVertexDecl(WgpuD3D9Device* dev, const D3DVERTEXELEMENT9* pElems)
    : device(dev), uniqueId((uint32_t)g_nextResourceId++) {
  ffpLayout = FfpVertexLayout{};
  if (!pElems) return;
  for (int i = 0; i < MAX_D3DVERTEXELEMENTS; ++i) {
    const D3DVERTEXELEMENT9& e = pElems[i];
    elements.push_back(e);
    if (e.Stream == 0xFF || e.Type == D3DDECLTYPE_UNUSED) break;
    if (e.Stream != 0) continue;
    uint16_t sz = 0;
    uint8_t fmt = MapD3DDeclTypeToVtxFmt(e.Type, &sz);
    if (fmt == WGPU_VTX_NONE) continue;
    uint16_t end = (uint16_t)(e.Offset + sz);
    if (end > ffpLayout.minStride) ffpLayout.minStride = end;

    FfpVertexAttr attr{e.Offset, fmt, 1};
    switch (e.Usage) {
      case D3DDECLUSAGE_POSITION:
        ffpLayout.pos = attr;
        ffpLayout.isRHW = false;
        break;
      case D3DDECLUSAGE_POSITIONT:
        ffpLayout.pos = attr;
        ffpLayout.isRHW = true;
        break;
      case D3DDECLUSAGE_NORMAL:
        ffpLayout.normal = attr;
        break;
      case D3DDECLUSAGE_COLOR:
        if (e.UsageIndex == 0) ffpLayout.diffuse = attr;
        else if (e.UsageIndex == 1) ffpLayout.specular = attr;
        break;
      case D3DDECLUSAGE_TEXCOORD:
        if (e.UsageIndex < 4) ffpLayout.tex[e.UsageIndex] = attr;
        break;
      default:
        break;
    }
  }
}

STDMETHODIMP WgpuVertexDecl::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuVertexDecl::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuVertexDecl::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuVertexDecl::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuVertexDecl::GetDeclaration(D3DVERTEXELEMENT9* pElement, UINT* pNumElements) {
  if (pNumElements) *pNumElements = (UINT)elements.size();
  if (pElement && !elements.empty()) {
    memcpy(pElement, elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
  }
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuVertexShader / WgpuPixelShader / WgpuQuery
// ---------------------------------------------------------------------------
static U32Vec copyShaderBytecode(const DWORD* pFunc) {
  U32Vec out;
  if (!pFunc) return out;
  size_t i = 0;
  while (i < 16384) {
    uint32_t tok = (uint32_t)pFunc[i++];
    out.push_back(tok);
    if (tok == 0x0000FFFFu) break; // D3DSIO_END
    if (i > 1 && (tok & 0xFFFFu) == 0xFFFEu) {
      size_t clen = (tok >> 16) & 0x7FFFu;
      for (size_t c = 0; c < clen && i < 16384; ++c) {
        out.push_back((uint32_t)pFunc[i++]);
      }
    }
  }
  return out;
}

WgpuVertexShader::WgpuVertexShader(WgpuD3D9Device* dev, const DWORD* pFunc)
    : device(dev), uniqueId((uint32_t)g_nextResourceId++), bytecode(copyShaderBytecode(pFunc)) {
  ParseSm1VertexShaderDcls(bytecode.data(), bytecode.size(), dcls);
  GetWgpuContext().allVertexShaders.push_back(this);
}
STDMETHODIMP WgpuVertexShader::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuVertexShader::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuVertexShader::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuVertexShader::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuVertexShader::GetFunction(void* pData, UINT* pSizeOfData) {
  UINT bytes = (UINT)(bytecode.size() * sizeof(uint32_t));
  if (pSizeOfData) *pSizeOfData = bytes;
  if (pData && bytes > 0) memcpy(pData, bytecode.data(), bytes);
  return D3D_OK;
}

WgpuPixelShader::WgpuPixelShader(WgpuD3D9Device* dev, const DWORD* pFunc)
    : device(dev), uniqueId((uint32_t)g_nextResourceId++), bytecode(copyShaderBytecode(pFunc)) {
  GetWgpuContext().allPixelShaders.push_back(this);
}
STDMETHODIMP WgpuPixelShader::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuPixelShader::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuPixelShader::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuPixelShader::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuPixelShader::GetFunction(void* pData, UINT* pSizeOfData) {
  UINT bytes = (UINT)(bytecode.size() * sizeof(uint32_t));
  if (pSizeOfData) *pSizeOfData = bytes;
  if (pData && bytes > 0) memcpy(pData, bytecode.data(), bytes);
  return D3D_OK;
}

STDMETHODIMP WgpuQuery::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuQuery::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuQuery::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}
STDMETHODIMP WgpuQuery::GetDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = reinterpret_cast<IDirect3DDevice9*>(device);
  if (*ppDevice) (*ppDevice)->AddRef();
  return D3D_OK;
}
STDMETHODIMP WgpuQuery::GetData(void* pData, DWORD dwSize, DWORD) {
  if (pData && dwSize >= sizeof(DWORD)) {
    // Event query is immediately signaled; occlusion query reports visible pixels (> 0) so
    // lens flares / entities are not falsely culled before hardware occlusion queries are wired.
    *(DWORD*)pData = (type == D3DQUERYTYPE_EVENT) ? 1u : 1u;
  }
  return S_OK;
}
