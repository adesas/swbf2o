// DirectX 9 Shader Model 1.1 (vs_1_1 / ps_1_1) + ps_2_0 Gamma LUT -> WebGPU WGSL Translator.
//
// Translates all 2,332 vs_1_1 vertex shaders and 226 ps_1_1 pixel shaders in SWBF2's core.lvl
// (plus pcGammaPass's inline kGammaPassPS) deterministically into WGSL at runtime, sharing the
// exact same FfpDrawUniforms (@group(0) @binding(0)), 4-stage texture/sampler bindings (@group(1)),
// and VSOut varying interface as the Phase 2 Fixed-Function Pipeline.

#include "d3d9_wgpu_internal.h"

#include <math.h>
#include <sstream>
#include <stdio.h>
#include <string.h>

static uint32_t g_totalVsTranslated = 0;
static uint32_t g_totalPsTranslated = 0;

uint32_t GetTranslatedVsCount() { return g_totalVsTranslated; }
uint32_t GetTranslatedPsCount() { return g_totalPsTranslated; }

void ParseSm1VertexShaderDcls(const uint32_t* words, size_t wordCount,
                              std::vector<Sm1VsDclInput, SysAllocator<Sm1VsDclInput>>& outDcls) {
  outDcls.clear();
  if (!words || wordCount < 2) return;
  uint32_t ver = words[0];
  if ((ver & 0xFFFF0000u) != 0xFFFE0000u) return;

  size_t k = 1;
  while (k < wordCount) {
    uint32_t tok = words[k];
    if (tok == 0x0000FFFFu) break;
    uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) {
      uint32_t clen = (tok >> 16) & 0x7FFFu;
      k += 1 + clen;
      continue;
    }
    ++k;
    if (op == 81u) { // D3DSIO_DEF
      k += 5;
      continue;
    }
    if (op == 31u) { // D3DSIO_DCL
      if (k + 1 < wordCount) {
        uint32_t usageTok = words[k];
        uint32_t regTok = words[k + 1];
        uint8_t usage = (uint8_t)(usageTok & 0x1Fu);
        uint8_t usageIdx = (uint8_t)((usageTok >> 16) & 0xFu);
        uint8_t rtype = (uint8_t)(((regTok >> 28) & 0x7u) | (((regTok >> 11) & 0x3u) << 3));
        uint8_t rnum = (uint8_t)(regTok & 0x7FFu);
        if (rtype == 1 && rnum < 16) {
          outDcls.push_back(Sm1VsDclInput{rnum, usage, usageIdx});
        }
      }
      k += 2;
      continue;
    }
    while (k < wordCount && (words[k] & 0x80000000u)) {
      ++k;
    }
  }
}

static const char* wgslRawInputType(uint8_t fmt) {
  switch (fmt) {
    case WGPU_VTX_FLOAT1:   return "f32";
    case WGPU_VTX_FLOAT2:   return "vec2<f32>";
    case WGPU_VTX_FLOAT3:   return "vec3<f32>";
    case WGPU_VTX_FLOAT4:   return "vec4<f32>";
    case WGPU_VTX_D3DCOLOR: return "vec4<f32>";
    case WGPU_VTX_SHORT2:   return "vec2<i32>";
    case WGPU_VTX_SHORT4:   return "vec4<i32>";
    case WGPU_VTX_UBYTE4:   return "vec4<u32>";
    default:                return "vec4<f32>";
  }
}

static std::string wgslUnpackInput(uint8_t fmt, int reg) {
  char buf[128];
  switch (fmt) {
    case WGPU_VTX_FLOAT1:
      snprintf(buf, sizeof(buf), "vec4<f32>(in.v%d_raw, 0.0, 0.0, 1.0)", reg);
      return buf;
    case WGPU_VTX_FLOAT2:
      snprintf(buf, sizeof(buf), "vec4<f32>(in.v%d_raw.x, in.v%d_raw.y, 0.0, 1.0)", reg, reg);
      return buf;
    case WGPU_VTX_FLOAT3:
      snprintf(buf, sizeof(buf), "vec4<f32>(in.v%d_raw.x, in.v%d_raw.y, in.v%d_raw.z, 1.0)", reg, reg, reg);
      return buf;
    case WGPU_VTX_FLOAT4:
      snprintf(buf, sizeof(buf), "in.v%d_raw", reg);
      return buf;
    case WGPU_VTX_D3DCOLOR:
      // D3DCOLOR is stored in memory as B, G, R, A -> Unorm8x4 reads .r=B, .g=G, .b=R, .a=A
      snprintf(buf, sizeof(buf), "in.v%d_raw.bgra", reg);
      return buf;
    case WGPU_VTX_SHORT2:
      snprintf(buf, sizeof(buf), "vec4<f32>(f32(in.v%d_raw.x), f32(in.v%d_raw.y), 0.0, 1.0)", reg, reg);
      return buf;
    case WGPU_VTX_SHORT4:
      snprintf(buf, sizeof(buf), "vec4<f32>(in.v%d_raw)", reg);
      return buf;
    case WGPU_VTX_UBYTE4:
      snprintf(buf, sizeof(buf), "vec4<f32>(in.v%d_raw)", reg);
      return buf;
    default:
      return "vec4<f32>(0.0, 0.0, 0.0, 1.0)";
  }
}

static std::string fmtFloatLiteral(float f) {
  if (!isfinite(f)) return "0.0";
  char buf[64];
  snprintf(buf, sizeof(buf), "%.8ef", f);
  // WGSL float literals do not take a trailing 'f' suffix.
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == 'f') buf[len - 1] = '\0';
  return buf;
}

static std::string vsRegBase(uint32_t pt, bool isSrc) {
  uint32_t rtype = ((pt >> 28) & 0x7u) | (((pt >> 11) & 0x3u) << 3);
  uint32_t rnum = pt & 0x7FFu;
  char buf[96];
  switch (rtype) {
    case 0: // D3DSPR_TEMP
      snprintf(buf, sizeof(buf), "r%u", rnum < 16 ? rnum : 0u);
      return buf;
    case 1: // D3DSPR_INPUT
      snprintf(buf, sizeof(buf), "v%u", rnum < 16 ? rnum : 0u);
      return buf;
    case 2: // D3DSPR_CONST
      if (isSrc && (pt & 0x00002000u)) {
        snprintf(buf, sizeof(buf), "u.vsConst[clamp(%d + a0_x, 0, 95)]", (int)rnum);
      } else {
        snprintf(buf, sizeof(buf), "u.vsConst[%u]", rnum < 96 ? rnum : 0u);
      }
      return buf;
    case 3: // D3DSPR_ADDR
      return "a0_x";
    case 4: // D3DSPR_RASTOUT
      if (rnum == 0) return "oPos";
      if (rnum == 1) return "oFog";
      return "oPts";
    case 5: // D3DSPR_ATTROUT
      snprintf(buf, sizeof(buf), "oD%u", rnum < 2 ? rnum : 0u);
      return buf;
    case 6: // D3DSPR_TEXCRDOUT
      snprintf(buf, sizeof(buf), "oT%u", rnum < 4 ? rnum : 0u);
      return buf;
    default:
      return "r0";
  }
}

static std::string psRegBase(uint32_t pt, bool isPs20) {
  uint32_t rtype = ((pt >> 28) & 0x7u) | (((pt >> 11) & 0x3u) << 3);
  uint32_t rnum = pt & 0x7FFu;
  char buf[64];
  switch (rtype) {
    case 0: // D3DSPR_TEMP
      snprintf(buf, sizeof(buf), "r%u", rnum < 8 ? rnum : 0u);
      return buf;
    case 1: // D3DSPR_INPUT (v0 = diffuse, v1 = specular)
      snprintf(buf, sizeof(buf), "v%u", rnum < 2 ? rnum : 0u);
      return buf;
    case 2: // D3DSPR_CONST
      snprintf(buf, sizeof(buf), "c%u", rnum < 8 ? rnum : 0u);
      return buf;
    case 3: // D3DSPR_TEXTURE (t0..t3)
      snprintf(buf, sizeof(buf), "t%u", rnum < 4 ? rnum : 0u);
      return buf;
    case 8: // D3DSPR_COLOROUT (ps_2_0 oC0)
      return isPs20 ? "r0" : "r0";
    default:
      return "r0";
  }
}

static std::string applySwizzleAndSrcMod(const std::string& base, uint32_t pt) {
  uint32_t swz = (pt >> 16) & 0xFFu;
  static const char kComp[4] = {'x', 'y', 'z', 'w'};
  char sstr[5] = {
    kComp[(swz >> 0) & 3],
    kComp[(swz >> 2) & 3],
    kComp[(swz >> 4) & 3],
    kComp[(swz >> 6) & 3],
    '\0'
  };
  std::string expr = base;
  if (strcmp(sstr, "xyzw") != 0) {
    expr += ".";
    expr += sstr;
  }

  uint32_t smod = (pt >> 24) & 0xFu;
  switch (smod) {
    case 1: // D3DSPSM_NEG
      expr = "(-" + expr + ")";
      break;
    case 2: // D3DSPSM_BIAS
      expr = "(" + expr + " - vec4<f32>(0.5))";
      break;
    case 3: // D3DSPSM_BIASNEG
      expr = "(-(" + expr + " - vec4<f32>(0.5)))";
      break;
    case 4: // D3DSPSM_SIGN (_bx2)
      expr = "((" + expr + " - vec4<f32>(0.5)) * 2.0)";
      break;
    case 5: // D3DSPSM_SIGNNEG (-_bx2)
      expr = "(-((" + expr + " - vec4<f32>(0.5)) * 2.0))";
      break;
    case 6: // D3DSPSM_COMP (1 - x)
      expr = "(vec4<f32>(1.0) - " + expr + ")";
      break;
    case 11: // D3DSPSM_ABS
      expr = "abs(" + expr + ")";
      break;
    case 12: // D3DSPSM_ABSNEG
      expr = "(-abs(" + expr + "))";
      break;
    default:
      break;
  }
  return expr;
}

static void emitMaskedAssign(std::ostringstream& ss, const std::string& dstBase, uint32_t dstTok,
                             const std::string& rhsExpr, int stepIdx) {
  uint32_t rtype = ((dstTok >> 28) & 0x7u) | (((dstTok >> 11) & 0x3u) << 3);
  if (rtype == 3) {
    // D3DSPR_ADDR (a0.x in vs_1_1): D3D9 rounds toward -infinity via floor().
    ss << "  a0_x = i32(floor((" << rhsExpr << ").x));\n";
    return;
  }

  std::string valExpr = rhsExpr;
  uint32_t dshift = (dstTok >> 24) & 0xFu;
  if (dshift == 1) {
    valExpr = "((" + valExpr + ") * 2.0)";
  } else if (dshift == 2) {
    valExpr = "((" + valExpr + ") * 4.0)";
  } else if (dshift == 3) {
    valExpr = "((" + valExpr + ") * 8.0)";
  } else if (dshift == 15) { // -1 (_d2)
    valExpr = "((" + valExpr + ") * 0.5)";
  } else if (dshift == 14) { // -2 (_d4)
    valExpr = "((" + valExpr + ") * 0.25)";
  }

  uint32_t dmod = (dstTok >> 20) & 0xFu;
  if (dmod & 1) { // D3DSPDM_SATURATE
    valExpr = "clamp(" + valExpr + ", vec4<f32>(0.0), vec4<f32>(1.0))";
  }

  uint32_t mask = (dstTok >> 16) & 0xFu;
  if (mask == 0) mask = 0xFu;
  ss << "  let v_step_" << stepIdx << ": vec4<f32> = " << valExpr << ";\n";
  if (mask == 0xFu) {
    ss << "  " << dstBase << " = v_step_" << stepIdx << ";\n";
  } else {
    static const char kComp[4] = {'x', 'y', 'z', 'w'};
    for (int c = 0; c < 4; ++c) {
      if (mask & (1u << c)) {
        ss << "  " << dstBase << "." << kComp[c] << " = v_step_" << stepIdx << "." << kComp[c] << ";\n";
      }
    }
  }
}

std::string GenerateSm1VertexShaderWGSL(const uint32_t* words, size_t wordCount, uint64_t vsInputTypeKey) {
  ++g_totalVsTranslated;
  std::ostringstream ss;

  // Emit VSIn struct matching @location(reg) for all active dcl registers in vsInputTypeKey.
  ss << "struct VSIn {\n";
  int attrCount = 0;
  for (int reg = 0; reg < 16; ++reg) {
    uint8_t fmt = (uint8_t)((vsInputTypeKey >> (reg * 4)) & 0xFULL);
    if (fmt != WGPU_VTX_NONE) {
      ss << "  @location(" << reg << ") v" << reg << "_raw: " << wgslRawInputType(fmt) << ",\n";
      ++attrCount;
    }
  }
  if (attrCount == 0) {
    ss << "  @builtin(vertex_index) vid: u32,\n";
  }
  ss << "};\n\n";

  // Helper for D3DSIO_LIT
  ss << R"(fn d3d_lit(s: vec4<f32>) -> vec4<f32> {
  let ndotl = max(s.x, 0.0);
  let ndoth = max(s.y, 0.0);
  let p = clamp(s.w, -128.0, 128.0);
  let spec = select(0.0, pow(ndoth, p), s.x > 0.0 && s.y > 0.0);
  return vec4<f32>(1.0, ndotl, spec, 1.0);
}

@vertex
fn vs_main(in: VSIn) -> VSOut {
)";

  // Unpack all 16 input registers v0..v15.
  for (int reg = 0; reg < 16; ++reg) {
    uint8_t fmt = (uint8_t)((vsInputTypeKey >> (reg * 4)) & 0xFULL);
    ss << "  let v" << reg << ": vec4<f32> = " << wgslUnpackInput(fmt, reg) << ";\n";
  }

  // Declare temp registers r0..r11, address register a0_x, and output registers.
  for (int r = 0; r < 12; ++r) {
    ss << "  var r" << r << ": vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 0.0);\n";
  }
  ss << "  var a0_x: i32 = 0;\n";
  ss << "  var oPos: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
  ss << "  var oFog: vec4<f32> = vec4<f32>(1.0, 0.0, 0.0, 0.0);\n";
  ss << "  var oPts: vec4<f32> = vec4<f32>(1.0, 0.0, 0.0, 0.0);\n";
  ss << "  var oD0: vec4<f32> = vec4<f32>(1.0, 1.0, 1.0, 1.0);\n";
  ss << "  var oD1: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 0.0);\n";
  ss << "  var oT0: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
  ss << "  var oT1: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
  ss << "  var oT2: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
  ss << "  var oT3: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";

  size_t k = 1;
  int stepIdx = 0;
  while (k < wordCount) {
    uint32_t tok = words[k];
    if (tok == 0x0000FFFFu) break;
    uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) {
      k += 1 + ((tok >> 16) & 0x7FFFu);
      continue;
    }
    ++k;
    if (op == 31u) { // dcl
      k += 2;
      continue;
    }
    if (op == 81u) { // def
      k += 5;
      continue;
    }
    uint32_t params[8]{};
    int np = 0;
    while (k < wordCount && (words[k] & 0x80000000u)) {
      if (np < 8) params[np] = words[k];
      ++np;
      ++k;
    }
    if (np < 1) continue;

    std::string dst = vsRegBase(params[0], false);
    auto S = [&](int idx) -> std::string {
      if (idx >= np) return "vec4<f32>(0.0)";
      return applySwizzleAndSrcMod(vsRegBase(params[idx], true), params[idx]);
    };

    std::string rhs;
    switch (op) {
      case 0: // nop
        continue;
      case 1:  // mov
      case 46: // mova
        rhs = S(1);
        break;
      case 2: // add
        rhs = "(" + S(1) + " + " + S(2) + ")";
        break;
      case 3: // sub
        rhs = "(" + S(1) + " - " + S(2) + ")";
        break;
      case 4: // mad
        rhs = "(" + S(1) + " * " + S(2) + " + " + S(3) + ")";
        break;
      case 5: // mul
        rhs = "(" + S(1) + " * " + S(2) + ")";
        break;
      case 6: // rcp
        rhs = "vec4<f32>(select(1.0 / (" + S(1) + ").x, 0.0, abs((" + S(1) + ").x) < 1e-12))";
        break;
      case 7: // rsq
        rhs = "vec4<f32>(inverseSqrt(max(abs((" + S(1) + ").x), 1e-12)))";
        break;
      case 8: // dp3
        rhs = "vec4<f32>(dot((" + S(1) + ").xyz, (" + S(2) + ").xyz))";
        break;
      case 9: // dp4
        rhs = "vec4<f32>(dot(" + S(1) + ", " + S(2) + "))";
        break;
      case 10: // min
        rhs = "min(" + S(1) + ", " + S(2) + ")";
        break;
      case 11: // max
        rhs = "max(" + S(1) + ", " + S(2) + ")";
        break;
      case 12: // slt
        rhs = "select(vec4<f32>(0.0), vec4<f32>(1.0), (" + S(1) + ") < (" + S(2) + "))";
        break;
      case 13: // sge
        rhs = "select(vec4<f32>(0.0), vec4<f32>(1.0), (" + S(1) + ") >= (" + S(2) + "))";
        break;
      case 14: // exp
      case 78: // expp
        rhs = "vec4<f32>(exp2((" + S(1) + ").x))";
        break;
      case 15: // log
      case 79: // logp
        rhs = "vec4<f32>(log2(max(abs((" + S(1) + ").x), 1e-12)))";
        break;
      case 16: // lit
        rhs = "d3d_lit(" + S(1) + ")";
        break;
      case 17: // dst
        rhs = "vec4<f32>(1.0, (" + S(1) + ").y * (" + S(2) + ").y, (" + S(1) + ").z, (" + S(2) + ").w)";
        break;
      case 18: // lrp
        rhs = "mix(" + S(3) + ", " + S(2) + ", " + S(1) + ")";
        break;
      case 19: // frc
        rhs = "fract(" + S(1) + ")";
        break;
      case 35: // abs
        rhs = "abs(" + S(1) + ")";
        break;
      default:
        continue;
    }
    emitMaskedAssign(ss, dst, params[0], rhs, ++stepIdx);
  }

  ss << R"(  var out: VSOut;
  out.clipPos = oPos;
  out.color = clamp(oD0, vec4<f32>(0.0), vec4<f32>(1.0));
  out.specColor = clamp(oD1, vec4<f32>(0.0), vec4<f32>(1.0));
  out.uv0 = oT0;
  out.uv1 = oT1;
  out.uv2 = oT2;
  out.uv3 = oT3;
  out.fogFactor = clamp(oFog.x, 0.0, 1.0);
  return out;
}
)";
  return ss.str();
}

static std::string emitStageSample(uint32_t stage, uint8_t texTypes,
                                   const std::string& coord2Expr,
                                   const std::string& coord3Expr) {
  uint8_t stType = (texTypes >> (stage * 2)) & 3u;
  char buf[256];
  if (stType == 1) { // Cube
    snprintf(buf, sizeof(buf), "textureSample(tTex%u, sSam%u, %s)", stage, stage, coord3Expr.c_str());
  } else if (stType == 2) { // 3D
    snprintf(buf, sizeof(buf), "textureSample(tTex%u, sSam%u, %s)", stage, stage, coord3Expr.c_str());
  } else { // 2D
    snprintf(buf, sizeof(buf), "textureSample(tTex%u, sSam%u, %s)", stage, stage, coord2Expr.c_str());
  }
  return buf;
}

std::string GenerateSm1PixelShaderWGSL(const uint32_t* words, size_t wordCount, uint8_t texTypes) {
  ++g_totalPsTranslated;
  std::ostringstream ss;

  bool isPs20 = (wordCount > 0 && words[0] == 0xFFFF0200u);

  // First pass: collect inline `def cN, f0, f1, f2, f3` constants.
  bool hasDef[8] = {};
  float defVal[8][4] = {};
  if (words && wordCount >= 2) {
    size_t k = 1;
    while (k < wordCount) {
      uint32_t tok = words[k];
      if (tok == 0x0000FFFFu) break;
      uint32_t op = tok & 0xFFFFu;
      if (op == 0xFFFEu) {
        k += 1 + ((tok >> 16) & 0x7FFFu);
        continue;
      }
      ++k;
      if (op == 81u) { // D3DSIO_DEF
        if (k + 4 < wordCount) {
          uint32_t rnum = words[k] & 0x7FFu;
          if (rnum < 8) {
            hasDef[rnum] = true;
            memcpy(defVal[rnum], &words[k + 1], 16);
          }
        }
        k += 5;
        continue;
      }
      if (op == 31u) { // D3DSIO_DCL (ps_2_0)
        k += 2;
        continue;
      }
      while (k < wordCount && (words[k] & 0x80000000u)) ++k;
    }
  }

  ss << R"(fn passAlphaTest(alpha: f32, func: u32, refVal: f32) -> bool {
  let eps = 0.5 / 255.0;
  switch (func) {
    case 1u: { return false; }
    case 2u: { return alpha < refVal - eps; }
    case 3u: { return abs(alpha - refVal) <= eps; }
    case 4u: { return alpha <= refVal + eps; }
    case 5u: { return alpha > refVal + eps; }
    case 6u: { return abs(alpha - refVal) > eps; }
    case 7u: { return alpha >= refVal - eps; }
    default: { return true; }
  }
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let v0: vec4<f32> = clamp(in.color, vec4<f32>(0.0), vec4<f32>(1.0));
  let v1: vec4<f32> = clamp(in.specColor, vec4<f32>(0.0), vec4<f32>(1.0));
  var r0: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  var r1: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  var r2: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  var r3: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  var t0: vec4<f32> = in.uv0;
  var t1: vec4<f32> = in.uv1;
  var t2: vec4<f32> = in.uv2;
  var t3: vec4<f32> = in.uv3;
  var texm_u: f32 = 0.0;
  var texm_v: f32 = 0.0;
  var texm_padCount: i32 = 0;
  var texm_w0: f32 = 0.0;
  var texm_w1: f32 = 0.0;
)";

  for (int c = 0; c < 8; ++c) {
    if (hasDef[c]) {
      ss << "  let c" << c << ": vec4<f32> = vec4<f32>("
         << fmtFloatLiteral(defVal[c][0]) << ", "
         << fmtFloatLiteral(defVal[c][1]) << ", "
         << fmtFloatLiteral(defVal[c][2]) << ", "
         << fmtFloatLiteral(defVal[c][3]) << ");\n";
    } else {
      ss << "  let c" << c << ": vec4<f32> = u.psConst[" << c << "];\n";
    }
  }

  // Pre-compute raw & perspective-divided texture coordinates per stage.
  for (int s = 0; s < 4; ++s) {
    ss << "  let raw_uv" << s << ": vec4<f32> = in.uv" << s << ";\n";
    ss << "  let proj_div" << s << ": f32 = select(1.0, raw_uv" << s << ".w, (u.stageTex[" << s
       << "].z & 256u) != 0u && abs(raw_uv" << s << ".w) > 1e-6);\n";
    ss << "  let tc2_" << s << ": vec2<f32> = raw_uv" << s << ".xy / proj_div" << s << ";\n";
    ss << "  let tc3_" << s << ": vec3<f32> = select(raw_uv" << s << ".xyz, raw_uv" << s
       << ".xyz / proj_div" << s << ", (u.stageTex[" << s << "].z & 256u) != 0u);\n";
  }

  size_t k = 1;
  int stepIdx = 0;
  while (k < wordCount) {
    uint32_t tok = words[k];
    if (tok == 0x0000FFFFu) break;
    uint32_t op = tok & 0xFFFFu;
    if (op == 0xFFFEu) {
      k += 1 + ((tok >> 16) & 0x7FFFu);
      continue;
    }
    ++k;
    if (op == 81u) { // def
      k += 5;
      continue;
    }
    if (op == 31u) { // dcl (ps_2_0)
      k += 2;
      continue;
    }
    uint32_t params[8]{};
    int np = 0;
    while (k < wordCount && (words[k] & 0x80000000u)) {
      if (np < 8) params[np] = words[k];
      ++np;
      ++k;
    }
    if (np < 1) continue;

    std::string dst = psRegBase(params[0], isPs20);
    uint32_t dstNum = params[0] & 0x7FFu;
    if (dstNum > 3) dstNum = 0;
    auto S = [&](int idx) -> std::string {
      if (idx >= np) return "vec4<f32>(0.0)";
      return applySwizzleAndSrcMod(psRegBase(params[idx], isPs20), params[idx]);
    };

    std::string rhs;
    switch (op) {
      case 0: // nop
        continue;
      case 64: // texcoord tN
        ss << "  t" << dstNum << " = clamp(vec4<f32>(raw_uv" << dstNum << ".xyz, raw_uv" << dstNum
           << ".w), vec4<f32>(0.0), vec4<f32>(1.0));\n";
        continue;
      case 66: // tex tN (ps_1_1) or texld rDst, tCoord, sStage (ps_2_0)
        if (isPs20 && np >= 3) {
          uint32_t sampNum = params[2] & 0x7FFu;
          if (sampNum > 3) sampNum = 0;
          std::string coordExpr = S(1);
          ss << "  " << dst << " = "
             << emitStageSample(sampNum, texTypes, "(" + coordExpr + ").xy", "(" + coordExpr + ").xyz")
             << ";\n";
        } else {
          ss << "  t" << dstNum << " = "
             << emitStageSample(dstNum, texTypes,
                                "tc2_" + std::to_string(dstNum),
                                "tc3_" + std::to_string(dstNum))
             << ";\n";
        }
        continue;
      case 67:   // texbem tDst, tSrc
      case 68: { // texbeml tDst, tSrc
        std::string srcReg = psRegBase(params[1], isPs20);
        ss << "  {\n"
           << "    let duv = (" << srcReg << ".xy - vec2<f32>(0.5)) * 2.0;\n"
           << "    let bm = u.bumpMat[" << dstNum << "];\n"
           << "    let uv_bem = tc2_" << dstNum << " + vec2<f32>(duv.x * bm.x + duv.y * bm.z, duv.x * bm.y + duv.y * bm.w);\n"
           << "    t" << dstNum << " = "
           << emitStageSample(dstNum, texTypes, "uv_bem", "vec3<f32>(uv_bem, 0.0)") << ";\n"
           << "  }\n";
        continue;
      }
      case 71: { // texm3x2pad tDst, tSrc
        ss << "  texm_u = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n";
        continue;
      }
      case 72: { // texm3x2tex tDst, tSrc
        ss << "  texm_v = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n"
           << "  t" << dstNum << " = "
           << emitStageSample(dstNum, texTypes, "vec2<f32>(texm_u, texm_v)", "vec3<f32>(texm_u, texm_v, 0.0)")
           << ";\n";
        continue;
      }
      case 73: { // texm3x3pad tDst, tSrc
        ss << "  if (texm_padCount == 0) {\n"
           << "    texm_u = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n"
           << "    texm_w0 = raw_uv" << dstNum << ".w;\n"
           << "    texm_padCount = 1;\n"
           << "  } else {\n"
           << "    texm_v = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n"
           << "    texm_w1 = raw_uv" << dstNum << ".w;\n"
           << "    texm_padCount = 0;\n"
           << "  }\n";
        continue;
      }
      case 74: { // texm3x3tex tDst, tSrc
        ss << "  {\n"
           << "    let texm_w = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n"
           << "    t" << dstNum << " = "
           << emitStageSample(dstNum, texTypes, "vec2<f32>(texm_u, texm_v)", "vec3<f32>(texm_u, texm_v, texm_w)")
           << ";\n"
           << "  }\n";
        continue;
      }
      case 77: { // texm3x3vspec tDst, tSrc
        ss << "  {\n"
           << "    let texm_w = dot(raw_uv" << dstNum << ".xyz, (" << S(1) << ").xyz);\n"
           << "    let nrm = vec3<f32>(texm_u, texm_v, texm_w);\n"
           << "    let eye = vec3<f32>(texm_w0, texm_w1, raw_uv" << dstNum << ".w);\n"
           << "    let refl = 2.0 * (dot(nrm, eye) / max(dot(nrm, nrm), 1e-6)) * nrm - eye;\n"
           << "    t" << dstNum << " = "
           << emitStageSample(dstNum, texTypes, "refl.xy", "refl")
           << ";\n"
           << "  }\n";
        continue;
      }
      case 1: // mov
        rhs = S(1);
        break;
      case 2: // add
        rhs = "(" + S(1) + " + " + S(2) + ")";
        break;
      case 3: // sub
        rhs = "(" + S(1) + " - " + S(2) + ")";
        break;
      case 4: // mad
        rhs = "(" + S(1) + " * " + S(2) + " + " + S(3) + ")";
        break;
      case 5: // mul
        rhs = "(" + S(1) + " * " + S(2) + ")";
        break;
      case 8: // dp3
        rhs = "vec4<f32>(dot((" + S(1) + ").xyz, (" + S(2) + ").xyz))";
        break;
      case 9: // dp4
        rhs = "vec4<f32>(dot(" + S(1) + ", " + S(2) + "))";
        break;
      case 18: // lrp
        rhs = "mix(" + S(3) + ", " + S(2) + ", " + S(1) + ")";
        break;
      case 80: // cnd (src0 > 0.5 ? src1 : src2)
        rhs = "select(" + S(3) + ", " + S(2) + ", (" + S(1) + ") > vec4<f32>(0.5))";
        break;
      case 88: // cmp (src0 >= 0.0 ? src1 : src2)
        rhs = "select(" + S(3) + ", " + S(2) + ", (" + S(1) + ") >= vec4<f32>(0.0))";
        break;
      default:
        continue;
    }
    emitMaskedAssign(ss, dst, params[0], rhs, ++stepIdx);
  }

  ss << R"(
  var outColor: vec4<f32> = clamp(r0, vec4<f32>(0.0), vec4<f32>(1.0));
  if (u.alphaTest.x > 0.5) {
    let aFunc = u32(u.alphaTest.y + 0.5);
    if (!passAlphaTest(outColor.a, aFunc, u.alphaTest.z)) {
      discard;
    }
  }
  if (u.fogParams.x > 0.5) {
    let ff = clamp(in.fogFactor, 0.0, 1.0);
    outColor = vec4<f32>(mix(u.fogColor.rgb, outColor.rgb, ff), outColor.a);
  }
)";
  // Debug-only early return (see WgpuDebugPsSnippet); empty in every normal run.
  if (g_wgpuDebugPsMode == 4) {
    ss << WgpuDebugIdSnippet();
  } else {
    ss << WgpuDebugPsSnippet(true);
  }
  ss << R"(
  return outColor;
}
)";
  return ss.str();
}
