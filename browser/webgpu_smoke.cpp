// Phase 0 WebGPU smoke test (emdawnwebgpu + Dedicated Worker OffscreenCanvas).
//
// Proves the four GPU primitives SWBF2's D3D9 -> WebGPU backend depends on:
//   1. wgpu::Instance -> RequestAdapter -> RequestDevice -> OffscreenCanvas Surface in a Worker.
//   2. D3D9 [0, 1] depth buffer (Depth24PlusStencil8, LessEqual) with red/green near/far swap.
//   3. BC1 (DXT1) compressed texture upload when `texture-compression-bc` is present, and
//      exact software BC1 -> RGBA8 decode fallback when it is absent (e.g. SwiftShader/mobile).
//   4. Exact GPU pixel readback via CopyTextureToBuffer + Buffer::MapAsync.

#include <webgpu/webgpu_cpp.h>
#include <emscripten.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EM_JS(void, js_register_canvas_target, (), {
  if (typeof specialHTMLTargets !== 'undefined' && Module['canvas']) {
    specialHTMLTargets['!canvas'] = Module['canvas'];
  }
});

EM_JS(void, js_report_result, (const char* json), {
  const s = UTF8ToString(json);
  if (typeof Module['onSmokeComplete'] === 'function') {
    Module['onSmokeComplete'](s);
  }
});

// Software BC1 (DXT1) 4x4 block -> RGBA8 decoder.
// Matches D3D9 DXT1 4-color mode (c0 > c1) and 3-color + 1-bit alpha mode (c0 <= c1).
static void decode_bc1_block(const uint8_t blk[8], uint8_t rgba[4 * 4 * 4]) {
  const uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
  const uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
  const uint32_t bits = (uint32_t)blk[4] | ((uint32_t)blk[5] << 8) |
                        ((uint32_t)blk[6] << 16) | ((uint32_t)blk[7] << 24);

  auto expand565 = [](uint16_t c, uint8_t out[4]) {
    uint8_t r = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g = (uint8_t)((c >> 5) & 0x3F);
    uint8_t b = (uint8_t)(c & 0x1F);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255;
  };

  uint8_t pal[4][4];
  expand565(c0, pal[0]);
  expand565(c1, pal[1]);
  if (c0 > c1) {
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

static const char* kColorShaderWGSL = R"(
struct Uniforms {
  color: vec4<f32>,
  depth: f32,
  _pad0: f32,
  _pad1: f32,
  _pad2: f32,
};
@group(0) @binding(0) var<uniform> u: Uniforms;

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> @builtin(position) vec4<f32> {
  var pos = array<vec2<f32>, 6>(
    vec2<f32>(-0.8, -0.8), vec2<f32>( 0.8, -0.8), vec2<f32>( 0.8,  0.8),
    vec2<f32>(-0.8, -0.8), vec2<f32>( 0.8,  0.8), vec2<f32>(-0.8,  0.8)
  );
  return vec4<f32>(pos[vid], u.depth, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
  return u.color;
}
)";

static const char* kTexShaderWGSL = R"(
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var tex: texture_2d<f32>;

struct VSOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VSOut {
  var pos = array<vec2<f32>, 6>(
    vec2<f32>(-1.0, -1.0), vec2<f32>( 1.0, -1.0), vec2<f32>( 1.0,  1.0),
    vec2<f32>(-1.0, -1.0), vec2<f32>( 1.0,  1.0), vec2<f32>(-1.0,  1.0)
  );
  var uv = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 1.0), vec2<f32>(1.0, 0.0),
    vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 0.0)
  );
  var o: VSOut;
  o.pos = vec4<f32>(pos[vid], 0.5, 1.0);
  o.uv = uv[vid];
  return o;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  return textureSample(tex, samp, in.uv);
}
)";

struct ColorUniforms {
  float color[4];
  float depth;
  float pad[3];
};

struct SmokeState {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Surface surface;
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::BGRA8Unorm;
  bool hasBC = false;
  bool surfaceOk = false;
  char vendor[128] = {};
  char arch[128] = {};
  char deviceName[128] = {};
  char desc[128] = {};
  bool isFallbackAdapter = false;
  uint32_t width = 256;
  uint32_t height = 256;

  // Readback buffers for the 3 GPU tests (each is a 64x64 RGBA8Unorm render target;
  // bytesPerRow = 256 satisfies WebGPU's 256-byte alignment requirement).
  static constexpr uint32_t kRT = 64;
  static constexpr uint32_t kRowBytes = 256;
  static constexpr uint32_t kBufBytes = kRowBytes * kRT;
  wgpu::Buffer readback[3];
  int mappedCount = 0;
  uint32_t centerRedNear = 0;
  uint32_t centerGreenNear = 0;
  uint32_t texLeftPixel = 0;
  uint32_t texRightPixel = 0;
  char errorMsg[1024] = {};
  void appendError(const char* fmt, ...) {
    size_t cur = strlen(errorMsg);
    if (cur >= sizeof(errorMsg) - 4) return;
    if (cur > 0) {
      errorMsg[cur++] = ';';
      errorMsg[cur++] = ' ';
      errorMsg[cur] = '\0';
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errorMsg + cur, sizeof(errorMsg) - cur, fmt, ap);
    va_end(ap);
  }
};

static SmokeState* g_state = nullptr;

static uint32_t pack_rgba(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void finish_report(SmokeState* st) {
  auto r = [](uint32_t px) { return (px >> 24) & 0xFF; };
  auto g = [](uint32_t px) { return (px >> 16) & 0xFF; };
  auto b = [](uint32_t px) { return (px >> 8) & 0xFF; };

  bool depthRedNearOk = (r(st->centerRedNear) > 200 && g(st->centerRedNear) < 30 && b(st->centerRedNear) < 30);
  bool depthGreenNearOk = (g(st->centerGreenNear) > 200 && r(st->centerGreenNear) < 30 && b(st->centerGreenNear) < 30);
  // Left half of BC1 block is yellow (255,255,0), right half is cyan (0,255,255)
  bool texLeftYellowOk = (r(st->texLeftPixel) > 200 && g(st->texLeftPixel) > 200 && b(st->texLeftPixel) < 30);
  bool texRightCyanOk = (r(st->texRightPixel) < 30 && g(st->texRightPixel) > 200 && b(st->texRightPixel) > 200);
  bool ok = (st->errorMsg[0] == '\0') && st->surfaceOk && depthRedNearOk &&
            depthGreenNearOk && texLeftYellowOk && texRightCyanOk;

  char safeErr[1024];
  size_t ei = 0;
  for (size_t i = 0; st->errorMsg[i] && ei + 1 < sizeof(safeErr); ++i) {
    char c = st->errorMsg[i];
    safeErr[ei++] = (c == '"' || c == '\\' || (unsigned char)c < 32) ? ' ' : c;
  }
  safeErr[ei] = '\0';

  char json[3072];
  snprintf(json, sizeof(json),
           "{"
           "\"ok\":%s,"
           "\"surfaceOk\":%s,"
           "\"hasTextureCompressionBC\":%s,"
           "\"bcMode\":\"%s\","
           "\"isFallbackAdapter\":%s,"
           "\"adapter\":{\"vendor\":\"%s\",\"architecture\":\"%s\",\"device\":\"%s\",\"description\":\"%s\"},"
           "\"depthRedNearOk\":%s,"
           "\"depthGreenNearOk\":%s,"
           "\"texLeftYellowOk\":%s,"
           "\"texRightCyanOk\":%s,"
           "\"pixels\":{"
             "\"redNear\":\"0x%08x\","
             "\"greenNear\":\"0x%08x\","
             "\"texLeft\":\"0x%08x\","
             "\"texRight\":\"0x%08x\""
           "},"
           "\"error\":\"%s\""
           "}",
           ok ? "true" : "false",
           st->surfaceOk ? "true" : "false",
           st->hasBC ? "true" : "false",
           st->hasBC ? "hardware-bc1" : "software-bc1-rgba8-fallback",
           st->isFallbackAdapter ? "true" : "false",
           st->vendor, st->arch, st->deviceName, st->desc,
           depthRedNearOk ? "true" : "false",
           depthGreenNearOk ? "true" : "false",
           texLeftYellowOk ? "true" : "false",
           texRightCyanOk ? "true" : "false",
           st->centerRedNear, st->centerGreenNear, st->texLeftPixel, st->texRightPixel,
           safeErr);
  js_report_result(json);
}

static void run_gpu_tests(SmokeState* st) {
  js_register_canvas_target();

  // 1. Create and configure the OffscreenCanvas WebGPU surface.
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
  canvasDesc.selector = "!canvas";
  wgpu::SurfaceDescriptor surfDesc{};
  surfDesc.nextInChain = &canvasDesc;
  st->surface = st->instance.CreateSurface(&surfDesc);
  if (st->surface) {
    wgpu::SurfaceCapabilities caps{};
    st->surface.GetCapabilities(st->adapter, &caps);
    if (caps.formatCount > 0) {
      st->surfaceFormat = caps.formats[0];
    }
    wgpu::SurfaceConfiguration config{};
    config.device = st->device;
    config.format = st->surfaceFormat;
    config.width = st->width;
    config.height = st->height;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.presentMode = wgpu::PresentMode::Fifo;
    st->surface.Configure(&config);
    st->surfaceOk = true;
  } else {
    snprintf(st->errorMsg, sizeof(st->errorMsg), "CreateSurface(!canvas) returned null");
    finish_report(st);
    return;
  }

  // 2. Build depth-tested solid color pipeline (D3D9 [0,1] depth, LessEqual).
  wgpu::ShaderSourceWGSL colorWgsl{};
  colorWgsl.code = kColorShaderWGSL;
  wgpu::ShaderModuleDescriptor colorSmDesc{};
  colorSmDesc.nextInChain = &colorWgsl;
  wgpu::ShaderModule colorMod = st->device.CreateShaderModule(&colorSmDesc);

  wgpu::ColorTargetState rgbaTarget{};
  rgbaTarget.format = wgpu::TextureFormat::RGBA8Unorm;

  wgpu::FragmentState colorFrag{};
  colorFrag.module = colorMod;
  colorFrag.entryPoint = "fs_main";
  colorFrag.targetCount = 1;
  colorFrag.targets = &rgbaTarget;

  wgpu::DepthStencilState depthState{};
  depthState.format = wgpu::TextureFormat::Depth24PlusStencil8;
  depthState.depthWriteEnabled = wgpu::OptionalBool::True;
  depthState.depthCompare = wgpu::CompareFunction::LessEqual;

  wgpu::RenderPipelineDescriptor colorPipeDesc{};
  colorPipeDesc.vertex.module = colorMod;
  colorPipeDesc.vertex.entryPoint = "vs_main";
  colorPipeDesc.fragment = &colorFrag;
  colorPipeDesc.depthStencil = &depthState;
  colorPipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  wgpu::RenderPipeline colorPipe = st->device.CreateRenderPipeline(&colorPipeDesc);

  // 3. Build textured quad pipeline with an EXPLICIT BindGroupLayout + PipelineLayout so both
  //    the offscreen RGBA8Unorm pipeline and the live swapchain surface pipeline share BindGroups.
  wgpu::BindGroupLayoutEntry texLayoutEntries[2]{};
  texLayoutEntries[0].binding = 0;
  texLayoutEntries[0].visibility = wgpu::ShaderStage::Fragment;
  texLayoutEntries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
  texLayoutEntries[1].binding = 1;
  texLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
  texLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
  texLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  wgpu::BindGroupLayoutDescriptor texBglDesc{};
  texBglDesc.entryCount = 2;
  texBglDesc.entries = texLayoutEntries;
  wgpu::BindGroupLayout texBgl = st->device.CreateBindGroupLayout(&texBglDesc);

  wgpu::PipelineLayoutDescriptor texPlDesc{};
  texPlDesc.bindGroupLayoutCount = 1;
  texPlDesc.bindGroupLayouts = &texBgl;
  wgpu::PipelineLayout texPipeLayout = st->device.CreatePipelineLayout(&texPlDesc);

  wgpu::ShaderSourceWGSL texWgsl{};
  texWgsl.code = kTexShaderWGSL;
  wgpu::ShaderModuleDescriptor texSmDesc{};
  texSmDesc.nextInChain = &texWgsl;
  wgpu::ShaderModule texMod = st->device.CreateShaderModule(&texSmDesc);

  wgpu::FragmentState texFrag{};
  texFrag.module = texMod;
  texFrag.entryPoint = "fs_main";
  texFrag.targetCount = 1;
  texFrag.targets = &rgbaTarget;

  wgpu::RenderPipelineDescriptor texPipeDesc{};
  texPipeDesc.layout = texPipeLayout;
  texPipeDesc.vertex.module = texMod;
  texPipeDesc.vertex.entryPoint = "vs_main";
  texPipeDesc.fragment = &texFrag;
  texPipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  wgpu::RenderPipeline texPipe = st->device.CreateRenderPipeline(&texPipeDesc);

  wgpu::ColorTargetState surfTarget{};
  surfTarget.format = st->surfaceFormat;
  wgpu::FragmentState surfFrag = texFrag;
  surfFrag.targets = &surfTarget;
  wgpu::RenderPipelineDescriptor surfPipeDesc = texPipeDesc;
  surfPipeDesc.layout = texPipeLayout;
  surfPipeDesc.fragment = &surfFrag;
  wgpu::RenderPipeline surfPipe = st->device.CreateRenderPipeline(&surfPipeDesc);

  // 4. Create 4x4 BC1 test texture:
  //    c0 = RGB565(31,63,0) = 0xFFE0 (yellow: 255,255,0)
  //    c1 = RGB565(0,63,31) = 0x07FF (cyan:   0,255,255)
  //    Since c0 (0xFFE0) > c1 (0x07FF), 4-color mode: index 0 = yellow, index 1 = cyan.
  //    Each 4-pixel row has bits: col0=0, col1=0, col2=1, col3=1 -> binary 01_01_00_00 = 0x50.
  const uint8_t bc1Block[8] = {0xE0, 0xFF, 0xFF, 0x07, 0x50, 0x50, 0x50, 0x50};

  wgpu::TextureDescriptor texDesc{};
  texDesc.size = {4, 4, 1};
  texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  texDesc.format = st->hasBC ? wgpu::TextureFormat::BC1RGBAUnorm : wgpu::TextureFormat::RGBA8Unorm;
  wgpu::Texture testTex = st->device.CreateTexture(&texDesc);

  wgpu::TexelCopyTextureInfo dstTex{};
  dstTex.texture = testTex;
  wgpu::Extent3D texExtent{4, 4, 1};
  if (st->hasBC) {
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 8;
    layout.rowsPerImage = 1;
    st->queue.WriteTexture(&dstTex, bc1Block, sizeof(bc1Block), &layout, &texExtent);
  } else {
    uint8_t rgba[4 * 4 * 4];
    decode_bc1_block(bc1Block, rgba);
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 16;
    layout.rowsPerImage = 4;
    st->queue.WriteTexture(&dstTex, rgba, sizeof(rgba), &layout, &texExtent);
  }

  wgpu::SamplerDescriptor sampDesc{};
  sampDesc.magFilter = wgpu::FilterMode::Nearest;
  sampDesc.minFilter = wgpu::FilterMode::Nearest;
  wgpu::Sampler samp = st->device.CreateSampler(&sampDesc);

  wgpu::BindGroupEntry texEntries[2]{};
  texEntries[0].binding = 0;
  texEntries[0].sampler = samp;
  texEntries[1].binding = 1;
  texEntries[1].textureView = testTex.CreateView();
  wgpu::BindGroupDescriptor texBgDesc{};
  texBgDesc.layout = texBgl;
  texBgDesc.entryCount = 2;
  texBgDesc.entries = texEntries;
  wgpu::BindGroup texBg = st->device.CreateBindGroup(&texBgDesc);

  // 5. Create offscreen render target + depth buffer + uniform buffers + readback buffers.
  wgpu::TextureDescriptor rtDesc{};
  rtDesc.size = {SmokeState::kRT, SmokeState::kRT, 1};
  rtDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  rtDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture rt = st->device.CreateTexture(&rtDesc);
  wgpu::TextureView rtView = rt.CreateView();

  wgpu::TextureDescriptor depthDesc{};
  depthDesc.size = {SmokeState::kRT, SmokeState::kRT, 1};
  depthDesc.format = wgpu::TextureFormat::Depth24PlusStencil8;
  depthDesc.usage = wgpu::TextureUsage::RenderAttachment;
  wgpu::Texture depthTex = st->device.CreateTexture(&depthDesc);
  wgpu::TextureView depthView = depthTex.CreateView();

  auto makeColorBG = [&](float r, float g, float b, float z) {
    ColorUniforms u{{r, g, b, 1.0f}, z, {0, 0, 0}};
    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(ColorUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer buf = st->device.CreateBuffer(&bd);
    st->queue.WriteBuffer(buf, 0, &u, sizeof(u));
    wgpu::BindGroupEntry e{};
    e.binding = 0;
    e.buffer = buf;
    e.size = sizeof(u);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = colorPipe.GetBindGroupLayout(0);
    bgd.entryCount = 1;
    bgd.entries = &e;
    return st->device.CreateBindGroup(&bgd);
  };

  wgpu::BindGroup bgRedNear = makeColorBG(1.0f, 0.0f, 0.0f, 0.2f);
  wgpu::BindGroup bgGreenFar = makeColorBG(0.0f, 1.0f, 0.0f, 0.8f);
  wgpu::BindGroup bgRedFar = makeColorBG(1.0f, 0.0f, 0.0f, 0.8f);
  wgpu::BindGroup bgGreenNear = makeColorBG(0.0f, 1.0f, 0.0f, 0.2f);

  for (int i = 0; i < 3; ++i) {
    wgpu::BufferDescriptor rbd{};
    rbd.size = SmokeState::kBufBytes;
    rbd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    st->readback[i] = st->device.CreateBuffer(&rbd);
  }

  wgpu::CommandEncoder enc = st->device.CreateCommandEncoder();

  auto copyRTToBuffer = [&](int idx) {
    wgpu::TexelCopyTextureInfo src{};
    src.texture = rt;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = st->readback[idx];
    dst.layout.bytesPerRow = SmokeState::kRowBytes;
    dst.layout.rowsPerImage = SmokeState::kRT;
    wgpu::Extent3D ext{SmokeState::kRT, SmokeState::kRT, 1};
    enc.CopyTextureToBuffer(&src, &dst, &ext);
  };

  auto runDepthPass = [&](wgpu::BindGroup first, wgpu::BindGroup second, int outIdx) {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = rtView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDepthStencilAttachment dsa{};
    dsa.view = depthView;
    dsa.depthLoadOp = wgpu::LoadOp::Clear;
    dsa.depthStoreOp = wgpu::StoreOp::Store;
    dsa.depthClearValue = 1.0f;
    dsa.stencilLoadOp = wgpu::LoadOp::Clear;
    dsa.stencilStoreOp = wgpu::StoreOp::Discard;
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &ca;
    rpd.depthStencilAttachment = &dsa;
    wgpu::RenderPassEncoder rp = enc.BeginRenderPass(&rpd);
    rp.SetPipeline(colorPipe);
    // Draw near first, then far second -> depth test must reject the far draw!
    rp.SetBindGroup(0, first);
    rp.Draw(6);
    rp.SetBindGroup(0, second);
    rp.Draw(6);
    rp.End();
    copyRTToBuffer(outIdx);
  };

  // Pass 0: Red near (z=0.2) then Green far (z=0.8) -> center must stay Red.
  runDepthPass(bgRedNear, bgGreenFar, 0);
  // Pass 1: Green near (z=0.2) then Red far (z=0.8) -> center must stay Green.
  runDepthPass(bgGreenNear, bgRedFar, 1);

  // Pass 2: Sample BC1/RGBA8 texture onto offscreen RT -> left yellow, right cyan.
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = rtView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &ca;
    wgpu::RenderPassEncoder rp = enc.BeginRenderPass(&rpd);
    rp.SetPipeline(texPipe);
    rp.SetBindGroup(0, texBg);
    rp.Draw(6);
    rp.End();
    copyRTToBuffer(2);
  }

  // Pass 3: Render BC1/RGBA8 texture onto the live OffscreenCanvas WebGPU surface.
  {
    wgpu::SurfaceTexture surfTex{};
    st->surface.GetCurrentTexture(&surfTex);
    if (surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
        surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
      wgpu::RenderPassColorAttachment ca{};
      ca.view = surfTex.texture.CreateView();
      ca.loadOp = wgpu::LoadOp::Clear;
      ca.storeOp = wgpu::StoreOp::Store;
      ca.clearValue = {0.05, 0.07, 0.12, 1.0};
      wgpu::RenderPassDescriptor rpd{};
      rpd.colorAttachmentCount = 1;
      rpd.colorAttachments = &ca;
      wgpu::RenderPassEncoder rp = enc.BeginRenderPass(&rpd);
      rp.SetPipeline(surfPipe);
      rp.SetBindGroup(0, texBg);
      rp.Draw(6);
      rp.End();
    } else {
      st->surfaceOk = false;
      snprintf(st->errorMsg, sizeof(st->errorMsg), "GetCurrentTexture failed (status %d)",
               (int)surfTex.status);
    }
  }

  wgpu::CommandBuffer cmd = enc.Finish();
  st->queue.Submit(1, &cmd);

  // 6. Map all 3 readback buffers and inspect exact pixels.
  for (int idx = 0; idx < 3; ++idx) {
        st->readback[idx].MapAsync(
        wgpu::MapMode::Read, 0, SmokeState::kBufBytes, wgpu::CallbackMode::AllowSpontaneous,
        [st, idx](wgpu::MapAsyncStatus status, wgpu::StringView msg) {
          if (status != wgpu::MapAsyncStatus::Success) {
            st->appendError("MapAsync[%d] failed (%d): %.*s",
                            idx, (int)status, (int)msg.length, msg.data ? msg.data : "");
          } else {
            const uint8_t* data =
                static_cast<const uint8_t*>(st->readback[idx].GetConstMappedRange(0, SmokeState::kBufBytes));
            if (data) {
              const uint32_t midY = SmokeState::kRT / 2;
              if (idx == 0) {
                st->centerRedNear = pack_rgba(data + midY * SmokeState::kRowBytes + (SmokeState::kRT / 2) * 4);
              } else if (idx == 1) {
                st->centerGreenNear = pack_rgba(data + midY * SmokeState::kRowBytes + (SmokeState::kRT / 2) * 4);
              } else if (idx == 2) {
                st->texLeftPixel = pack_rgba(data + midY * SmokeState::kRowBytes + (SmokeState::kRT / 4) * 4);
                st->texRightPixel = pack_rgba(data + midY * SmokeState::kRowBytes + ((SmokeState::kRT * 3) / 4) * 4);
              }
            }
            st->readback[idx].Unmap();
          }
          st->mappedCount++;
          if (st->mappedCount == 3) {
            finish_report(st);
          }
        });
  }
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
void wgpu_smoke_run(int width, int height) {
  delete g_state;
  g_state = new SmokeState();
  g_state->width = width > 0 ? (uint32_t)width : 256;
  g_state->height = height > 0 ? (uint32_t)height : 256;
  g_state->instance = wgpu::CreateInstance();

  wgpu::RequestAdapterOptions opts{};
  opts.powerPreference = wgpu::PowerPreference::HighPerformance;

  g_state->instance.RequestAdapter(
      &opts, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
        SmokeState* st = g_state;
        if (status != wgpu::RequestAdapterStatus::Success || !adapter) {
          snprintf(st->errorMsg, sizeof(st->errorMsg), "RequestAdapter failed: %.*s",
                   (int)msg.length, msg.data ? msg.data : "no adapter");
          finish_report(st);
          return;
        }
        st->adapter = adapter;

        wgpu::AdapterInfo info{};
        adapter.GetInfo(&info);
        auto copyStr = [](char* dst, size_t n, wgpu::StringView sv) {
          size_t len = sv.data ? sv.length : 0;
          if (len >= n) len = n - 1;
          for (size_t i = 0; i < len; ++i) {
            char c = sv.data[i];
            dst[i] = (c == '"' || c == '\\' || (unsigned char)c < 32) ? '_' : c;
          }
          dst[len] = '\0';
        };
        copyStr(st->vendor, sizeof(st->vendor), info.vendor);
        copyStr(st->arch, sizeof(st->arch), info.architecture);
        copyStr(st->deviceName, sizeof(st->deviceName), info.device);
        copyStr(st->desc, sizeof(st->desc), info.description);
        st->isFallbackAdapter = (info.adapterType == wgpu::AdapterType::CPU);

        st->hasBC = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
        wgpu::FeatureName reqFeat = wgpu::FeatureName::TextureCompressionBC;
        wgpu::DeviceDescriptor devDesc{};
        if (st->hasBC) {
          devDesc.requiredFeatureCount = 1;
          devDesc.requiredFeatures = &reqFeat;
        }
        devDesc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView err) {
              if (g_state) {
                g_state->appendError("UncapturedError(%d): %.*s",
                                     (int)type, (int)err.length, err.data ? err.data : "");
              }
            });
        devDesc.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
              if (g_state) {
                g_state->appendError("DeviceLost(%d): %.*s",
                                     (int)reason, (int)msg.length, msg.data ? msg.data : "");
              }
            });

        adapter.RequestDevice(
            &devDesc, wgpu::CallbackMode::AllowSpontaneous,
            [](wgpu::RequestDeviceStatus dstatus, wgpu::Device device, wgpu::StringView dmsg) {
              SmokeState* st2 = g_state;
              if (dstatus != wgpu::RequestDeviceStatus::Success || !device) {
                snprintf(st2->errorMsg, sizeof(st2->errorMsg), "RequestDevice failed: %.*s",
                         (int)dmsg.length, dmsg.data ? dmsg.data : "no device");
                finish_report(st2);
                return;
              }
              st2->device = device;
              st2->queue = device.GetQueue();
              run_gpu_tests(st2);
            });
      });
}

}  // extern "C"
