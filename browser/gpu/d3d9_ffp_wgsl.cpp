// Fixed-Function Pipeline (FFP) WGSL generator + fullscreen blit WGSL for D3D9 -> WebGPU.
//
// Parameterized strictly by vertex input layout (FfpVertexLayout) while keeping all D3D9
// transforms, lights, material, 4-stage texture combiner ops/args/TCI/matrices, fog, and
// alpha-test uniform-driven. This yields < 30 unique WGSL shader modules for an entire SWBF2
// map while faithfully supporting every fixed-function render state used by SWBF2's 25 shaders.

#include "d3d9_wgpu_internal.h"

#include <sstream>

uint64_t FfpVertexLayout::shaderKey() const {
  uint64_t k = isRHW ? 1ULL : 0ULL;
  k |= ((uint64_t)pos.format << 4);
  k |= ((uint64_t)normal.format << 8);
  k |= ((uint64_t)diffuse.format << 12);
  k |= ((uint64_t)specular.format << 16);
  for (int i = 0; i < 4; ++i) {
    k |= ((uint64_t)tex[i].format << (20 + i * 4));
  }
  return k;
}

wgpu::VertexFormat ToWgpuVertexFormat(uint8_t fmt) {
  switch (fmt) {
    case WGPU_VTX_FLOAT1:   return wgpu::VertexFormat::Float32;
    case WGPU_VTX_FLOAT2:   return wgpu::VertexFormat::Float32x2;
    case WGPU_VTX_FLOAT3:   return wgpu::VertexFormat::Float32x3;
    case WGPU_VTX_FLOAT4:   return wgpu::VertexFormat::Float32x4;
    case WGPU_VTX_D3DCOLOR: return wgpu::VertexFormat::Unorm8x4;
    case WGPU_VTX_SHORT2:   return wgpu::VertexFormat::Sint16x2;
    case WGPU_VTX_SHORT4:   return wgpu::VertexFormat::Sint16x4;
    case WGPU_VTX_UBYTE4:   return wgpu::VertexFormat::Uint8x4;
    default:                return wgpu::VertexFormat::Float32x3;
  }
}

FfpVertexLayout BuildFfpLayoutFromFVF(DWORD fvf) {
  FfpVertexLayout l{};
  uint16_t off = 0;
  DWORD posMask = fvf & D3DFVF_POSITION_MASK;
  if (posMask == D3DFVF_XYZRHW) {
    l.isRHW = true;
    l.pos = {off, WGPU_VTX_FLOAT4, 1};
    off += 16;
  } else if (posMask == D3DFVF_XYZ) {
    l.pos = {off, WGPU_VTX_FLOAT3, 1};
    off += 12;
  } else if (posMask == D3DFVF_XYZB1) {
    l.pos = {off, WGPU_VTX_FLOAT3, 1};
    off += 16;
  } else if (posMask == D3DFVF_XYZB2) {
    l.pos = {off, WGPU_VTX_FLOAT3, 1};
    off += 20;
  } else if (posMask == D3DFVF_XYZB3) {
    l.pos = {off, WGPU_VTX_FLOAT3, 1};
    off += 24;
  } else if (posMask == D3DFVF_XYZB4) {
    l.pos = {off, WGPU_VTX_FLOAT3, 1};
    off += 28;
  }
  if (fvf & D3DFVF_NORMAL) {
    l.normal = {off, WGPU_VTX_FLOAT3, 1};
    off += 12;
  }
  if (fvf & D3DFVF_DIFFUSE) {
    l.diffuse = {off, WGPU_VTX_D3DCOLOR, 1};
    off += 4;
  }
  if (fvf & D3DFVF_SPECULAR) {
    l.specular = {off, WGPU_VTX_D3DCOLOR, 1};
    off += 4;
  }
  uint32_t texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  if (texCount > 4) texCount = 4;
  for (uint32_t i = 0; i < texCount; ++i) {
    l.tex[i] = {off, WGPU_VTX_FLOAT2, 1};
    off += 8;
  }
  l.minStride = off;
  return l;
}

static const char* wgslInputType(uint8_t fmt) {
  switch (fmt) {
    case WGPU_VTX_FLOAT1:   return "f32";
    case WGPU_VTX_FLOAT2:   return "vec2<f32>";
    case WGPU_VTX_FLOAT3:   return "vec3<f32>";
    case WGPU_VTX_FLOAT4:   return "vec4<f32>";
    case WGPU_VTX_D3DCOLOR: return "vec4<f32>";
    case WGPU_VTX_SHORT2:   return "vec2<i32>";
    case WGPU_VTX_SHORT4:   return "vec4<i32>";
    case WGPU_VTX_UBYTE4:   return "vec4<u32>";
    default:                return "vec3<f32>";
  }
}

static void emitCommonHeader(std::ostringstream& ss, uint8_t texTypes) {
  ss << R"(
struct FfpDrawUniforms {
  world: mat4x4<f32>,
  view: mat4x4<f32>,
  proj: mat4x4<f32>,
  texMat: array<mat4x4<f32>, 4>,
  matDiffuse: vec4<f32>,
  matAmbient: vec4<f32>,
  matEmissive: vec4<f32>,
  ambientColor: vec4<f32>,
  tfactor: vec4<f32>,
  fogColor: vec4<f32>,
  fogParams: vec4<f32>,
  viewport: vec4<f32>,
  alphaTest: vec4<f32>,
  matSources: vec4<f32>,
  lightPos: array<vec4<f32>, 4>,
  lightDiff: array<vec4<f32>, 4>,
  lightAtten: array<vec4<f32>, 4>,
  stageColor: array<vec4<u32>, 4>,
  stageAlpha: array<vec4<u32>, 4>,
  stageTex: array<vec4<u32>, 4>,
  stageConst: array<vec4<f32>, 4>,
  bumpMat: array<vec4<f32>, 4>,
  vsConst: array<vec4<f32>, 96>,
  psConst: array<vec4<f32>, 8>,
  dbg: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: FfpDrawUniforms;
)";
  for (int s = 0; s < 4; ++s) {
    uint8_t st = (texTypes >> (s * 2)) & 3u;
    const char* wgslTexType = (st == 1) ? "texture_cube<f32>" : (st == 2 ? "texture_3d<f32>" : "texture_2d<f32>");
    ss << "@group(1) @binding(" << s << ") var tTex" << s << ": " << wgslTexType << ";\n";
  }
  ss << R"(@group(1) @binding(4) var sSam0: sampler;
@group(1) @binding(5) var sSam1: sampler;
@group(1) @binding(6) var sSam2: sampler;
@group(1) @binding(7) var sSam3: sampler;

struct VSOut {
  @builtin(position) clipPos: vec4<f32>,
  @location(0) color: vec4<f32>,
  @location(1) specColor: vec4<f32>,
  @location(2) uv0: vec4<f32>,
  @location(3) uv1: vec4<f32>,
  @location(4) uv2: vec4<f32>,
  @location(5) uv3: vec4<f32>,
  @location(6) fogFactor: f32,
};
)";
}

static void emitFfpVertexShader(std::ostringstream& ss, const FfpVertexLayout& layout) {
  ss << "struct VSIn {\n";

  int loc = 0;
  if (layout.pos.present) {
    ss << "  @location(" << loc++ << ") pos: " << wgslInputType(layout.pos.format) << ",\n";
  }
  if (layout.normal.present) {
    ss << "  @location(" << loc++ << ") normal: " << wgslInputType(layout.normal.format) << ",\n";
  }
  if (layout.diffuse.present) {
    ss << "  @location(" << loc++ << ") diffuse: " << wgslInputType(layout.diffuse.format) << ",\n";
  }
  if (layout.specular.present) {
    ss << "  @location(" << loc++ << ") specular: " << wgslInputType(layout.specular.format) << ",\n";
  }
  for (int i = 0; i < 4; ++i) {
    if (layout.tex[i].present) {
      ss << "  @location(" << loc++ << ") tex" << i << ": " << wgslInputType(layout.tex[i].format) << ",\n";
    }
  }
  if (loc == 0) {
    ss << "  @builtin(vertex_index) vid: u32,\n";
  }

  ss << R"(};

fn computeStageUV(stage: u32, rawUVs: array<vec2<f32>, 4>, camPos: vec3<f32>, camNrm: vec3<f32>) -> vec4<f32> {
  let cfg = u.stageTex[stage];
  let uvIdx = min(cfg.x, 3u);
  let tci = cfg.y;
  let ttff = cfg.z;

  var base: vec4<f32>;
  if (tci == 0x10000u) {
    // D3DTSS_TCI_CAMERASPACENORMAL
    base = vec4<f32>(camNrm, 1.0);
  } else if (tci == 0x20000u) {
    // D3DTSS_TCI_CAMERASPACEPOSITION
    base = vec4<f32>(camPos, 1.0);
  } else if (tci == 0x30000u) {
    // D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR
    let v = normalize(camPos);
    base = vec4<f32>(reflect(v, camNrm), 1.0);
  } else {
    // D3DTSS_TCI_PASSTHRU: D3D9 extends 2D UV (u,v) to (u,v,1,0) so row 2 translates 2D UVs.
    let uv = rawUVs[uvIdx];
    base = vec4<f32>(uv.x, uv.y, 1.0, 0.0);
  }

  let count = ttff & 0xFFu;
  let projFlag = (ttff & 0x100u) != 0u;
  if (count == 0u) {
    return vec4<f32>(base.x, base.y, 1.0, 1.0);
  }
  let t = u.texMat[stage] * base;
  if (projFlag) {
    if (count == 3u) {
      return vec4<f32>(t.x, t.y, t.z, 1.0);
    }
    return vec4<f32>(t.x, t.y, t.w, 1.0);
  }
  return vec4<f32>(t.x, t.y, 1.0, 1.0);
}

@vertex
fn vs_main(in: VSIn) -> VSOut {
  var o: VSOut;
)";

  // Decode diffuse & specular vertex colors (D3DCOLOR = BGRA byte order in Unorm8x4).
  if (layout.diffuse.present) {
    if (layout.diffuse.format == WGPU_VTX_D3DCOLOR) {
      ss << "  let vDiff = in.diffuse.bgra;\n";
    } else if (layout.diffuse.format == WGPU_VTX_FLOAT4) {
      ss << "  let vDiff = in.diffuse;\n";
    } else {
      ss << "  let vDiff = vec4<f32>(1.0, 1.0, 1.0, 1.0);\n";
    }
  } else {
    ss << "  let vDiff = vec4<f32>(1.0, 1.0, 1.0, 1.0);\n";
  }

  if (layout.specular.present) {
    if (layout.specular.format == WGPU_VTX_D3DCOLOR) {
      ss << "  let vSpec = in.specular.bgra;\n";
    } else if (layout.specular.format == WGPU_VTX_FLOAT4) {
      ss << "  let vSpec = in.specular;\n";
    } else {
      ss << "  let vSpec = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
    }
  } else {
    ss << "  let vSpec = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
  }

  // Raw UVs array
  ss << "  var rawUVs = array<vec2<f32>, 4>(vec2<f32>(0.0), vec2<f32>(0.0), vec2<f32>(0.0), vec2<f32>(0.0));\n";
  for (int i = 0; i < 4; ++i) {
    if (layout.tex[i].present) {
      if (layout.tex[i].format == WGPU_VTX_FLOAT2) {
        ss << "  rawUVs[" << i << "] = in.tex" << i << ";\n";
      } else if (layout.tex[i].format == WGPU_VTX_FLOAT3 || layout.tex[i].format == WGPU_VTX_FLOAT4) {
        ss << "  rawUVs[" << i << "] = in.tex" << i << ".xy;\n";
      } else if (layout.tex[i].format == WGPU_VTX_SHORT2 || layout.tex[i].format == WGPU_VTX_SHORT4) {
        ss << "  rawUVs[" << i << "] = vec2<f32>(f32(in.tex" << i << ".x), f32(in.tex" << i << ".y)) * (1.0 / 2048.0);\n";
      }
    }
  }

  if (layout.isRHW) {
    // Pre-transformed screen-space vertex (D3DDECLUSAGE_POSITIONT / D3DFVF_XYZRHW).
    ss << R"(
  let sx = in.pos.x;
  let sy = in.pos.y;
  let sz = clamp(in.pos.z, 0.0, 1.0);
  let rhw = in.pos.w;
  let invRhw = select(1.0, 1.0 / rhw, abs(rhw) > 1e-6);
  let vpW = max(u.viewport.z, 1.0);
  let vpH = max(u.viewport.w, 1.0);
  let ndcX = ((sx - u.viewport.x) / vpW) * 2.0 - 1.0;
  let ndcY = 1.0 - ((sy - u.viewport.y) / vpH) * 2.0;
  o.clipPos = vec4<f32>(ndcX * invRhw, ndcY * invRhw, sz * invRhw, invRhw);
  o.color = vDiff;
  o.specColor = vSpec;
  let camPos = vec3<f32>(sx, sy, sz);
  let camNrm = vec3<f32>(0.0, 0.0, 1.0);
  o.fogFactor = 1.0;
)";
  } else {
    // 3D position decode
    if (!layout.pos.present) {
      ss << "  let pos4 = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
    } else if (layout.pos.format == WGPU_VTX_FLOAT3) {
      ss << "  let pos4 = vec4<f32>(in.pos, 1.0);\n";
    } else if (layout.pos.format == WGPU_VTX_FLOAT4) {
      ss << "  let pos4 = vec4<f32>(in.pos.xyz, 1.0);\n";
    } else if (layout.pos.format == WGPU_VTX_FLOAT2) {
      ss << "  let pos4 = vec4<f32>(in.pos.x, in.pos.y, 0.0, 1.0);\n";
    } else if (layout.pos.format == WGPU_VTX_SHORT4) {
      ss << "  let pos4 = vec4<f32>(f32(in.pos.x), f32(in.pos.y), f32(in.pos.z), 1.0);\n";
    } else {
      ss << "  let pos4 = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
    }

    // Normal decode
    if (layout.normal.present && layout.normal.format == WGPU_VTX_FLOAT3) {
      ss << "  let localNrm = in.normal;\n  let hasNrm = true;\n";
    } else if (layout.normal.present && layout.normal.format == WGPU_VTX_D3DCOLOR) {
      ss << "  let localNrm = in.normal.bgra.xyz * 2.0 - vec3<f32>(1.0);\n  let hasNrm = true;\n";
    } else {
      ss << "  let localNrm = vec3<f32>(0.0, 0.0, 0.0);\n  let hasNrm = false;\n";
    }

    ss << R"(
  let worldPos = u.world * pos4;
  let camPos4 = u.view * worldPos;
  let camPos = camPos4.xyz;
  o.clipPos = u.proj * camPos4;

  var worldNrm = (u.world * vec4<f32>(localNrm, 0.0)).xyz;
  let nLen = length(worldNrm);
  if (nLen > 1e-5) {
    worldNrm = worldNrm / nLen;
  }
  var camNrm = (u.view * vec4<f32>(worldNrm, 0.0)).xyz;
  let cnLen = length(camNrm);
  if (cnLen > 1e-5) {
    camNrm = camNrm / cnLen;
  }

  // Fixed-Function Lighting
  if (u.alphaTest.w > 0.5) {
    let cv = u.matSources.x > 0.5;
    let dSrc = i32(u.matSources.y);
    let aSrc = i32(u.matSources.z);
    let eSrc = i32(u.matSources.w);
    let matD = select(u.matDiffuse, vDiff, cv && dSrc == 1);
    let matA = select(u.matAmbient, vDiff, cv && aSrc == 1);
    let matE = select(u.matEmissive, vDiff, cv && eSrc == 1);

    var litRgb = matE.rgb + matA.rgb * u.ambientColor.rgb;
    if (hasNrm) {
      for (var li: u32 = 0u; li < 4u; li = li + 1u) {
        let lPos = u.lightPos[li];
        let lType = i32(lPos.w + 0.5);
        if (lType == 3) {
          // D3DLIGHT_DIRECTIONAL: lightPos.xyz stores normalized -Direction
          let ndotl = max(dot(worldNrm, lPos.xyz), 0.0);
          litRgb = litRgb + matD.rgb * u.lightDiff[li].rgb * ndotl;
        } else if (lType == 1 || lType == 2) {
          // D3DLIGHT_POINT / SPOT
          let toL = lPos.xyz - worldPos.xyz;
          let dist = length(toL);
          let range = u.lightDiff[li].a;
          if (dist < range && dist > 1e-5) {
            let att = u.lightAtten[li];
            let denom = max(att.x + att.y * dist + att.z * dist * dist, 1e-5);
            let atten = min(1.0 / denom, 1.0);
            let ndotl = max(dot(worldNrm, toL / dist), 0.0);
            litRgb = litRgb + matD.rgb * u.lightDiff[li].rgb * (ndotl * atten);
          }
        }
      }
    }
    o.color = vec4<f32>(clamp(litRgb, vec3<f32>(0.0), vec3<f32>(1.0)), clamp(matD.a, 0.0, 1.0));
  } else {
    o.color = vDiff;
  }
  o.specColor = vSpec;

  // Vertex linear fog
  if (u.fogParams.x > 0.5) {
    let fogDist = abs(camPos.z);
    let fStart = u.fogParams.y;
    let fEnd = u.fogParams.z;
    if (fEnd > fStart + 1e-4) {
      o.fogFactor = clamp((fEnd - fogDist) / (fEnd - fStart), 0.0, 1.0);
    } else {
      o.fogFactor = 1.0;
    }
  } else {
    o.fogFactor = 1.0;
  }
)";
  }

  ss << R"(
  o.uv0 = computeStageUV(0u, rawUVs, camPos, camNrm);
  o.uv1 = computeStageUV(1u, rawUVs, camPos, camNrm);
  o.uv2 = computeStageUV(2u, rawUVs, camPos, camNrm);
  o.uv3 = computeStageUV(3u, rawUVs, camPos, camNrm);
  return o;
}
)";
}

static void emitFfpPixelShader(std::ostringstream& ss, uint8_t texTypes) {
  ss << R"(
fn evalArg(arg: u32, cur: vec4<f32>, diff: vec4<f32>, spec: vec4<f32>,
           tex: vec4<f32>, tfac: vec4<f32>, stgConst: vec4<f32>, tmp: vec4<f32>) -> vec4<f32> {
  let sel = arg & 0x0Fu;
  var v: vec4<f32>;
  switch (sel) {
    case 0u: { v = diff; }      // D3DTA_DIFFUSE
    case 1u: { v = cur; }       // D3DTA_CURRENT
    case 2u: { v = tex; }       // D3DTA_TEXTURE
    case 3u: { v = tfac; }      // D3DTA_TFACTOR
    case 4u: { v = spec; }      // D3DTA_SPECULAR
    case 5u: { v = tmp; }       // D3DTA_TEMP
    case 6u: { v = stgConst; }  // D3DTA_CONSTANT
    default: { v = cur; }
  }
  if ((arg & 0x20u) != 0u) {
    // D3DTA_ALPHAREPLICATE
    v = vec4<f32>(v.a, v.a, v.a, v.a);
  }
  if ((arg & 0x10u) != 0u) {
    // D3DTA_COMPLEMENT
    v = vec4<f32>(1.0) - v;
  }
  return v;
}

fn evalColorOp(op: u32, a1: vec4<f32>, a2: vec4<f32>, a0: vec4<f32>,
               cur: vec4<f32>, diff: vec4<f32>, tex: vec4<f32>, tfac: vec4<f32>) -> vec3<f32> {
  switch (op) {
    case 2u:  { return a1.rgb; }                                                        // SELECTARG1
    case 3u:  { return a2.rgb; }                                                        // SELECTARG2
    case 4u:  { return a1.rgb * a2.rgb; }                                               // MODULATE
    case 5u:  { return clamp(a1.rgb * a2.rgb * 2.0, vec3<f32>(0.0), vec3<f32>(1.0)); }  // MODULATE2X
    case 7u:  { return clamp(a1.rgb + a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }        // ADD
    case 8u:  { return clamp(a1.rgb + a2.rgb - vec3<f32>(0.5), vec3<f32>(0.0), vec3<f32>(1.0)); } // ADDSIGNED
    case 11u: { return clamp(a1.rgb + a2.rgb - a1.rgb * a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); } // ADDSMOOTH
    case 12u: { return mix(a2.rgb, a1.rgb, diff.a); }                                   // BLENDDIFFUSEALPHA
    case 13u: { return mix(a2.rgb, a1.rgb, tex.a); }                                    // BLENDTEXTUREALPHA
    case 14u: { return mix(a2.rgb, a1.rgb, tfac.a); }                                   // BLENDFACTORALPHA
    case 16u: { return mix(a2.rgb, a1.rgb, cur.a); }                                    // BLENDCURRENTALPHA
    case 19u: { return clamp(a1.rgb * a2.rgb + vec3<f32>(a1.a), vec3<f32>(0.0), vec3<f32>(1.0)); } // MODULATECOLOR_ADDALPHA
    case 25u: { return clamp(a0.rgb + a1.rgb * a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); } // MULTIPLYADD
    default:  { return cur.rgb; }
  }
}

fn evalAlphaOp(op: u32, a1: f32, a2: f32, a0: f32,
               curA: f32, diffA: f32, texA: f32, tfacA: f32) -> f32 {
  switch (op) {
    case 2u:  { return a1; }
    case 3u:  { return a2; }
    case 4u:  { return a1 * a2; }
    case 5u:  { return clamp(a1 * a2 * 2.0, 0.0, 1.0); }
    case 7u:  { return clamp(a1 + a2, 0.0, 1.0); }
    case 8u:  { return clamp(a1 + a2 - 0.5, 0.0, 1.0); }
    case 11u: { return clamp(a1 + a2 - a1 * a2, 0.0, 1.0); }
    case 12u: { return mix(a2, a1, diffA); }
    case 13u: { return mix(a2, a1, texA); }
    case 14u: { return mix(a2, a1, tfacA); }
    case 16u: { return mix(a2, a1, curA); }
    case 25u: { return clamp(a0 + a1 * a2, 0.0, 1.0); }
    default:  { return curA; }
  }
}

fn passAlphaTest(alpha: f32, func: u32, refVal: f32) -> bool {
  let eps = 0.5 / 255.0;
  switch (func) {
    case 1u: { return false; }                    // NEVER
    case 2u: { return alpha < refVal - eps; }     // LESS
    case 3u: { return abs(alpha - refVal) <= eps; } // EQUAL
    case 4u: { return alpha <= refVal + eps; }    // LESSEQUAL
    case 5u: { return alpha > refVal + eps; }     // GREATER
    case 6u: { return abs(alpha - refVal) > eps; }// NOTEQUAL
    case 7u: { return alpha >= refVal - eps; }    // GREATEREQUAL
    default: { return true; }                     // ALWAYS
  }
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  // Sample all 4 stages in uniform control flow (unbound stages bind 1x1 white).
  let p0 = select(in.uv0.xy, in.uv0.xy / in.uv0.z, abs(in.uv0.z - 1.0) > 1e-6 && abs(in.uv0.z) > 1e-6);
  let p1 = select(in.uv1.xy, in.uv1.xy / in.uv1.z, abs(in.uv1.z - 1.0) > 1e-6 && abs(in.uv1.z) > 1e-6);
  let p2 = select(in.uv2.xy, in.uv2.xy / in.uv2.z, abs(in.uv2.z - 1.0) > 1e-6 && abs(in.uv2.z) > 1e-6);
  let p3 = select(in.uv3.xy, in.uv3.xy / in.uv3.z, abs(in.uv3.z - 1.0) > 1e-6 && abs(in.uv3.z) > 1e-6);
)";
  auto sampleExpr = [&](int s) -> std::string {
    uint8_t st = (texTypes >> (s * 2)) & 3u;
    char buf[96];
    if (st == 1 || st == 2) {
      snprintf(buf, sizeof(buf), "textureSample(tTex%d, sSam%d, in.uv%d.xyz)", s, s, s);
    } else {
      snprintf(buf, sizeof(buf), "textureSample(tTex%d, sSam%d, p%d)", s, s, s);
    }
    return buf;
  };
  ss << "  var texSamples = array<vec4<f32>, 4>(\n"
     << "    " << sampleExpr(0) << ",\n"
     << "    " << sampleExpr(1) << ",\n"
     << "    " << sampleExpr(2) << ",\n"
     << "    " << sampleExpr(3) << "\n"
     << "  );\n";
  ss << R"(
  var cur = in.color;
  var tmp = vec4<f32>(0.0, 0.0, 0.0, 1.0);

  for (var s: u32 = 0u; s < 4u; s = s + 1u) {
    let cCfg = u.stageColor[s];
    let aCfg = u.stageAlpha[s];
    let cOp = cCfg.x;
    if (cOp <= 1u) {
      break; // D3DTOP_DISABLE terminates the cascade
    }
    if (cOp == 22u) {
      // D3DTOP_BUMPENVMAP: pass current through cleanly in FFP bring-up
      continue;
    }
    let tex = texSamples[s];
    let stgConst = u.stageConst[s];
    let cA1 = evalArg(cCfg.y, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp);
    let cA2 = evalArg(cCfg.z, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp);
    let cA0 = evalArg(cCfg.w, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp);
    let newRgb = evalColorOp(cOp, cA1, cA2, cA0, cur, in.color, tex, u.tfactor);

    var newA = cur.a;
    let aOp = aCfg.x;
    if (aOp > 1u) {
      let aA1 = evalArg(aCfg.y, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp).a;
      let aA2 = evalArg(aCfg.z, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp).a;
      let aA0 = evalArg(aCfg.w, cur, in.color, in.specColor, tex, u.tfactor, stgConst, tmp).a;
      newA = evalAlphaOp(aOp, aA1, aA2, aA0, cur.a, in.color.a, tex.a, u.tfactor.a);
    }

    let res = vec4<f32>(newRgb, newA);
    if (u.stageTex[s].w == 5u) {
      tmp = res;
    } else {
      cur = res;
    }
  }

  // Alpha test
  if (u.alphaTest.x > 0.5) {
    if (!passAlphaTest(cur.a, u32(u.alphaTest.y + 0.5), u.alphaTest.z)) {
      discard;
    }
  }

  // Fog
  if (u.fogParams.x > 0.5) {
    cur = vec4<f32>(mix(u.fogColor.rgb, cur.rgb, in.fogFactor), cur.a);
  }
)";
  // Debug-only early return (see WgpuDebugPsSnippet); empty in every normal run.
  if (g_wgpuDebugPsMode == 4) {
    ss << WgpuDebugIdSnippet();
  } else {
    ss << WgpuDebugPsSnippet(false);
  }
  ss << R"(
  return cur;
}
)";
}

std::string GenerateFfpWGSL(const FfpVertexLayout& layout) {
  std::ostringstream ss;
  emitCommonHeader(ss, 0);
  emitFfpVertexShader(ss, layout);
  emitFfpPixelShader(ss, 0);
  return ss.str();
}

std::string GenerateCombinedWGSL(const WgpuVertexShader* vs, uint64_t vsInputTypeKey,
                                 const FfpVertexLayout& ffpLayout,
                                 const WgpuPixelShader* ps, uint8_t texTypes) {
  std::ostringstream ss;
  emitCommonHeader(ss, texTypes);
  if (vs && !vs->bytecode.empty()) {
    ss << GenerateSm1VertexShaderWGSL(vs->bytecode.data(), vs->bytecode.size(), vsInputTypeKey);
  } else {
    emitFfpVertexShader(ss, ffpLayout);
  }
  if (ps && !ps->bytecode.empty()) {
    ss << GenerateSm1PixelShaderWGSL(ps->bytecode.data(), ps->bytecode.size(), texTypes);
  } else {
    emitFfpPixelShader(ss, texTypes);
  }
  return ss.str();
}

const char* GetBlitWGSL() {
  return R"(
@group(0) @binding(0) var sBlit: sampler;
@group(0) @binding(1) var tBlit: texture_2d<f32>;
@group(0) @binding(2) var<uniform> uBlitUv: vec4<f32>;

struct BlitOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_blit(@builtin(vertex_index) vid: u32) -> BlitOut {
  var p = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 3.0, -1.0),
    vec2<f32>(-1.0,  3.0)
  );
  var uv = array<vec2<f32>, 3>(
    vec2<f32>(0.0,  1.0),
    vec2<f32>(2.0,  1.0),
    vec2<f32>(0.0, -1.0)
  );
  var o: BlitOut;
  o.pos = vec4<f32>(p[vid], 0.0, 1.0);
  o.uv = uv[vid];
  return o;
}

@fragment
fn fs_blit(in: BlitOut) -> @location(0) vec4<f32> {
  let uv = uBlitUv.xy + in.uv * uBlitUv.zw;
  let c = textureSample(tBlit, sBlit, uv);
  return vec4<f32>(c.rgb, 1.0);
}

@fragment
fn fs_blit_copy(in: BlitOut) -> @location(0) vec4<f32> {
  let uv = uBlitUv.xy + in.uv * uBlitUv.zw;
  return textureSample(tBlit, sBlit, uv);
}
)";
}
