// Supporting definitions paired with RedEngineFL/Graphics/PC/pcRedRenderer.cpp.
//
// As of Phase 2, the retail engine's RedEngineFL/Graphics/PC/pcRedRenderer.cpp is compiled
// directly into build/core.a and build/native_core.a (773 / 773 TUs). When g_bNoRender is false,
// RedRenderer::InitStatic() calls Direct3DCreate9(D3D_SDK_VERSION), which creates our WebGPU
// IDirect3D9/IDirect3DDevice9 backend (browser/gpu/). When g_bNoRender is true (Node.js and
// native i386 oracles), pcRedRenderer.cpp uses its own FakeD3D / FakeD3DDevice automatically.

#include "RedEngineFL/Graphics/PC/pcRedRenderer.h"

#include <time.h>

double swbf2_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// Weak fallback so headless Node.js / native i386 oracle links resolve Direct3DCreate9 without
// pulling in emdawnwebgpu. Browser builds link browser/gpu/d3d9_device_wgpu.cpp which provides
// the strong WebGPU-backed Direct3DCreate9 implementation.
__attribute__((weak)) IDirect3D9* Direct3DCreate9(unsigned int sdkVersion)
{
    (void)sdkVersion;
    return RedRenderer::CreateFakeD3D();
}
