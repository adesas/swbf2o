// Direct3D 9 Device & Adapter implementation over WebGPU (emdawnwebgpu).
//
// Owns:
//   - Async WebGPU bootstrap inside the Dedicated Worker against the transferred
//     OffscreenCanvas (`!canvas`)
//   - Strong `Direct3DCreate9()` returning `WgpuD3D9` (which creates `WgpuD3D9Device`)
//   - Complete D3D9 state machine (transforms, render states, texture stages, samplers,
//     lights, materials, streams, indices, vertex declarations, FVF, viewport, scissor,
//     render targets, depth targets, state blocks)
//   - Lazy WebGPU `RenderPassEncoder` lifecycle, pipeline/sampler/bind-group caches,
//     per-frame dynamic ring uploads, StretchRect / ColorFill / Present blit, and
//     async GPU backbuffer readback for headless Chrome visual verification.

#include <map>
#include <string>
#include <vector>
#include "d3d9_wgpu_internal.h"
#include "PebbleFL/Common/PblFile.h"

#include <emscripten.h>

// One entry per draw submitted in a frame, tagged with the same id the pixel was painted with. Only
// filled while ps mode 4 is active, so the normal render path pays nothing but a branch.
struct DbgDrawRec {
  uint32_t id = 0, tris = 0, stride = 0, declId = 0, vsId = 0, psId = 0, fvf = 0;
  uint64_t vtxKey = 0;
  uint32_t texId[4] = {}, texW[4] = {}, texH[4] = {}, texFmt[4] = {};
  int texNull[4] = {};
  uint32_t uvIdx[4] = {}, colOp[4] = {}, colA1[4] = {}, colA2[4] = {};
  uint32_t zEnable = 0, zFunc = 0, blendEnable = 0, srcBlend = 0, dstBlend = 0, alphaTest = 0;
  uint32_t vpW = 0, vpH = 0, alphaRef = 0;
};
// Debug aid: suppress the submission of draws whose per-frame id falls in [g_dbgSkipLo, g_dbgSkipHi)
// while still counting them, so ids stay stable and a region of the frame can be bisected down to the
// single draw that produces it. Used to identify the flat black panel in live missions without
// touching any shader (the ps-mode-4 id overlay changes every pipeline, and that reliably crashes Lua
// GC - recorded as its own defect).
int g_dbgSkipLo = -1, g_dbgSkipHi = -1;

// Record draw state without touching any shader. Deliberately separate from ps mode 4, whose id
// overlay recompiles every fragment shader and reliably crashes Lua GC during mission script load.
bool g_dbgRecordDraws = false;

// Kept in fixed static storage on purpose: per-draw std::vector growth would allocate from the same
// WASM heap the engine's own allocators carve up, and a diagnostic that reshuffles the heap changes
// which victim an existing out-of-bounds write hits (that is exactly how both debug modes first
// crashed - in Lua GC, and in a null indirect call inside FirstPersonRenderable::RenderSoldier).
static constexpr uint32_t kDbgRecMax = 4096;
static DbgDrawRec g_dbgRecs[2][kDbgRecMax];
static uint32_t g_dbgRecCount[2] = {0, 0};
static uint32_t g_dbgRecOverflow[2] = {0, 0};
static int g_dbgRecBuf = 0;   // index of the buffer currently being filled
static uint32_t g_drawSeq = 0;
static uint32_t g_dbgDrawSeq = 0;

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

// Declared in browser/platform/pc_globals.cpp
extern int pcWidth;
extern int pcHeight;

// ---------------------------------------------------------------------------
// JS hooks for OffscreenCanvas target registration & async completion events
// ---------------------------------------------------------------------------
EM_JS(void, js_wgpu_register_canvas_target, (), {
  if (typeof specialHTMLTargets !== 'undefined' && Module['canvas']) {
    specialHTMLTargets['!canvas'] = Module['canvas'];
  }
});

EM_JS(void, js_wgpu_resize_canvas, (int w, int h), {
  if (Module['canvas'] && w > 0 && h > 0) {
    if (Module['canvas'].width !== w) Module['canvas'].width = w;
    if (Module['canvas'].height !== h) Module['canvas'].height = h;
  }
});

EM_JS(void, js_wgpu_notify_ready, (const char* json), {
  const s = UTF8ToString(json);
  if (typeof Module['onWgpuReady'] === 'function') {
    Module['onWgpuReady'](s);
  }
});

EM_JS(void, js_wgpu_notify_readback, (const char* json, const uint8_t* rgbaPtr, int byteLen, int w, int h), {
  const s = UTF8ToString(json);
  if (typeof Module['onWgpuReadback'] === 'function') {
    let pixels = null;
    if (rgbaPtr && byteLen > 0) {
      pixels = HEAPU8.slice(rgbaPtr, rgbaPtr + byteLen);
    }
    Module['onWgpuReadback'](s, pixels, w, h);
  }
});

// ---------------------------------------------------------------------------
// Global WgpuGpuContext
// ---------------------------------------------------------------------------
static WgpuGpuContext g_wgpuCtx;

WgpuGpuContext& GetWgpuContext() { return g_wgpuCtx; }

void WgpuGpuContext::recordError(const char* msg) {
  ++errorCount;
  if (msg) {
    snprintf(lastError, sizeof(lastError), "%s", msg);
  }
  fprintf(stderr, "[wgpu:err #%u] %s\n", errorCount, lastError);
}

bool WgpuGpuContext::recreateBackbuffer(uint32_t w, uint32_t h) {
  WgpuSysHeapGuard _sysHeap;
  if (!device) return false;
  if (w == 0) w = 1;
  if (h == 0) h = 1;
  width = w;
  height = h;

  js_wgpu_resize_canvas((int)width, (int)height);

  if (surface) {
    wgpu::SurfaceConfiguration config{};
    config.device = device;
    config.format = surfaceFormat;
    config.width = width;
    config.height = height;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.presentMode = wgpu::PresentMode::Fifo;
    surface.Configure(&config);
  }

  wgpu::TextureDescriptor bbDesc{};
  bbDesc.label = "swbf2_backbuffer";
  bbDesc.size = {width, height, 1};
  bbDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  bbDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  backbufferTex = device.CreateTexture(&bbDesc);
  backbufferView = backbufferTex ? backbufferTex.CreateView() : nullptr;

  wgpu::TextureDescriptor depthDesc{};
  depthDesc.label = "swbf2_depth";
  depthDesc.size = {width, height, 1};
  depthDesc.format = wgpu::TextureFormat::Depth24PlusStencil8;
  depthDesc.usage = wgpu::TextureUsage::RenderAttachment;
  depthTex = device.CreateTexture(&depthDesc);
  depthView = depthTex ? depthTex.CreateView() : nullptr;

  return (bool)backbufferView && (bool)depthView;
}

bool WgpuGpuContext::initSharedResources() {
  WgpuSysHeapGuard _sysHeap;
  // 1x1 opaque white fallback textures (2D, Cube 1x1x6, Volume 1x1x1) for unbound D3D9 stages.
  {
    const uint8_t px[4] = {255, 255, 255, 255};
    wgpu::TextureDescriptor td{};
    td.label = "swbf2_white_1x1";
    td.size = {1, 1, 1};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    whiteTex = device.CreateTexture(&td);
    whiteView = whiteTex.CreateView();
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = whiteTex;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D ext{1, 1, 1};
    queue.WriteTexture(&dst, px, 4, &layout, &ext);

    wgpu::TextureDescriptor ctd{};
    ctd.label = "swbf2_white_cube";
    ctd.dimension = wgpu::TextureDimension::e2D;
    ctd.size = {1, 1, 6};
    ctd.format = wgpu::TextureFormat::RGBA8Unorm;
    ctd.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    whiteCubeTex = device.CreateTexture(&ctd);
    wgpu::TextureViewDescriptor cvd{};
    cvd.format = wgpu::TextureFormat::RGBA8Unorm;
    cvd.dimension = wgpu::TextureViewDimension::Cube;
    cvd.mipLevelCount = 1;
    cvd.arrayLayerCount = 6;
    whiteCubeView = whiteCubeTex.CreateView(&cvd);
    for (uint32_t f = 0; f < 6; ++f) {
      wgpu::TexelCopyTextureInfo cdst{};
      cdst.texture = whiteCubeTex;
      cdst.origin = {0, 0, f};
      queue.WriteTexture(&cdst, px, 4, &layout, &ext);
    }

    wgpu::TextureDescriptor vtd{};
    vtd.label = "swbf2_white_3d";
    vtd.dimension = wgpu::TextureDimension::e3D;
    vtd.size = {1, 1, 1};
    vtd.format = wgpu::TextureFormat::RGBA8Unorm;
    vtd.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    whiteVolTex = device.CreateTexture(&vtd);
    wgpu::TextureViewDescriptor vvd{};
    vvd.format = wgpu::TextureFormat::RGBA8Unorm;
    vvd.dimension = wgpu::TextureViewDimension::e3D;
    vvd.mipLevelCount = 1;
    whiteVolView = whiteVolTex.CreateView(&vvd);
    wgpu::TexelCopyTextureInfo vdst{};
    vdst.texture = whiteVolTex;
    queue.WriteTexture(&vdst, px, 4, &layout, &ext);
  }

  {
    wgpu::SamplerDescriptor sd{};
    sd.addressModeU = wgpu::AddressMode::ClampToEdge;
    sd.addressModeV = wgpu::AddressMode::ClampToEdge;
    sd.magFilter = wgpu::FilterMode::Linear;
    sd.minFilter = wgpu::FilterMode::Linear;
    linearClampSampler = device.CreateSampler(&sd);
  }

  // Per-frame dynamic rings (16 MiB vert, 4 MiB idx, 16 MiB uniform -> 5,957 draws/frame).
  if (!dynVertRing.init(device, 16u * 1024u * 1024u, wgpu::BufferUsage::Vertex, "swbf2_dyn_vb")) return false;
  if (!dynIdxRing.init(device, 4u * 1024u * 1024u, wgpu::BufferUsage::Index, "swbf2_dyn_ib")) return false;
  if (!dynUniformRing.init(device, 16u * 1024u * 1024u, wgpu::BufferUsage::Uniform, "swbf2_dyn_ubo")) return false;

  // Explicit BindGroupLayout 0: FFP dynamic uniform buffer.
  {
    wgpu::BindGroupLayoutEntry e{};
    e.binding = 0;
    e.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    e.buffer.type = wgpu::BufferBindingType::Uniform;
    e.buffer.hasDynamicOffset = true;
    e.buffer.minBindingSize = sizeof(FfpDrawUniforms);
    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = "ffp_ubo_bgl";
    bglDesc.entryCount = 1;
    bglDesc.entries = &e;
    ffpUniformBgl = device.CreateBindGroupLayout(&bglDesc);

    wgpu::BindGroupEntry bge{};
    bge.binding = 0;
    bge.buffer = dynUniformRing.gpu;
    bge.offset = 0;
    bge.size = sizeof(FfpDrawUniforms);
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = ffpUniformBgl;
    bgDesc.entryCount = 1;
    bgDesc.entries = &bge;
    ffpUniformBg = device.CreateBindGroup(&bgDesc);
  }

  // Explicit BindGroupLayout 1 & PipelineLayout for default 2D textures (texTypes == 0).
  ffpTexBgl = getOrCreateTexBgl(0);
  ffpPipeLayout = getOrCreatePipeLayout(0);

  // Present / StretchRect blit pipeline.
  {
    wgpu::BindGroupLayoutEntry entries[3]{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[2].buffer.hasDynamicOffset = true;
    entries[2].buffer.minBindingSize = 16;
    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    blitBgl = device.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &blitBgl;
    blitPipeLayout = device.CreatePipelineLayout(&plDesc);

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = GetBlitWGSL();
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    wgpu::ShaderModule mod = device.CreateShaderModule(&smDesc);

    auto makeBlitPipe = [&](wgpu::TextureFormat fmt, const char* fragEntry) {
      wgpu::ColorTargetState target{};
      target.format = fmt;
      wgpu::FragmentState frag{};
      frag.module = mod;
      frag.entryPoint = fragEntry;
      frag.targetCount = 1;
      frag.targets = &target;
      wgpu::RenderPipelineDescriptor rp{};
      rp.layout = blitPipeLayout;
      rp.vertex.module = mod;
      rp.vertex.entryPoint = "vs_blit";
      rp.fragment = &frag;
      rp.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
      return device.CreateRenderPipeline(&rp);
    };
    blitToSurfacePipe = makeBlitPipe(surfaceFormat, "fs_blit");
    blitToRgbaPipe = makeBlitPipe(wgpu::TextureFormat::RGBA8Unorm, "fs_blit_copy");
  }

  return recreateBackbuffer(width, height);
}

wgpu::BindGroupLayout WgpuGpuContext::getOrCreateTexBgl(uint8_t texTypes) {
  WgpuSysHeapGuard _sysHeap;
  if (texBglByTypes[texTypes]) return texBglByTypes[texTypes];
  wgpu::BindGroupLayoutEntry entries[8]{};
  for (uint32_t i = 0; i < 4; ++i) {
    uint8_t st = (texTypes >> (i * 2)) & 3u;
    entries[i].binding = i;
    entries[i].visibility = wgpu::ShaderStage::Fragment;
    entries[i].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[i].texture.viewDimension =
        (st == 1) ? wgpu::TextureViewDimension::Cube
                  : (st == 2 ? wgpu::TextureViewDimension::e3D : wgpu::TextureViewDimension::e2D);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    entries[4 + i].binding = 4 + i;
    entries[4 + i].visibility = wgpu::ShaderStage::Fragment;
    entries[4 + i].sampler.type = wgpu::SamplerBindingType::Filtering;
  }
  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = "swbf2_tex_bgl";
  bglDesc.entryCount = 8;
  bglDesc.entries = entries;
  texBglByTypes[texTypes] = device.CreateBindGroupLayout(&bglDesc);
  return texBglByTypes[texTypes];
}

wgpu::PipelineLayout WgpuGpuContext::getOrCreatePipeLayout(uint8_t texTypes) {
  WgpuSysHeapGuard _sysHeap;
  if (pipeLayoutByTypes[texTypes]) return pipeLayoutByTypes[texTypes];
  wgpu::BindGroupLayout bgls[2] = {ffpUniformBgl, getOrCreateTexBgl(texTypes)};
  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = "swbf2_pipe_layout";
  plDesc.bindGroupLayoutCount = 2;
  plDesc.bindGroupLayouts = bgls;
  pipeLayoutByTypes[texTypes] = device.CreatePipelineLayout(&plDesc);
  return pipeLayoutByTypes[texTypes];
}

// ---------------------------------------------------------------------------
// D3D9 enum -> WebGPU enum mapping helpers
// ---------------------------------------------------------------------------
static wgpu::CompareFunction toWgpuCompare(DWORD fn) {
  switch (fn) {
    case D3DCMP_NEVER:        return wgpu::CompareFunction::Never;
    case D3DCMP_LESS:         return wgpu::CompareFunction::Less;
    case D3DCMP_EQUAL:        return wgpu::CompareFunction::Equal;
    case D3DCMP_LESSEQUAL:    return wgpu::CompareFunction::LessEqual;
    case D3DCMP_GREATER:      return wgpu::CompareFunction::Greater;
    case D3DCMP_NOTEQUAL:     return wgpu::CompareFunction::NotEqual;
    case D3DCMP_GREATEREQUAL: return wgpu::CompareFunction::GreaterEqual;
    case D3DCMP_ALWAYS:       return wgpu::CompareFunction::Always;
    default:                  return wgpu::CompareFunction::LessEqual;
  }
}

static wgpu::StencilOperation toWgpuStencilOp(DWORD op) {
  switch (op) {
    case D3DSTENCILOP_KEEP: return wgpu::StencilOperation::Keep;
    case 2:                 return wgpu::StencilOperation::Zero;
    case 3:                 return wgpu::StencilOperation::Replace;
    case 4:                 return wgpu::StencilOperation::IncrementClamp;
    case 5:                 return wgpu::StencilOperation::DecrementClamp;
    case 6:                 return wgpu::StencilOperation::Invert;
    case D3DSTENCILOP_INCR: return wgpu::StencilOperation::IncrementWrap;
    case D3DSTENCILOP_DECR: return wgpu::StencilOperation::DecrementWrap;
    default:                return wgpu::StencilOperation::Keep;
  }
}

static wgpu::BlendFactor toWgpuBlendFactor(DWORD b, bool isAlpha) {
  switch (b) {
    case D3DBLEND_ZERO:         return wgpu::BlendFactor::Zero;
    case D3DBLEND_ONE:          return wgpu::BlendFactor::One;
    case D3DBLEND_SRCCOLOR:     return isAlpha ? wgpu::BlendFactor::SrcAlpha : wgpu::BlendFactor::Src;
    case D3DBLEND_INVSRCCOLOR:  return isAlpha ? wgpu::BlendFactor::OneMinusSrcAlpha : wgpu::BlendFactor::OneMinusSrc;
    case D3DBLEND_SRCALPHA:     return wgpu::BlendFactor::SrcAlpha;
    case D3DBLEND_INVSRCALPHA:  return wgpu::BlendFactor::OneMinusSrcAlpha;
    case D3DBLEND_DESTALPHA:    return wgpu::BlendFactor::DstAlpha;
    case D3DBLEND_INVDESTALPHA: return wgpu::BlendFactor::OneMinusDstAlpha;
    case D3DBLEND_DESTCOLOR:    return isAlpha ? wgpu::BlendFactor::DstAlpha : wgpu::BlendFactor::Dst;
    case D3DBLEND_INVDESTCOLOR: return isAlpha ? wgpu::BlendFactor::OneMinusDstAlpha : wgpu::BlendFactor::OneMinusDst;
    case 11:                    return wgpu::BlendFactor::SrcAlphaSaturated;
    case 14:                    return wgpu::BlendFactor::Constant;
    case 15:                    return wgpu::BlendFactor::OneMinusConstant;
    default:                    return wgpu::BlendFactor::One;
  }
}

static wgpu::BlendOperation toWgpuBlendOp(DWORD op) {
  switch (op) {
    case 1:  return wgpu::BlendOperation::Add;
    case 2:  return wgpu::BlendOperation::Subtract;
    case 3:  return wgpu::BlendOperation::ReverseSubtract;
    case 4:  return wgpu::BlendOperation::Min;
    case 5:  return wgpu::BlendOperation::Max;
    default: return wgpu::BlendOperation::Add;
  }
}

static wgpu::AddressMode toWgpuAddress(DWORD m) {
  switch (m) {
    case D3DTADDRESS_WRAP:   return wgpu::AddressMode::Repeat;
    case D3DTADDRESS_MIRROR: return wgpu::AddressMode::MirrorRepeat;
    case D3DTADDRESS_CLAMP:
    case D3DTADDRESS_BORDER:
    default:                 return wgpu::AddressMode::ClampToEdge;
  }
}

static wgpu::FilterMode toWgpuFilter(DWORD f) {
  return (f == D3DTEXF_POINT || f == D3DTEXF_NONE) ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear;
}

static wgpu::MipmapFilterMode toWgpuMipFilter(DWORD f) {
  return (f == D3DTEXF_POINT || f == D3DTEXF_NONE) ? wgpu::MipmapFilterMode::Nearest : wgpu::MipmapFilterMode::Linear;
}

static void setIdentityMatrix(D3DMATRIX& m) {
  memset(&m, 0, sizeof(m));
  m._11 = m._22 = m._33 = m._44 = 1.0f;
}

static void unpackD3DColor(D3DCOLOR c, float out[4]) {
  out[0] = ((c >> 16) & 0xFF) / 255.0f;
  out[1] = ((c >> 8) & 0xFF) / 255.0f;
  out[2] = (c & 0xFF) / 255.0f;
  out[3] = ((c >> 24) & 0xFF) / 255.0f;
}

// ---------------------------------------------------------------------------
// WgpuD3D9Device
// ---------------------------------------------------------------------------
struct WgpuStateBlock;

struct WgpuD3D9Device : public IDirect3DDevice9, public WgpuSysObj {
  int refCount = 1;
  IDirect3D9* parentD3D = nullptr;
  D3DPRESENT_PARAMETERS presentParams{};

  // Active render target & depth-stencil surfaces.
  WgpuSurface* backbufferSurf = nullptr;
  WgpuSurface* depthStencilSurf = nullptr;
  WgpuSurface* curRT = nullptr;
  WgpuSurface* curDS = nullptr;

  // Frame command encoder & active render pass.
  wgpu::CommandEncoder encoder;
  wgpu::RenderPassEncoder pass;
  bool passHasDepth = true;
  bool pendingClearColor = false;
  bool pendingClearDepth = false;
  bool pendingClearStencil = false;
  float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float clearZ = 1.0f;
  uint32_t clearStencil = 0;

  // D3D9 pipeline state.
  DWORD renderStates[256]{};
  DWORD texStageStates[8][36]{};
  DWORD samplerStates[16][16]{};
  D3DMATRIX transforms[300]{};
  D3DMATERIAL9 material{};
  D3DLIGHT9 lights[8]{};
  bool lightEnabled[8]{};
  D3DVIEWPORT9 viewport{};
  RECT scissorRect{};
  IDirect3DBaseTexture9* boundTex[8]{};

  struct StreamBinding {
    WgpuVertexBuffer* vb = nullptr;
    uint32_t offset = 0;
    uint32_t stride = 0;
  };
  StreamBinding streams[16]{};
  WgpuIndexBuffer* boundIB = nullptr;
  WgpuVertexDecl* boundDecl = nullptr;
  DWORD boundFVF = 0;
  IDirect3DVertexShader9* boundVS = nullptr;
  IDirect3DPixelShader9* boundPS = nullptr;
  float vsConsts[256][4]{};
  float psConsts[32][4]{};

  // GPU object caches (using SysAllocator so hash map buckets never hit RedMemory).
  struct ProgramCacheKey {
    uint32_t vsId = 0;
    uint32_t psId = 0;
    uint64_t vsKey = 0;
    uint8_t texTypes = 0;
    uint8_t _pad[7] = {};
    bool operator==(const ProgramCacheKey& o) const {
      return memcmp(this, &o, sizeof(*this)) == 0;
    }
  };
  struct ProgramCacheKeyHash {
    size_t operator()(const ProgramCacheKey& k) const {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
      uint64_t h = 14695981039346656037ULL;
      for (size_t i = 0; i < sizeof(k); ++i) { h ^= p[i]; h *= 1099511628211ULL; }
      return (size_t)h;
    }
  };
  std::unordered_map<ProgramCacheKey, wgpu::ShaderModule, ProgramCacheKeyHash,
                     std::equal_to<ProgramCacheKey>,
                     SysAllocator<std::pair<const ProgramCacheKey, wgpu::ShaderModule>>> shaderCache;
  std::unordered_map<FfpPipelineKey, wgpu::RenderPipeline, FfpPipelineKeyHash,
                     std::equal_to<FfpPipelineKey>,
                     SysAllocator<std::pair<const FfpPipelineKey, wgpu::RenderPipeline>>> pipeCache;
  std::unordered_map<uint64_t, wgpu::Sampler, std::hash<uint64_t>, std::equal_to<uint64_t>,
                     SysAllocator<std::pair<const uint64_t, wgpu::Sampler>>> samplerCache;

  struct TexBgKey {
    uint64_t texIds[4];
    uint64_t samIds[4];
    uint8_t texTypes = 0;
    uint8_t _pad[7] = {};
    bool operator==(const TexBgKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
  };
  struct TexBgKeyHash {
    size_t operator()(const TexBgKey& k) const {
      const uint64_t* p = reinterpret_cast<const uint64_t*>(&k);
      uint64_t h = 14695981039346656037ULL;
      for (int i = 0; i < 9; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
      return (size_t)h;
    }
  };
  std::unordered_map<TexBgKey, wgpu::BindGroup, TexBgKeyHash,
                     std::equal_to<TexBgKey>,
                     SysAllocator<std::pair<const TexBgKey, wgpu::BindGroup>>> texBgCache;

  WgpuD3D9Device(IDirect3D9* d3d, const D3DPRESENT_PARAMETERS* pp);
  ~WgpuD3D9Device();

  void initDefaultStates();
  void ensureEncoder();
  void endActivePass();
  void flushPendingClears();
  void ensureRenderPass();
  wgpu::Sampler getOrCreateSampler(uint32_t stage);
  wgpu::BindGroup getOrCreateTexBindGroup(uint8_t* outTexTypes);
  wgpu::ShaderModule getOrCreateProgramShader(const WgpuVertexShader* vs, uint64_t vsInputTypeKey,
                                              const FfpVertexLayout& ffpLayout,
                                              const WgpuPixelShader* ps, uint8_t texTypes);
  wgpu::RenderPipeline getOrCreateFfpPipeline(const FfpPipelineKey& key,
                                              const wgpu::VertexAttribute* sm1Attrs,
                                              uint32_t sm1AttrCount);
  void fillFfpUniforms(FfpDrawUniforms& u);
  void emitDraw(D3DPRIMITIVETYPE primType, bool indexed,
                int32_t baseVertex, uint32_t startIndexOrVertex, uint32_t primCount,
                const void* upVerts, uint32_t upStride, uint32_t upVertCount,
                const void* upIndices, D3DFORMAT upIdxFmt);

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  // IDirect3DDevice9
  STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
  STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 256u * 1024u * 1024u; }
  STDMETHOD(EvictManagedResources)() override { return D3D_OK; }
  STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9) override;
  STDMETHOD(GetDeviceCaps)(D3DCAPS9* pCaps) override;
  STDMETHOD(GetDisplayMode)(UINT iSwapChain, D3DDISPLAYMODE* pMode) override;
  STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override;
  STDMETHOD(Present)(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override;
  STDMETHOD(GetBackBuffer)(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                           IDirect3DSurface9** ppBackBuffer) override;
  STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                           D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE*) override;
  STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                 D3DFORMAT Format, D3DPOOL Pool,
                                 IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE*) override;
  STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                               D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE*) override;
  STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE*) override;
  STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                               IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE*) override;
  STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE,
                                DWORD, BOOL, IDirect3DSurface9** ppSurface, HANDLE*) override;
  STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE,
                                       DWORD, BOOL, IDirect3DSurface9** ppSurface, HANDLE*) override;
  STDMETHOD(CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                         IDirect3DSurface9** ppSurface, HANDLE*) override;
  STDMETHOD(StretchRect)(IDirect3DSurface9* pSrc, CONST RECT* pSrcRect,
                         IDirect3DSurface9* pDst, CONST RECT* pDstRect, D3DTEXTUREFILTERTYPE Filter) override;
  STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) override;
  STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override;
  STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override;
  STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) override;
  STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface) override;
  STDMETHOD(BeginScene)() override { ensureEncoder(); return D3D_OK; }
  STDMETHOD(EndScene)() override { endActivePass(); return D3D_OK; }
  STDMETHOD(Clear)(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
  STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override;
  STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override;
  STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override;
  STDMETHOD(SetViewport)(CONST D3DVIEWPORT9* pViewport) override;
  STDMETHOD(GetViewport)(D3DVIEWPORT9* pViewport) override;
  STDMETHOD(SetMaterial)(CONST D3DMATERIAL9* pMaterial) override;
  STDMETHOD(GetMaterial)(D3DMATERIAL9* pMaterial) override;
  STDMETHOD(SetLight)(DWORD Index, CONST D3DLIGHT9* pLight) override;
  STDMETHOD(GetLight)(DWORD Index, D3DLIGHT9* pLight) override;
  STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) override;
  STDMETHOD(GetLightEnable)(DWORD Index, BOOL* pEnable) override;
  STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override;
  STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD* pValue) override;
  STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override;
  STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override;
  STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture9* pTexture) override;
  STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override;
  STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
  STDMETHOD(GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override;
  STDMETHOD(SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
  STDMETHOD(SetScissorRect)(CONST RECT* pRect) override;
  STDMETHOD(GetScissorRect)(RECT* pRect) override;
  STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
  STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                  UINT MinVertexIndex, UINT NumVertices,
                                  UINT startIndex, UINT primCount) override;
  STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                             CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
  STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                    UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
  STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9* pVertexElements,
                                     IDirect3DVertexDeclaration9** ppDecl) override;
  STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl) override;
  STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl) override;
  STDMETHOD(SetFVF)(DWORD FVF) override;
  STDMETHOD(GetFVF)(DWORD* pFVF) override;
  STDMETHOD(CreateVertexShader)(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override;
  STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* pShader) override;
  STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** ppShader) override;
  STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override;
  STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
  STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                             UINT OffsetInBytes, UINT Stride) override;
  STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
                             UINT* pOffsetInBytes, UINT* pStride) override;
  STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* pIndexData) override;
  STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** ppIndexData) override;
  STDMETHOD(CreatePixelShader)(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override;
  STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* pShader) override;
  STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ppShader) override;
  STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override;
  STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
  STDMETHOD(CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override;
};

struct WgpuStateBlock : public IDirect3DStateBlock9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* dev = nullptr;
  DWORD rs[256]{};
  DWORD tss[8][36]{};
  DWORD samp[16][16]{};
  D3DMATRIX xform[300]{};
  D3DMATERIAL9 mat{};
  D3DVIEWPORT9 vp{};
  DWORD fvf = 0;

  WgpuStateBlock(WgpuD3D9Device* d) : dev(d) { Capture(); }
  STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override {
    if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
  }
  STDMETHOD_(ULONG, AddRef)() override { return (ULONG)++refCount; }
  STDMETHOD_(ULONG, Release)() override {
    int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
  }
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = dev; if (dev) dev->AddRef(); return D3D_OK;
  }
  STDMETHOD(Capture)() override {
    if (!dev) return D3D_OK;
    memcpy(rs, dev->renderStates, sizeof(rs));
    memcpy(tss, dev->texStageStates, sizeof(tss));
    memcpy(samp, dev->samplerStates, sizeof(samp));
    memcpy(xform, dev->transforms, sizeof(xform));
    mat = dev->material;
    vp = dev->viewport;
    fvf = dev->boundFVF;
    return D3D_OK;
  }
  STDMETHOD(Apply)() override {
    if (!dev) return D3D_OK;
    memcpy(dev->renderStates, rs, sizeof(rs));
    memcpy(dev->texStageStates, tss, sizeof(tss));
    memcpy(dev->samplerStates, samp, sizeof(samp));
    memcpy(dev->transforms, xform, sizeof(xform));
    dev->material = mat;
    dev->viewport = vp;
    dev->boundFVF = fvf;
    return D3D_OK;
  }
};

static void fillWebGPUDeviceCaps(D3DCAPS9* pCaps) {
  memset(pCaps, 0, sizeof(*pCaps));
  pCaps->DeviceType = D3DDEVTYPE_HAL;
  pCaps->AdapterOrdinal = 0;
  pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
  pCaps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT;
  pCaps->DevCaps2 = D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
  pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_PERSTAGECONSTANT;
  pCaps->StencilCaps = D3DSTENCILCAPS_TWOSIDED;
  pCaps->TextureCaps = (1u << 11) | (1u << 13); // cubemaps + volumemaps, non-pow2 supported (no POW2 bit)
  pCaps->TextureOpCaps = D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_MULTIPLYADD;
  pCaps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR;
  pCaps->MaxTextureWidth = 4096;
  pCaps->MaxTextureHeight = 4096;
  pCaps->MaxVolumeExtent = 256;
  pCaps->MaxTextureRepeat = 4096;
  pCaps->MaxAnisotropy = 8;
  pCaps->MaxTextureAspectRatio = 4096;
  pCaps->MaxPrimitiveCount = 0xFFFFF;
  pCaps->MaxVertexIndex = 0xFFFFF;
  pCaps->MaxStreams = 16;
  pCaps->MaxStreamStride = 256;
  pCaps->MaxTextureBlendStages = 4;
  pCaps->MaxSimultaneousTextures = 4;
  // Phase 3 Programmable Pipeline: report vs_1_1 (0xFFFE0101), ps_1_1 (0xFFFF0101), and
  // MaxVertexShaderConst = 96 so RedRenderer::internalSetupCaps() enables PIPELINE_PROG1,
  // internalRenderProg1, GEOMETRYMODE_COMPRESSED, and SKINNING_HARDWARE.
  pCaps->VertexShaderVersion = D3DVS_VERSION(1, 1);
  pCaps->PixelShaderVersion = D3DPS_VERSION(1, 1);
  pCaps->MaxVertexShaderConst = 96;
}

WgpuD3D9Device::WgpuD3D9Device(IDirect3D9* d3d, const D3DPRESENT_PARAMETERS* pp)
    : parentD3D(d3d) {
  if (parentD3D) parentD3D->AddRef();
  auto& ctx = GetWgpuContext();
  if (pp) {
    presentParams = *pp;
  }
  if (presentParams.BackBufferWidth == 0) presentParams.BackBufferWidth = ctx.width;
  if (presentParams.BackBufferHeight == 0) presentParams.BackBufferHeight = ctx.height;
  presentParams.BackBufferFormat = D3DFMT_A8R8G8B8;
  presentParams.AutoDepthStencilFormat = D3DFMT_D24S8;
  presentParams.EnableAutoDepthStencil = TRUE;

  if (ctx.ready && (presentParams.BackBufferWidth != ctx.width || presentParams.BackBufferHeight != ctx.height)) {
    ctx.recreateBackbuffer(presentParams.BackBufferWidth, presentParams.BackBufferHeight);
  }

  backbufferSurf = new WgpuSurface(this, ctx.width, ctx.height, D3DFMT_A8R8G8B8,
                                   D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, true, false);
  depthStencilSurf = new WgpuSurface(this, ctx.width, ctx.height, D3DFMT_D24S8,
                                     D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, true, true);
  curRT = backbufferSurf; curRT->AddRef();
  curDS = depthStencilSurf; curDS->AddRef();

  initDefaultStates();
}

WgpuD3D9Device::~WgpuD3D9Device() {
  endActivePass();
  for (int i = 0; i < 8; ++i) {
    if (boundTex[i]) { boundTex[i]->Release(); boundTex[i] = nullptr; }
  }
  for (int i = 0; i < 16; ++i) {
    if (streams[i].vb) { streams[i].vb->Release(); streams[i].vb = nullptr; }
  }
  if (boundIB) { boundIB->Release(); boundIB = nullptr; }
  if (boundDecl) { boundDecl->Release(); boundDecl = nullptr; }
  if (boundVS) { boundVS->Release(); boundVS = nullptr; }
  if (boundPS) { boundPS->Release(); boundPS = nullptr; }
  if (curRT) { curRT->Release(); curRT = nullptr; }
  if (curDS) { curDS->Release(); curDS = nullptr; }
  if (backbufferSurf) { backbufferSurf->Release(); backbufferSurf = nullptr; }
  if (depthStencilSurf) { depthStencilSurf->Release(); depthStencilSurf = nullptr; }
  if (parentD3D) { parentD3D->Release(); parentD3D = nullptr; }
}

void WgpuD3D9Device::initDefaultStates() {
  memset(renderStates, 0, sizeof(renderStates));
  renderStates[D3DRS_ZENABLE] = D3DZB_TRUE;
  renderStates[D3DRS_FILLMODE] = D3DFILL_SOLID;
  renderStates[D3DRS_ZWRITEENABLE] = TRUE;
  renderStates[D3DRS_ALPHATESTENABLE] = FALSE;
  renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  renderStates[D3DRS_BLENDOP] = 1; // ADD
  renderStates[D3DRS_CULLMODE] = D3DCULL_CCW;
  renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  renderStates[D3DRS_ALPHAREF] = 0;
  renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  renderStates[D3DRS_ALPHABLENDENABLE] = FALSE;
  renderStates[D3DRS_FOGENABLE] = FALSE;
  renderStates[D3DRS_FOGCOLOR] = 0;
  renderStates[D3DRS_STENCILENABLE] = FALSE;
  renderStates[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  renderStates[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  renderStates[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  renderStates[D3DRS_STENCILREF] = 0;
  renderStates[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
  renderStates[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
  renderStates[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
  renderStates[D3DRS_LIGHTING] = TRUE;
  renderStates[D3DRS_AMBIENT] = 0;
  renderStates[D3DRS_COLORVERTEX] = TRUE;
  renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  renderStates[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  renderStates[D3DRS_COLORWRITEENABLE] = 0x0Fu;
  renderStates[D3DRS_SCISSORTESTENABLE] = FALSE;
  renderStates[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  renderStates[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  renderStates[D3DRS_BLENDOPALPHA] = 1;

  memset(texStageStates, 0, sizeof(texStageStates));
  for (int s = 0; s < 8; ++s) {
    texStageStates[s][D3DTSS_COLOROP] = (s == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    texStageStates[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    texStageStates[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    texStageStates[s][D3DTSS_COLORARG0] = D3DTA_CURRENT;
    texStageStates[s][D3DTSS_ALPHAOP] = (s == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    texStageStates[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    texStageStates[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    texStageStates[s][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    texStageStates[s][D3DTSS_TEXCOORDINDEX] = (DWORD)s;
    texStageStates[s][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    texStageStates[s][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    texStageStates[s][D3DTSS_CONSTANT] = 0xFFFFFFFFu;
  }

  memset(samplerStates, 0, sizeof(samplerStates));
  for (int s = 0; s < 16; ++s) {
    samplerStates[s][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    samplerStates[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    samplerStates[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    samplerStates[s][D3DSAMP_MAGFILTER] = D3DTEXF_LINEAR;
    samplerStates[s][D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
    samplerStates[s][D3DSAMP_MIPFILTER] = D3DTEXF_LINEAR;
    samplerStates[s][D3DSAMP_MAXANISOTROPY] = 1;
  }

  for (int i = 0; i < 300; ++i) setIdentityMatrix(transforms[i]);
  memset(&material, 0, sizeof(material));
  material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = material.Diffuse.a = 1.0f;
  material.Ambient.r = material.Ambient.g = material.Ambient.b = material.Ambient.a = 1.0f;
  memset(lights, 0, sizeof(lights));
  memset(lightEnabled, 0, sizeof(lightEnabled));

  auto& ctx = GetWgpuContext();
  viewport = {0, 0, ctx.width, ctx.height, 0.0f, 1.0f};
  scissorRect = {0, 0, (long)ctx.width, (long)ctx.height};
}

void WgpuD3D9Device::ensureEncoder() {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready) return;
  if (!encoder) {
    encoder = ctx.device.CreateCommandEncoder();
  }
}

void WgpuD3D9Device::endActivePass() {
  if (pass) {
    pass.End();
    pass = nullptr;
  }
}

void WgpuD3D9Device::flushPendingClears() {
  if (pendingClearColor || pendingClearDepth || pendingClearStencil) {
    ensureRenderPass();
    endActivePass();
  }
}

void WgpuD3D9Device::ensureRenderPass() {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || pass) return;
  ensureEncoder();

  WgpuSurface* rt = curRT ? curRT : backbufferSurf;
  wgpu::TextureView colorView = rt ? rt->getView() : ctx.backbufferView;
  wgpu::TextureView depthView = (curDS && curDS->getView()) ? curDS->getView() : nullptr;

  // WebGPU requires color and depth attachments to have identical dimensions.
  if (rt && curDS && (rt->getWidth() != curDS->getWidth() || rt->getHeight() != curDS->getHeight())) {
    depthView = nullptr;
  }

  wgpu::RenderPassColorAttachment colorAtt{};
  colorAtt.view = colorView;
  colorAtt.loadOp = pendingClearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
  colorAtt.storeOp = wgpu::StoreOp::Store;
  colorAtt.clearValue = {clearColor[0], clearColor[1], clearColor[2], clearColor[3]};

  wgpu::RenderPassDepthStencilAttachment depthAtt{};
  if (depthView) {
    depthAtt.view = depthView;
    depthAtt.depthLoadOp = pendingClearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = clearZ;
    depthAtt.stencilLoadOp = pendingClearStencil ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = clearStencil;
  }

  wgpu::RenderPassDescriptor rpDesc{};
  rpDesc.colorAttachmentCount = 1;
  rpDesc.colorAttachments = &colorAtt;
  rpDesc.depthStencilAttachment = depthView ? &depthAtt : nullptr;

  pass = encoder.BeginRenderPass(&rpDesc);
  passHasDepth = (depthView != nullptr);
  pendingClearColor = false;
  pendingClearDepth = false;
  pendingClearStencil = false;
}

wgpu::Sampler WgpuD3D9Device::getOrCreateSampler(uint32_t stage) {
  WgpuSysHeapGuard _sysHeap;
  SamplerKey sk{};
  sk.addrU = (uint8_t)samplerStates[stage][D3DSAMP_ADDRESSU];
  sk.addrV = (uint8_t)samplerStates[stage][D3DSAMP_ADDRESSV];
  sk.addrW = (uint8_t)samplerStates[stage][D3DSAMP_ADDRESSW];
  sk.magFilter = (uint8_t)samplerStates[stage][D3DSAMP_MAGFILTER];
  sk.minFilter = (uint8_t)samplerStates[stage][D3DSAMP_MINFILTER];
  sk.mipFilter = (uint8_t)samplerStates[stage][D3DSAMP_MIPFILTER];
  uint64_t pk = sk.pack();
  auto it = samplerCache.find(pk);
  if (it != samplerCache.end()) return it->second;

  wgpu::SamplerDescriptor sd{};
  sd.addressModeU = toWgpuAddress(sk.addrU);
  sd.addressModeV = toWgpuAddress(sk.addrV);
  sd.addressModeW = toWgpuAddress(sk.addrW);
  sd.magFilter = toWgpuFilter(sk.magFilter);
  sd.minFilter = toWgpuFilter(sk.minFilter);
  sd.mipmapFilter = toWgpuMipFilter(sk.mipFilter);
  wgpu::Sampler s = GetWgpuContext().device.CreateSampler(&sd);
  samplerCache[pk] = s;
  return s;
}

wgpu::BindGroup WgpuD3D9Device::getOrCreateTexBindGroup(uint8_t* outTexTypes) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  TexBgKey key{};
  wgpu::TextureView views[4];
  wgpu::Sampler sams[4];
  uint8_t texTypes = 0;

  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t tid = 0;
    uint8_t st = 0;
    wgpu::TextureView v = ctx.whiteView;
    if (boundTex[i]) {
      D3DRESOURCETYPE rt = boundTex[i]->GetType();
      if (rt == D3DRTYPE_TEXTURE) {
        auto* t2d = static_cast<WgpuTexture2D*>(boundTex[i]);
        bool isFeedback = (curRT && curRT->parentTex == t2d);
        if (!isFeedback && t2d->fullView) {
          v = t2d->fullView;
          tid = t2d->uniqueId;
        }
      } else if (rt == D3DRTYPE_CUBETEXTURE) {
        st = 1;
        auto* tcube = static_cast<WgpuCubeTexture*>(boundTex[i]);
        v = tcube->cubeView ? tcube->cubeView : ctx.whiteCubeView;
        tid = tcube->uniqueId;
      } else if (rt == D3DRTYPE_VOLUMETEXTURE) {
        st = 2;
        auto* tvol = static_cast<WgpuVolumeTexture*>(boundTex[i]);
        v = tvol->volView ? tvol->volView : ctx.whiteVolView;
        tid = tvol->uniqueId;
      }
    }
    texTypes |= (uint8_t)(st << (i * 2));
    views[i] = v;
    key.texIds[i] = tid;

    SamplerKey sk{};
    sk.addrU = (uint8_t)samplerStates[i][D3DSAMP_ADDRESSU];
    sk.addrV = (uint8_t)samplerStates[i][D3DSAMP_ADDRESSV];
    sk.addrW = (uint8_t)samplerStates[i][D3DSAMP_ADDRESSW];
    sk.magFilter = (uint8_t)samplerStates[i][D3DSAMP_MAGFILTER];
    sk.minFilter = (uint8_t)samplerStates[i][D3DSAMP_MINFILTER];
    sk.mipFilter = (uint8_t)samplerStates[i][D3DSAMP_MIPFILTER];
    key.samIds[i] = sk.pack();
    sams[i] = getOrCreateSampler(i);
  }
  key.texTypes = texTypes;
  if (outTexTypes) *outTexTypes = texTypes;

  auto it = texBgCache.find(key);
  if (it != texBgCache.end()) return it->second;

  if (texBgCache.size() > 2048) texBgCache.clear();

  wgpu::BindGroupEntry entries[8]{};
  for (uint32_t i = 0; i < 4; ++i) {
    entries[i].binding = i;
    entries[i].textureView = views[i];
  }
  for (uint32_t i = 0; i < 4; ++i) {
    entries[4 + i].binding = 4 + i;
    entries[4 + i].sampler = sams[i];
  }
  wgpu::BindGroupDescriptor bgd{};
  bgd.layout = ctx.getOrCreateTexBgl(texTypes);
  bgd.entryCount = 8;
  bgd.entries = entries;
  wgpu::BindGroup bg = ctx.device.CreateBindGroup(&bgd);
  texBgCache[key] = bg;
  return bg;
}

wgpu::ShaderModule WgpuD3D9Device::getOrCreateProgramShader(const WgpuVertexShader* vs,
                                                            uint64_t vsInputTypeKey,
                                                            const FfpVertexLayout& ffpLayout,
                                                            const WgpuPixelShader* ps,
                                                            uint8_t texTypes) {
  WgpuSysHeapGuard _sysHeap;
  ProgramCacheKey pk{};
  pk.vsId = vs ? vs->uniqueId : 0;
  pk.psId = ps ? ps->uniqueId : 0;
  pk.vsKey = vs ? vsInputTypeKey : ffpLayout.shaderKey();
  pk.texTypes = texTypes;

  auto it = shaderCache.find(pk);
  if (it != shaderCache.end()) return it->second;

  std::string code = GenerateCombinedWGSL(vs, vsInputTypeKey, ffpLayout, ps, texTypes);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor smd{};
  smd.nextInChain = &wgsl;
  wgpu::ShaderModule mod = GetWgpuContext().device.CreateShaderModule(&smd);
  shaderCache[pk] = mod;
  return mod;
}

wgpu::RenderPipeline WgpuD3D9Device::getOrCreateFfpPipeline(const FfpPipelineKey& key,
                                                            const wgpu::VertexAttribute* sm1Attrs,
                                                            uint32_t sm1AttrCount) {
  WgpuSysHeapGuard _sysHeap;
  auto it = pipeCache.find(key);
  if (it != pipeCache.end()) return it->second;

  auto& ctx = GetWgpuContext();
  const auto* vs = static_cast<const WgpuVertexShader*>(boundVS);
  const auto* ps = static_cast<const WgpuPixelShader*>(boundPS);
  wgpu::ShaderModule mod = getOrCreateProgramShader(vs, key.vsInputTypeKey, key.vtx, ps, key.texTypes);

  wgpu::VertexAttribute ffpAttrs[8]{};
  uint32_t attrCount = 0;
  const wgpu::VertexAttribute* activeAttrs = nullptr;

  if (key.vsId != 0) {
    activeAttrs = sm1Attrs;
    attrCount = sm1AttrCount;
  } else {
    auto pushAttr = [&](const FfpVertexAttr& a) {
      if (!a.present) return;
      ffpAttrs[attrCount].shaderLocation = attrCount;
      ffpAttrs[attrCount].offset = a.offset;
      ffpAttrs[attrCount].format = ToWgpuVertexFormat(a.format);
      ++attrCount;
    };
    pushAttr(key.vtx.pos);
    pushAttr(key.vtx.normal);
    pushAttr(key.vtx.diffuse);
    pushAttr(key.vtx.specular);
    for (int i = 0; i < 4; ++i) pushAttr(key.vtx.tex[i]);
    activeAttrs = ffpAttrs;
  }

  wgpu::VertexBufferLayout vbLayout{};
  vbLayout.arrayStride = key.stride ? key.stride : key.vtx.minStride;
  vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vbLayout.attributeCount = attrCount;
  vbLayout.attributes = activeAttrs;

  wgpu::ColorTargetState colorTarget{};
  colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
  wgpu::ColorWriteMask mask = wgpu::ColorWriteMask::None;
  if (key.colorWriteMask & 1) mask |= wgpu::ColorWriteMask::Red;
  if (key.colorWriteMask & 2) mask |= wgpu::ColorWriteMask::Green;
  if (key.colorWriteMask & 4) mask |= wgpu::ColorWriteMask::Blue;
  if (key.colorWriteMask & 8) mask |= wgpu::ColorWriteMask::Alpha;
  colorTarget.writeMask = mask;

  wgpu::BlendState blendState{};
  if (key.blendEnable) {
    blendState.color.srcFactor = toWgpuBlendFactor(key.srcBlend, false);
    blendState.color.dstFactor = toWgpuBlendFactor(key.dstBlend, false);
    blendState.color.operation = toWgpuBlendOp(key.blendOp);
    if (key.sepAlphaBlend) {
      blendState.alpha.srcFactor = toWgpuBlendFactor(key.srcBlendAlpha, true);
      blendState.alpha.dstFactor = toWgpuBlendFactor(key.dstBlendAlpha, true);
      blendState.alpha.operation = toWgpuBlendOp(key.blendOpAlpha);
    } else {
      blendState.alpha.srcFactor = toWgpuBlendFactor(key.srcBlend, true);
      blendState.alpha.dstFactor = toWgpuBlendFactor(key.dstBlend, true);
      blendState.alpha.operation = toWgpuBlendOp(key.blendOp);
    }
    colorTarget.blend = &blendState;
  }

  wgpu::FragmentState frag{};
  frag.module = mod;
  frag.entryPoint = "fs_main";
  frag.targetCount = 1;
  frag.targets = &colorTarget;

  wgpu::DepthStencilState ds{};
  if (key.hasDepth) {
    ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
    ds.depthWriteEnabled = (key.zEnable && key.zWrite) ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
    ds.depthCompare = key.zEnable ? toWgpuCompare(key.zFunc) : wgpu::CompareFunction::Always;
    if (key.stencilEnable) {
      ds.stencilReadMask = key.stencilReadMask;
      ds.stencilWriteMask = key.stencilWriteMask;
      ds.stencilFront.compare = toWgpuCompare(key.stencilFunc);
      ds.stencilFront.failOp = toWgpuStencilOp(key.stencilFail);
      ds.stencilFront.depthFailOp = toWgpuStencilOp(key.stencilZFail);
      ds.stencilFront.passOp = toWgpuStencilOp(key.stencilPass);
      if (key.twoSidedStencil) {
        ds.stencilBack.compare = toWgpuCompare(key.ccwStencilFunc);
        ds.stencilBack.failOp = toWgpuStencilOp(key.ccwStencilFail);
        ds.stencilBack.depthFailOp = toWgpuStencilOp(key.ccwStencilZFail);
        ds.stencilBack.passOp = toWgpuStencilOp(key.ccwStencilPass);
      } else {
        ds.stencilBack = ds.stencilFront;
      }
    }
  }

  wgpu::RenderPipelineDescriptor rp{};
  rp.layout = ctx.getOrCreatePipeLayout(key.texTypes);
  rp.vertex.module = mod;
  rp.vertex.entryPoint = "vs_main";
  rp.vertex.bufferCount = attrCount > 0 ? 1 : 0;
  rp.vertex.buffers = attrCount > 0 ? &vbLayout : nullptr;
  rp.fragment = &frag;
  rp.depthStencil = key.hasDepth ? &ds : nullptr;

  switch (key.topology) {
    case D3DPT_POINTLIST:     rp.primitive.topology = wgpu::PrimitiveTopology::PointList; break;
    case D3DPT_LINELIST:      rp.primitive.topology = wgpu::PrimitiveTopology::LineList; break;
    case D3DPT_LINESTRIP:     rp.primitive.topology = wgpu::PrimitiveTopology::LineStrip; break;
    case D3DPT_TRIANGLESTRIP: rp.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip; break;
    default:                  rp.primitive.topology = wgpu::PrimitiveTopology::TriangleList; break;
  }
  if (key.indexedStrip && (key.topology == D3DPT_TRIANGLESTRIP || key.topology == D3DPT_LINESTRIP)) {
    rp.primitive.stripIndexFormat = wgpu::IndexFormat::Uint16;
  }
  // D3D9 screen-space winding matches WebGPU/Vulkan framebuffer winding:
  // FrontFace::CCW => CullMode::Back culls CW triangles (D3DCULL_CW), CullMode::Front culls CCW (D3DCULL_CCW).
  rp.primitive.frontFace = wgpu::FrontFace::CCW;
  if (key.cullMode == D3DCULL_CW) {
    rp.primitive.cullMode = wgpu::CullMode::Back;
  } else if (key.cullMode == D3DCULL_CCW) {
    rp.primitive.cullMode = wgpu::CullMode::Front;
  } else {
    rp.primitive.cullMode = wgpu::CullMode::None;
  }

  wgpu::RenderPipeline p = ctx.device.CreateRenderPipeline(&rp);
  pipeCache[key] = p;
  return p;
}

void WgpuD3D9Device::fillFfpUniforms(FfpDrawUniforms& u) {
  memset(&u, 0, sizeof(u));
  memcpy(u.world, &transforms[D3DTS_WORLD], 64);
  memcpy(u.view, &transforms[D3DTS_VIEW], 64);
  memcpy(u.proj, &transforms[D3DTS_PROJECTION], 64);
  for (int s = 0; s < 4; ++s) {
    memcpy(u.texMat[s], &transforms[D3DTS_TEXTURE0 + s], 64);
  }
  u.matDiffuse[0] = material.Diffuse.r; u.matDiffuse[1] = material.Diffuse.g;
  u.matDiffuse[2] = material.Diffuse.b; u.matDiffuse[3] = material.Diffuse.a;
  u.matAmbient[0] = material.Ambient.r; u.matAmbient[1] = material.Ambient.g;
  u.matAmbient[2] = material.Ambient.b; u.matAmbient[3] = material.Ambient.a;
  u.matEmissive[0] = material.Emissive.r; u.matEmissive[1] = material.Emissive.g;
  u.matEmissive[2] = material.Emissive.b; u.matEmissive[3] = material.Emissive.a;

  unpackD3DColor(renderStates[D3DRS_AMBIENT], u.ambientColor);
  unpackD3DColor(renderStates[D3DRS_TEXTUREFACTOR], u.tfactor);
  unpackD3DColor(renderStates[D3DRS_FOGCOLOR], u.fogColor);

  float fogStart = 0.0f, fogEnd = 0.0f;
  memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], 4);
  memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], 4);
  u.fogParams[0] = renderStates[D3DRS_FOGENABLE] ? 1.0f : 0.0f;
  u.fogParams[1] = fogStart;
  u.fogParams[2] = fogEnd;
  u.fogParams[3] = (float)renderStates[D3DRS_FOGVERTEXMODE];

  u.viewport[0] = (float)viewport.X;
  u.viewport[1] = (float)viewport.Y;
  u.viewport[2] = (float)(viewport.Width ? viewport.Width : 1);
  u.viewport[3] = (float)(viewport.Height ? viewport.Height : 1);

  u.alphaTest[0] = renderStates[D3DRS_ALPHATESTENABLE] ? 1.0f : 0.0f;
  u.alphaTest[1] = (float)renderStates[D3DRS_ALPHAFUNC];
  u.alphaTest[2] = (renderStates[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
  u.alphaTest[3] = renderStates[D3DRS_LIGHTING] ? 1.0f : 0.0f;

  u.matSources[0] = renderStates[D3DRS_COLORVERTEX] ? 1.0f : 0.0f;
  u.matSources[1] = (float)renderStates[D3DRS_DIFFUSEMATERIALSOURCE];
  u.matSources[2] = (float)renderStates[D3DRS_AMBIENTMATERIALSOURCE];
  u.matSources[3] = (float)renderStates[D3DRS_EMISSIVEMATERIALSOURCE];

  int dstLight = 0;
  for (int i = 0; i < 8 && dstLight < 4; ++i) {
    if (!lightEnabled[i]) continue;
    const D3DLIGHT9& L = lights[i];
    if (L.Type == D3DLIGHT_DIRECTIONAL) {
      float dx = -L.Direction.x, dy = -L.Direction.y, dz = -L.Direction.z;
      float len = sqrtf(dx * dx + dy * dy + dz * dz);
      if (len > 1e-5f) { dx /= len; dy /= len; dz /= len; }
      u.lightPos[dstLight][0] = dx;
      u.lightPos[dstLight][1] = dy;
      u.lightPos[dstLight][2] = dz;
      u.lightPos[dstLight][3] = (float)D3DLIGHT_DIRECTIONAL;
    } else {
      u.lightPos[dstLight][0] = L.Position.x;
      u.lightPos[dstLight][1] = L.Position.y;
      u.lightPos[dstLight][2] = L.Position.z;
      u.lightPos[dstLight][3] = (float)(L.Type ? L.Type : D3DLIGHT_POINT);
    }
    u.lightDiff[dstLight][0] = L.Diffuse.r;
    u.lightDiff[dstLight][1] = L.Diffuse.g;
    u.lightDiff[dstLight][2] = L.Diffuse.b;
    u.lightDiff[dstLight][3] = L.Range > 0.0f ? L.Range : 1e6f;
    u.lightAtten[dstLight][0] = L.Attenuation0;
    u.lightAtten[dstLight][1] = L.Attenuation1;
    u.lightAtten[dstLight][2] = L.Attenuation2;
    ++dstLight;
  }

  for (int s = 0; s < 4; ++s) {
    u.stageColor[s][0] = texStageStates[s][D3DTSS_COLOROP];
    u.stageColor[s][1] = texStageStates[s][D3DTSS_COLORARG1];
    u.stageColor[s][2] = texStageStates[s][D3DTSS_COLORARG2];
    u.stageColor[s][3] = texStageStates[s][D3DTSS_COLORARG0];

    u.stageAlpha[s][0] = texStageStates[s][D3DTSS_ALPHAOP];
    u.stageAlpha[s][1] = texStageStates[s][D3DTSS_ALPHAARG1];
    u.stageAlpha[s][2] = texStageStates[s][D3DTSS_ALPHAARG2];
    u.stageAlpha[s][3] = texStageStates[s][D3DTSS_ALPHAARG0];

    DWORD tciRaw = texStageStates[s][D3DTSS_TEXCOORDINDEX];
    u.stageTex[s][0] = tciRaw & 0xFFFFu;
    u.stageTex[s][1] = tciRaw & 0xFFFF0000u;
    u.stageTex[s][2] = texStageStates[s][D3DTSS_TEXTURETRANSFORMFLAGS];
    u.stageTex[s][3] = texStageStates[s][D3DTSS_RESULTARG];

    unpackD3DColor(texStageStates[s][D3DTSS_CONSTANT], u.stageConst[s]);
    memcpy(&u.bumpMat[s][0], &texStageStates[s][D3DTSS_BUMPENVMAT00], 4);
    memcpy(&u.bumpMat[s][1], &texStageStates[s][D3DTSS_BUMPENVMAT01], 4);
    memcpy(&u.bumpMat[s][2], &texStageStates[s][D3DTSS_BUMPENVMAT10], 4);
    memcpy(&u.bumpMat[s][3], &texStageStates[s][D3DTSS_BUMPENVMAT11], 4);
  }
  memcpy(u.vsConst, vsConsts, 96 * 16);
  memcpy(u.psConst, psConsts, 8 * 16);
}

static uint32_t primCountToIndexCount(D3DPRIMITIVETYPE pt, uint32_t primCount) {
  switch (pt) {
    case D3DPT_POINTLIST:     return primCount;
    case D3DPT_LINELIST:      return primCount * 2u;
    case D3DPT_LINESTRIP:     return primCount + 1u;
    case D3DPT_TRIANGLELIST:  return primCount * 3u;
    case D3DPT_TRIANGLESTRIP: return primCount + 2u;
    case D3DPT_TRIANGLEFAN:   return primCount + 2u;
    default:                  return 0;
  }
}

void WgpuD3D9Device::emitDraw(D3DPRIMITIVETYPE primType, bool indexed,
                              int32_t baseVertex, uint32_t startIndexOrVertex, uint32_t primCount,
                              const void* upVerts, uint32_t upStride, uint32_t upVertCount,
                              const void* upIndices, D3DFORMAT upIdxFmt) {
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || primCount == 0) return;

  // Determine vertex layout & stride.
  FfpVertexLayout layout{};
  uint32_t stride = 0;
  if (boundDecl) {
    layout = boundDecl->ffpLayout;
  } else if (boundFVF) {
    layout = BuildFfpLayoutFromFVF(boundFVF);
  } else {
    return;
  }

  wgpu::Buffer vbGpu;
  uint32_t vbOffset = 0;
  uint32_t vbSize = 0;

  if (upVerts) {
    stride = upStride ? upStride : layout.minStride;
    uint32_t bytes = upVertCount * stride;
    if (bytes == 0) return;
    void* dst = nullptr;
    vbOffset = ctx.dynVertRing.alloc(bytes, 16, &dst);
    if (!dst) return;
    memcpy(dst, upVerts, bytes);
    vbGpu = ctx.dynVertRing.gpu;
    vbSize = (bytes + 3u) & ~3u;
  } else {
    WgpuVertexBuffer* vb = streams[0].vb;
    if (!vb) return;
    stride = streams[0].stride ? streams[0].stride : layout.minStride;
    if (vb->isDynamic) {
      if (!vb->ringValid || vb->ringFrame != ctx.frameCount) return;
      vbGpu = ctx.dynVertRing.gpu;
      vbOffset = vb->ringBaseOffset + streams[0].offset;
      vbSize = (vb->length > streams[0].offset) ? (vb->length - streams[0].offset) : 0;
    } else {
      vbGpu = vb->staticGpu;
      vbOffset = streams[0].offset;
      vbSize = (vb->length > streams[0].offset) ? (vb->length - streams[0].offset) : 0;
    }
    vbSize = (vbSize + 3u) & ~3u;
    if (!vbGpu || vbSize == 0) return;
  }

  // Handle TRIANGLEFAN expansion into TriangleList via dynamic index ring.
  D3DPRIMITIVETYPE drawType = primType;
  wgpu::Buffer ibGpu;
  uint32_t ibOffset = 0;
  uint32_t ibSize = 0;
  uint32_t drawCount = primCountToIndexCount(primType, primCount);
  uint32_t firstIndex = startIndexOrVertex;
  int32_t drawBaseVertex = baseVertex;
  bool drawIndexed = indexed;

  if (primType == D3DPT_TRIANGLEFAN) {
    drawType = D3DPT_TRIANGLELIST;
    drawIndexed = true;
    uint32_t outIdxCount = primCount * 3u;
    void* dst = nullptr;
    ibOffset = ctx.dynIdxRing.alloc(outIdxCount * 2u, 4, &dst);
    if (!dst) return;
    uint16_t* out = reinterpret_cast<uint16_t*>(dst);

    if (!indexed) {
      for (uint32_t t = 0; t < primCount; ++t) {
        out[t * 3 + 0] = (uint16_t)startIndexOrVertex;
        out[t * 3 + 1] = (uint16_t)(startIndexOrVertex + t + 1);
        out[t * 3 + 2] = (uint16_t)(startIndexOrVertex + t + 2);
      }
      drawBaseVertex = 0;
    } else {
      const uint16_t* srcIdx = nullptr;
      if (upIndices) {
        srcIdx = reinterpret_cast<const uint16_t*>(upIndices);
      } else if (boundIB) {
        if (boundIB->isDynamic && boundIB->ringValid) {
          srcIdx = reinterpret_cast<const uint16_t*>(ctx.dynIdxRing.cpu.data() + boundIB->ringBaseOffset);
        } else if (!boundIB->cpuShadow.empty()) {
          srcIdx = reinterpret_cast<const uint16_t*>(boundIB->cpuShadow.data());
        }
      }
      if (!srcIdx) return;
      for (uint32_t t = 0; t < primCount; ++t) {
        out[t * 3 + 0] = srcIdx[startIndexOrVertex];
        out[t * 3 + 1] = srcIdx[startIndexOrVertex + t + 1];
        out[t * 3 + 2] = srcIdx[startIndexOrVertex + t + 2];
      }
    }
    ibGpu = ctx.dynIdxRing.gpu;
    ibSize = (outIdxCount * 2u + 3u) & ~3u;
    firstIndex = 0;
    drawCount = outIdxCount;
  } else if (indexed) {
    if (upIndices) {
      uint32_t idxBytes = drawCount * (upIdxFmt == D3DFMT_INDEX32 ? 4u : 2u);
      void* dst = nullptr;
      ibOffset = ctx.dynIdxRing.alloc(idxBytes, 4, &dst);
      if (!dst) return;
      memcpy(dst, upIndices, idxBytes);
      ibGpu = ctx.dynIdxRing.gpu;
      ibSize = (idxBytes + 3u) & ~3u;
      firstIndex = 0;
    } else {
      if (!boundIB) return;
      if (boundIB->isDynamic) {
        if (!boundIB->ringValid || boundIB->ringFrame != ctx.frameCount) return;
        ibGpu = ctx.dynIdxRing.gpu;
        ibOffset = boundIB->ringBaseOffset;
      } else {
        ibGpu = boundIB->staticGpu;
        ibOffset = 0;
      }
      ibSize = (boundIB->length + 3u) & ~3u;
      if (!ibGpu || ibSize == 0) return;
    }
  }

  ensureRenderPass();
  if (!pass) return;

  uint8_t texTypes = 0;
  wgpu::BindGroup texBg = getOrCreateTexBindGroup(&texTypes);

  const auto* activeVS = static_cast<const WgpuVertexShader*>(boundVS);
  const auto* activePS = static_cast<const WgpuPixelShader*>(boundPS);

  uint64_t vsInputTypeKey = 0;
  wgpu::VertexAttribute sm1Attrs[16]{};
  uint32_t sm1AttrCount = 0;
  if (activeVS) {
    bool usedReg[16]{};
    for (const Sm1VsDclInput& d : activeVS->dcls) {
      if (d.reg >= 16 || usedReg[d.reg]) continue;
      uint8_t fmt = WGPU_VTX_NONE;
      uint16_t off = 0;
      if (boundDecl) {
        for (const D3DVERTEXELEMENT9& e : boundDecl->elements) {
          if (e.Stream == 0xFF || e.Type == D3DDECLTYPE_UNUSED) break;
          if (e.Stream != 0) continue;
          bool usageMatch = (e.Usage == d.usage) ||
                            (e.Usage == D3DDECLUSAGE_POSITIONT && d.usage == D3DDECLUSAGE_POSITION);
          if (usageMatch && e.UsageIndex == d.usageIndex) {
            uint16_t sz = 0;
            fmt = MapD3DDeclTypeToVtxFmt(e.Type, &sz);
            off = e.Offset;
            break;
          }
        }
      } else {
        if (d.usage == D3DDECLUSAGE_POSITION && d.usageIndex == 0 && layout.pos.present) {
          fmt = layout.pos.format; off = layout.pos.offset;
        } else if (d.usage == D3DDECLUSAGE_NORMAL && d.usageIndex == 0 && layout.normal.present) {
          fmt = layout.normal.format; off = layout.normal.offset;
        } else if (d.usage == D3DDECLUSAGE_COLOR && d.usageIndex == 0 && layout.diffuse.present) {
          fmt = layout.diffuse.format; off = layout.diffuse.offset;
        } else if (d.usage == D3DDECLUSAGE_COLOR && d.usageIndex == 1 && layout.specular.present) {
          fmt = layout.specular.format; off = layout.specular.offset;
        } else if (d.usage == D3DDECLUSAGE_TEXCOORD && d.usageIndex < 4 && layout.tex[d.usageIndex].present) {
          fmt = layout.tex[d.usageIndex].format; off = layout.tex[d.usageIndex].offset;
        }
      }
      if (fmt != WGPU_VTX_NONE) {
        usedReg[d.reg] = true;
        vsInputTypeKey |= ((uint64_t)fmt << (d.reg * 4));
        sm1Attrs[sm1AttrCount].shaderLocation = d.reg;
        sm1Attrs[sm1AttrCount].offset = off;
        sm1Attrs[sm1AttrCount].format = ToWgpuVertexFormat(fmt);
        ++sm1AttrCount;
      }
    }
  }

  // Build pipeline key.
  FfpPipelineKey pk{};
  pk.vtx = layout;
  pk.vsInputTypeKey = vsInputTypeKey;
  pk.vsId = activeVS ? activeVS->uniqueId : 0;
  pk.psId = activePS ? activePS->uniqueId : 0;
  pk.declId = boundDecl ? boundDecl->uniqueId : 0;
  pk.stride = (uint16_t)stride;
  pk.texTypes = texTypes;
  pk.topology = (uint8_t)drawType;
  pk.cullMode = (uint8_t)((!activeVS && layout.isRHW) ? D3DCULL_NONE : renderStates[D3DRS_CULLMODE]);
  pk.zEnable = (uint8_t)(renderStates[D3DRS_ZENABLE] ? 1 : 0);
  pk.zWrite = (uint8_t)(renderStates[D3DRS_ZWRITEENABLE] ? 1 : 0);
  pk.zFunc = (uint8_t)renderStates[D3DRS_ZFUNC];
  pk.hasDepth = passHasDepth ? 1 : 0;
  pk.blendEnable = (uint8_t)(renderStates[D3DRS_ALPHABLENDENABLE] ? 1 : 0);
  pk.srcBlend = (uint8_t)renderStates[D3DRS_SRCBLEND];
  pk.dstBlend = (uint8_t)renderStates[D3DRS_DESTBLEND];
  pk.blendOp = (uint8_t)renderStates[D3DRS_BLENDOP];
  pk.sepAlphaBlend = (uint8_t)(renderStates[D3DRS_SEPARATEALPHABLENDENABLE] ? 1 : 0);
  pk.srcBlendAlpha = (uint8_t)renderStates[D3DRS_SRCBLENDALPHA];
  pk.dstBlendAlpha = (uint8_t)renderStates[D3DRS_DESTBLENDALPHA];
  pk.blendOpAlpha = (uint8_t)renderStates[D3DRS_BLENDOPALPHA];
  pk.colorWriteMask = (uint8_t)(renderStates[D3DRS_COLORWRITEENABLE] & 0x0Fu);
  pk.stencilEnable = (uint8_t)(renderStates[D3DRS_STENCILENABLE] ? 1 : 0);
  pk.stencilFunc = (uint8_t)renderStates[D3DRS_STENCILFUNC];
  pk.stencilFail = (uint8_t)renderStates[D3DRS_STENCILFAIL];
  pk.stencilZFail = (uint8_t)renderStates[D3DRS_STENCILZFAIL];
  pk.stencilPass = (uint8_t)renderStates[D3DRS_STENCILPASS];
  pk.stencilReadMask = (uint8_t)(renderStates[D3DRS_STENCILMASK] & 0xFFu);
  pk.stencilWriteMask = (uint8_t)(renderStates[D3DRS_STENCILWRITEMASK] & 0xFFu);
  pk.twoSidedStencil = (uint8_t)(renderStates[D3DRS_TWOSIDEDSTENCILMODE] ? 1 : 0);
  pk.ccwStencilFunc = (uint8_t)renderStates[D3DRS_CCW_STENCILFUNC];
  pk.ccwStencilFail = (uint8_t)renderStates[D3DRS_CCW_STENCILFAIL];
  pk.ccwStencilZFail = (uint8_t)renderStates[D3DRS_CCW_STENCILZFAIL];
  pk.ccwStencilPass = (uint8_t)renderStates[D3DRS_CCW_STENCILPASS];
  pk.indexedStrip = drawIndexed ? 1 : 0;

  // Every submitted draw takes the next id, before any debug suppression, so ids stay stable.
  const uint32_t drawId = g_drawSeq++;
  if (g_dbgSkipLo >= 0 && drawId >= (uint32_t)g_dbgSkipLo && drawId < (uint32_t)g_dbgSkipHi) return;

  wgpu::RenderPipeline pipe = getOrCreateFfpPipeline(pk, sm1Attrs, sm1AttrCount);
  if (!pipe) return;

  // Allocate & populate per-draw FFP + SM 1.1 uniform block in dynamic uniform ring (256-byte aligned).
  void* uboPtr = nullptr;
  uint32_t uboOffset = ctx.dynUniformRing.alloc(sizeof(FfpDrawUniforms), 256, &uboPtr);
  if (!uboPtr) return;
  auto* ubo = reinterpret_cast<FfpDrawUniforms*>(uboPtr);
  fillFfpUniforms(*ubo);

  if (g_wgpuDebugPsMode == 4 || g_dbgRecordDraws) {
    ubo->dbg[0] = (float)drawId;
    DbgDrawRec r{};
    r.id = drawId;
    r.tris = primCountToIndexCount(drawType, primCount) / 3u;
    r.stride = (uint32_t)stride;
    r.declId = pk.declId; r.vsId = pk.vsId; r.psId = pk.psId; r.fvf = boundFVF;
    r.vtxKey = pk.vtx.shaderKey();
    for (int s = 0; s < 4; ++s) {
      const DWORD tci = texStageStates[s][D3DTSS_TEXCOORDINDEX];
      r.uvIdx[s] = tci & 0xFFFFu;
      r.colOp[s] = texStageStates[s][D3DTSS_COLOROP];
      r.colA1[s] = texStageStates[s][D3DTSS_COLORARG1];
      r.colA2[s] = texStageStates[s][D3DTSS_COLORARG2];
      r.texNull[s] = boundTex[s] ? 0 : 1;
      if (boundTex[s] && boundTex[s]->GetType() == D3DRTYPE_TEXTURE) {
        auto* t2 = static_cast<WgpuTexture2D*>(boundTex[s]);
        r.texId[s] = (uint32_t)t2->uniqueId; r.texW[s] = t2->width; r.texH[s] = t2->height;
        r.texFmt[s] = (uint32_t)t2->d3dFormat;
      }
    }
    r.zEnable = renderStates[D3DRS_ZENABLE] ? 1u : 0u;
    r.zFunc = (uint32_t)renderStates[D3DRS_ZFUNC];
    r.blendEnable = renderStates[D3DRS_ALPHABLENDENABLE] ? 1u : 0u;
    r.srcBlend = (uint32_t)renderStates[D3DRS_SRCBLEND];
    r.dstBlend = (uint32_t)renderStates[D3DRS_DESTBLEND];
    r.alphaTest = renderStates[D3DRS_ALPHATESTENABLE] ? 1u : 0u;
    r.alphaRef = (uint32_t)renderStates[D3DRS_ALPHAREF];
    r.vpW = viewport.Width; r.vpH = viewport.Height;
    if (g_dbgRecCount[g_dbgRecBuf] < kDbgRecMax) g_dbgRecs[g_dbgRecBuf][g_dbgRecCount[g_dbgRecBuf]++] = r;
    else ++g_dbgRecOverflow[g_dbgRecBuf];
  }

  // Clamp viewport & scissor to active RT dimensions (WebGPU strict validation).
  WgpuSurface* rt = curRT ? curRT : backbufferSurf;
  uint32_t rtW = rt ? rt->getWidth() : ctx.width;
  uint32_t rtH = rt ? rt->getHeight() : ctx.height;
  uint32_t vpX = std::min(viewport.X, rtW > 0 ? rtW - 1 : 0);
  uint32_t vpY = std::min(viewport.Y, rtH > 0 ? rtH - 1 : 0);
  uint32_t vpW = std::min(viewport.Width ? viewport.Width : rtW, rtW - vpX);
  uint32_t vpH = std::min(viewport.Height ? viewport.Height : rtH, rtH - vpY);
  float minZ = std::max(0.0f, std::min(1.0f, viewport.MinZ));
  float maxZ = std::max(minZ, std::min(1.0f, viewport.MaxZ));
  if (vpW == 0 || vpH == 0) return;

  pass.SetViewport((float)vpX, (float)vpY, (float)vpW, (float)vpH, minZ, maxZ);
  if (renderStates[D3DRS_SCISSORTESTENABLE]) {
    uint32_t sx = (uint32_t)std::max(0L, std::min((long)rtW, scissorRect.left));
    uint32_t sy = (uint32_t)std::max(0L, std::min((long)rtH, scissorRect.top));
    uint32_t ex = (uint32_t)std::max((long)sx, std::min((long)rtW, scissorRect.right));
    uint32_t ey = (uint32_t)std::max((long)sy, std::min((long)rtH, scissorRect.bottom));
    if (ex == sx || ey == sy) return;
    pass.SetScissorRect(sx, sy, ex - sx, ey - sy);
  } else {
    pass.SetScissorRect(0, 0, rtW, rtH);
  }

  if (renderStates[D3DRS_STENCILENABLE]) {
    pass.SetStencilReference(renderStates[D3DRS_STENCILREF] & 0xFFu);
  }

  pass.SetPipeline(pipe);
  pass.SetBindGroup(0, ctx.ffpUniformBg, 1, &uboOffset);
  pass.SetBindGroup(1, texBg);
  pass.SetVertexBuffer(0, vbGpu, vbOffset, vbSize);

  if (drawIndexed) {
    wgpu::IndexFormat ifmt = (upIndices && upIdxFmt == D3DFMT_INDEX32)
                                 ? wgpu::IndexFormat::Uint32
                                 : wgpu::IndexFormat::Uint16;
    pass.SetIndexBuffer(ibGpu, ifmt, ibOffset, ibSize);
    pass.DrawIndexed(drawCount, 1, firstIndex, drawBaseVertex, 0);
  } else {
    pass.Draw(drawCount, 1, startIndexOrVertex, 0);
  }

  ++ctx.curFrameDraws;
  if (primType == D3DPT_TRIANGLELIST || primType == D3DPT_TRIANGLESTRIP || primType == D3DPT_TRIANGLEFAN) {
    ctx.curFrameTris += primCount;
  }
  bool isSkinned = false;
  if (boundDecl) {
    for (const D3DVERTEXELEMENT9& e : boundDecl->elements) {
      if (e.Stream == 0xFF || e.Type == D3DDECLTYPE_UNUSED) break;
      if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT || e.Usage == D3DDECLUSAGE_BLENDINDICES) {
        isSkinned = true;
        break;
      }
    }
  }
  if (!isSkinned && activeVS) {
    for (const Sm1VsDclInput& d : activeVS->dcls) {
      if (d.usage == D3DDECLUSAGE_BLENDWEIGHT || d.usage == D3DDECLUSAGE_BLENDINDICES) {
        isSkinned = true;
        break;
      }
    }
  }
  if (isSkinned) {
    ++ctx.curFrameSkinnedDraws;
    if (primType == D3DPT_TRIANGLELIST || primType == D3DPT_TRIANGLESTRIP || primType == D3DPT_TRIANGLEFAN) {
      ctx.curFrameSkinnedTris += primCount;
    }
  }
}

// ---------------------------------------------------------------------------
// IDirect3DDevice9 COM methods
// ---------------------------------------------------------------------------
STDMETHODIMP WgpuD3D9Device::QueryInterface(REFIID, void** ppvObj) {
  if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) WgpuD3D9Device::AddRef() { return (ULONG)++refCount; }
STDMETHODIMP_(ULONG) WgpuD3D9Device::Release() {
  int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
}

STDMETHODIMP WgpuD3D9Device::GetDirect3D(IDirect3D9** ppD3D9) {
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  *ppD3D9 = parentD3D;
  if (parentD3D) parentD3D->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetDeviceCaps(D3DCAPS9* pCaps) {
  if (!pCaps) return D3DERR_INVALIDCALL;
  fillWebGPUDeviceCaps(pCaps);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetDisplayMode(UINT, D3DDISPLAYMODE* pMode) {
  if (!pMode) return D3DERR_INVALIDCALL;
  auto& ctx = GetWgpuContext();
  pMode->Width = ctx.width;
  pMode->Height = ctx.height;
  pMode->RefreshRate = 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::Reset(D3DPRESENT_PARAMETERS* pp) {
  endActivePass();
  encoder = nullptr;
  auto& ctx = GetWgpuContext();
  if (pp) {
    presentParams = *pp;
    if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0 &&
        (pp->BackBufferWidth != ctx.width || pp->BackBufferHeight != ctx.height)) {
      ctx.recreateBackbuffer(pp->BackBufferWidth, pp->BackBufferHeight);
    }
  }
  if (backbufferSurf) {
    backbufferSurf->width = ctx.width;
    backbufferSurf->height = ctx.height;
  }
  if (depthStencilSurf) {
    depthStencilSurf->width = ctx.width;
    depthStencilSurf->height = ctx.height;
  }
  initDefaultStates();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready) return D3D_OK;

  ensureEncoder();
  // If frame had a Clear() with zero draws, execute the clear pass on backbuffer now.
  if (pendingClearColor || pendingClearDepth || pendingClearStencil) {
    ensureRenderPass();
  }
  endActivePass();

  // Blit internal RGBA8Unorm backbuffer onto the live OffscreenCanvas swapchain texture.
  if (ctx.surface && ctx.backbufferView && ctx.blitToSurfacePipe) {
    wgpu::SurfaceTexture st{};
    ctx.surface.GetCurrentTexture(&st);
    if (st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
        st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
      wgpu::TextureView surfView = st.texture.CreateView();

      void* uPtr = nullptr;
      uint32_t uOff = ctx.dynUniformRing.alloc(16, 256, &uPtr);
      if (uPtr) {
        float uvRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        memcpy(uPtr, uvRect, sizeof(uvRect));
      }

      wgpu::BindGroupEntry entries[3]{};
      entries[0].binding = 0;
      entries[0].sampler = ctx.linearClampSampler;
      entries[1].binding = 1;
      entries[1].textureView = ctx.backbufferView;
      entries[2].binding = 2;
      entries[2].buffer = ctx.dynUniformRing.gpu;
      entries[2].offset = 0;
      entries[2].size = 16;
      wgpu::BindGroupDescriptor bgd{};
      bgd.layout = ctx.blitBgl;
      bgd.entryCount = 3;
      bgd.entries = entries;
      wgpu::BindGroup blitBg = ctx.device.CreateBindGroup(&bgd);

      wgpu::RenderPassColorAttachment ca{};
      ca.view = surfView;
      ca.loadOp = wgpu::LoadOp::Clear;
      ca.storeOp = wgpu::StoreOp::Store;
      ca.clearValue = {0.0, 0.0, 0.0, 1.0};
      wgpu::RenderPassDescriptor rpd{};
      rpd.colorAttachmentCount = 1;
      rpd.colorAttachments = &ca;
      wgpu::RenderPassEncoder bp = encoder.BeginRenderPass(&rpd);
      bp.SetPipeline(ctx.blitToSurfacePipe);
      bp.SetBindGroup(0, blitBg, 1, &uOff);
      bp.Draw(3);
      bp.End();
    }
  }

  // Upload all dynamic per-frame rings once before Submit().
  ctx.dynVertRing.upload(ctx.queue);
  ctx.dynIdxRing.upload(ctx.queue);
  ctx.dynUniformRing.upload(ctx.queue);

  wgpu::CommandBuffer cmd = encoder.Finish();
  ctx.queue.Submit(1, &cmd);
  encoder = nullptr;

  if (g_wgpuDebugPsMode == 4 || g_dbgRecordDraws) {
    // The frame just submitted is what a later readback will sample, so keep its records readable.
    g_dbgRecBuf ^= 1;
    g_dbgRecCount[g_dbgRecBuf] = 0;
    g_dbgRecOverflow[g_dbgRecBuf] = 0;
  }
  g_drawSeq = 0;

  // Reset streaming rings and advance frame counter (never call surface.Present() on Web!).
  ctx.dynVertRing.reset();
  ctx.dynIdxRing.reset();
  ctx.dynUniformRing.reset();
  ctx.lastFrameDraws = ctx.curFrameDraws;
  ctx.lastFrameTris = ctx.curFrameTris;
  ctx.lastFrameSkinnedDraws = ctx.curFrameSkinnedDraws;
  ctx.lastFrameSkinnedTris = ctx.curFrameSkinnedTris;
  ctx.curFrameDraws = 0;
  ctx.curFrameTris = 0;
  ctx.curFrameSkinnedDraws = 0;
  ctx.curFrameSkinnedTris = 0;
  ++ctx.frameCount;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** ppBackBuffer) {
  if (!ppBackBuffer) return D3DERR_INVALIDCALL;
  *ppBackBuffer = backbufferSurf;
  if (backbufferSurf) backbufferSurf->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                           D3DFORMAT Format, D3DPOOL Pool,
                                           IDirect3DTexture9** ppTexture, HANDLE*) {
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = new WgpuTexture2D(this, Width, Height, Levels, Usage, Format, Pool);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels,
                                                 DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                 IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE*) {
  if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
  *ppVolumeTexture = new WgpuVolumeTexture(this, Width, Height, Depth, Levels, Usage, Format, Pool);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage,
                                               D3DFORMAT Format, D3DPOOL Pool,
                                               IDirect3DCubeTexture9** ppCubeTexture, HANDLE*) {
  if (!ppCubeTexture) return D3DERR_INVALIDCALL;
  *ppCubeTexture = new WgpuCubeTexture(this, EdgeLength, Levels, Usage, Format, Pool);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE*) {
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
  *ppVertexBuffer = new WgpuVertexBuffer(this, Length, Usage, FVF, Pool);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                               IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE*) {
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  *ppIndexBuffer = new WgpuIndexBuffer(this, Length, Usage, Format, Pool);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                                D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                                IDirect3DSurface9** ppSurface, HANDLE*) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = new WgpuSurface(this, Width, Height, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, false, false);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                       D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                                       IDirect3DSurface9** ppSurface, HANDLE*) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = new WgpuSurface(this, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false, true);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                         D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE*) {
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = new WgpuSurface(this, Width, Height, Format, 0, Pool, false, false);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::StretchRect(IDirect3DSurface9* pSrc, CONST RECT* pSrcRect,
                                         IDirect3DSurface9* pDst, CONST RECT* pDstRect, D3DTEXTUREFILTERTYPE) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !pSrc || !pDst || !ctx.blitToRgbaPipe) return D3D_OK;
  auto* src = static_cast<WgpuSurface*>(pSrc);
  auto* dst = static_cast<WgpuSurface*>(pDst);
  if (!src->getView() || !dst->getView() || src->getView().Get() == dst->getView().Get()) return D3D_OK;

  const uint32_t srcW = src->getWidth() > 0 ? src->getWidth() : 1u;
  const uint32_t srcH = src->getHeight() > 0 ? src->getHeight() : 1u;
  const uint32_t dstW = dst->getWidth() > 0 ? dst->getWidth() : 1u;
  const uint32_t dstH = dst->getHeight() > 0 ? dst->getHeight() : 1u;

  float sL = 0.0f, sT = 0.0f, sR = (float)srcW, sB = (float)srcH;
  if (pSrcRect) {
    sL = (float)pSrcRect->left;
    sT = (float)pSrcRect->top;
    sR = (float)pSrcRect->right;
    sB = (float)pSrcRect->bottom;
    if (sL < 0.0f) sL = 0.0f;
    if (sT < 0.0f) sT = 0.0f;
    if (sR > (float)srcW) sR = (float)srcW;
    if (sB > (float)srcH) sB = (float)srcH;
    if (sR <= sL || sB <= sT) return D3D_OK;
  }

  int32_t dL = 0, dT = 0, dR = (int32_t)dstW, dB = (int32_t)dstH;
  if (pDstRect) {
    dL = pDstRect->left;
    dT = pDstRect->top;
    dR = pDstRect->right;
    dB = pDstRect->bottom;
    if (dL < 0) dL = 0;
    if (dT < 0) dT = 0;
    if (dR > (int32_t)dstW) dR = (int32_t)dstW;
    if (dB > (int32_t)dstH) dB = (int32_t)dstH;
    if (dR <= dL || dB <= dT) return D3D_OK;
  }
  const bool fullDst = (dL == 0 && dT == 0 && dR == (int32_t)dstW && dB == (int32_t)dstH);

  flushPendingClears();
  endActivePass();
  ensureEncoder();

  void* uPtr = nullptr;
  uint32_t uOff = ctx.dynUniformRing.alloc(16, 256, &uPtr);
  if (uPtr) {
    float uvRect[4] = {
      sL / (float)srcW,
      sT / (float)srcH,
      (sR - sL) / (float)srcW,
      (sB - sT) / (float)srcH,
    };
    memcpy(uPtr, uvRect, sizeof(uvRect));
  }

  wgpu::BindGroupEntry entries[3]{};
  entries[0].binding = 0;
  entries[0].sampler = ctx.linearClampSampler;
  entries[1].binding = 1;
  entries[1].textureView = src->getView();
  entries[2].binding = 2;
  entries[2].buffer = ctx.dynUniformRing.gpu;
  entries[2].offset = 0;
  entries[2].size = 16;
  wgpu::BindGroupDescriptor bgd{};
  bgd.layout = ctx.blitBgl;
  bgd.entryCount = 3;
  bgd.entries = entries;
  wgpu::BindGroup bg = ctx.device.CreateBindGroup(&bgd);

  wgpu::RenderPassColorAttachment ca{};
  ca.view = dst->getView();
  ca.loadOp = fullDst ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
  ca.storeOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor rpd{};
  rpd.colorAttachmentCount = 1;
  rpd.colorAttachments = &ca;
  wgpu::RenderPassEncoder bp = encoder.BeginRenderPass(&rpd);
  bp.SetViewport((float)dL, (float)dT, (float)(dR - dL), (float)(dB - dT), 0.0f, 1.0f);
  bp.SetScissorRect((uint32_t)dL, (uint32_t)dT, (uint32_t)(dR - dL), (uint32_t)(dB - dT));
  bp.SetPipeline(ctx.blitToRgbaPipe);
  bp.SetBindGroup(0, bg, 1, &uOff);
  bp.Draw(3);
  bp.End();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT*, D3DCOLOR color) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !pSurface) return D3D_OK;
  auto* dst = static_cast<WgpuSurface*>(pSurface);
  if (!dst->getView()) return D3D_OK;
  flushPendingClears();
  endActivePass();
  ensureEncoder();

  float c[4];
  unpackD3DColor(color, c);
  wgpu::RenderPassColorAttachment ca{};
  ca.view = dst->getView();
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = {c[0], c[1], c[2], c[3]};
  wgpu::RenderPassDescriptor rpd{};
  rpd.colorAttachmentCount = 1;
  rpd.colorAttachments = &ca;
  wgpu::RenderPassEncoder rp = encoder.BeginRenderPass(&rpd);
  rp.End();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
  if (RenderTargetIndex != 0) return D3D_OK;
  auto* next = static_cast<WgpuSurface*>(pRenderTarget);
  if (next == curRT) return D3D_OK;
  flushPendingClears();
  endActivePass();
  if (next) next->AddRef();
  if (curRT) curRT->Release();
  curRT = next;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
  if (!ppRenderTarget) return D3DERR_INVALIDCALL;
  if (RenderTargetIndex != 0) { *ppRenderTarget = nullptr; return D3DERR_NOTAVAILABLE; }
  *ppRenderTarget = curRT ? curRT : backbufferSurf;
  if (*ppRenderTarget) (*ppRenderTarget)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
  auto* next = static_cast<WgpuSurface*>(pNewZStencil);
  if (next == curDS) return D3D_OK;
  flushPendingClears();
  endActivePass();
  if (next) next->AddRef();
  if (curDS) curDS->Release();
  curDS = next;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
  if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
  *ppZStencilSurface = curDS;
  if (curDS) curDS->AddRef();
  return D3D_OK;
}

int g_wgpuDebugPsMode = 0;

// Baked into the generated fragment shader just before its final return. sm11Coords selects between
// the two coordinate spellings the translators use: the SM 1.1 translator pre-computes a
// perspective-divided tc2_<stage> for every stage unconditionally, while the FFP translator carries
// in.uv<stage> in a fixed varying struct.
const char* WgpuDebugPsSnippet(bool sm11Coords) {
  switch (g_wgpuDebugPsMode) {
    case 1: return sm11Coords ? "  return vec4<f32>(fract(tc2_0), 0.0, 1.0);\n"
                              : "  return vec4<f32>(fract(in.uv0.xy), 0.0, 1.0);\n";
    case 2: return "  return vec4<f32>(1.0, 1.0, 1.0, 1.0);\n";
    default: return "";
  }
}

// ps mode 4: paint each draw with an id code so a screenshot can be interrogated pixel by pixel.
const char* WgpuDebugIdSnippet() {
  return "  let dbgIdU: u32 = u32(u.dbg.x + 0.5);\n"
         "  let dbgR: f32 = f32(dbgIdU % 256u) / 255.0;\n"
         "  let dbgG: f32 = f32((dbgIdU / 256u) % 256u) / 255.0;\n"
         "  let dbgB: f32 = f32((dbgIdU / 65536u) % 256u) / 255.0;\n"
         "  return vec4<f32>(dbgR, dbgG, dbgB, 1.0);\n";
}



// Debug aid for the "why is this part of the world black?" question, driven from JS via
// swbf2_wgpu_debug_set_clear_colour(). Forcing every D3DCLEAR_TARGET to a colour the scene never
// produces separates the two possible answers at a glance: a region that shows the forced colour means
// nothing was drawn over it (a hole - we are looking at the clear), while a region that stays black
// means geometry really was submitted and shaded black.
//
// The colour has to be *dim*. The engine runs an HDR scene target through HDREffect bloom, and a
// full-intensity magenta clear passes the bright-pass threshold over the whole frame, so every pixel
// - geometry included - accumulates glow and the answer is destroyed (measured: 0,0,1 clear turned the
// entire Tatooine frame white). Keep each channel well under the bloom threshold.
//
// It stays off unless something turns it on, so no check can pass by accident.
static bool  g_forceClearColor = false;
static float g_forcedClear[4] = { 0.3f, 0.0f, 0.3f, 1.0f };

STDMETHODIMP WgpuD3D9Device::Clear(DWORD, CONST D3DRECT*, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  endActivePass();
  if (Flags & D3DCLEAR_TARGET) {
    unpackD3DColor(Color, clearColor);
    if (g_forceClearColor) {
      clearColor[0] = g_forcedClear[0]; clearColor[1] = g_forcedClear[1];
      clearColor[2] = g_forcedClear[2]; clearColor[3] = g_forcedClear[3];
    }
    pendingClearColor = true;
  }
  if (Flags & D3DCLEAR_ZBUFFER) {
    clearZ = std::max(0.0f, std::min(1.0f, Z));
    pendingClearDepth = true;
  }
  if (Flags & D3DCLEAR_STENCIL) {
    clearStencil = Stencil;
    pendingClearStencil = true;
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  if (pMatrix && State >= 0 && State < 300) {
    transforms[State] = *pMatrix;
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
  if (pMatrix && State >= 0 && State < 300) {
    *pMatrix = transforms[State];
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  if (pMatrix && State >= 0 && State < 300) {
    D3DMATRIX a = transforms[State];
    D3DMATRIX out{};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        out.m[r][c] = a.m[r][0] * pMatrix->m[0][c] + a.m[r][1] * pMatrix->m[1][c] +
                      a.m[r][2] * pMatrix->m[2][c] + a.m[r][3] * pMatrix->m[3][c];
      }
    }
    transforms[State] = out;
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport) {
  if (pViewport) viewport = *pViewport;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetViewport(D3DVIEWPORT9* pViewport) {
  if (pViewport) *pViewport = viewport;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial) {
  if (pMaterial) material = *pMaterial;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetMaterial(D3DMATERIAL9* pMaterial) {
  if (pMaterial) *pMaterial = material;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight) {
  if (pLight && Index < 8) lights[Index] = *pLight;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight) {
  if (pLight && Index < 8) *pLight = lights[Index];
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  if (Index < 8) lightEnabled[Index] = (Enable != 0);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable) {
  if (pEnable && Index < 8) *pEnable = lightEnabled[Index] ? TRUE : FALSE;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  if (State >= 0 && State < 256) renderStates[State] = Value;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
  if (pValue) *pValue = (State >= 0 && State < 256) ? renderStates[State] : 0;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** ppSB) {
  if (!ppSB) return D3DERR_INVALIDCALL;
  *ppSB = new WgpuStateBlock(this);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = (Stage < 8) ? boundTex[Stage] : nullptr;
  if (*ppTexture) (*ppTexture)->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  if (Stage >= 8) return D3D_OK;
  if (pTexture == boundTex[Stage]) return D3D_OK;
  if (pTexture) pTexture->AddRef();
  if (boundTex[Stage]) boundTex[Stage]->Release();
  boundTex[Stage] = pTexture;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) {
  if (pValue) *pValue = (Stage < 8 && Type >= 0 && Type < 36) ? texStageStates[Stage][Type] : 0;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  if (Stage < 8 && Type >= 0 && Type < 36) texStageStates[Stage][Type] = Value;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
  if (pValue) *pValue = (Sampler < 16 && Type >= 0 && Type < 16) ? samplerStates[Sampler][Type] : 0;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  if (Sampler < 16 && Type >= 0 && Type < 16) samplerStates[Sampler][Type] = Value;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetScissorRect(CONST RECT* pRect) {
  if (pRect) scissorRect = *pRect;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetScissorRect(RECT* pRect) {
  if (pRect) *pRect = scissorRect;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  emitDraw(PrimitiveType, false, 0, StartVertex, PrimitiveCount, nullptr, 0, 0, nullptr, D3DFMT_UNKNOWN);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                                  UINT, UINT, UINT startIndex, UINT primCount) {
  emitDraw(PrimitiveType, true, BaseVertexIndex, startIndex, primCount, nullptr, 0, 0, nullptr, D3DFMT_UNKNOWN);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                             CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  uint32_t vCount = primCountToIndexCount(PrimitiveType, PrimitiveCount);
  emitDraw(PrimitiveType, false, 0, 0, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride, vCount,
           nullptr, D3DFMT_UNKNOWN);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                    UINT NumVertices, UINT PrimitiveCount,
                                                    CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                                    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  emitDraw(PrimitiveType, true, 0, 0, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride,
           MinVertexIndex + NumVertices, pIndexData, IndexDataFormat);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements,
                                                     IDirect3DVertexDeclaration9** ppDecl) {
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = new WgpuVertexDecl(this, pVertexElements);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
  auto* next = static_cast<WgpuVertexDecl*>(pDecl);
  if (next == boundDecl) return D3D_OK;
  if (next) next->AddRef();
  if (boundDecl) boundDecl->Release();
  boundDecl = next;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = boundDecl;
  if (boundDecl) boundDecl->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetFVF(DWORD FVF) {
  boundFVF = FVF;
  if (FVF != 0 && boundDecl) {
    boundDecl->Release();
    boundDecl = nullptr;
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetFVF(DWORD* pFVF) {
  if (pFVF) *pFVF = boundFVF;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = new WgpuVertexShader(this, pFunction);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader) {
  if (pShader == boundVS) return D3D_OK;
  if (pShader) pShader->AddRef();
  if (boundVS) boundVS->Release();
  boundVS = pShader;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = boundVS;
  if (boundVS) boundVS->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  if (pConstantData && StartRegister < 256) {
    UINT n = std::min(Vector4fCount, 256u - StartRegister);
    memcpy(vsConsts[StartRegister], pConstantData, n * 16u);
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  if (pConstantData && StartRegister < 256) {
    UINT n = std::min(Vector4fCount, 256u - StartRegister);
    memcpy(pConstantData, vsConsts[StartRegister], n * 16u);
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                                             UINT OffsetInBytes, UINT Stride) {
  if (StreamNumber >= 16) return D3D_OK;
  auto* next = static_cast<WgpuVertexBuffer*>(pStreamData);
  if (next) next->AddRef();
  if (streams[StreamNumber].vb) streams[StreamNumber].vb->Release();
  streams[StreamNumber].vb = next;
  streams[StreamNumber].offset = OffsetInBytes;
  streams[StreamNumber].stride = Stride;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
                                             UINT* pOffsetInBytes, UINT* pStride) {
  if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
  if (ppStreamData) {
    *ppStreamData = streams[StreamNumber].vb;
    if (*ppStreamData) (*ppStreamData)->AddRef();
  }
  if (pOffsetInBytes) *pOffsetInBytes = streams[StreamNumber].offset;
  if (pStride) *pStride = streams[StreamNumber].stride;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
  auto* next = static_cast<WgpuIndexBuffer*>(pIndexData);
  if (next == boundIB) return D3D_OK;
  if (next) next->AddRef();
  if (boundIB) boundIB->Release();
  boundIB = next;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
  if (!ppIndexData) return D3DERR_INVALIDCALL;
  *ppIndexData = boundIB;
  if (boundIB) boundIB->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = new WgpuPixelShader(this, pFunction);
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader) {
  if (pShader == boundPS) return D3D_OK;
  if (pShader) pShader->AddRef();
  if (boundPS) boundPS->Release();
  boundPS = pShader;
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = boundPS;
  if (boundPS) boundPS->AddRef();
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  if (pConstantData && StartRegister < 32) {
    UINT n = std::min(Vector4fCount, 32u - StartRegister);
    memcpy(psConsts[StartRegister], pConstantData, n * 16u);
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  if (pConstantData && StartRegister < 32) {
    UINT n = std::min(Vector4fCount, 32u - StartRegister);
    memcpy(pConstantData, psConsts[StartRegister], n * 16u);
  }
  return D3D_OK;
}

STDMETHODIMP WgpuD3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
  if (!ppQuery) return D3DERR_INVALIDCALL;
  *ppQuery = new WgpuQuery(this, Type);
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// WgpuD3D9 (IDirect3D9 adapter enumerator + device factory)
// ---------------------------------------------------------------------------
struct WgpuD3D9 : public IDirect3D9, public WgpuSysObj {
  int refCount = 1;

  STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override {
    if (!ppvObj) return E_FAIL; *ppvObj = this; AddRef(); return S_OK;
  }
  STDMETHOD_(ULONG, AddRef)() override { return (ULONG)++refCount; }
  STDMETHOD_(ULONG, Release)() override {
    int r = --refCount; if (r <= 0) delete this; return (ULONG)(r > 0 ? r : 0);
  }
  STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
  STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD, D3DADAPTER_IDENTIFIER9* pId) override {
    if (Adapter != 0 || !pId) return D3DERR_INVALIDCALL;
    memset(pId, 0, sizeof(*pId));
    auto& ctx = GetWgpuContext();
    snprintf(pId->Driver, sizeof(pId->Driver), "emdawnwebgpu");
    snprintf(pId->Description, sizeof(pId->Description), "WebGPU (%s %s)",
             ctx.adapterVendor[0] ? ctx.adapterVendor : "GPU", ctx.adapterDevice);
    snprintf(pId->DeviceName, sizeof(pId->DeviceName), "\\\\.\\DISPLAY1");
    return D3D_OK;
  }
  STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) override {
    if (Adapter != 0) return 0;
    if (Format != D3DFMT_X8R8G8B8 && Format != D3DFMT_A8R8G8B8 && Format != D3DFMT_R5G6B5) return 0;
    return 10;
  }
  STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
    if (Adapter != 0 || !pMode || Mode >= 10) return D3DERR_INVALIDCALL;
    static const uint32_t kW[10] = {800, 1024, 1280, 1280, 1366, 1440, 1600, 1920, 2560, 3840};
    static const uint32_t kH[10] = {600, 768,  720,  960,  768,  900,  900,  1080, 1440, 2160};
    pMode->Width = kW[Mode];
    pMode->Height = kH[Mode];
    pMode->RefreshRate = 60;
    pMode->Format = Format;
    return D3D_OK;
  }
  STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) override {
    if (Adapter != 0 || !pMode) return D3DERR_INVALIDCALL;
    auto& ctx = GetWgpuContext();
    uint32_t w = ctx.preferredWidth ? ctx.preferredWidth : 1600;
    uint32_t h = ctx.preferredHeight ? ctx.preferredHeight : 900;
    if (ctx.width > w) w = ctx.width;
    if (ctx.height > h) h = ctx.height;
    pMode->Width = w;
    pMode->Height = h;
    pMode->RefreshRate = 60;
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
  }
  STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT, BOOL) override {
    return (Adapter == 0 && DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override {
    return (Adapter == 0 && DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE, D3DFORMAT, BOOL,
                                        D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override {
    if (pQualityLevels) *pQualityLevels = 1;
    return (Adapter == 0 && MultiSampleType == D3DMULTISAMPLE_NONE) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override {
    return (Adapter == 0) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override {
    return (Adapter == 0) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE, D3DCAPS9* pCaps) override {
    if (Adapter != 0 || !pCaps) return D3DERR_INVALIDCALL;
    fillWebGPUDeviceCaps(pCaps);
    return D3D_OK;
  }
  STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override { return (HMONITOR)(uintptr_t)1; }
  STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE, HWND, DWORD,
                          D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev) override {
    if (Adapter != 0 || !ppDev) return D3DERR_INVALIDCALL;
    *ppDev = new WgpuD3D9Device(this, pPP);
    return D3D_OK;
  }
};

// Strong override of weak fallback in browser/platform/pcRedRenderer_browser.cpp.
IDirect3D9* Direct3DCreate9(unsigned int sdkVersion) {
  (void)sdkVersion;
  return new WgpuD3D9();
}

// ---------------------------------------------------------------------------
// Async WebGPU Bootstrap + Readback (C-linkage entry points for Worker)
// ---------------------------------------------------------------------------
static void copyStrView(char* dst, size_t cap, wgpu::StringView sv) {
  if (!dst || cap == 0) return;
  size_t n = (sv.data && sv.length) ? (sv.length < cap - 1 ? sv.length : cap - 1) : 0;
  if (n > 0) memcpy(dst, sv.data, n);
  dst[n] = '\0';
}

static void notifyWgpuInitResult(bool ok, const char* err) {
  auto& ctx = GetWgpuContext();
  char safeErr[512];
  size_t ei = 0;
  const char* src = err ? err : "";
  for (size_t i = 0; src[i] && ei + 1 < sizeof(safeErr); ++i) {
    char c = src[i];
    safeErr[ei++] = (c == '"' || c == '\\' || (unsigned char)c < 32) ? ' ' : c;
  }
  safeErr[ei] = '\0';

  char json[1536];
  snprintf(json, sizeof(json),
           "{"
           "\"ok\":%s,"
           "\"width\":%u,"
           "\"height\":%u,"
           "\"hasTextureCompressionBC\":%s,"
           "\"isFallbackAdapter\":%s,"
           "\"adapter\":{\"vendor\":\"%s\",\"architecture\":\"%s\",\"device\":\"%s\",\"description\":\"%s\"},"
           "\"error\":\"%s\""
           "}",
           ok ? "true" : "false",
           ctx.width, ctx.height,
           ctx.hasBC ? "true" : "false",
           ctx.isFallbackAdapter ? "true" : "false",
           ctx.adapterVendor, ctx.adapterArch, ctx.adapterDevice, ctx.adapterDesc,
           safeErr);
  js_wgpu_notify_ready(json);
}

extern "C" {

/* See WgpuD3D9Device::Clear: replaces the scene clear colour so that undrawn regions stop hiding as
 * black-on-black. Off by default; keep the colour dim (see the note on g_forcedClear). */
/* See WgpuDebugPsSnippet: bake a pixel-shader override into every fragment shader generated after
 * this call. Set it before booting - generated shaders are cached, so switching mode mid-session
 * would leave already-compiled pipelines in the old mode. */
void swbf2_wgpu_debug_set_ps_mode(int mode) { g_wgpuDebugPsMode = mode; }

/* See g_dbgSkipLo: hide draws by per-frame id range (ids are still counted, so they stay stable).
 * Pass (-1, -1) to stop hiding. */
void swbf2_wgpu_debug_skip_draws(int lo, int hi) { g_dbgSkipLo = lo; g_dbgSkipHi = hi; }

/* Start/stop capturing per-draw records for swbf2_wgpu_debug_draw_record_json() without changing any
 * generated shader. */
void swbf2_wgpu_debug_record_draws(int enable) { g_dbgRecordDraws = enable != 0; }

/* See WgpuD3D9Device::Clear: replaces the scene clear colour so that undrawn regions stop hiding as
 * black-on-black. Off by default; keep each channel dim (see the note above g_forcedClear). */
void swbf2_wgpu_debug_set_clear_colour(int enable, float r, float g, float b) {
  g_forceClearColor = enable != 0;
  g_forcedClear[0] = r; g_forcedClear[1] = g; g_forcedClear[2] = b; g_forcedClear[3] = 1.0f;
}

/* JSON for the draw that painted a given pixel in a ps-mode-4 frame (id = r + 256*g + 65536*b, read
 * back from the screenshot). Looked up in the records of the last completed frame. */
const char* swbf2_wgpu_debug_draw_record_json(int id) {
  static std::string s_json;
  const int buf = g_dbgRecBuf ^ 1;
  for (uint32_t n = 0; n < g_dbgRecCount[buf]; ++n) {
    const DbgDrawRec& r = g_dbgRecs[buf][n];
    if (r.id != (uint32_t)id) continue;
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"id\":%u,\"tris\":%u,\"stride\":%u,\"declId\":%u,\"vsId\":%u,\"psId\":%u,"
             "\"fvf\":\"0x%08x\",\"vtxKey\":\"0x%016llx\",\"vp\":%ux%u,"
             "\"z\":{\"enable\":%u,\"func\":%u},\"blend\":{\"enable\":%u,\"src\":%u,\"dst\":%u},"
             "\"alphaTest\":%u,\"alphaRef\":%u,\"stages\":[",
             r.id, r.tris, r.stride, r.declId, r.vsId, r.psId, r.fvf,
             (unsigned long long)r.vtxKey, r.vpW, r.vpH, r.zEnable, r.zFunc,
             r.blendEnable, r.srcBlend, r.dstBlend, r.alphaTest, r.alphaRef);
    s_json = buf;
    for (int s = 0; s < 4; ++s) {
      snprintf(buf, sizeof(buf),
               "%s{\"uvIdx\":%u,\"colorOp\":%u,\"arg1\":%u,\"arg2\":%u,\"tex\":%s,"
               "\"texId\":%u,\"texW\":%u,\"texH\":%u,\"texFmt\":\"0x%x\"}",
               s ? "," : "", r.uvIdx[s], r.colOp[s], r.colA1[s], r.colA2[s],
               r.texNull[s] ? "null" : "bound", r.texId[s], r.texW[s], r.texH[s], r.texFmt[s]);
      s_json += buf;
    }
    s_json += "]}";
    return s_json.c_str();
  }
  return "{}";
}

/* One entry per texture bound by the last completed ps-mode-4 frame: what it is, how often it was
 * bound, and how many triangles it carried - enough to spot a draw sampling an implausibly small
 * page or a stage with no texture at all. */
const char* swbf2_wgpu_debug_frame_texture_summary() {
  static std::string s_json;
  const int recBuf = g_dbgRecBuf ^ 1;
  struct Agg { uint32_t draws = 0; uint32_t tris = 0; };
  std::map<std::string, Agg> byTex;   // diagnostic path only; never taken by a normal frame
  for (uint32_t n = 0; n < g_dbgRecCount[recBuf]; ++n) {
    const DbgDrawRec& r = g_dbgRecs[recBuf][n];
    for (int s = 0; s < 4; ++s) {
      char key[96];
      snprintf(key, sizeof(key), "s%d tex %u %ux%u fmt 0x%x%s", s, r.texId[s], r.texW[s], r.texH[s],
               r.texFmt[s], r.texNull[s] ? " (unbound)" : "");
      Agg& a = byTex[key];
      ++a.draws;
      a.tris += r.tris;
    }
  }
  char buf[192];
  snprintf(buf, sizeof(buf), "{\"draws\":%u,\"overflowed\":%u,\"textures\":[",
           g_dbgRecCount[recBuf], g_dbgRecOverflow[recBuf]);
  s_json = buf;
  size_t emitted = 0;
  for (const std::map<std::string, Agg>::value_type& kv : byTex) {
    if (++emitted > 96) { s_json += "\"...(truncated)\""; break; }
    snprintf(buf, sizeof(buf), "%s\"%s draws=%u tris=%u\"", emitted > 1 ? "," : "",
             kv.first.c_str(), kv.second.draws, kv.second.tris);
    s_json += buf;
  }
  s_json += "]}";
  return s_json.c_str();
}

void swbf2_wgpu_init(int width, int height) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  ctx.width = width > 0 ? (uint32_t)width : 1280;
  ctx.height = height > 0 ? (uint32_t)height : 720;

  if (ctx.ready) {
    ctx.recreateBackbuffer(ctx.width, ctx.height);
    notifyWgpuInitResult(true, "");
    return;
  }

  js_wgpu_register_canvas_target();

  wgpu::InstanceDescriptor instDesc{};
  ctx.instance = wgpu::CreateInstance(&instDesc);
  if (!ctx.instance) {
    notifyWgpuInitResult(false, "wgpu::CreateInstance returned null");
    return;
  }

  wgpu::RequestAdapterOptions opts{};
  opts.powerPreference = wgpu::PowerPreference::HighPerformance;

  ctx.instance.RequestAdapter(
      &opts, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
        WgpuSysHeapGuard _sysHeap;
        auto& ctx = GetWgpuContext();
        if (status != wgpu::RequestAdapterStatus::Success || !adapter) {
          char buf[256];
          copyStrView(buf, sizeof(buf), message);
          notifyWgpuInitResult(false, buf[0] ? buf : "RequestAdapter failed");
          return;
        }
        ctx.adapter = std::move(adapter);

        wgpu::AdapterInfo info{};
        ctx.adapter.GetInfo(&info);
        copyStrView(ctx.adapterVendor, sizeof(ctx.adapterVendor), info.vendor);
        copyStrView(ctx.adapterArch, sizeof(ctx.adapterArch), info.architecture);
        copyStrView(ctx.adapterDevice, sizeof(ctx.adapterDevice), info.device);
        copyStrView(ctx.adapterDesc, sizeof(ctx.adapterDesc), info.description);
        ctx.isFallbackAdapter = (info.adapterType == wgpu::AdapterType::CPU);
        ctx.hasBC = ctx.adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);

        wgpu::FeatureName reqFeatures[1];
        uint32_t reqCount = 0;
        if (ctx.hasBC) {
          reqFeatures[reqCount++] = wgpu::FeatureName::TextureCompressionBC;
        }

        wgpu::DeviceDescriptor devDesc{};
        devDesc.requiredFeatureCount = reqCount;
        devDesc.requiredFeatures = reqCount ? reqFeatures : nullptr;
        devDesc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
              char buf[512];
              copyStrView(buf, sizeof(buf), msg);
              char full[600];
              snprintf(full, sizeof(full), "UncapturedError(%d): %s", (int)type, buf);
              GetWgpuContext().recordError(full);
            });
        devDesc.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
              if (reason == wgpu::DeviceLostReason::Destroyed) return;
              char buf[512];
              copyStrView(buf, sizeof(buf), msg);
              char full[600];
              snprintf(full, sizeof(full), "DeviceLost(%d): %s", (int)reason, buf);
              GetWgpuContext().recordError(full);
            });

        ctx.adapter.RequestDevice(
            &devDesc, wgpu::CallbackMode::AllowSpontaneous,
            [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
              WgpuSysHeapGuard _sysHeap;
              auto& ctx = GetWgpuContext();
              if (status != wgpu::RequestDeviceStatus::Success || !device) {
                char buf[256];
                copyStrView(buf, sizeof(buf), message);
                notifyWgpuInitResult(false, buf[0] ? buf : "RequestDevice failed");
                return;
              }
              ctx.device = std::move(device);
              ctx.queue = ctx.device.GetQueue();

              wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
              canvasDesc.selector = "!canvas";
              wgpu::SurfaceDescriptor surfDesc{};
              surfDesc.nextInChain = &canvasDesc;
              ctx.surface = ctx.instance.CreateSurface(&surfDesc);
              if (ctx.surface) {
                wgpu::SurfaceCapabilities caps{};
                ctx.surface.GetCapabilities(ctx.adapter, &caps);
                if (caps.formatCount > 0) {
                  ctx.surfaceFormat = caps.formats[0];
                }
              }

              if (!ctx.initSharedResources()) {
                notifyWgpuInitResult(false, "initSharedResources failed");
                return;
              }
              ctx.ready = true;
              notifyWgpuInitResult(true, "");
            });
      });
}

int swbf2_wgpu_is_ready(void) { return GetWgpuContext().ready ? 1 : 0; }
int swbf2_wgpu_has_bc(void) { return GetWgpuContext().hasBC ? 1 : 0; }
int swbf2_wgpu_error_count(void) { return (int)GetWgpuContext().errorCount; }
const char* swbf2_wgpu_last_error(void) { return GetWgpuContext().lastError; }
int swbf2_wgpu_draw_calls(void) { return (int)GetWgpuContext().lastFrameDraws; }
int swbf2_wgpu_triangles(void) { return (int)GetWgpuContext().lastFrameTris; }
int swbf2_wgpu_skinned_draws(void) { return (int)GetWgpuContext().lastFrameSkinnedDraws; }
int swbf2_wgpu_skinned_triangles(void) { return (int)GetWgpuContext().lastFrameSkinnedTris; }

const char* swbf2_wgpu_validate_all_shaders(void) {
  WgpuSysHeapGuard _sysHeap;
  static char s_report[512];
  auto& ctx = GetWgpuContext();
  if (!ctx.ready) {
    snprintf(s_report, sizeof(s_report), "{\"ok\":false,\"error\":\"wgpu not ready\"}");
    return s_report;
  }
  uint32_t errBefore = ctx.errorCount;
  uint32_t engineVsTotal = (uint32_t)ctx.allVertexShaders.size();
  uint32_t enginePsTotal = (uint32_t)ctx.allPixelShaders.size();
  uint32_t engineVsCompiled = 0;
  uint32_t enginePsCompiled = 0;
  FfpVertexLayout dummyLayout{};
  dummyLayout.pos = {0, WGPU_VTX_FLOAT3, 1};
  dummyLayout.minStride = 12;

  auto compileVs = [&](const WgpuVertexShader* vs) -> bool {
    if (!vs || vs->bytecode.empty()) return false;
    uint64_t key = 0;
    for (const Sm1VsDclInput& d : vs->dcls) {
      if (d.reg < 16) key |= ((uint64_t)WGPU_VTX_FLOAT4 << (d.reg * 4));
    }
    std::string code = GenerateCombinedWGSL(vs, key, dummyLayout, nullptr, 0);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgsl;
    wgpu::ShaderModule mod = ctx.device.CreateShaderModule(&smd);
    return (bool)mod;
  };

  auto compilePs = [&](const WgpuPixelShader* ps) -> bool {
    if (!ps || ps->bytecode.empty()) return false;
    std::string code = GenerateCombinedWGSL(nullptr, 0, dummyLayout, ps, 0);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgsl;
    wgpu::ShaderModule mod = ctx.device.CreateShaderModule(&smd);
    return (bool)mod;
  };

  for (const WgpuVertexShader* vs : ctx.allVertexShaders) {
    if (compileVs(vs)) ++engineVsCompiled;
  }
  for (const WgpuPixelShader* ps : ctx.allPixelShaders) {
    if (compilePs(ps)) ++enginePsCompiled;
  }

  // Also scan all 30 SHDR chunks across both PIPELINE_PROG1 and PIPELINE_PROG2 PIPE
  // sub-chunks in data\_lvl_pc\core.lvl (2,332 vs_1_1 + 226 ps_1_1 shaders) and compile
  // every single shader blob through Dawn/Tint CreateShaderModule.
  uint32_t coreVsTotal = 0;
  uint32_t coreVsCompiled = 0;
  uint32_t corePsTotal = 0;
  uint32_t corePsCompiled = 0;
  {
    PblFile f;
    if (f.Open("data\\_lvl_pc\\core.lvl", false, false, false)) {
      int sz = f.GetSize();
      if (sz > 16) {
        ByteVec buf((size_t)sz);
        f.ReadData(buf.data(), sz);
        size_t pos = 8;
        size_t end = (size_t)sz;
        auto rd32 = [&](size_t off) -> uint32_t {
          uint32_t v = 0;
          memcpy(&v, buf.data() + off, 4);
          return v;
        };
        while (pos + 8 <= end) {
          uint32_t csz = rd32(pos + 4);
          size_t payload = pos + 8;
          size_t cend = payload + csz;
          if (cend > end) break;
          if (memcmp(buf.data() + pos, "SHDR", 4) == 0) {
            size_t i = payload;
            while (i + 8 <= cend) {
              uint32_t w = rd32(i);
              if (w == 0xFFFE0101u || w == 0xFFFF0101u) {
                size_t j = i + 4;
                bool foundEnd = false;
                while (j + 4 <= cend) {
                  uint32_t tok = rd32(j);
                  if ((tok & 0xFFFFu) == 0xFFFEu) {
                    j += 4 + (size_t)((tok >> 16) & 0x7FFFu) * 4;
                    continue;
                  }
                  if (tok == 0x0000FFFFu) {
                    size_t wordCount = (j + 4 - i) / 4;
                    if (w == 0xFFFE0101u) {
                      ++coreVsTotal;
                      WgpuVertexShader tmpVs;
                      tmpVs.bytecode.resize(wordCount);
                      memcpy(tmpVs.bytecode.data(), buf.data() + i, wordCount * 4);
                      ParseSm1VertexShaderDcls(tmpVs.bytecode.data(), tmpVs.bytecode.size(), tmpVs.dcls);
                      if (compileVs(&tmpVs)) ++coreVsCompiled;
                    } else {
                      ++corePsTotal;
                      WgpuPixelShader tmpPs;
                      tmpPs.bytecode.resize(wordCount);
                      memcpy(tmpPs.bytecode.data(), buf.data() + i, wordCount * 4);
                      if (compilePs(&tmpPs)) ++corePsCompiled;
                    }
                    i = j + 4;
                    foundEnd = true;
                    break;
                  }
                  j += 4;
                }
                if (!foundEnd) i += 4;
              } else {
                i += 4;
              }
            }
          }
          pos = (cend + 3u) & ~(size_t)3u;
        }
      }
      f.Close();
    }
  }

  uint32_t vsTotal = coreVsTotal > 0 ? coreVsTotal : engineVsTotal;
  uint32_t vsCompiled = coreVsTotal > 0 ? coreVsCompiled : engineVsCompiled;
  uint32_t psTotal = corePsTotal > 0 ? corePsTotal : enginePsTotal;
  uint32_t psCompiled = corePsTotal > 0 ? corePsCompiled : enginePsCompiled;
  uint32_t newErrs = ctx.errorCount - errBefore;
  snprintf(s_report, sizeof(s_report),
           "{"
           "\"ok\":%s,"
           "\"vsTotal\":%u,\"vsCompiled\":%u,"
           "\"psTotal\":%u,\"psCompiled\":%u,"
           "\"engineVsTotal\":%u,\"engineVsCompiled\":%u,"
           "\"enginePsTotal\":%u,\"enginePsCompiled\":%u,"
           "\"shaderCompileErrors\":%u"
           "}",
           (vsCompiled == vsTotal && psCompiled == psTotal &&
            engineVsCompiled == engineVsTotal && enginePsCompiled == enginePsTotal &&
            newErrs == 0) ? "true" : "false",
           vsTotal, vsCompiled, psTotal, psCompiled,
           engineVsTotal, engineVsCompiled, enginePsTotal, enginePsCompiled,
           newErrs);
  return s_report;
}

void swbf2_wgpu_resize(int width, int height) {
  auto& ctx = GetWgpuContext();
  if (ctx.ready && width > 0 && height > 0) {
    ctx.recreateBackbuffer((uint32_t)width, (uint32_t)height);
  }
}

void swbf2_wgpu_readback_frame(void) {
  WgpuSysHeapGuard _sysHeap;
  auto& ctx = GetWgpuContext();
  if (!ctx.ready || !ctx.backbufferTex || ctx.readbackPending) {
    js_wgpu_notify_readback("{\"ok\":false,\"error\":\"not ready or readback pending\"}", nullptr, 0, 0, 0);
    return;
  }

  uint32_t w = ctx.width;
  uint32_t h = ctx.height;
  uint32_t rowBytes = ((w * 4u) + 255u) & ~255u;
  uint32_t totalBytes = rowBytes * h;

  if (!ctx.readbackBuf || ctx.readbackW != w || ctx.readbackH != h) {
    wgpu::BufferDescriptor bd{};
    bd.label = "swbf2_readback";
    bd.size = totalBytes;
    bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    ctx.readbackBuf = ctx.device.CreateBuffer(&bd);
    ctx.readbackW = w;
    ctx.readbackH = h;
    ctx.readbackRowBytes = rowBytes;
  }

  wgpu::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo src{};
  src.texture = ctx.backbufferTex;
  wgpu::TexelCopyBufferInfo dst{};
  dst.buffer = ctx.readbackBuf;
  dst.layout.bytesPerRow = rowBytes;
  dst.layout.rowsPerImage = h;
  wgpu::Extent3D ext{w, h, 1};
  enc.CopyTextureToBuffer(&src, &dst, &ext);
  wgpu::CommandBuffer cmd = enc.Finish();
  ctx.queue.Submit(1, &cmd);

  ctx.readbackPending = true;
  ctx.readbackBuf.MapAsync(
      wgpu::MapMode::Read, 0, totalBytes, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::MapAsyncStatus status, wgpu::StringView) {
        auto& ctx = GetWgpuContext();
        ctx.readbackPending = false;
        if (status != wgpu::MapAsyncStatus::Success) {
          js_wgpu_notify_readback("{\"ok\":false,\"error\":\"MapAsync failed\"}", nullptr, 0, 0, 0);
          return;
        }
        const uint32_t w = ctx.readbackW;
        const uint32_t h = ctx.readbackH;
        const uint32_t rowBytes = ctx.readbackRowBytes;
        const uint8_t* mapped = reinterpret_cast<const uint8_t*>(
            ctx.readbackBuf.GetConstMappedRange(0, (size_t)rowBytes * h));

        ByteVec tight((size_t)w * h * 4);
        uint32_t nonBlack = 0;
        double sumLuma = 0.0;
        double quadLuma[4] = {0, 0, 0, 0};
        uint32_t quadCount[4] = {0, 0, 0, 0};
        ByteVec seen(4096, 0);
        uint32_t distinctQuant = 0;

        for (uint32_t y = 0; y < h; ++y) {
          const uint8_t* row = mapped + (size_t)y * rowBytes;
          uint8_t* dstRow = tight.data() + (size_t)y * w * 4;
          memcpy(dstRow, row, (size_t)w * 4);
          for (uint32_t x = 0; x < w; ++x) {
            dstRow[x * 4 + 3] = 255; // Match Present() fs_blit / D3DFMT_X8R8G8B8 opaque alpha.
            const uint8_t* p = row + x * 4;
            uint32_t r = p[0], g = p[1], b = p[2];
            if (r > 4 || g > 4 || b > 4) ++nonBlack;
            double luma = 0.299 * r + 0.587 * g + 0.114 * b;
            sumLuma += luma;
            int q = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
            quadLuma[q] += luma;
            ++quadCount[q];
            uint32_t bin = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            if (!seen[bin]) { seen[bin] = 1; ++distinctQuant; }
          }
        }

        const uint8_t* cp = tight.data() + ((size_t)(h / 2) * w + (w / 2)) * 4;
        uint32_t centerPacked = ((uint32_t)cp[0] << 24) | ((uint32_t)cp[1] << 16) |
                                ((uint32_t)cp[2] << 8) | cp[3];
        ctx.readbackBuf.Unmap();

        double totalPx = (double)w * (double)h;
        char json[1024];
        snprintf(json, sizeof(json),
                 "{"
                 "\"ok\":true,"
                 "\"width\":%u,\"height\":%u,"
                 "\"nonBlackPixels\":%u,\"nonBlackPct\":%.2f,"
                 "\"distinctColors12bit\":%u,"
                 "\"meanLuma\":%.2f,"
                 "\"quadrantLumas\":[%.2f,%.2f,%.2f,%.2f],"
                 "\"centerRgba\":\"0x%08x\","
                 "\"draws\":%u,\"triangles\":%u,"
                 "\"skinnedDraws\":%u,\"skinnedTriangles\":%u,"
                 "\"wgpuErrors\":%u"
                 "}",
                 w, h, nonBlack, totalPx > 0 ? (100.0 * nonBlack / totalPx) : 0.0,
                 distinctQuant, totalPx > 0 ? (sumLuma / totalPx) : 0.0,
                 quadCount[0] ? quadLuma[0] / quadCount[0] : 0.0,
                 quadCount[1] ? quadLuma[1] / quadCount[1] : 0.0,
                 quadCount[2] ? quadLuma[2] / quadCount[2] : 0.0,
                 quadCount[3] ? quadLuma[3] / quadCount[3] : 0.0,
                 centerPacked, ctx.lastFrameDraws, ctx.lastFrameTris,
                 ctx.lastFrameSkinnedDraws, ctx.lastFrameSkinnedTris, ctx.errorCount);
        js_wgpu_notify_readback(json, tight.data(), (int)tight.size(), (int)w, (int)h);
      });
}

} // extern "C"
