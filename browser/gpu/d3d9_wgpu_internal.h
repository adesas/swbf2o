#pragma once
// Internal header for the SWBF2 Direct3D 9 -> WebGPU (emdawnwebgpu) backend.
//
// Only included by browser/gpu/*.cpp so <webgpu/webgpu_cpp.h> never leaks into
// engine translation units or native i386 oracle builds.

#include <d3d9.h>
#include <webgpu/webgpu_cpp.h>

#include <stdint.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdlib.h>
#include <new>

// The decompiled engine replaces global operator new with its RedMemory heap, which has a fixed
// budget and returns NULL (no exception: the engine is built -fno-exceptions) when exhausted.
// A NULL from a 16 MiB std::vector then makes memset/memory.fill zero linear memory from address 0.
// All large CPU-side GPU staging memory therefore comes straight from libc malloc.
template <class T>
struct SysAllocator {
  typedef T value_type;
  SysAllocator() = default;
  template <class U> SysAllocator(const SysAllocator<U>&) {}
  T* allocate(size_t n) {
    void* p = malloc(n * sizeof(T));
    if (!p && n) abort();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t) { free(p); }
  template <class U> bool operator==(const SysAllocator<U>&) const { return true; }
  template <class U> bool operator!=(const SysAllocator<U>&) const { return false; }
};
typedef std::vector<uint8_t, SysAllocator<uint8_t>> ByteVec;
typedef std::vector<uint32_t, SysAllocator<uint32_t>> U32Vec;

// Base mixin so all WebGPU D3D9 COM objects allocate from system malloc rather than RedMemory.
struct WgpuSysObj {
  static void* operator new(size_t n) {
    void* p = malloc(n);
    if (!p && n) abort();
    return p;
  }
  static void operator delete(void* p) noexcept { free(p); }
  static void operator delete(void* p, size_t) noexcept { free(p); }
};

// Routes global ::operator new (used internally by emdawnwebgpu's C++ RefCounted wrappers and
// std::string inside WGSL codegen) to libc malloc instead of SWBF2's RunTimeHeap, so WebGPU
// objects survive ShellState <-> MissionState heap teardowns intact.
extern "C" int __RedCurrHeap;
struct WgpuSysHeapGuard {
  int savedHeap;
  WgpuSysHeapGuard() : savedHeap(__RedCurrHeap) { __RedCurrHeap = -2; }
  ~WgpuSysHeapGuard() { __RedCurrHeap = savedHeap; }
};

struct WgpuD3D9Device;
// Debug-only pixel-shader override, set from JS (swbf2_wgpu_debug_set_ps_mode) before anything is
// rendered, and baked into generated WGSL at shader-generation time:
//   0 off, 1 output the stage-0 texture coordinates, 2 output opaque white.
// Mode 1 answers "are these surfaces getting sensible UVs?" - a surface that samples texture-page
// padding because its UV channel is wrong shows a smooth gradient instead of the flat near-black
// colour it renders as. Mode 2 answers "is geometry even submitted here?" - anything that turns
// white is drawn, so a region that stays dark is not covered by geometry at all.
// They are diagnostic aids for reading a frame, not a rendering feature: 0 is the only value any
// check runs with, and nothing else in the build reads this.
extern int g_wgpuDebugPsMode;
const char* WgpuDebugPsSnippet(bool sm11Coords);
// ps mode 4: paint every draw with its own id (r = id & 255, g = (id >> 8) & 255), so a pixel of
// interest names the draw that produced it, and the same id names its record.
const char* WgpuDebugIdSnippet();

struct WgpuTexture2D;
struct WgpuSurface;
struct WgpuVertexShader;
struct WgpuPixelShader;

struct Sm1VsDclInput {
  uint8_t reg = 0;        // v0..v15
  uint8_t usage = 0;      // D3DDECLUSAGE_*
  uint8_t usageIndex = 0; // 0..3
};

// ---------------------------------------------------------------------------
// Per-frame CPU-mirrored streaming GPU ring buffer.
//
// Dynamic vertex buffers (D3DLOCK_DISCARD / D3DLOCK_NOOVERWRITE), dynamic
// index buffers, Draw*UP draws, and per-draw FFP uniform blocks write into
// sequential aligned regions of the CPU mirror during the frame and upload in
// a single queue.WriteBuffer() right before queue.Submit() at Present().
// This guarantees zero overwrite hazards across multiple DISCARD locks in one
// frame and minimizes WASM -> JS WebGPU FFI overhead.
// ---------------------------------------------------------------------------
struct WgpuDynRing {
  wgpu::Buffer gpu;
  ByteVec cpu;
  uint32_t capacity = 0;
  uint32_t used = 0;

  bool init(wgpu::Device& dev, uint32_t capBytes, wgpu::BufferUsage usage, const char* label);
  uint32_t alloc(uint32_t bytes, uint32_t align, void** outPtr);
  void upload(wgpu::Queue& queue);
  void reset() { used = 0; }
};

// ---------------------------------------------------------------------------
// Fixed-Function Pipeline (FFP) uniform block (1280 bytes = 5 * 256 B).
// Matches WGSL struct FfpDrawUniforms in browser/gpu/d3d9_ffp_wgsl.cpp.
// ---------------------------------------------------------------------------
struct alignas(16) FfpDrawUniforms {
  float world[16];          // 0..63
  float view[16];           // 64..127
  float proj[16];           // 128..191
  float texMat[4][16];      // 192..447
  float matDiffuse[4];      // 448..463
  float matAmbient[4];      // 464..479
  float matEmissive[4];     // 480..495
  float ambientColor[4];    // 496..511
  float tfactor[4];         // 512..527
  float fogColor[4];        // 528..543
  float fogParams[4];       // 544..559: (fogEnable, fogStart, fogEnd, fogMode)
  float viewport[4];        // 560..575: (vpX, vpY, vpW, vpH)
  float alphaTest[4];       // 576..591: (alphaTestEnable, alphaFunc, alphaRef, lightingEnable)
  float matSources[4];      // 592..607: (colorVertex, diffuseSrc, ambientSrc, emissiveSrc)
  float lightPos[4][4];     // 608..671: .xyz = pos or -dir, .w = type (0=off,1=pt,2=spot,3=dir)
  float lightDiff[4][4];    // 672..735: .rgb = diffuse, .a = range
  float lightAtten[4][4];   // 736..799: (a0, a1, a2, 0)
  uint32_t stageColor[4][4];// 800..863: (colorOp, colorArg1, colorArg2, colorArg0)
  uint32_t stageAlpha[4][4];// 864..927: (alphaOp, alphaArg1, alphaArg2, alphaArg0)
  uint32_t stageTex[4][4];  // 928..991: (uvIdx, tciMode, ttff, resultArg)
  float stageConst[4][4];   // 992..1055: D3DTSS_CONSTANT per stage
  float bumpMat[4][4];      // 1056..1119: D3DTSS_BUMPENVMAT00/01/10/11 per stage
  float vsConst[96][4];     // 1120..2655: vs_1_1 c0..c95
  float psConst[8][4];      // 2656..2783: ps_1_1 c0..c7
  // 2784..2815: dbg is only ever written/read by the draw-identity debug mode (ps mode 4), where it
  // carries the per-draw id that the baked fragment snippet turns into an RGB code. It shares this
  // tail with _pad so the block size (and the static_assert below) never move.
  float dbg[4];             // 2784..2799
  float _pad[4];            // 2800..2815 -> total 2816 bytes (11 * 256)
};
static_assert(sizeof(FfpDrawUniforms) == 2816, "FfpDrawUniforms must be 2816 bytes");

// ---------------------------------------------------------------------------
// Vertex attribute layout key for FFP WGSL module & RenderPipeline caching.
// ---------------------------------------------------------------------------
struct FfpVertexAttr {
  uint16_t offset = 0;
  uint8_t format = 0;    // WgpuVtxFmt enum below
  uint8_t present = 0;
};

enum WgpuVtxFmt : uint8_t {
  WGPU_VTX_NONE = 0,
  WGPU_VTX_FLOAT1 = 1,
  WGPU_VTX_FLOAT2 = 2,
  WGPU_VTX_FLOAT3 = 3,
  WGPU_VTX_FLOAT4 = 4,
  WGPU_VTX_D3DCOLOR = 5, // Unorm8x4 swizzled .bgra
  WGPU_VTX_SHORT2 = 6,
  WGPU_VTX_SHORT4 = 7,
  WGPU_VTX_UBYTE4 = 8,
};

struct FfpVertexLayout {
  bool isRHW = false;
  FfpVertexAttr pos;
  FfpVertexAttr normal;
  FfpVertexAttr diffuse;
  FfpVertexAttr specular;
  FfpVertexAttr tex[4];
  uint16_t minStride = 0;

  uint64_t shaderKey() const;
  bool operator==(const FfpVertexLayout& o) const {
    return memcmp(this, &o, sizeof(*this)) == 0;
  }
};

struct FfpPipelineKey {
  FfpVertexLayout vtx;
  uint64_t vsInputTypeKey = 0; // 4-bit WgpuVtxFmt per v0..v15 when vsId != 0
  uint32_t vsId = 0;           // 0 = FFP VS, >0 = WgpuVertexShader::uniqueId
  uint32_t psId = 0;           // 0 = FFP PS, >0 = WgpuPixelShader::uniqueId
  uint32_t declId = 0;         // 0 = FVF, >0 = WgpuVertexDecl::uniqueId
  uint16_t stride = 0;
  uint8_t texTypes = 0;        // 2 bits per stage 0..3: 0=2D, 1=Cube, 2=3D
  uint8_t topology = 0;       // D3DPRIMITIVETYPE (after TRIANGLEFAN->TRIANGLELIST)
  uint8_t cullMode = 0;       // D3DCULL
  uint8_t zEnable = 0;
  uint8_t zWrite = 0;
  uint8_t zFunc = 0;
  uint8_t hasDepth = 0;
  uint8_t blendEnable = 0;
  uint8_t srcBlend = 0;
  uint8_t dstBlend = 0;
  uint8_t blendOp = 0;
  uint8_t sepAlphaBlend = 0;
  uint8_t srcBlendAlpha = 0;
  uint8_t dstBlendAlpha = 0;
  uint8_t blendOpAlpha = 0;
  uint8_t colorWriteMask = 0xF;
  uint8_t stencilEnable = 0;
  uint8_t stencilFunc = 0;
  uint8_t stencilFail = 0;
  uint8_t stencilZFail = 0;
  uint8_t stencilPass = 0;
  uint8_t stencilReadMask = 0xFF;
  uint8_t stencilWriteMask = 0xFF;
  uint8_t twoSidedStencil = 0;
  uint8_t ccwStencilFunc = 0;
  uint8_t ccwStencilFail = 0;
  uint8_t ccwStencilZFail = 0;
  uint8_t ccwStencilPass = 0;
  uint8_t rtFormat = 0;       // 0 = RGBA8Unorm, 1 = BGRA8Unorm
  uint8_t indexedStrip = 0;   // 1 if strip & indexed (Uint16 stripIndexFormat)
  uint8_t _padKey[3] = {};

  bool operator==(const FfpPipelineKey& o) const {
    return memcmp(this, &o, sizeof(*this)) == 0;
  }
};

struct FfpPipelineKeyHash {
  size_t operator()(const FfpPipelineKey& k) const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(k); ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
    return (size_t)h;
  }
};

struct SamplerKey {
  uint8_t addrU = D3DTADDRESS_WRAP;
  uint8_t addrV = D3DTADDRESS_WRAP;
  uint8_t addrW = D3DTADDRESS_WRAP;
  uint8_t magFilter = D3DTEXF_LINEAR;
  uint8_t minFilter = D3DTEXF_LINEAR;
  uint8_t mipFilter = D3DTEXF_LINEAR;
  uint8_t maxAniso = 1;
  uint8_t _pad = 0;

  uint64_t pack() const {
    return (uint64_t)addrU | ((uint64_t)addrV << 8) | ((uint64_t)addrW << 16) |
           ((uint64_t)magFilter << 24) | ((uint64_t)minFilter << 32) |
           ((uint64_t)mipFilter << 40) | ((uint64_t)maxAniso << 48);
  }
};

// ---------------------------------------------------------------------------
// Global WebGPU GPU context owned by the Dedicated Worker.
// ---------------------------------------------------------------------------
struct WgpuGpuContext {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Surface surface;
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::BGRA8Unorm;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t preferredWidth = 1600;
  uint32_t preferredHeight = 900;
  bool hasBC = false;
  bool ready = false;
  bool isFallbackAdapter = false;
  char adapterVendor[128] = {};
  char adapterArch[128] = {};
  char adapterDevice[128] = {};
  char adapterDesc[128] = {};

  uint32_t errorCount = 0;
  char lastError[512] = {};

  // Internal renderable backbuffer (RGBA8Unorm) + Depth24PlusStencil8.
  wgpu::Texture backbufferTex;
  wgpu::TextureView backbufferView;
  wgpu::Texture depthTex;
  wgpu::TextureView depthView;

  // Fallback 1x1 opaque white 2D, Cube (1x1x6), and 3D Volume (1x1x1) textures + linear sampler.
  wgpu::Texture whiteTex;
  wgpu::TextureView whiteView;
  wgpu::Texture whiteCubeTex;
  wgpu::TextureView whiteCubeView;
  wgpu::Texture whiteVolTex;
  wgpu::TextureView whiteVolView;
  wgpu::Sampler linearClampSampler;

  // Shared FFP / SM 1.1 explicit BindGroupLayouts + PipelineLayouts (indexed by 8-bit texTypes).
  wgpu::BindGroupLayout ffpUniformBgl;
  wgpu::BindGroupLayout ffpTexBgl;
  wgpu::PipelineLayout ffpPipeLayout;
  wgpu::BindGroupLayout texBglByTypes[256];
  wgpu::PipelineLayout pipeLayoutByTypes[256];
  wgpu::BindGroup ffpUniformBg;

  // Registry of all created VS/PS objects for 100% Dawn/Tint shader validation.
  std::vector<WgpuVertexShader*, SysAllocator<WgpuVertexShader*>> allVertexShaders;
  std::vector<WgpuPixelShader*, SysAllocator<WgpuPixelShader*>> allPixelShaders;

  // Fullscreen blit pipeline (RGBA8Unorm backbuffer -> surfaceFormat or RT->RT).
  wgpu::BindGroupLayout blitBgl;
  wgpu::PipelineLayout blitPipeLayout;
  wgpu::RenderPipeline blitToSurfacePipe;
  wgpu::RenderPipeline blitToRgbaPipe;

  // Per-frame streaming ring buffers.
  WgpuDynRing dynVertRing;
  WgpuDynRing dynIdxRing;
  WgpuDynRing dynUniformRing;

  // Readback staging buffer (created lazily on swbf2_wgpu_readback_frame).
  wgpu::Buffer readbackBuf;
  uint32_t readbackW = 0;
  uint32_t readbackH = 0;
  uint32_t readbackRowBytes = 0;
  bool readbackPending = false;

  // Per-frame counters for diagnostics.
  uint32_t frameCount = 0;
  uint32_t lastFrameDraws = 0;
  uint32_t lastFrameTris = 0;
  uint32_t curFrameDraws = 0;
  uint32_t curFrameTris = 0;
  uint32_t lastFrameSkinnedDraws = 0;
  uint32_t lastFrameSkinnedTris = 0;
  uint32_t curFrameSkinnedDraws = 0;
  uint32_t curFrameSkinnedTris = 0;

  void recordError(const char* msg);
  bool recreateBackbuffer(uint32_t w, uint32_t h);
  bool initSharedResources();
  wgpu::BindGroupLayout getOrCreateTexBgl(uint8_t texTypes);
  wgpu::PipelineLayout getOrCreatePipeLayout(uint8_t texTypes);
};

WgpuGpuContext& GetWgpuContext();

// ---------------------------------------------------------------------------
// D3D9 resource classes backed by WebGPU (browser/gpu/d3d9_resources_wgpu.cpp).
// ---------------------------------------------------------------------------
struct WgpuSurface : public IDirect3DSurface9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  WgpuTexture2D* parentTex = nullptr; // non-null if mip 0..N of a texture
  uint32_t parentMip = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  D3DFORMAT format = D3DFMT_A8R8G8B8;
  D3DUSAGE usage = 0;
  D3DPOOL pool = D3DPOOL_DEFAULT;
  bool isBackbuffer = false;
  bool isDepth = false;

  // Owned GPU texture/view when standalone (not backbuffer and not parentTex).
  wgpu::Texture ownedTex;
  wgpu::TextureView ownedView;
  ByteVec cpuShadow;

  WgpuSurface(WgpuD3D9Device* dev, uint32_t w, uint32_t h, D3DFORMAT fmt,
              D3DUSAGE u, D3DPOOL p, bool backbuf, bool depth);
  WgpuSurface(WgpuD3D9Device* dev, WgpuTexture2D* parent, uint32_t mip);
  ~WgpuSurface();

  wgpu::Texture getTexture() const;
  wgpu::TextureView getView() const;
  uint32_t getWidth() const;
  uint32_t getHeight() const;

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_SURFACE; }
  STDMETHOD(GetDesc)(D3DSURFACE_DESC* pDesc) override;
  STDMETHOD(LockRect)(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
  STDMETHOD(UnlockRect)() override;
};

struct WgpuTexture2D : public IDirect3DTexture9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 1;
  D3DFORMAT d3dFormat = D3DFMT_A8R8G8B8;
  D3DUSAGE usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  wgpu::TextureFormat wgpuFormat = wgpu::TextureFormat::RGBA8Unorm;
  bool useHardwareBC = false;
  uint64_t uniqueId = 0;

  wgpu::Texture gpuTex;
  wgpu::TextureView fullView;
  std::vector<wgpu::TextureView, SysAllocator<wgpu::TextureView>> mipViews;

  struct MipLock {
    ByteVec data;
    uint32_t pitch = 0;
    bool locked = false;
    bool readonly = false;
  };
  std::vector<MipLock, SysAllocator<MipLock>> locks;

  WgpuTexture2D(WgpuD3D9Device* dev, uint32_t w, uint32_t h, uint32_t lvls,
                D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p);
  ~WgpuTexture2D();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
  STDMETHOD_(DWORD, GetLevelCount)() override { return levels; }
  STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override;
  STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override;
  STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
  STDMETHOD(UnlockRect)(UINT Level) override;
  STDMETHOD(AddDirtyRect)(CONST RECT*) override { return D3D_OK; }

  void uploadMip(uint32_t level);
};

struct WgpuCubeTexture : public IDirect3DCubeTexture9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t edge = 0;
  uint32_t levels = 1;
  D3DFORMAT d3dFormat = D3DFMT_A8R8G8B8;
  D3DUSAGE usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  uint64_t uniqueId = 0;
  wgpu::Texture gpuTex;
  wgpu::TextureView cubeView;
  ByteVec scratch;

  WgpuCubeTexture(WgpuD3D9Device* dev, uint32_t edgeLen, uint32_t lvls,
                  D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p);
  ~WgpuCubeTexture();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
  STDMETHOD_(DWORD, GetLevelCount)() override { return levels; }
  STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override;
  STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppSurf) override;
  STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect,
                      CONST RECT* pRect, DWORD Flags) override;
  STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override;
  STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, CONST RECT*) override { return D3D_OK; }
};

struct WgpuVolumeTexture : public IDirect3DVolumeTexture9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t width = 0, height = 0, depth = 0, levels = 1;
  D3DFORMAT d3dFormat = D3DFMT_A8R8G8B8;
  D3DUSAGE usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  uint64_t uniqueId = 0;
  wgpu::Texture gpuTex;
  wgpu::TextureView volView;
  ByteVec scratch;

  WgpuVolumeTexture(WgpuD3D9Device* dev, uint32_t w, uint32_t h, uint32_t d, uint32_t lvls,
                    D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p);
  ~WgpuVolumeTexture();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VOLUMETEXTURE; }
  STDMETHOD_(DWORD, GetLevelCount)() override { return levels; }
  STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC* pDesc) override;
  STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) override;
  STDMETHOD(UnlockBox)(UINT Level) override;
  STDMETHOD(AddDirtyBox)(CONST D3DBOX*) override { return D3D_OK; }
};

struct WgpuVertexBuffer : public IDirect3DVertexBuffer9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t length = 0;
  D3DUSAGE usage = 0;
  DWORD fvf = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  bool isDynamic = false;

  // Static buffers hold their own wgpu::Buffer + CPU shadow.
  wgpu::Buffer staticGpu;
  ByteVec cpuShadow;
  uint32_t lockOffset = 0;
  uint32_t lockSize = 0;

  // Dynamic buffers allocate a fresh window inside ctx.dynVertRing on DISCARD / new frame.
  uint32_t ringFrame = 0xFFFFFFFFu;
  uint32_t ringBaseOffset = 0;
  bool ringValid = false;

  WgpuVertexBuffer(WgpuD3D9Device* dev, uint32_t len, D3DUSAGE u, DWORD fvfIn, D3DPOOL p);
  ~WgpuVertexBuffer();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }
  STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override;
  STDMETHOD(Unlock)() override;
  STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* pDesc) override;
};

struct WgpuIndexBuffer : public IDirect3DIndexBuffer9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t length = 0;
  D3DUSAGE usage = 0;
  D3DFORMAT format = D3DFMT_INDEX16;
  D3DPOOL pool = D3DPOOL_MANAGED;
  bool isDynamic = false;

  wgpu::Buffer staticGpu;
  ByteVec cpuShadow;
  uint32_t lockOffset = 0;
  uint32_t lockSize = 0;

  uint32_t ringFrame = 0xFFFFFFFFu;
  uint32_t ringBaseOffset = 0;
  bool ringValid = false;

  WgpuIndexBuffer(WgpuD3D9Device* dev, uint32_t len, D3DUSAGE u, D3DFORMAT fmt, D3DPOOL p);
  ~WgpuIndexBuffer();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }
  STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override;
  STDMETHOD(Unlock)() override;
  STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* pDesc) override;
};

struct WgpuVertexDecl : public IDirect3DVertexDeclaration9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t uniqueId = 0;
  std::vector<D3DVERTEXELEMENT9, SysAllocator<D3DVERTEXELEMENT9>> elements;
  FfpVertexLayout ffpLayout;

  WgpuVertexDecl(WgpuD3D9Device* dev, const D3DVERTEXELEMENT9* pElems);
  ~WgpuVertexDecl() = default;

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9* pElement, UINT* pNumElements) override;
};

struct WgpuVertexShader : public IDirect3DVertexShader9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t uniqueId = 0;
  U32Vec bytecode;
  std::vector<Sm1VsDclInput, SysAllocator<Sm1VsDclInput>> dcls;

  WgpuVertexShader() = default;
  WgpuVertexShader(WgpuD3D9Device* dev, const DWORD* pFunc);
  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) override;
};

struct WgpuPixelShader : public IDirect3DPixelShader9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  uint32_t uniqueId = 0;
  U32Vec bytecode;

  WgpuPixelShader() = default;
  WgpuPixelShader(WgpuD3D9Device* dev, const DWORD* pFunc);
  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) override;
};

struct WgpuQuery : public IDirect3DQuery9, public WgpuSysObj {
  int refCount = 1;
  WgpuD3D9Device* device = nullptr;
  D3DQUERYTYPE type = D3DQUERYTYPE_EVENT;

  WgpuQuery(WgpuD3D9Device* dev, D3DQUERYTYPE t) : device(dev), type(t) {}
  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
  STDMETHOD_(D3DQUERYTYPE, GetType)() override { return type; }
  STDMETHOD_(DWORD, GetDataSize)() override { return sizeof(DWORD); }
  STDMETHOD(Issue)(DWORD) override { return D3D_OK; }
  STDMETHOD(GetData)(void* pData, DWORD dwSize, DWORD) override;
};

// ---------------------------------------------------------------------------
// FFP + DX9 SM 1.1 WGSL generator & helper declarations
// (browser/gpu/d3d9_ffp_wgsl.cpp & browser/gpu/dx9_sm1_wgsl.cpp).
// ---------------------------------------------------------------------------
FfpVertexLayout BuildFfpLayoutFromFVF(DWORD fvf);
std::string GenerateFfpWGSL(const FfpVertexLayout& layout);
std::string GenerateCombinedWGSL(const WgpuVertexShader* vs, uint64_t vsInputTypeKey,
                                 const FfpVertexLayout& ffpLayout,
                                 const WgpuPixelShader* ps, uint8_t texTypes);
const char* GetBlitWGSL();

void ParseSm1VertexShaderDcls(const uint32_t* words, size_t wordCount,
                              std::vector<Sm1VsDclInput, SysAllocator<Sm1VsDclInput>>& outDcls);
std::string GenerateSm1VertexShaderWGSL(const uint32_t* words, size_t wordCount, uint64_t vsInputTypeKey);
std::string GenerateSm1PixelShaderWGSL(const uint32_t* words, size_t wordCount, uint8_t texTypes);
uint32_t GetTranslatedVsCount();
uint32_t GetTranslatedPsCount();
uint8_t MapD3DDeclTypeToVtxFmt(unsigned char d3dDeclType, uint16_t* outBytes);
wgpu::VertexFormat ToWgpuVertexFormat(uint8_t fmt);

// Decode BC1/2/3 & uncompressed formats into RGBA8Unorm.
void DecodeTextureLevelToRGBA8(D3DFORMAT fmt, uint32_t w, uint32_t h,
                               const uint8_t* src, uint32_t srcPitch,
                               uint8_t* dstRGBA8);
uint32_t CalcD3D9SurfacePitch(D3DFORMAT fmt, uint32_t w);
uint32_t CalcD3D9SurfaceByteSize(D3DFORMAT fmt, uint32_t w, uint32_t h);
bool IsCompressedD3DFormat(D3DFORMAT fmt);

// C-linkage host entry points (exported to browser/worker.mjs).
extern "C" {
void swbf2_wgpu_init(int width, int height);
int swbf2_wgpu_is_ready(void);
int swbf2_wgpu_has_bc(void);
int swbf2_wgpu_error_count(void);
const char* swbf2_wgpu_last_error(void);
void swbf2_wgpu_resize(int width, int height);
void swbf2_wgpu_readback_frame(void);
int swbf2_wgpu_draw_calls(void);
int swbf2_wgpu_triangles(void);
int swbf2_wgpu_skinned_draws(void);
int swbf2_wgpu_skinned_triangles(void);
}
