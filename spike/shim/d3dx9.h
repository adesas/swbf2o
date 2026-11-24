#pragma once
/* <d3dx9.h> stand-in for the browser build.
 *
 * RedEngineFL/Graphics/PC includes <d3dx9.h> for two things only: the D3D9 device vocabulary
 * (supplied by d3d9.h) and the D3DX math + texture-helper entry points the engine calls by name.
 * The math lives in d3dx9math.h (a real implementation - the game uses D3DX as its linear-algebra
 * library); what is left here are the D3DX9 library entry points, which the browser build replaces
 * the same way it replaces the device: declarations only, behaviour in the backend.
 *
 * Signatures are Microsoft's (d3dx9tex.h), so a call written against the SDK type-checks here.
 */
#ifdef __cplusplus  /* this header models C++ COM/D3D APIs: never include it from C */
#ifndef SWBF2_D3DX9_H
#define SWBF2_D3DX9_H

#include "swbf2_prefix.h"
#include "d3d9.h"
#include "d3dx9math.h"

/* ---- D3DX9 texture helpers the engine calls -------------------------------- */
/* D3DXCheckTextureRequirements(pDevice, &width, &height, &mips, &usage, &format, pool): returns
 * D3D_OK when the format is supported, adjusting the requested size/mip count/format in place. */
long D3DXCheckTextureRequirements(IDirect3DDevice9* pDevice, unsigned int* pWidth,
                                  unsigned int* pHeight, unsigned int* pNumMipMaps,
                                  unsigned int* pUsage, D3DFORMAT* pFormat, D3DPOOL Pool);
long D3DXCheckCubeTextureRequirements(IDirect3DDevice9* pDevice, unsigned int* pWidth,
                                      unsigned int* pHeight, unsigned int* pNumMipMaps,
                                      unsigned int* pUsage, D3DFORMAT* pFormat, D3DPOOL Pool);
long D3DXCheckVolumeTextureRequirements(IDirect3DDevice9* pDevice, unsigned int* pWidth,
                                        unsigned int* pHeight, unsigned int* pDepth,
                                        unsigned int* pNumMipMaps, unsigned int* pUsage,
                                        D3DFORMAT* pFormat, D3DPOOL Pool);

#ifndef D3DX_FILTER_NONE
#define D3DX_FILTER_NONE 1u
#endif
long D3DXLoadSurfaceFromSurface(IDirect3DSurface9* pDestSurface, const PALETTEENTRY* pDestPalette,
                                const RECT* pDestRect, IDirect3DSurface9* pSrcSurface,
                                const PALETTEENTRY* pSrcPalette, const RECT* pSrcRect,
                                unsigned int Filter, D3DCOLOR ColorKey);

#endif  /* SWBF2_D3DX9_H */
#endif  /* C++ only */
