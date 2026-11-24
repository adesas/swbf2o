#pragma once
/* objbase.h stand-in for the browser build.
 *
 * The COM runtime does not exist on wasm32-unknown-emscripten. What the retained tree still asks
 * of it is: apartment init/uninit around the DirectSound path, CoCreateInstance for the DMO
 * effects the WebAudio backend replaces, task-memory allocation, and the interface identifiers
 * those calls pass around. This header declares that vocabulary (spellings and values follow
 * Microsoft's objbase.h / ole2.h); the behaviour belongs to the audio backend.
 *
 * GUID / IID / REFIID / HRESULT / S_OK and the E-code constants come from swbf2_prefix.h, which is
 * force-included into every translation unit anyway - this header must not restate them.
 */
#include "swbf2_prefix.h"
#ifndef SWBF2_OBJBASE_H
#define SWBF2_OBJBASE_H

#ifdef __cplusplus

/* ---- CLSCTX_ / COINIT_ (Microsoft's ole2.h values) -------------------------
 * Only the spellings the retained code names: CLSCTX_INPROC_SERVER (sndengine.cpp:941) and
 * CLSCTX_INPROC (snddsdmo.cpp:82), which the old DirectSound/EAX code uses as "in-proc, handler or
 * server". */
#ifndef CLSCTX_INPROC_SERVER
#define CLSCTX_INPROC_SERVER  0x1
#define CLSCTX_INPROC_HANDLER 0x2
#define CLSCTX_INPROC         (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)
#endif

typedef GUID CLSID;

/* Apartment plumbing: a browser process is single-threaded, so CoInitialize is a backend no-op; the
 * DMO object CoCreateInstance would build is replaced by the WebAudio effect chain. */
long  CoInitialize(void* pvReserved);
void  CoUninitialize(void);
long  CoCreateInstance(const CLSID& rclsid, IUnknown* pUnkOuter, unsigned dwClsContext,
                       const IID& riid, void** ppv);

/* Interface identifiers the tree names by value. Microsoft declares them in its headers and
 * defines them once in uuid.lib / the *_i.c stubs; here they stay declarations until the code that
 * owns the COM boundary is linked in. Nothing in this spike links them yet
 * (see docs/platform-shim-notes.md). IID_IUnknown and IID_IKsPropertySet are declared in
 * swbf2_prefix.h, which is where Windows declares them (unknwn.h / ksproxy.h). */
/* IID_IMediaObject / IID_IMediaObjectInPlace / IID_IMediaBuffer are declared in mediaobj.h, next
 * to the interfaces, which is where Windows declares them too. */

#endif  /* __cplusplus */
#endif  /* SWBF2_OBJBASE_H */
