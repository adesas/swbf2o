/* The browser's COM layer: there is no COM runtime on wasm32, and the engine has to be told so in
 * the shape it checks.
 *
 * What the retained tree asks of COM:
 *
 *   - CoInitialize / CoUninitialize around the DirectSound path (sndengine.cpp:936). A browser
 *     page is one thread with one apartment, so initialising an apartment is a no-op that
 *     succeeds.
 *   - CoCreateInstance for the DirectSound 8 class (sndengine.cpp:941) and for the DirectSound
 *     DMO effects (snddsdmo.cpp). There is no in-process server to instantiate in a browser, so
 *     the call fails with CO_E_CLASSSTRING / CLASS_E_NOAGGREGATION and the engine takes the path
 *     it already has for a missing class: the sound engine falls back to DirectSoundCreate8, and
 *     that has no device either, so the mix config drops to mixConfig_Disabled.
 *
 * The interface pointer is set to null on failure, which is what the engine's own error paths
 * expect: the first thing sndengine.cpp does with the result is test SUCCEEDED().
 */

#include <objbase.h>

// Shared with browser/main.cpp: the engine-facing log ring the page shows.
extern void Log(const char* fmt, ...);

long CoInitialize(void* pvReserved)
{
    // One thread, one apartment, no COM runtime to initialise. S_OK is the honest answer: the
    // apartment the caller asked for exists.
    (void)pvReserved;
    return S_OK;
}

void CoUninitialize()
{
}

long CoCreateInstance(const CLSID& rclsid, IUnknown* pUnkOuter, unsigned dwClsContext,
                       const IID& riid, void** ppv)
{
    (void)rclsid;
    (void)pUnkOuter;
    (void)dwClsContext;
    (void)riid;
    if (ppv)
    {
        *ppv = 0;
    }
    Log("com: CoCreateInstance asked for an in-process server and there is none; "
        "the caller falls back (said once)");
    return CLASS_E_NOAGGREGATION;
}
