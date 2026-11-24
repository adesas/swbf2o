/* D3DX9 texture helpers backed by our D3D9-over-WebGPU device.
 *
 * RedTexture::pcChooseFormat() calls D3DXCheckTextureRequirements() for every candidate D3DFORMAT
 * in a .lvl texture's INFO chunk and requires that (*pFormat) remain unchanged on S_OK.
 */

#include <d3dx9.h>
#include <string.h>

static bool IsSupportedTextureFormat(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:
    case D3DFMT_L16:
    case D3DFMT_V8U8:
    case D3DFMT_X8V8U8:
    case D3DFMT_L6V5U5:
        return true;
    default:
        return false;
    }
}

long D3DXCheckTextureRequirements(IDirect3DDevice9* pDevice, unsigned int* pWidth,
                                  unsigned int* pHeight, unsigned int* pNumMipMaps,
                                  unsigned int* pUsage, D3DFORMAT* pFormat, D3DPOOL Pool)
{
    (void)pDevice;
    (void)pUsage;
    (void)Pool;
    if (pWidth && *pWidth == 0) *pWidth = 1;
    if (pHeight && *pHeight == 0) *pHeight = 1;
    if (pNumMipMaps && *pNumMipMaps == 0) *pNumMipMaps = 1;
    if (pFormat)
    {
        if (*pFormat == D3DFMT_UNKNOWN)
        {
            *pFormat = D3DFMT_A8R8G8B8;
        }
        else if (!IsSupportedTextureFormat(*pFormat))
        {
            *pFormat = D3DFMT_A8R8G8B8;
        }
    }
    return D3D_OK;
}

long D3DXLoadSurfaceFromSurface(IDirect3DSurface9* pDestSurface, const PALETTEENTRY* pDestPalette,
                                const RECT* pDestRect, IDirect3DSurface9* pSrcSurface,
                                const PALETTEENTRY* pSrcPalette, const RECT* pSrcRect,
                                unsigned int Filter, D3DCOLOR ColorKey)
{
    (void)pDestPalette;
    (void)pSrcPalette;
    (void)Filter;
    (void)ColorKey;
    if (!pDestSurface || !pSrcSurface)
    {
        return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC dstDesc = {};
    D3DSURFACE_DESC srcDesc = {};
    if (FAILED(pDestSurface->GetDesc(&dstDesc)) || FAILED(pSrcSurface->GetDesc(&srcDesc)))
    {
        return E_FAIL;
    }
    if (dstDesc.Format != srcDesc.Format || dstDesc.Width != srcDesc.Width || dstDesc.Height != srcDesc.Height)
    {
        return E_NOTIMPL;
    }
    D3DLOCKED_RECT dstLock = {};
    D3DLOCKED_RECT srcLock = {};
    if (FAILED(pSrcSurface->LockRect(&srcLock, pSrcRect, D3DLOCK_READONLY)))
    {
        return E_FAIL;
    }
    if (FAILED(pDestSurface->LockRect(&dstLock, pDestRect, 0)))
    {
        pSrcSurface->UnlockRect();
        return E_FAIL;
    }
    if (dstLock.pBits && srcLock.pBits && dstLock.Pitch > 0 && srcLock.Pitch > 0)
    {
        int copyPitch = dstLock.Pitch < srcLock.Pitch ? dstLock.Pitch : srcLock.Pitch;
        for (unsigned int y = 0; y < dstDesc.Height; ++y)
        {
            memcpy((unsigned char*)dstLock.pBits + y * dstLock.Pitch,
                   (const unsigned char*)srcLock.pBits + y * srcLock.Pitch,
                   (size_t)copyPitch);
        }
    }
    pDestSurface->UnlockRect();
    pSrcSurface->UnlockRect();
    return D3D_OK;
}
