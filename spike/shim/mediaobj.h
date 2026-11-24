#pragma once
/* This header models C++ COM/D3D APIs: never include it from C. */
#ifdef __cplusplus  /* C++ only */
#include "swbf2_prefix.h"
typedef long long REFERENCE_TIME;
/* dmo.h layouts, field for field (MediaType in SoundFL/source/win/snddsdmo.h derives from
 * DMO_MEDIA_TYPE and memsets it by sizeof, so the field list has to be the real one - this shim was
 * missing cbFormat, and DMO_OUTPUT_DATA_BUFFER's fourth field, which is what
 * SoundFL/source/win/snddsdmo.cpp:35 initialises). */
struct DMO_MEDIA_TYPE {
  GUID majortype, subtype; int bFixedSizeSamples, bTemporalCompression;
  unsigned long lSampleSize; GUID formattype; IUnknown* pUnk;
  unsigned long cbFormat; void* pbFormat;
};
struct IMediaBuffer;  /* forward declaration: dmo.h's buffer arrays point at one */
struct DMO_OUTPUT_DATA_BUFFER {
  IMediaBuffer* pBuffer; unsigned long dwStatus;
  REFERENCE_TIME rtTimestamp, rtTimelength;
};
struct IMediaBuffer;
struct IMediaBuffer { STDMETHOD_(unsigned long, AddRef)() { return 1; } STDMETHOD_(unsigned long, Release)() { return 1; } STDMETHOD(QueryInterface)(REFIID, void**) { return 0; } STDMETHOD(SetLength)(unsigned long) { return 0; } STDMETHOD(GetMaxLength)(unsigned long* p) { if (p) *p = 0; return 0; } };
struct IMediaEvent { STDMETHOD_(unsigned long, AddRef)() { return 1; } STDMETHOD_(unsigned long, Release)() { return 1; } STDMETHOD(QueryInterface)(REFIID, void**) { return 0; } };
struct IMediaEventEx : IMediaEvent {};
struct IMediaSeeking { STDMETHOD_(unsigned long, AddRef)() { return 1; } STDMETHOD_(unsigned long, Release)() { return 1; } STDMETHOD(QueryInterface)(REFIID, void**) { return 0; } };
struct IMediaControl { STDMETHOD_(unsigned long, AddRef)() { return 1; } STDMETHOD_(unsigned long, Release)() { return 1; } STDMETHOD(QueryInterface)(REFIID, void**) { return 0; } };
struct IMediaVideoAcceleration : IMediaBuffer {};
struct IMediaStream { STDMETHOD_(unsigned long, AddRef)() { return 1; } STDMETHOD_(unsigned long, Release)() { return 1; } STDMETHOD(QueryInterface)(REFIID, void**) { return 0; } };
struct IAMStreamSelect : IMediaSeeking {};

/* Media Object interfaces (dmo.h). The WebAudio backend replaces the DMO effect pipeline, so the
 * member set is the permissive no-op form used by generated_types.h: any call the retained code
 * makes type-checks, and tools/shim_autogen.py keeps whatever it finds here (it never overwrites a
 * name a shim header declares). */
struct IMediaObject : IUnknown {
  template <class... _A> long GetDMOs(_A&&...) { return 0; }
  template <class... _A> long GetInputStreamCount(_A&&...) { return 0; }
  template <class... _A> long GetOutputStreamCount(_A&&...) { return 0; }
  template <class... _A> long GetInputStreamType(_A&&...) { return 0; }
  template <class... _A> long GetOutputStreamType(_A&&...) { return 0; }
  template <class... _A> long SetInputType(_A&&...) { return 0; }
  template <class... _A> long SetOutputType(_A&&...) { return 0; }
  template <class... _A> long InputTypeChanged(_A&&...) { return 0; }
  template <class... _A> long EnumInputTypes(_A&&...) { return 0; }
  template <class... _A> long EnumOutputTypes(_A&&...) { return 0; }
  template <class... _A> long GetInputSizeInfo(_A&&...) { return 0; }
  template <class... _A> long GetInputBuffering(_A&&...) { return 0; }
  template <class... _A> long ProcessInput(_A&&...) { return 0; }
  template <class... _A> long ProcessOutput(_A&&...) { return 0; }
  template <class... _A> long Flush(_A&&...) { return 0; }
  template <class... _A> long AllocateStreamingResources(_A&&...) { return 0; }
  template <class... _A> long FreeStreamingResources(_A&&...) { return 0; }
  template <class... _A> long GetInputPRMPacket(_A&&...) { return 0; }
  template <class... _A> long GetOutputLatency(_A&&...) { return 0; }
  template <class... _A> long SetOutputDuration(_A&&...) { return 0; }
  template <class... _A> long GetOutputDurationByDate(_A&&...) { return 0; }
  template <class... _A> long GetInputStreamInfo(_A&&...) { return 0; }
  template <class... _A> long GetOutputStreamInfo(_A&&...) { return 0; }
  template <class... _A> long GetAttributes(_A&&...) { return 0; }
  template <class... _A> long QueryAttributeSupport(_A&&...) { return 0; }
  template <class... _A> long QueryAttribute(_A&&...) { return 0; }
  template <class... _A> long SetAttribute(_A&&...) { return 0; }
};
struct IMediaObjectInPlace : IUnknown {
  template <class... _A> long Process(_A&&...) { return 0; }
  template <class... _A> long Reset(_A&&...) { return 0; }
  template <class... _A> long GetLatency(_A&&...) { return 0; }
};

/* dmo.h: IMediaObject::SetInputType / SetOutputType flags. The tree only tests the query-only one
 * (SoundFL/source/win/snddsdmo.cpp), so only that one is here. */
#ifndef DMO_SET_TYPEF_TEST_ONLY
#define DMO_SET_TYPEF_TEST_ONLY 0x00000200
#endif

/* dmo.h owns these interface identifiers; Windows declares them next to the interfaces. */
extern const GUID IID_IMediaObject;
extern const GUID IID_IMediaObjectInPlace;
extern const GUID IID_IMediaBuffer;

#endif  /* C++ only */
