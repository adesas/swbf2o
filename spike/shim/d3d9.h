#pragma once
/* Direct3D9 declarations for the browser build.
 *
 * RedEngineFL/Graphics/PC (the D3D device implementation) is NOT compiled: it is replaced by a
 * WebGPU/WebGL backend. But a lot of retained game code still includes the renderer's header for
 * RedRenderer:: statics, present parameters and format enums, so this file supplies the D3D9
 * vocabulary those declarations mention. Values here are only produced/consumed by the
 * replacement backend, which owns its own state model.
 */
/* This header models C++ COM/D3D APIs: never include it from C. */
#ifdef __cplusplus  /* C++ only */
#include "swbf2_prefix.h"
#include "d3dx9math.h"

/* ---- enums ---------------------------------------------------------------- */
typedef int D3DFORMAT;
typedef int D3DRESOURCETYPE;
typedef int D3DPOOL;
typedef int D3DPRIMITIVETYPE;
typedef int D3DDEVTYPE;
typedef int D3DQUERYTYPE;
typedef int D3DMULTISAMPLE_TYPE;
typedef int D3DTEXTUREFILTERTYPE;
typedef int D3DTEXTUREADDRESS;
typedef int D3DTRANSFORMSTATETYPE;
typedef int D3DRENDERSTATETYPE;
typedef int D3DTEXTURESTAGESTATETYPE;
typedef int D3DSAMPLERSTATETYPE;
typedef int D3DBLEND;
typedef int D3DBLENDOP;
typedef int D3DCMPFUNC;
typedef int D3DSTENCILOP;
typedef int D3DTEXOP;
typedef int D3DTEXOPARG;
typedef int D3DFVF;
typedef int D3DUSAGE;
typedef int D3DSWAPEFFECT;
typedef int D3DSHADERADDRESSMODE;
typedef unsigned int D3DCOLOR;
/* Microsoft's real D3D9 gamma ramp (d3d9types.h): three named 256-entry 16-bit ramps. The tree
 * indexes it as ramp.red[i]/ramp.green[i]/ramp.blue[i] (pcColorControl.cpp) and passes its
 * address to SetGammaRamp / GetDeviceGammaRamp, which needs an aggregate, not a bare array. */
typedef struct { unsigned short red[256], green[256], blue[256]; } D3DGAMMARAMP;

#define D3DFMT_UNKNOWN        0
#define D3DFMT_A8             28
#define D3DFMT_A8R8G8B8       21
#define D3DFMT_X8R8G8B8       22
#define D3DFMT_R5G6B5         23
#define D3DFMT_A2B10G10R10    31
#define D3DFMT_L8             50
#define D3DFMT_V8U8           60
#define D3DFMT_L6V5U5         61
#define D3DFMT_X8V8U8         63
#define D3DFMT_D24X8          77
#define D3DFMT_D24S8          75
#define D3DFMT_R16F           111
#define D3DFMT_G16R16F        112
#define D3DFMT_A16B16G16R16F  113
#define D3DFMT_DXT1           0x31545844
#define D3DFMT_DXT2           0x32545844
#define D3DFMT_DXT3           0x33545844
#define D3DFMT_DXT4           0x34545844
#define D3DFMT_DXT5           0x35545844

#define D3DMULTISAMPLE_NONE            0
#define D3DMULTISAMPLE_2_SAMPLES       2
#define D3DMULTISAMPLE_4_SAMPLES       4
#define D3DMULTISAMPLE_NONMASKABLE     1

#define D3DPT_POINTLIST     1
#define D3DPT_LINELIST      2
#define D3DPT_LINESTRIP     3
#define D3DPT_TRIANGLELIST  4
#define D3DPT_TRIANGLESTRIP 5
#define D3DPT_TRIANGLEFAN   6

#define D3DTOP_DISABLE     1
#define D3DTOP_SELECTARG1  2
#define D3DTOP_SELECTARG2  3
#define D3DTOP_MODULATE    4
#define D3DTOP_ADD         7
#define D3DTOP_ADDSIGNED   8
#define D3DTOP_BLENDTEXTUREALPHA 13
#define D3DTOP_ADDSMOOTHEDALPHA  10

#define D3DTA_SELECTMASK 0x0000000f
#define D3DTA_DIFFUSE    0x00000000
#define D3DTA_CURRENT    0x00000001
#define D3DTA_TEXTURE    0x00000002
#define D3DTA_TFACTOR    0x00000003
#define D3DTA_CONSTANT   0x00000006

#define D3DTSS_COLOROP        1
#define D3DTSS_COLORARG1      2
#define D3DTSS_COLORARG2      3
#define D3DTSS_ALPHAOP        4
#define D3DTSS_ALPHAARG1      5
#define D3DTSS_ALPHAARG2      6
#define D3DTSS_BUMPENVMAT00   7
#define D3DTSS_ADDRESSU      13
#define D3DTSS_ADDRESSV      14
#define D3DTSS_BORDERCOLOR   15
#define D3DTSS_MAGFILTER     16
#define D3DTSS_MINFILTER     17
#define D3DTSS_MIPFILTER     18
#define D3DTSS_MAXMIPLEVEL   21
#define D3DTSS_MAXANISOTROPY 22
#define D3DTSS_TEXTURETRANSFORMFLAGS 24

#define D3DSAMP_ADDRESSU   1
#define D3DSAMP_ADDRESSV   2
#define D3DSAMP_ADDRESSW   3
#define D3DSAMP_BORDERCOLOR 4
#define D3DSAMP_MAGFILTER  5
#define D3DSAMP_MINFILTER  6
#define D3DSAMP_MIPFILTER  7
#define D3DSAMP_MAXANISOTROPY 10

#define D3DTEXF_NONE     0
#define D3DTEXF_POINT    1
#define D3DTEXF_LINEAR   2
#define D3DTEXF_ANISOTROPIC 3

#define D3DTADDRESS_WRAP     1
#define D3DTADDRESS_MIRROR   2
#define D3DTADDRESS_CLAMP    3
#define D3DTADDRESS_BORDER   4

#define D3DRS_ZENABLE                 7
#define D3DRS_FILLMODE                8
#define D3DRS_SHADEMODE               9
#define D3DRS_ZWRITEENABLE            14
#define D3DRS_ALPHATESTENABLE         15
#define D3DRS_LASTPIXEL               16
#define D3DRS_SRCBLEND                19
#define D3DRS_DESTBLEND               20
#define D3DRS_CULLMODE                22
#define D3DRS_ZFUNC                   23
#define D3DRS_ALPHAREF                24
#define D3DRS_ALPHAFUNC               25
#define D3DRS_DITHERENABLE            26
#define D3DRS_ALPHABLENDENABLE        27
#define D3DRS_FOGENABLE               28
#define D3DRS_SPECULARENABLE          29
#define D3DRS_FOGCOLOR                34
#define D3DRS_FOGTABLEMODE            35
#define D3DRS_FOGSTART                36
#define D3DRS_FOGEND                  37
#define D3DRS_FOGDENSITY              38
#define D3DRS_RANGEFOGENABLE          48
#define D3DRS_STENCILENABLE           52
#define D3DRS_STENCILFAIL             53
#define D3DRS_STENCILZFAIL            54
#define D3DRS_STENCILPASS             55
#define D3DRS_STENCILFUNC             56
#define D3DRS_STENCILREF              57
#define D3DRS_STENCILMASK             58
#define D3DRS_STENCILWRITEMASK        59
#define D3DRS_TEXTUREFACTOR           60
#define D3DRS_WRAP0                   128
#define D3DRS_WRAP1                   129
#define D3DRS_WRAP2                   130
#define D3DRS_WRAP3                   131
#define D3DRS_WRAP4                   132
#define D3DRS_WRAP5                   133
#define D3DRS_WRAP6                   134
#define D3DRS_WRAP7                   135
#define D3DRS_CLIPPING                136
#define D3DRS_LIGHTING                137
#define D3DRS_AMBIENT                 139
#define D3DRS_FOGVERTEXMODE           140
#define D3DRS_COLORVERTEX             141
#define D3DRS_LOCALVIEWER             142
#define D3DRS_NORMALIZENORMALS        143
#define D3DRS_DIFFUSEMATERIALSOURCE   145
#define D3DRS_SPECULARMATERIALSOURCE  146
#define D3DRS_AMBIENTMATERIALSOURCE   147
#define D3DRS_EMISSIVEMATERIALSOURCE  148
#define D3DRS_VERTEXBLEND             151
#define D3DRS_CLIPPLANEENABLE         152
#define D3DRS_SOFTWAREVERTEXPROCESSING 153
#define D3DRS_POINTSIZE               154
#define D3DRS_POINTSIZE_MIN           155
#define D3DRS_POINTSPRITEENABLE       156
#define D3DRS_POINTSCALEENABLE        157
#define D3DRS_POINTSCALE_A            158
#define D3DRS_POINTSCALE_B            159
#define D3DRS_POINTSCALE_C            160
#define D3DRS_MULTISAMPLEANTIALIAS    161
#define D3DRS_MULTISAMPLEMASK         162
#define D3DRS_PATCHEDGESTYLE          163
#define D3DRS_DEBUGMONITORTOKEN       165
#define D3DRS_POINTSIZE_MAX           166
#define D3DRS_INDEXEDVERTEXBLENDENABLE 167
#define D3DRS_COLORWRITEENABLE        168
#define D3DRS_TWEENFACTOR             170
#define D3DRS_BLENDOP                 171
#define D3DRS_POSITIONDEGREE          172
#define D3DRS_NORMALDEGREE            173
#define D3DRS_SCISSORTESTENABLE       174
#define D3DRS_SLOPESCALEDEPTHBIAS     175
#define D3DRS_ANTIALIASEDLINEENABLE   176
#define D3DRS_MINTESSELLATIONLEVEL    178
#define D3DRS_MAXTESSELLATIONLEVEL    179
#define D3DRS_ADAPTIVETESS_X          180
#define D3DRS_ADAPTIVETESS_Y          181
#define D3DRS_ADAPTIVETESS_Z          182
#define D3DRS_ADAPTIVETESS_W          183
#define D3DRS_ENABLEADAPTIVETESSELLATION 184
#define D3DRS_TWOSIDEDSTENCILMODE     185
#define D3DRS_CCW_STENCILFAIL         186
#define D3DRS_CCW_STENCILZFAIL        187
#define D3DRS_CCW_STENCILPASS         188
#define D3DRS_CCW_STENCILFUNC         189
#define D3DRS_COLORWRITEENABLE1       190
#define D3DRS_COLORWRITEENABLE2       191
#define D3DRS_COLORWRITEENABLE3       192
#define D3DRS_BLENDFACTOR             193
#define D3DRS_SRGBWRITEENABLE         194
#define D3DRS_DEPTHBIAS               195
#define D3DRS_WRAP8                   198
#define D3DRS_WRAP9                   199
#define D3DRS_WRAP10                  200
#define D3DRS_WRAP11                  201
#define D3DRS_WRAP12                  202
#define D3DRS_WRAP13                  203
#define D3DRS_WRAP14                  204
#define D3DRS_WRAP15                  205
#define D3DRS_SEPARATEALPHABLENDENABLE 206
#define D3DRS_SRCBLENDALPHA           207
#define D3DRS_DESTBLENDALPHA          208
#define D3DRS_BLENDOPALPHA            209

#define D3DTSS_BUMPENVLSCALE          22
#define D3DTSS_BUMPENVLOFFSET         23
#define D3DTSS_RESULTARG              28
#define D3DSAMP_SRGBTEXTURE           11
#define D3DSAMP_ELEMENTINDEX          12
#define D3DSAMP_DMAPOFFSET            13
#define D3DSBT_ALL                    1
#define D3DSBT_PIXELSTATE             2
#define D3DSBT_VERTEXSTATE            3
#define D3DCULL_NONE                  1
#define D3DCULL_CW                    2
#define D3DCULL_CCW                   3

#define D3DBLEND_ZERO              1
#define D3DBLEND_ONE               2
#define D3DBLEND_SRCCOLOR          3
#define D3DBLEND_INVSRCCOLOR       4
#define D3DBLEND_SRCALPHA          5
#define D3DBLEND_INVSRCALPHA       6
#define D3DBLEND_DESTALPHA         7
#define D3DBLEND_INVDESTALPHA      8
#define D3DBLEND_DESTCOLOR         9
#define D3DBLEND_INVDESTCOLOR     10

#define D3DCMP_NEVER      1
#define D3DCMP_LESS       2
#define D3DCMP_EQUAL      3
#define D3DCMP_LESSEQUAL  4
#define D3DCMP_GREATER    5
#define D3DCMP_NOTEQUAL   6
#define D3DCMP_GREATEREQUAL 7
#define D3DCMP_ALWAYS     8
/* the previous spelling of D3DCMP_NOTEQUAL was mis-cased; kept so nothing silently breaks */
#define D3DCMP_NOTEqual  D3DCMP_NOTEQUAL

#define D3DFOG_EXP     1
#define D3DFOG_EXP2    2
#define D3DFOG_LINEAR  3

#define D3DSWAPEFFECT_DISCARD  1
#define D3DSWAPEFFECT_FLIP     2
#define D3DSWAPEFFECT_COPY     3
#define D3DSWAPEFFECT_VERTICAL_BLIT 6

#define D3DPOOL_DEFAULT 0
#define D3DPOOL_MANAGED 1
#define D3DPOOL_SYSTEMMEM 2
#define D3DPOOL_SCRATCH   3

#define D3DUSAGE_DYNAMIC    0x00000200
#define D3DUSAGE_WRITEONLY  0x00000008
#define D3DUSAGE_DEPTHSTENCIL 0x00000002
#define D3DUSAGE_RENDERTARGET 0x00000001
#define D3DUSAGE_QUERY        0x00001000

#define D3DLOCK_DISCARD     0x00002000
#define D3DLOCK_READONLY    0x00000010
#define D3DLOCK_NOSYSLOCK   0x00000800

#define D3DSCANLINEORDERING_PROGRESSIVE 1

#define MAX_D3DVERTEXELEMENTS 32

/* ---- structs -------------------------------------------------------------- */
struct D3DDISPLAYMODE      { unsigned int Width = 0, Height = 0, RefreshRate = 0; D3DFORMAT Format = 0; };
struct D3DDISPLAYMODEFILTER { unsigned int Size = 0; D3DDISPLAYMODE DM; };
struct D3DSURFACE_DESC     { D3DFORMAT Format = 0; D3DRESOURCETYPE Type = 0; D3DUSAGE Usage = 0; D3DPOOL Pool = 0; D3DMULTISAMPLE_TYPE MultiSampleType = 0; DWORD MultiSampleQuality = 0; unsigned int Width = 0, Height = 0; };
struct D3DVERTEXBUFFER_DESC { unsigned int Size = 0; D3DFORMAT Format = 0; D3DRESOURCETYPE Type = 0; D3DPOOL Pool = 0; D3DUSAGE Usage = 0; unsigned int FVF = 0; };
struct D3DINDEXBUFFER_DESC { unsigned int Size = 0; D3DFORMAT Format = 0; D3DRESOURCETYPE Type = 0; D3DPOOL Pool = 0; D3DUSAGE Usage = 0; };
struct D3DVOLUME_DESC      { unsigned int Width = 0, Height = 0, Depth = 0; D3DFORMAT Format = 0; D3DRESOURCETYPE Type = 0; D3DPOOL Pool = 0; D3DUSAGE Usage = 0; };
struct D3DLOCKED_RECT      { void* pBits = 0; int Pitch = 0; };
struct D3DLOCKED_BOX       { void* pBits = 0; int RowPitch = 0; int SlicePitch = 0; };
struct D3DRECT             { long x1, y1, x2, y2; };
struct D3DRECT9            { long x1, y1, x2, y2; };
struct D3DVIEWPORT9        { unsigned int X, Y, Width, Height; float MinZ, MaxZ; };
#ifndef SWBF2_D3DMATRIX_DEFINED
#define SWBF2_D3DMATRIX_DEFINED
struct D3DMATRIX           { union { float m[4][4]; struct { float _11,_12,_13,_14,_21,_22,_23,_24,_31,_32,_33,_34,_41,_42,_43,_44; }; }; };
#endif
struct D3DMATERIAL9        { D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive; float Power; };
struct D3DLIGHT9 {
  int Type;
  D3DCOLORVALUE Diffuse, Specular, Ambient;
  D3DXVECTOR3 Position, Direction, Velocity;
  float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};
struct D3DVERTEXELEMENT9   { unsigned short Stream, Offset; unsigned char Type, Method, Usage, UsageIndex; };
struct D3DPRESENT_PARAMETERS {
  unsigned int       BackBufferWidth, BackBufferHeight, BackBufferCount;
  D3DMULTISAMPLE_TYPE MultiSampleType;
  int                MultiSampleQuality;
  D3DSWAPEFFECT      SwapEffect;
  void*              hDeviceWindow;
  D3DFORMAT          BackBufferFormat;
  int                Windowed;
  int                EnableAutoDepthStencil;
  D3DUSAGE           AutoDepthStencilFormat;
  int                Flags;
  int                FullScreen_RefreshRateInHz;
  int                PresentationInterval;
  D3DFORMAT          DepthStencilFormat;
};
struct D3DCAPS9 {
  unsigned int DeviceType, AdapterOrdinal;
  unsigned int Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps,
               PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps,
               AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps,
               CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps,
               VertexTextureFilterCaps, TextureAddressCapsV;
  unsigned int StencilCaps, TextureOpCaps, LineCaps, TextureMipMapCaps, VolumeTextureCaps;
  int MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat;
  int MaxAnisotropy, MaxTextureAspectRatio, MaxVertexBlendMatrixIndex, MaxPrimitiveCount;
  int MaxVertexIndex, MaxStreams, MaxStreamStride;
  unsigned int VertexShaderVersion, PixelShaderVersion, MaxVertexShaderConst;
  int MaxTextureBlendStages, MaxSimultaneousTextures;
  float MaxVertexDeviation;
  unsigned int DevCaps2, Reserved[32];
  unsigned int StretchRectFilterCaps;
  int MaxVertexBlend, MaxVertexWinding, MaxPrimitiveCount2;
  int MaxActiveHardwareTextures, MaxStreamStride2;
  unsigned int GlobalFlags, VOLTEXTCAPS;
};
struct D3DADAPTER_IDENTIFIER9 {
  char Driver[512], Description[512], DeviceName[32];
  unsigned long DriverVersion, VendorId, DeviceId, SubSysId, Revision;
  GUID DriverGuid;
};
struct D3DFONT { int shim; };


/* ---- D3D9 vocabulary the platform translation units spell directly ----------
 * Everything above this line was the vocabulary the *retained* (non-platform) code needed. The
 * block below is what RedEngineFL/Graphics/PC plus the Battlefront2 PC effect code name. Each
 * value is Microsoft's real Direct3D 9 value (cross-checked against the official enum bodies as
 * carried in Wine's d3d9types.h / d3d9caps.h), because these numbers are the contract: the
 * WebGPU backend will decode render-state / texture-stage / vertex-declaration ids that the
 * engine writes with these names. tools/shim_autogen.py never regenerates a name that is defined
 * here, so this section - not generated_types.h - is the source of truth for these spellings. */

/* SDK / adapter identity */
#define D3D_SDK_VERSION                        32

/* FourCC, shader version words, colours, the declaration terminator the engine uses */
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
  ((unsigned int)(unsigned char)(ch0) | ((unsigned int)(unsigned char)(ch1) << 8) | \
   ((unsigned int)(unsigned char)(ch2) << 16) | ((unsigned int)(unsigned char)(ch3) << 24))
#endif
#define D3DCOLOR_ARGB(a, r, g, b) \
  ((D3DCOLOR)((((a) & 0xffu) << 24) | (((r) & 0xffu) << 16) | (((g) & 0xffu) << 8) | ((b) & 0xffu)))
#define D3DPS_VERSION(major, minor) (0xFFFF0000 | ((major) << 8) | (minor))
#define D3DVS_VERSION(major, minor) (0xFFFE0000 | ((major) << 8) | (minor))
#define D3DDECL_END() { 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }
#define D3DTS_WORLDMATRIX(index) (D3DTRANSFORMSTATETYPE)((index) + 256)
#define D3DTS_WORLD D3DTS_WORLDMATRIX(0)


/* HRESULTs the device layer reports (same facility values as Microsoft's d3d9.h) */
#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev, fac, code) \
  ((long)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#endif
#ifndef _FACD3D
#define _FACD3D 0x876
#endif
#define MAKE_D3DHRESULT(code) MAKE_HRESULT(1, _FACD3D, code)
#define D3D_OK                                 S_OK
#define D3DERR_DRIVERINTERNALERROR             MAKE_D3DHRESULT(2087)
#define D3DERR_DEVICELOST                      MAKE_D3DHRESULT(2152)
#define D3DERR_DEVICENOTRESET                  MAKE_D3DHRESULT(2153)
#define D3DERR_NOTAVAILABLE                    MAKE_D3DHRESULT(2154)
#define D3DERR_INVALIDCALL                     MAKE_D3DHRESULT(2156)
#define D3DERR_OUTOFVIDEOMEMORY                MAKE_D3DHRESULT(380)

/* enums the engine declares variables with */
typedef int D3DCUBEMAP_FACES;
typedef int D3DSTATEBLOCKTYPE;
typedef int D3DBACKBUFFER_TYPE;
typedef int D3DZBUFFERTYPE;
typedef int D3DBASISTYPE;
typedef int D3DDEGREETYPE;

#define D3DBACKBUFFER_TYPE_MONO      0

/* remaining descriptor structs the PC renderer fills in */
struct D3DBOX                    { unsigned int Left, Top, Right, Bottom, Front, Back; };
struct D3DCLIPSTATUS9            { unsigned int ClipUnion, ClipIntersection; };
struct D3DRASTER_STATUS          { int InVBlank; unsigned int ScanLine; };
struct D3DDEVICE_CREATION_PARAMETERS {
  unsigned int AdapterOrdinal; D3DDEVTYPE DeviceType; void* hFocusWindow; unsigned int BehaviorFlags;
};
struct D3DRECTPATCH_INFO {
  unsigned int StartVertexOffsetWidth, StartVertexOffsetHeight, Width, Height, Stride;
  D3DBASISTYPE Basis; D3DDEGREETYPE Degree;
};
struct D3DTRIPATCH_INFO {
  unsigned int StartVertexOffset, NumVertices; D3DBASISTYPE Basis; D3DDEGREETYPE Degree;
};

/* formats - D3DFORMAT */
#define D3DFMT_A16B16G16R16                    36
#define D3DFMT_A1R5G5B5                        25
#define D3DFMT_A2R10G10B10                     35
#define D3DFMT_A4L4                            52
#define D3DFMT_A8B8G8R8                        32
#define D3DFMT_A8L8                            51
#define D3DFMT_A8P8                            40
#define D3DFMT_A8R3G3B2                        29
#define D3DFMT_D15S1                           73
#define D3DFMT_D24X4S4                         79
#define D3DFMT_D32                             71
#define D3DFMT_G16R16                          34
#define D3DFMT_INDEX16                         101
#define D3DFMT_L16                             81
#define D3DFMT_P8                              41
#define D3DFMT_R3G3B2                          27
#define D3DFMT_R8G8B8                          20
#define D3DFMT_VERTEXDATA                      100
#define D3DFMT_X1R5G5B5                        24
#define D3DFMT_X4R4G4B4                        30
#define D3DFMT_X8B8G8R8                        33

/* vertex declaration - D3DDECL* */
#define D3DDECLMETHOD_DEFAULT                  0
#define D3DDECLTYPE_D3DCOLOR                   4
#define D3DDECLTYPE_FLOAT1                     0
#define D3DDECLTYPE_FLOAT2                     1
#define D3DDECLTYPE_FLOAT3                     2
#define D3DDECLTYPE_FLOAT4                     3
#define D3DDECLTYPE_SHORT2                     6
#define D3DDECLTYPE_SHORT4                     7
#define D3DDECLTYPE_UNUSED                     17
#define D3DDECLUSAGE_BINORMAL                  7
#define D3DDECLUSAGE_BLENDINDICES              2
#define D3DDECLUSAGE_BLENDWEIGHT               1
#define D3DDECLUSAGE_COLOR                     10
#define D3DDECLUSAGE_NORMAL                    3
#define D3DDECLUSAGE_POSITION                  0
#define D3DDECLUSAGE_POSITIONT                 9
#define D3DDECLUSAGE_TANGENT                   6
#define D3DDECLUSAGE_TEXCOORD                  5

/* render states - D3DRENDERSTATETYPE (the state ids the WebGPU backend decodes) */
#define D3DRS_ALPHAFUNC                        25
#define D3DRS_AMBIENT                          139
#define D3DRS_AMBIENTMATERIALSOURCE            147
#define D3DRS_CCW_STENCILFUNC                  189
#define D3DRS_CCW_STENCILPASS                  188
#define D3DRS_CCW_STENCILZFAIL                 187
#define D3DRS_COLORVERTEX                      141
#define D3DRS_DIFFUSEMATERIALSOURCE            145
#define D3DRS_EMISSIVEMATERIALSOURCE           148
#define D3DRS_FILLMODE                         8
#define D3DRS_LIGHTING                         137
#define D3DRS_NORMALIZENORMALS                 143
#define D3DRS_SPECULARENABLE                   29
#define D3DRS_SPECULARMATERIALSOURCE           146
#define D3DRS_STENCILMASK                      58
#define D3DRS_STENCILPASS                      55
#define D3DRS_STENCILREF                       57
#define D3DRS_STENCILWRITEMASK                 59
#define D3DRS_STENCILZFAIL                     54
#define D3DRS_TEXTUREFACTOR                    60
#define D3DRS_TWOSIDEDSTENCILMODE              185
#define D3DRS_ZFUNC                            23

/* texture stages - D3DTEXTURESTAGESTATETYPE / D3DSAMPLERSTATETYPE / transform flags / args */
#define D3DMCS_COLOR1                          1
#define D3DMCS_MATERIAL                        0
#define D3DSAMP_MAXMIPLEVEL                    9
#define D3DSAMP_MIPMAPLODBIAS                  8
#define D3DTA_ALPHAREPLICATE                   0x00000020
#define D3DTA_SPECULAR                         0x00000004
#define D3DTOP_ADDSMOOTH                       11
#define D3DTOP_BLENDCURRENTALPHA               16
#define D3DTOP_BLENDDIFFUSEALPHA               12
#define D3DTOP_BLENDFACTORALPHA                14
#define D3DTOP_BUMPENVMAP                      22
#define D3DTOP_MODULATE2X                      5
#define D3DTOP_MODULATECOLOR_ADDALPHA          19
#define D3DTOP_MULTIPLYADD                     25
#define D3DTSS_ALPHAARG0                       27
#define D3DTSS_BUMPENVMAT01                    8
#define D3DTSS_BUMPENVMAT10                    9
#define D3DTSS_BUMPENVMAT11                    10
#define D3DTSS_COLORARG0                       26
#define D3DTSS_CONSTANT                        32
#define D3DTSS_TCI_CAMERASPACENORMAL           0x10000
#define D3DTSS_TCI_CAMERASPACEPOSITION         0x20000
#define D3DTSS_TEXCOORDINDEX                   11
#define D3DTTFF_COUNT2                         2
#define D3DTTFF_COUNT3                         3
#define D3DTTFF_DISABLE                        0
#define D3DTTFF_PROJECTED                      256

/* device capability bits (D3DCAPS9 fields) */
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT         0x0010000
#define D3DDEVCAPS_PUREDEVICE                  0x0100000
#define D3DPMISCCAPS_PERSTAGECONSTANT          (0x00008000)
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL     (0x00000100)
#define D3DPTEXTURECAPS_NOPROJECTEDBUMPENV     (0x00200000)
#define D3DPTEXTURECAPS_POW2                   (0x00000002)
#define D3DSTENCILCAPS_TWOSIDED                0x100
#define D3DTEXOPCAPS_BUMPENVMAP                0x0200000
#define D3DTEXOPCAPS_MULTIPLYADD               0x1000000

/* comparison / stencil / fill / z-buffer / resource-type enums */
#define D3DCMP_NOTEQUAL                        6
#define D3DDEVTYPE_HAL                         1
#define D3DFILL_SOLID                          3
#define D3DFILL_WIREFRAME                      2
#define D3DRTYPE_CUBETEXTURE                   5
#define D3DRTYPE_INDEXBUFFER                   7
#define D3DRTYPE_SURFACE                       1
#define D3DRTYPE_TEXTURE                       3
#define D3DRTYPE_VERTEXBUFFER                  6
#define D3DRTYPE_VOLUMETEXTURE                 4
#define D3DSTENCILOP_DECR                      8
#define D3DSTENCILOP_INCR                      7
#define D3DSTENCILOP_KEEP                      1
#define D3DTS_PROJECTION                       3
#define D3DTS_VIEW                             2
#define D3DZB_FALSE                            0
#define D3DZB_TRUE                             1

/* presentation intervals, MSAA levels, vertex-buffer FVFs, lock flags */
#define D3DFVF_DIFFUSE                         0x0040
#define D3DFVF_LASTBETA_UBYTE4                 0x1000
#define D3DFVF_NORMAL                          0x0010
#define D3DFVF_SPECULAR                        0x0080
#define D3DFVF_TEX1                            0x0100
#define D3DFVF_TEX2                            0x0200
#define D3DFVF_XYZ                             0x0002
#define D3DFVF_XYZB1                           0x0006
#define D3DFVF_XYZB2                           0x0008
#define D3DFVF_XYZB3                           0x000a
#define D3DFVF_XYZB4                           0x000c
#define D3DFVF_XYZRHW                          0x0004
#define D3DLOCK_NOOVERWRITE                    0x1000
#define D3DMULTISAMPLE_10_SAMPLES              10
#define D3DMULTISAMPLE_11_SAMPLES              11
#define D3DMULTISAMPLE_12_SAMPLES              12
#define D3DMULTISAMPLE_13_SAMPLES              13
#define D3DMULTISAMPLE_14_SAMPLES              14
#define D3DMULTISAMPLE_15_SAMPLES              15
#define D3DMULTISAMPLE_3_SAMPLES               3
#define D3DMULTISAMPLE_5_SAMPLES               5
#define D3DMULTISAMPLE_6_SAMPLES               6
#define D3DMULTISAMPLE_7_SAMPLES               7
#define D3DMULTISAMPLE_8_SAMPLES               8
#define D3DMULTISAMPLE_9_SAMPLES               9
#define D3DPRESENT_INTERVAL_DEFAULT            0x00000000
#define D3DPRESENT_INTERVAL_FOUR               0x00000008
#define D3DPRESENT_INTERVAL_IMMEDIATE          0x80000000
#define D3DPRESENT_INTERVAL_ONE                0x00000001
#define D3DPRESENT_INTERVAL_THREE              0x00000004
#define D3DPRESENT_INTERVAL_TWO                0x00000002

/* other referenced D3D9 constants (real Microsoft Direct3D 9 SDK values) */
#define D3DCOLORWRITEENABLE_RED                ((1)<<0)
#define D3DCOLORWRITEENABLE_GREEN              ((1)<<1)
#define D3DCOLORWRITEENABLE_BLUE               ((1)<<2)
#define D3DCOLORWRITEENABLE_ALPHA              ((1)<<3)
#define D3DCREATE_HARDWARE_VERTEXPROCESSING    (0x00000040)
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING    (0x00000020)
#define D3DCLEAR_TARGET                        0x00000001
#define D3DCLEAR_ZBUFFER                       0x00000002
#define D3DCLEAR_STENCIL                       0x00000004
#define D3DFMT_A4R4G4B4                        26
#define D3DFMT_D16_LOCKABLE                    70
#define D3DFMT_D16                             80
#define D3DFMT_INDEX32                         102
#define D3DLIGHT_POINT                         1
#define D3DLIGHT_SPOT                          2
#define D3DLIGHT_DIRECTIONAL                   3
#define D3DQUERYTYPE_EVENT                     8
#define D3DQUERYTYPE_OCCLUSION                 9
#define D3DISSUE_END                           (1 << 0)
#define D3DISSUE_BEGIN                         (1 << 1)
#define D3DGETDATA_FLUSH                       (1 << 0)
#define D3DMULTISAMPLE_16_SAMPLES              16
#define D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES 0x00000010
#define D3DPTFILTERCAPS_MINFPOINT              0x00000100
#define D3DPTFILTERCAPS_MINFLINEAR             0x00000200
#define D3DPTFILTERCAPS_MAGFPOINT              0x01000000
#define D3DPTFILTERCAPS_MAGFLINEAR             0x02000000
#define D3DTS_TEXTURE0                         16
#define D3DTS_TEXTURE1                         17
#define D3DTS_TEXTURE2                         18
#define D3DTS_TEXTURE3                         19
#define D3DTS_TEXTURE4                         20
#define D3DTS_TEXTURE5                         21
#define D3DTS_TEXTURE6                         22
#define D3DTS_TEXTURE7                         23
#define D3DTA_TEMP                             0x00000005
#define D3DTA_COMPLEMENT                       0x00000010
#define D3DFVF_POSITION_MASK                   0x400E
#define D3DFVF_TEXCOUNT_MASK                   0x0F00
#define D3DFVF_TEXCOUNT_SHIFT                  8
#define D3DFVF_TEX3                            0x0300
#define D3DFVF_TEX4                            0x0400

typedef int D3DCULL;
typedef D3DFORMAT _D3DFORMAT;
typedef D3DMATERIAL9 _D3DMATERIAL9;
typedef D3DPOOL _D3DPOOL;
typedef D3DRENDERSTATETYPE _D3DRENDERSTATETYPE;
typedef D3DSAMPLERSTATETYPE _D3DSAMPLERSTATETYPE;
typedef D3DTEXTURESTAGESTATETYPE _D3DTEXTURESTAGESTATETYPE;
typedef D3DTRANSFORMSTATETYPE _D3DTRANSFORMSTATETYPE;

/* ---- Real Virtual Direct3D 9 COM Interfaces --------------------------------
 * Both the engine's FakeD3D* fallback classes (used when g_bNoRender == true in
 * Node/native oracle runs) and the WebGPU D3D9 backend (browser/gpu/) derive
 * from these exact interfaces, matching Microsoft's d3d9.h vtable signatures. */
struct IDirect3D9;                struct IDirect3DDevice9;          struct IDirect3DSurface9;
struct IDirect3DBaseTexture9;     struct IDirect3DTexture9;         struct IDirect3DCubeTexture9;
struct IDirect3DVolumeTexture9;   struct IDirect3DVolume9;          struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;     struct IDirect3DVertexShader9;    struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9; struct IDirect3DStateBlock9;    struct IDirect3DSwapChain9;
struct IDirect3DQuery9;           struct IDirect3DResource9;        struct IDirect3DBaseVertexDecl9;
struct IDirectD3D9;

struct IDirect3D9 : IUnknown {
  STDMETHOD(RegisterSoftwareDevice)(void* pInitializeFunction) { (void)pInitializeFunction; return E_NOTIMPL; }
  STDMETHOD_(UINT, GetAdapterCount)() { return 0; }
  STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) {
    (void)Adapter; (void)Flags; if (pIdentifier) memset(pIdentifier, 0, sizeof(*pIdentifier)); return E_NOTIMPL;
  }
  STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) { (void)Adapter; (void)Format; return 0; }
  STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) {
    (void)Adapter; (void)Format; (void)Mode; (void)pMode; return E_NOTIMPL;
  }
  STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) {
    (void)Adapter; if (pMode) memset(pMode, 0, sizeof(*pMode)); return E_NOTIMPL;
  }
  STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
                             D3DFORMAT BackBufferFormat, BOOL bWindowed) {
    (void)Adapter; (void)DevType; (void)AdapterFormat; (void)BackBufferFormat; (void)bWindowed; return E_NOTIMPL;
  }
  STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                               DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
    (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)Usage; (void)RType; (void)CheckFormat; return E_NOTIMPL;
  }
  STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
                                        BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
                                        DWORD* pQualityLevels) {
    (void)Adapter; (void)DeviceType; (void)SurfaceFormat; (void)Windowed; (void)MultiSampleType;
    if (pQualityLevels) *pQualityLevels = 0; return E_NOTIMPL;
  }
  STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                    D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
    (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)RenderTargetFormat; (void)DepthStencilFormat; return E_NOTIMPL;
  }
  STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType,
                                         D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    (void)Adapter; (void)DeviceType; (void)SourceFormat; (void)TargetFormat; return E_NOTIMPL;
  }
  STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
    (void)Adapter; (void)DeviceType; if (pCaps) memset(pCaps, 0, sizeof(*pCaps)); return E_NOTIMPL;
  }
  STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) { (void)Adapter; return 0; }
  STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                          D3DPRESENT_PARAMETERS* pPresentationParameters,
                          IDirect3DDevice9** ppReturnedDeviceInterface) {
    (void)Adapter; (void)DeviceType; (void)hFocusWindow; (void)BehaviorFlags; (void)pPresentationParameters;
    if (ppReturnedDeviceInterface) *ppReturnedDeviceInterface = 0; return E_NOTIMPL;
  }
};

struct IDirect3DResource9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(SetPrivateData)(REFGUID refguid, CONST void* pData, DWORD SizeOfData, DWORD Flags) {
    (void)refguid; (void)pData; (void)SizeOfData; (void)Flags; return E_NOTIMPL;
  }
  STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) {
    (void)refguid; (void)pData; (void)pSizeOfData; return E_NOTIMPL;
  }
  STDMETHOD(FreePrivateData)(REFGUID refguid) { (void)refguid; return E_NOTIMPL; }
  STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) { (void)PriorityNew; return 0; }
  STDMETHOD_(DWORD, GetPriority)() { return 0; }
  STDMETHOD_(void, PreLoad)() {}
  STDMETHOD_(D3DRESOURCETYPE, GetType)() { return 0; }
};

struct IDirect3DSurface9 : IDirect3DResource9 {
  STDMETHOD(GetContainer)(REFIID riid, void** ppContainer) {
    (void)riid; if (ppContainer) *ppContainer = 0; return E_NOTIMPL;
  }
  STDMETHOD(GetDesc)(D3DSURFACE_DESC* pDesc) {
    if (pDesc) *pDesc = D3DSURFACE_DESC{}; return E_NOTIMPL;
  }
  STDMETHOD(LockRect)(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
    (void)pRect; (void)Flags; if (pLockedRect) *pLockedRect = D3DLOCKED_RECT{}; return E_NOTIMPL;
  }
  STDMETHOD(UnlockRect)() { return E_NOTIMPL; }
  STDMETHOD(GetDC)(HDC* phdc) { if (phdc) *phdc = 0; return E_NOTIMPL; }
  STDMETHOD(ReleaseDC)(HDC hdc) { (void)hdc; return E_NOTIMPL; }
};

struct IDirect3DVolume9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(SetPrivateData)(REFGUID refguid, CONST void* pData, DWORD SizeOfData, DWORD Flags) {
    (void)refguid; (void)pData; (void)SizeOfData; (void)Flags; return E_NOTIMPL;
  }
  STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) {
    (void)refguid; (void)pData; (void)pSizeOfData; return E_NOTIMPL;
  }
  STDMETHOD(FreePrivateData)(REFGUID refguid) { (void)refguid; return E_NOTIMPL; }
  STDMETHOD(GetContainer)(REFIID riid, void** ppContainer) {
    (void)riid; if (ppContainer) *ppContainer = 0; return E_NOTIMPL;
  }
  STDMETHOD(GetDesc)(D3DVOLUME_DESC* pDesc) {
    if (pDesc) *pDesc = D3DVOLUME_DESC{}; return E_NOTIMPL;
  }
  STDMETHOD(LockBox)(D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) {
    (void)pBox; (void)Flags; if (pLockedVolume) *pLockedVolume = D3DLOCKED_BOX{}; return E_NOTIMPL;
  }
  STDMETHOD(UnlockBox)() { return E_NOTIMPL; }
};

struct IDirect3DBaseTexture9 : IDirect3DResource9 {
  STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) { (void)LODNew; return 0; }
  STDMETHOD_(DWORD, GetLOD)() { return 0; }
  STDMETHOD_(DWORD, GetLevelCount)() { return 0; }
  STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) { (void)FilterType; return E_NOTIMPL; }
  STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() { return D3DTEXF_NONE; }
  STDMETHOD_(void, GenerateMipSubLevels)() {}
};

struct IDirect3DTexture9 : IDirect3DBaseTexture9 {
  STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) {
    (void)Level; if (pDesc) *pDesc = D3DSURFACE_DESC{}; return E_NOTIMPL;
  }
  STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface9** ppSurfaceLevel) {
    (void)Level; if (ppSurfaceLevel) *ppSurfaceLevel = 0; return E_NOTIMPL;
  }
  STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
    (void)Level; (void)pRect; (void)Flags; if (pLockedRect) *pLockedRect = D3DLOCKED_RECT{}; return E_NOTIMPL;
  }
  STDMETHOD(UnlockRect)(UINT Level) { (void)Level; return E_NOTIMPL; }
  STDMETHOD(AddDirtyRect)(CONST RECT* pDirtyRect) { (void)pDirtyRect; return E_NOTIMPL; }
};

struct IDirect3DCubeTexture9 : IDirect3DBaseTexture9 {
  STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) {
    (void)Level; if (pDesc) *pDesc = D3DSURFACE_DESC{}; return E_NOTIMPL;
  }
  STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppCubeMapSurface) {
    (void)FaceType; (void)Level; if (ppCubeMapSurface) *ppCubeMapSurface = 0; return E_NOTIMPL;
  }
  STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect,
                      CONST RECT* pRect, DWORD Flags) {
    (void)FaceType; (void)Level; (void)pRect; (void)Flags;
    if (pLockedRect) *pLockedRect = D3DLOCKED_RECT{}; return E_NOTIMPL;
  }
  STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) { (void)FaceType; (void)Level; return E_NOTIMPL; }
  STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES FaceType, CONST RECT* pDirtyRect) {
    (void)FaceType; (void)pDirtyRect; return E_NOTIMPL;
  }
};

struct IDirect3DVolumeTexture9 : IDirect3DBaseTexture9 {
  STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC* pDesc) {
    (void)Level; if (pDesc) *pDesc = D3DVOLUME_DESC{}; return E_NOTIMPL;
  }
  STDMETHOD(GetVolumeLevel)(UINT Level, IDirect3DVolume9** ppVolumeLevel) {
    (void)Level; if (ppVolumeLevel) *ppVolumeLevel = 0; return E_NOTIMPL;
  }
  STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) {
    (void)Level; (void)pBox; (void)Flags; if (pLockedVolume) *pLockedVolume = D3DLOCKED_BOX{}; return E_NOTIMPL;
  }
  STDMETHOD(UnlockBox)(UINT Level) { (void)Level; return E_NOTIMPL; }
  STDMETHOD(AddDirtyBox)(CONST D3DBOX* pDirtyBox) { (void)pDirtyBox; return E_NOTIMPL; }
};

struct IDirect3DVertexBuffer9 : IDirect3DResource9 {
  STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
    (void)OffsetToLock; (void)SizeToLock; (void)Flags; if (ppbData) *ppbData = 0; return E_NOTIMPL;
  }
  STDMETHOD(Unlock)() { return E_NOTIMPL; }
  STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* pDesc) {
    if (pDesc) *pDesc = D3DVERTEXBUFFER_DESC{}; return E_NOTIMPL;
  }
};

struct IDirect3DIndexBuffer9 : IDirect3DResource9 {
  STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
    (void)OffsetToLock; (void)SizeToLock; (void)Flags; if (ppbData) *ppbData = 0; return E_NOTIMPL;
  }
  STDMETHOD(Unlock)() { return E_NOTIMPL; }
  STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* pDesc) {
    if (pDesc) *pDesc = D3DINDEXBUFFER_DESC{}; return E_NOTIMPL;
  }
};

struct IDirect3DVertexDeclaration9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9* pElement, UINT* pNumElements) {
    (void)pElement; if (pNumElements) *pNumElements = 0; return E_NOTIMPL;
  }
};

struct IDirect3DBaseVertexDecl9 : IDirect3DVertexDeclaration9 {};

struct IDirect3DVertexShader9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) {
    (void)pData; if (pSizeOfData) *pSizeOfData = 0; return E_NOTIMPL;
  }
};

struct IDirect3DPixelShader9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) {
    (void)pData; if (pSizeOfData) *pSizeOfData = 0; return E_NOTIMPL;
  }
};

struct IDirect3DStateBlock9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(Capture)() { return E_NOTIMPL; }
  STDMETHOD(Apply)() { return E_NOTIMPL; }
};

struct IDirect3DSwapChain9 : IUnknown {
  STDMETHOD(Present)(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride,
                     CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
    (void)pSourceRect; (void)pDestRect; (void)hDestWindowOverride; (void)pDirtyRegion; (void)dwFlags; return E_NOTIMPL;
  }
  STDMETHOD(GetFrontBufferData)(IDirect3DSurface9* pDestSurface) { (void)pDestSurface; return E_NOTIMPL; }
  STDMETHOD(GetBackBuffer)(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) {
    (void)iBackBuffer; (void)Type; if (ppBackBuffer) *ppBackBuffer = 0; return E_NOTIMPL;
  }
  STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS* pRasterStatus) {
    if (pRasterStatus) memset(pRasterStatus, 0, sizeof(*pRasterStatus)); return E_NOTIMPL;
  }
  STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* pMode) {
    if (pMode) *pMode = D3DDISPLAYMODE{}; return E_NOTIMPL;
  }
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (pPresentationParameters) memset(pPresentationParameters, 0, sizeof(*pPresentationParameters)); return E_NOTIMPL;
  }
};

struct IDirect3DQuery9 : IUnknown {
  STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) { if (ppDevice) *ppDevice = 0; return E_NOTIMPL; }
  STDMETHOD_(D3DQUERYTYPE, GetType)() { return 0; }
  STDMETHOD_(DWORD, GetDataSize)() { return 0; }
  STDMETHOD(Issue)(DWORD dwIssueFlags) { (void)dwIssueFlags; return E_NOTIMPL; }
  STDMETHOD(GetData)(void* pData, DWORD dwSize, DWORD dwGetDataFlags) {
    (void)pData; (void)dwSize; (void)dwGetDataFlags; return S_OK;
  }
};

struct IDirect3DDevice9 : IUnknown {
  STDMETHOD(TestCooperativeLevel)() { return D3D_OK; }
  STDMETHOD_(UINT, GetAvailableTextureMem)() { return 0; }
  STDMETHOD(EvictManagedResources)() { return D3D_OK; }
  STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9) { if (ppD3D9) *ppD3D9 = 0; return E_NOTIMPL; }
  STDMETHOD(GetDeviceCaps)(D3DCAPS9* pCaps) { if (pCaps) memset(pCaps, 0, sizeof(*pCaps)); return E_NOTIMPL; }
  STDMETHOD(GetDisplayMode)(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    (void)iSwapChain; if (pMode) *pMode = D3DDISPLAYMODE{}; return E_NOTIMPL;
  }
  STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* pParameters) {
    if (pParameters) memset(pParameters, 0, sizeof(*pParameters)); return E_NOTIMPL;
  }
  STDMETHOD(SetCursorProperties)(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) {
    (void)XHotSpot; (void)YHotSpot; (void)pCursorBitmap; return D3D_OK;
  }
  STDMETHOD_(void, SetCursorPosition)(int X, int Y, DWORD Flags) { (void)X; (void)Y; (void)Flags; }
  STDMETHOD_(BOOL, ShowCursor)(BOOL bShow) { (void)bShow; return FALSE; }
  STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                       IDirect3DSwapChain9** pSwapChain) {
    (void)pPresentationParameters; if (pSwapChain) *pSwapChain = 0; return E_NOTIMPL;
  }
  STDMETHOD(GetSwapChain)(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    (void)iSwapChain; if (pSwapChain) *pSwapChain = 0; return E_NOTIMPL;
  }
  STDMETHOD_(UINT, GetNumberOfSwapChains)() { return 1; }
  STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) { (void)pPresentationParameters; return D3D_OK; }
  STDMETHOD(Present)(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                     HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
    (void)pSourceRect; (void)pDestRect; (void)hDestWindowOverride; (void)pDirtyRegion; return D3D_OK;
  }
  STDMETHOD(GetBackBuffer)(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                           IDirect3DSurface9** ppBackBuffer) {
    (void)iSwapChain; (void)iBackBuffer; (void)Type; if (ppBackBuffer) *ppBackBuffer = 0; return E_NOTIMPL;
  }
  STDMETHOD(GetRasterStatus)(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
    (void)iSwapChain; if (pRasterStatus) memset(pRasterStatus, 0, sizeof(*pRasterStatus)); return E_NOTIMPL;
  }
  STDMETHOD(SetDialogBoxMode)(BOOL bEnableDialogs) { (void)bEnableDialogs; return D3D_OK; }
  STDMETHOD_(void, SetGammaRamp)(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) {
    (void)iSwapChain; (void)Flags; (void)pRamp;
  }
  STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
    (void)iSwapChain; if (pRamp) memset(pRamp, 0, sizeof(*pRamp));
  }
  STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                           D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
    (void)Width; (void)Height; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (ppTexture) *ppTexture = 0; return E_FAIL;
  }
  STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                 D3DFORMAT Format, D3DPOOL Pool,
                                 IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) {
    (void)Width; (void)Height; (void)Depth; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (ppVolumeTexture) *ppVolumeTexture = 0; return E_FAIL;
  }
  STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                               D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) {
    (void)EdgeLength; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (ppCubeTexture) *ppCubeTexture = 0; return E_FAIL;
  }
  STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) {
    (void)Length; (void)Usage; (void)FVF; (void)Pool; (void)pSharedHandle;
    if (ppVertexBuffer) *ppVertexBuffer = 0; return E_FAIL;
  }
  STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                               IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) {
    (void)Length; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (ppIndexBuffer) *ppIndexBuffer = 0; return E_FAIL;
  }
  STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality, BOOL Lockable,
                                IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
    (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)MultisampleQuality; (void)Lockable; (void)pSharedHandle;
    if (ppSurface) *ppSurface = 0; return E_FAIL;
  }
  STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                       DWORD MultisampleQuality, BOOL Discard,
                                       IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
    (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)MultisampleQuality; (void)Discard; (void)pSharedHandle;
    if (ppSurface) *ppSurface = 0; return E_FAIL;
  }
  STDMETHOD(UpdateSurface)(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                           IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) {
    (void)pSourceSurface; (void)pSourceRect; (void)pDestinationSurface; (void)pDestPoint; return E_NOTIMPL;
  }
  STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
    (void)pSourceTexture; (void)pDestinationTexture; return E_NOTIMPL;
  }
  STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) {
    (void)pRenderTarget; (void)pDestSurface; return E_NOTIMPL;
  }
  STDMETHOD(GetFrontBufferData)(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
    (void)iSwapChain; (void)pDestSurface; return E_NOTIMPL;
  }
  STDMETHOD(StretchRect)(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                         IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) {
    (void)pSourceSurface; (void)pSourceRect; (void)pDestSurface; (void)pDestRect; (void)Filter; return E_NOTIMPL;
  }
  STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) {
    (void)pSurface; (void)pRect; (void)color; return E_NOTIMPL;
  }
  STDMETHOD(CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                         IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
    (void)Width; (void)Height; (void)Format; (void)Pool; (void)pSharedHandle;
    if (ppSurface) *ppSurface = 0; return E_FAIL;
  }
  STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    (void)RenderTargetIndex; (void)pRenderTarget; return E_NOTIMPL;
  }
  STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
    (void)RenderTargetIndex; if (ppRenderTarget) *ppRenderTarget = 0; return E_NOTIMPL;
  }
  STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) { (void)pNewZStencil; return E_NOTIMPL; }
  STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface) {
    if (ppZStencilSurface) *ppZStencilSurface = 0; return E_NOTIMPL;
  }
  STDMETHOD(BeginScene)() { return D3D_OK; }
  STDMETHOD(EndScene)() { return D3D_OK; }
  STDMETHOD(Clear)(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
    (void)Count; (void)pRects; (void)Flags; (void)Color; (void)Z; (void)Stencil; return D3D_OK;
  }
  STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
    (void)State; (void)pMatrix; return D3D_OK;
  }
  STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    (void)State; if (pMatrix) memset(pMatrix, 0, sizeof(*pMatrix)); return D3D_OK;
  }
  STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
    (void)State; (void)pMatrix; return D3D_OK;
  }
  STDMETHOD(SetViewport)(CONST D3DVIEWPORT9* pViewport) { (void)pViewport; return D3D_OK; }
  STDMETHOD(GetViewport)(D3DVIEWPORT9* pViewport) {
    if (pViewport) memset(pViewport, 0, sizeof(*pViewport)); return D3D_OK;
  }
  STDMETHOD(SetMaterial)(CONST D3DMATERIAL9* pMaterial) { (void)pMaterial; return D3D_OK; }
  STDMETHOD(GetMaterial)(D3DMATERIAL9* pMaterial) {
    if (pMaterial) memset(pMaterial, 0, sizeof(*pMaterial)); return D3D_OK;
  }
  STDMETHOD(SetLight)(DWORD Index, CONST D3DLIGHT9* pLight) { (void)Index; (void)pLight; return D3D_OK; }
  STDMETHOD(GetLight)(DWORD Index, D3DLIGHT9* pLight) {
    (void)Index; if (pLight) memset(pLight, 0, sizeof(*pLight)); return D3D_OK;
  }
  STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) { (void)Index; (void)Enable; return D3D_OK; }
  STDMETHOD(GetLightEnable)(DWORD Index, BOOL* pEnable) {
    (void)Index; if (pEnable) *pEnable = FALSE; return D3D_OK;
  }
  STDMETHOD(SetClipPlane)(DWORD Index, CONST float* pPlane) { (void)Index; (void)pPlane; return D3D_OK; }
  STDMETHOD(GetClipPlane)(DWORD Index, float* pPlane) { (void)Index; (void)pPlane; return D3D_OK; }
  STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) { (void)State; (void)Value; return D3D_OK; }
  STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD* pValue) {
    (void)State; if (pValue) *pValue = 0; return D3D_OK;
  }
  long GetRenderState(D3DRENDERSTATETYPE State, unsigned long* pValue) {
    DWORD v = 0; long hr = GetRenderState(State, &v); if (pValue) *pValue = v; return hr;
  }
  STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
    (void)Type; if (ppSB) *ppSB = 0; return E_FAIL;
  }
  STDMETHOD(BeginStateBlock)() { return E_NOTIMPL; }
  STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** ppSB) { if (ppSB) *ppSB = 0; return E_FAIL; }
  STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS9* pClipStatus) { (void)pClipStatus; return D3D_OK; }
  STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9* pClipStatus) {
    if (pClipStatus) memset(pClipStatus, 0, sizeof(*pClipStatus)); return D3D_OK;
  }
  STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    (void)Stage; if (ppTexture) *ppTexture = 0; return D3D_OK;
  }
  STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture9* pTexture) { (void)Stage; (void)pTexture; return D3D_OK; }
  STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) {
    (void)Stage; (void)Type; if (pValue) *pValue = 0; return D3D_OK;
  }
  long GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, unsigned long* pValue) {
    DWORD v = 0; long hr = GetTextureStageState(Stage, Type, &v); if (pValue) *pValue = v; return hr;
  }
  STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    (void)Stage; (void)Type; (void)Value; return D3D_OK;
  }
  STDMETHOD(GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
    (void)Sampler; (void)Type; if (pValue) *pValue = 0; return D3D_OK;
  }
  long GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, unsigned long* pValue) {
    DWORD v = 0; long hr = GetSamplerState(Sampler, Type, &v); if (pValue) *pValue = v; return hr;
  }
  STDMETHOD(SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    (void)Sampler; (void)Type; (void)Value; return D3D_OK;
  }
  STDMETHOD(ValidateDevice)(DWORD* pNumPasses) { if (pNumPasses) *pNumPasses = 1; return D3D_OK; }
  STDMETHOD(SetPaletteEntries)(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) {
    (void)PaletteNumber; (void)pEntries; return D3D_OK;
  }
  STDMETHOD(GetPaletteEntries)(UINT PaletteNumber, PALETTEENTRY* pEntries) {
    (void)PaletteNumber; (void)pEntries; return D3D_OK;
  }
  STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber) { (void)PaletteNumber; return D3D_OK; }
  STDMETHOD(GetCurrentTexturePalette)(UINT* PaletteNumber) {
    if (PaletteNumber) *PaletteNumber = 0; return D3D_OK;
  }
  STDMETHOD(SetScissorRect)(CONST RECT* pRect) { (void)pRect; return D3D_OK; }
  STDMETHOD(GetScissorRect)(RECT* pRect) { if (pRect) memset(pRect, 0, sizeof(*pRect)); return D3D_OK; }
  STDMETHOD(SetSoftwareVertexProcessing)(BOOL bSoftware) { (void)bSoftware; return D3D_OK; }
  STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() { return FALSE; }
  STDMETHOD(SetNPatchMode)(float nSegments) { (void)nSegments; return D3D_OK; }
  STDMETHOD_(float, GetNPatchMode)() { return 0.0f; }
  STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    (void)PrimitiveType; (void)StartVertex; (void)PrimitiveCount; return D3D_OK;
  }
  STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                  UINT MinVertexIndex, UINT NumVertices,
                                  UINT startIndex, UINT primCount) {
    (void)PrimitiveType; (void)BaseVertexIndex; (void)MinVertexIndex; (void)NumVertices; (void)startIndex; (void)primCount;
    return D3D_OK;
  }
  STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                             CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    (void)PrimitiveType; (void)PrimitiveCount; (void)pVertexStreamZeroData; (void)VertexStreamZeroStride;
    return D3D_OK;
  }
  STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
                                    UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    (void)PrimitiveType; (void)MinVertexIndex; (void)NumVertices; (void)PrimitiveCount;
    (void)pIndexData; (void)IndexDataFormat; (void)pVertexStreamZeroData; (void)VertexStreamZeroStride;
    return D3D_OK;
  }
  STDMETHOD(ProcessVertices)(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                             IDirect3DVertexBuffer9* pDestBuffer,
                             IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) {
    (void)SrcStartIndex; (void)DestIndex; (void)VertexCount; (void)pDestBuffer; (void)pVertexDecl; (void)Flags;
    return E_NOTIMPL;
  }
  STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9* pVertexElements,
                                     IDirect3DVertexDeclaration9** ppDecl) {
    (void)pVertexElements; if (ppDecl) *ppDecl = 0; return E_FAIL;
  }
  STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl) { (void)pDecl; return D3D_OK; }
  STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl) {
    if (ppDecl) *ppDecl = 0; return D3D_OK;
  }
  STDMETHOD(SetFVF)(DWORD FVF) { (void)FVF; return D3D_OK; }
  STDMETHOD(GetFVF)(DWORD* pFVF) { if (pFVF) *pFVF = 0; return D3D_OK; }
  long GetFVF(unsigned long* pFVF) {
    DWORD v = 0; long hr = GetFVF(&v); if (pFVF) *pFVF = v; return hr;
  }
  STDMETHOD(CreateVertexShader)(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
    (void)pFunction; if (ppShader) *ppShader = 0; return E_FAIL;
  }
  long CreateVertexShader(const unsigned long* pFunction, IDirect3DVertexShader9** ppShader) {
    return CreateVertexShader((const DWORD*)pFunction, ppShader);
  }
  STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* pShader) { (void)pShader; return D3D_OK; }
  STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** ppShader) { if (ppShader) *ppShader = 0; return D3D_OK; }
  STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4fCount; return D3D_OK;
  }
  STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4fCount; return D3D_OK;
  }
  STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4iCount; return D3D_OK;
  }
  STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4iCount; return D3D_OK;
  }
  STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) {
    (void)StartRegister; (void)pConstantData; (void)BoolCount; return D3D_OK;
  }
  STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
    (void)StartRegister; (void)pConstantData; (void)BoolCount; return D3D_OK;
  }
  STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                             UINT OffsetInBytes, UINT Stride) {
    (void)StreamNumber; (void)pStreamData; (void)OffsetInBytes; (void)Stride; return D3D_OK;
  }
  STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
                             UINT* pOffsetInBytes, UINT* pStride) {
    (void)StreamNumber; if (ppStreamData) *ppStreamData = 0;
    if (pOffsetInBytes) *pOffsetInBytes = 0; if (pStride) *pStride = 0; return D3D_OK;
  }
  STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber, UINT Setting) { (void)StreamNumber; (void)Setting; return D3D_OK; }
  STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber, UINT* pSetting) {
    (void)StreamNumber; if (pSetting) *pSetting = 1; return D3D_OK;
  }
  STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* pIndexData) { (void)pIndexData; return D3D_OK; }
  STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** ppIndexData) { if (ppIndexData) *ppIndexData = 0; return D3D_OK; }
  STDMETHOD(CreatePixelShader)(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
    (void)pFunction; if (ppShader) *ppShader = 0; return E_FAIL;
  }
  long CreatePixelShader(const unsigned long* pFunction, IDirect3DPixelShader9** ppShader) {
    return CreatePixelShader((const DWORD*)pFunction, ppShader);
  }
  STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* pShader) { (void)pShader; return D3D_OK; }
  STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ppShader) { if (ppShader) *ppShader = 0; return D3D_OK; }
  STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4fCount; return D3D_OK;
  }
  STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4fCount; return D3D_OK;
  }
  STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4iCount; return D3D_OK;
  }
  STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
    (void)StartRegister; (void)pConstantData; (void)Vector4iCount; return D3D_OK;
  }
  STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) {
    (void)StartRegister; (void)pConstantData; (void)BoolCount; return D3D_OK;
  }
  STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
    (void)StartRegister; (void)pConstantData; (void)BoolCount; return D3D_OK;
  }
  STDMETHOD(DrawRectPatch)(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) {
    (void)Handle; (void)pNumSegs; (void)pRectPatchInfo; return E_NOTIMPL;
  }
  STDMETHOD(DrawTriPatch)(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) {
    (void)Handle; (void)pNumSegs; (void)pTriPatchInfo; return E_NOTIMPL;
  }
  STDMETHOD(DeletePatch)(UINT Handle) { (void)Handle; return E_NOTIMPL; }
  STDMETHOD(CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    (void)Type; if (ppQuery) *ppQuery = 0; return E_FAIL;
  }
};

/* Direct3D9 entry point: returns our WebGPU IDirect3D9 implementation in browser
 * builds (browser/gpu/d3d9_device_wgpu.cpp), or FakeD3D when g_bNoRender is set. */
IDirect3D9* Direct3DCreate9(unsigned int sdkVersion);
#endif  /* C++ only */
