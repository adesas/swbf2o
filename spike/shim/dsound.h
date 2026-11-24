#pragma once
/* DirectSound types. The mixer/output path is replaced by WebAudio, so these only need to
 * declare; behaviour lives in the audio backend. */
/* This header models C++ COM/D3D APIs: never include it from C. */
#ifdef __cplusplus  /* C++ only */
#include "mmreg.h"

#ifndef DS_OK
#define DS_OK 0
/* Microsoft's own values (dsound.h): the four E_-coded ones are COM severities from the prefix, and
 * DSERR_BUFFERLOST is MAKE_DSHRESULT(150), i.e. MAKE_HRESULT(1, _FACDS, 150) with _FACDS 0x878. */
#define DSERR_GENERIC E_FAIL
#define DSERR_UNSUPPORTED E_NOTIMPL
#define DSERR_BADCALL (-3)
#define DSERR_BUFFERLOST ((long)0x88780096L)
#define DSERR_INVALIDPARAM E_INVALIDARG
#endif
#ifndef DSCAPS_CONTINUOUSRATE
#define DSCAPS_CONTINUOUSRATE 0x00000010
#endif
#ifndef DSBCAPS_CTRLVOLUME
#define DSBCAPS_CTRLVOLUME 0x00000080
#endif
#ifndef DSBCAPS_CTRL3D
#define DSBCAPS_CTRL3D 0x00000010
#endif
#ifndef DSBCAPS_GLOBALFOCUS
#define DSBCAPS_GLOBALFOCUS 0x00008000
#endif
#ifndef DSBCAPS_GETCURRENTPOSITION2
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#endif
#ifndef DSBPLAY_LOOPING
#define DSBPLAY_LOOPING 1
#endif
#ifndef DS3DMODE_NORMAL
#define DS3DMODE_NORMAL 0
#endif
#ifndef DS3DMODE_HEADRELATIVE
#define DS3DMODE_HEADRELATIVE 1
#endif
#ifndef DS3DMODE_DISABLE
#define DS3DMODE_DISABLE 2
#endif
#ifndef DSBSTATUS_BUFFERLOST
#define DSBSTATUS_BUFFERLOST 2
#endif

struct DSCAPS {
  unsigned int dwSize, dwFlags;
  unsigned int dwMinSecondarySampleRate, dwMaxSecondarySampleRate;
  unsigned int dwPrimaryBuffers;
  unsigned int dwMaxHwMixingAllBuffers, dwMaxHwMixingStaticBuffers, dwMaxHwMixingStreamingBuffers;
  unsigned int dwFreeHwMixingAllBuffers, dwFreeHwMixingStaticBuffers, dwFreeHwMixingStreamingBuffers;
  unsigned int dwMaxHw3DAllBuffers, dwMaxHw3DStaticBuffers, dwMaxHw3DStreamingBuffers;
  unsigned int dwFreeHw3DAllBuffers, dwFreeHw3DStaticBuffers, dwFreeHw3DStreamingBuffers;
  unsigned int dwTotalHwMemBytes, dwFreeHwMemBytes, dwMaxContigFreeHwMemBytes;
  unsigned int dwUnlockTransferRateHwBuffers, dwPlayCpuOverheadSwBuffers, dwReserved1, dwReserved2;
  /* legacy shim spellings (not Microsoft's): kept so nothing that used them silently breaks */
  unsigned int dwSecondaryBuffers, dwCacheBufferSizes;
};
struct DSBUFFERDESC { unsigned int dwSize, dwFlags, dwBufferBytes, dwReserved; LPWAVEFORMATEX lpwfxFormat;
                     GUID guid3DAlgorithm; };
struct DSBPOSITIONNOTIFY { unsigned int dwOffset; void* hEventNotify; };
struct DS3DBUFFER {
  unsigned int dwSize;
  struct { float x, y, z; } vPosition, vVelocity, vInsideConePosition, vOutsideConePosition;
  float fInsideConeAngle, fOutsideConeAngle, fDistanceFactor, fRolloffFactor, fDopplerFactor;
  int   dwMode;
  float flMinDistance, flMaxDistance;
  /* Microsoft's real DS3DBUFFER spellings (dsound.h): dwSize, vPosition, vVelocity,
   * dwInsideConeAngle, dwOutsideConeAngle, vConeOrientation, lConeOutsideVolume,
   * flMinDistance, flMaxDistance, dwMode. The win/ backends use these. */
  unsigned int dwInsideConeAngle, dwOutsideConeAngle;
  struct { float x, y, z; } vConeOrientation;
  long lConeOutsideVolume;
};
struct DS3DLISTENER {
  struct { float x, y, z; } vPosition, vVelocity, vInsideConePosition, vOutsideConePosition;
  float fInsideConeAngle, fOutsideConeAngle, fDistanceFactor, fRolloffFactor, fDopplerFactor;
};
struct DSBVOLUME { long lVolume; };

#ifdef __cplusplus
/* Forward declarations; full virtual COM definitions follow the descriptor structs below. */
struct IDirectSound; struct IDirectSound8; struct IDirectSoundBuffer; struct IDirectSoundBuffer8;
struct IDirectSoundCapture; struct IDirectSoundCapture8; struct IDirectSoundCaptureBuffer;
struct IDirectSoundCaptureBuffer8; struct IDirectSound3DListener; struct IDirectSound3DBuffer;
struct IDirectSoundFXI3DL2Reverb8; struct IDirectSoundNotify;
struct DSFXI3DL2Reverb;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;
typedef const DSBUFFERDESC* LPCDSBUFFERDESC;
typedef const DS3DBUFFER* LPCDS3DBUFFER;
typedef const DS3DLISTENER* LPCDS3DLISTENER;
typedef const DSFXI3DL2Reverb* LPCDSFXI3DL2Reverb;
#else
struct IDirectSound; struct IDirectSound8; struct IDirectSoundBuffer; struct IDirectSoundBuffer8;
struct IDirectSoundCapture; struct IDirectSoundCapture8; struct IDirectSoundCaptureBuffer;
struct IDirectSoundCaptureBuffer8; struct IDirectSound3DListener; struct IDirectSound3DBuffer;
struct IDirectSoundFXI3DL2Reverb8;
#endif

/* ---- DirectSound 8 descriptor structs the win/ backends fill in ------------
 * Field names and order follow Microsoft's dsound.h so the WebAudio backend can consume exactly
 * what the game produces. */
struct DSBCAPS       { unsigned int dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead; };
struct DSCEFFECTDESC { unsigned int dwSize, dwFlags; GUID guidDSCFXClass, guidDSCFXInstance;
                       unsigned int dwReserved1, dwReserved2; };
struct DSCBUFFERDESC1 { unsigned int dwSize, dwFlags, dwBufferBytes, dwReserved; LPWAVEFORMATEX lpwfxFormat; };
struct DSCBUFFERDESC {
  unsigned int dwSize, dwFlags, dwBufferBytes, dwReserved;
  LPWAVEFORMATEX lpwfxFormat; unsigned int dwFXCount; DSCEFFECTDESC* lpDSCFXDesc;
};
struct DSFXI3DL2Reverb {
  long   lRoom, lRoomHF;
  float  flRoomRolloffFactor, flDecayTime, flDecayHFRatio;
  long   lReflections;
  float  flReflectionsDelay;
  long   lReverb;
  float  flReverbDelay, flDiffusion, flDensity, flHFReference;
};
typedef DSFXI3DL2Reverb DSFX_I3DL2REVERB;

/* DirectSound's own HRESULT facility (dsound.h: _FACDS 0x878) */
#ifndef _FACDS
#define _FACDS 0x878
#endif
#ifndef MAKE_DSHRESULT
#define MAKE_DSHRESULT(code) MAKE_HRESULT(1, _FACDS, (code))
#endif
#ifndef DSERR_ACCESSDENIED
#define DSERR_ACCESSDENIED              E_ACCESSDENIED
#define DSERR_ALLOCATED                 MAKE_DSHRESULT(10)
#define DSERR_ALREADYINITIALIZED        MAKE_DSHRESULT(130)
#define DSERR_BADFORMAT                 MAKE_DSHRESULT(100)
#define DSERR_BADSENDBUFFERGUID         MAKE_DSHRESULT(210)
#define DSERR_BUFFERTOOSMALL            MAKE_DSHRESULT(180)
#define DSERR_CONTROLUNAVAIL            MAKE_DSHRESULT(30)
#define DSERR_DS8_REQUIRED              MAKE_DSHRESULT(190)
#define DSERR_FXUNAVAILABLE             MAKE_DSHRESULT(220)
#define DSERR_INVALIDCALL               MAKE_DSHRESULT(50)
#define DSERR_NOAGGREGATION             CLASS_E_NOAGGREGATION
#define DSERR_NODRIVER                  MAKE_DSHRESULT(120)
#define DSERR_NOINTERFACE               E_NOINTERFACE
#define DSERR_OBJECTNOTFOUND            MAKE_DSHRESULT(4449)
#define DSERR_OTHERAPPHASPRIO           MAKE_DSHRESULT(160)
#define DSERR_OUTOFMEMORY               E_OUTOFMEMORY
#define DSERR_PRIOLEVELNEEDED           MAKE_DSHRESULT(70)
#define DSERR_SENDLOOP                  MAKE_DSHRESULT(200)
#define DSERR_UNINITIALIZED             MAKE_DSHRESULT(170)
#endif

/* Speaker position bits (WAVEFORMATEXTENSIBLE::dwChannelMask) - audioclient.h */
#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT              0x00000001
#define SPEAKER_FRONT_RIGHT             0x00000002
#define SPEAKER_FRONT_CENTER            0x00000004
#define SPEAKER_LOW_FREQUENCY           0x00000008
#define SPEAKER_BACK_LEFT               0x00000010
#define SPEAKER_BACK_RIGHT              0x00000020
#define SPEAKER_FRONT_LEFT_OF_CENTER    0x00000040
#define SPEAKER_FRONT_RIGHT_OF_CENTER   0x00000080
#endif

/* The DirectSound object identifiers the game asks the (replaced) enumeration APIs for.
 * Defined once, with Microsoft's GUID bytes, in spike/shim/guids.cpp. */
extern const GUID IID_IDirectSound8;
extern const GUID IID_IDirectSound3DListener;
extern const GUID IID_IDirectSound3DBuffer;
extern const GUID IID_IDirectSoundFXI3DL2Reverb8;
extern const GUID DSDEVID_DefaultPlayback;
extern const GUID DSDEVID_DefaultVoicePlayback;
extern const GUID DS3DALG_DEFAULT;
extern const GUID GUID_DSFX_STANDARD_GARGLE;
extern const GUID GUID_DSFX_STANDARD_CHORUS;
extern const GUID GUID_DSFX_STANDARD_FLANGER;
extern const GUID GUID_DSFX_STANDARD_ECHO;
extern const GUID GUID_DSFX_STANDARD_DISTORTION;
extern const GUID GUID_DSFX_STANDARD_COMPRESSOR;
extern const GUID GUID_DSFX_STANDARD_PARAMEQ;
extern const GUID GUID_DSFX_STANDARD_I3DL2REVERB;
extern const GUID GUID_DSFX_WAVES_REVERB;

/* DirectSound's creation/enumeration entry points: declarations only, the WebAudio backend
 * supplies the replacements (same convention as DirectInput8Create in dinput.h). */
typedef int (DSDENUMCALLBACK)(GUID* pguidDevice, const char* szName, const char* szDesc, void* ref);
long DirectSoundCreate(const GUID* pcGuidDevice, IDirectSound** ppDS, IUnknown* pUnkOuter);
long DirectSoundCreate8(const GUID* pcGuidDevice, IDirectSound8** ppDS8, IUnknown* pUnkOuter);
long DirectSoundEnumerateA(DSDENUMCALLBACK* pCallback, void* ref);
long DirectSoundCaptureCreate8(const GUID* pcGuidDevice, IDirectSoundCapture8** ppDSC8, IUnknown* pUnkOuter);
long DirectSoundCaptureEnumerateA(DSDENUMCALLBACK* pCallback, void* ref);
long GetDeviceID(const GUID* pcGuidDevice, GUID* pcGuidDeviceID);


/* ---- DirectSound 8 vocabulary the win/ backends spell directly -----------
 * Values are Microsoft's (dsound.h as carried by Wine): the WebAudio backend reads the same
 * names, so a flag the game sets means the same thing on both sides of the boundary.
 * tools/shim_autogen.py never regenerates a name that is defined here.
 */

/* buffer capability bits (DSBUFFERDESC::dwFlags) */
#define DSBCAPS_CTRLFREQUENCY                  0x00000020
#define DSBCAPS_CTRLPAN                        0x00000040
#define DSBCAPS_LOCDEFER                       0x00040000
#define DSBCAPS_LOCHARDWARE                    0x00000004
#define DSBCAPS_LOCSOFTWARE                    0x00000008
#define DSBCAPS_PRIMARYBUFFER                  0x00000001

/* play flags (IDirectSoundBuffer::Play) */
#define DSBPLAY_LOCHARDWARE                    0x00000002
#define DSBPLAY_LOCSOFTWARE                    0x00000004
#define DSBPLAY_TERMINATEBY_PRIORITY           0x000000020
#define DSBPLAY_TERMINATEBY_TIME               0x00000008

/* buffer status bits (GetStatus) */
#define DSBSTATUS_LOCHARDWARE                  0x00000008
#define DSBSTATUS_LOCSOFTWARE                  0x00000010
#define DSBSTATUS_LOOPING                      0x00000004
#define DSBSTATUS_PLAYING                      0x00000001
#define DSBSTATUS_TERMINATED                   0x00000020

/* capture buffer flags / start flags */
#define DSCBCAPS_WAVEMAPPED                    0x80000000
#define DSCBSTART_LOOPING                      0x00000001

/* device capability bits (GetCaps) */
#define DSCAPS_PRIMARYMONO                     0x00000001
#define DSCAPS_PRIMARYSTEREO                   0x00000002
#define DSCAPS_SECONDARY16BIT                  0x00000800
#define DSCAPS_SECONDARY8BIT                   0x00000400
#define DSCAPS_SECONDARYMONO                   0x00000100
#define DSCAPS_SECONDARYSTEREO                 0x00000200

/* cooperative levels / speaker configuration */
#define DSSCL_EXCLUSIVE                        3
#define DSSCL_NORMAL                           1
#define DSSPEAKER_5POINT1                      6
#define DSSPEAKER_7POINT1                      7
#define DSSPEAKER_HEADPHONE                    1
#define DSSPEAKER_MONO                         2
#define DSSPEAKER_QUAD                         3
#define DSSPEAKER_STEREO                       4
#define DSSPEAKER_SURROUND                     5

/* deferred parameter setting */
#define DS3D_DEFERRED                          0x00000001
#define DS3D_IMMEDIATE                         0x00000000

/* volume range */
#define DSBVOLUME_MAX                          0
#define DSBVOLUME_MIN                          -10000

/* I3DL2 reverb parameter ranges (SetAllParameters validation) */
#define DSFX_I3DL2REVERB_DECAYHFRATIO_MAX      2.0f
#define DSFX_I3DL2REVERB_DECAYHFRATIO_MIN      0.1f
#define DSFX_I3DL2REVERB_DECAYTIME_MAX         20.0f
#define DSFX_I3DL2REVERB_DECAYTIME_MIN         0.1f
#define DSFX_I3DL2REVERB_DENSITY_MAX           100.0f
#define DSFX_I3DL2REVERB_DENSITY_MIN           0.0f
#define DSFX_I3DL2REVERB_DIFFUSION_MAX         100.0f
#define DSFX_I3DL2REVERB_DIFFUSION_MIN         0.0f
#define DSFX_I3DL2REVERB_HFREFERENCE_MAX       20000.0f
#define DSFX_I3DL2REVERB_HFREFERENCE_MIN       20.0f
#define DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN  0.0f
#define DSFX_I3DL2REVERB_REFLECTIONS_MAX       1000
#define DSFX_I3DL2REVERB_REFLECTIONS_MIN       (-10000)
#define DSFX_I3DL2REVERB_REVERBDELAY_MAX       0.1f
#define DSFX_I3DL2REVERB_REVERBDELAY_MIN       0.0f
#define DSFX_I3DL2REVERB_REVERB_MAX            2000
#define DSFX_I3DL2REVERB_REVERB_MIN            (-10000)
#define DSFX_I3DL2REVERB_ROOMHF_MAX            0
#define DSFX_I3DL2REVERB_ROOMHF_MIN            (-10000)
#define DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX 10.0f
#define DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN 0.0f
#define DSFX_I3DL2REVERB_ROOM_MAX              0
#define DSFX_I3DL2REVERB_ROOM_MIN              (-10000)

/* ---- Real virtual COM DirectSound 8 interfaces ---------------------------------------------
 * Replaces generated_types.h's non-virtual template placeholders so SoundFL's calls through
 * IDirectSound8*, IDirectSoundBuffer8*, IDirectSound3DListener*, and IDirectSound3DBuffer*
 * dispatch cleanly to browser/platform/dsound_browser.cpp. */
struct IDirectSoundBuffer : IUnknown {
  virtual long __stdcall GetCaps(DSBCAPS* pDSBufferCaps) { (void)pDSBufferCaps; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetCurrentPosition(unsigned long* pdwCurrentPlayCursor, unsigned long* pdwCurrentWriteCursor) { (void)pdwCurrentPlayCursor; (void)pdwCurrentWriteCursor; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetFormat(WAVEFORMATEX* pwfxFormat, unsigned long dwSizeAllocated, unsigned long* pdwSizeWritten) { (void)pwfxFormat; (void)dwSizeAllocated; (void)pdwSizeWritten; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetVolume(long* plVolume) { (void)plVolume; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetPan(long* plPan) { (void)plPan; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetFrequency(unsigned long* pdwFrequency) { (void)pdwFrequency; return DSERR_UNSUPPORTED; }
  virtual long __stdcall GetStatus(unsigned long* pdwStatus) { (void)pdwStatus; return DSERR_UNSUPPORTED; }
  virtual long __stdcall Initialize(IDirectSound* pDirectSound, LPCDSBUFFERDESC pcDSBufferDesc) { (void)pDirectSound; (void)pcDSBufferDesc; return DS_OK; }
  virtual long __stdcall Lock(unsigned long dwOffset, unsigned long dwBytes, void** ppvAudioPtr1, unsigned long* pdwAudioBytes1, void** ppvAudioPtr2, unsigned long* pdwAudioBytes2, unsigned long dwFlags) {
    (void)dwOffset; (void)dwBytes; (void)ppvAudioPtr1; (void)pdwAudioBytes1; (void)ppvAudioPtr2; (void)pdwAudioBytes2; (void)dwFlags; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall Play(unsigned long dwReserved1, unsigned long dwPriority, unsigned long dwFlags) { (void)dwReserved1; (void)dwPriority; (void)dwFlags; return DSERR_UNSUPPORTED; }
  virtual long __stdcall SetCurrentPosition(unsigned long dwNewPosition) { (void)dwNewPosition; return DSERR_UNSUPPORTED; }
  virtual long __stdcall SetFormat(LPCWAVEFORMATEX pcfxFormat) { (void)pcfxFormat; return DSERR_UNSUPPORTED; }
  virtual long __stdcall SetVolume(long lVolume) { (void)lVolume; return DSERR_UNSUPPORTED; }
  virtual long __stdcall SetPan(long lPan) { (void)lPan; return DSERR_UNSUPPORTED; }
  virtual long __stdcall SetFrequency(unsigned long dwFrequency) { (void)dwFrequency; return DSERR_UNSUPPORTED; }
  virtual long __stdcall Stop() { return DSERR_UNSUPPORTED; }
  virtual long __stdcall Unlock(void* pvAudioPtr1, unsigned long dwAudioBytes1, void* pvAudioPtr2, unsigned long dwAudioBytes2) {
    (void)pvAudioPtr1; (void)dwAudioBytes1; (void)pvAudioPtr2; (void)dwAudioBytes2; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall Restore() { return DS_OK; }
};

struct IDirectSoundBuffer8 : IDirectSoundBuffer {
  virtual long __stdcall AcquireResources(unsigned long dwFlags, unsigned long dwEffectsCount, unsigned long* pdwResultCodes) {
    (void)dwFlags; (void)dwEffectsCount; (void)pdwResultCodes; return DS_OK;
  }
};

struct IDirectSound : IUnknown {
  virtual long __stdcall CreateSoundBuffer(LPCDSBUFFERDESC pcDSBufferDesc, IDirectSoundBuffer** ppDSBuffer, IUnknown* pUnkOuter) {
    (void)pcDSBufferDesc; (void)ppDSBuffer; (void)pUnkOuter; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall GetCaps(DSCAPS* pDSCaps) { (void)pDSCaps; return DSERR_UNSUPPORTED; }
  virtual long __stdcall DuplicateSoundBuffer(IDirectSoundBuffer* pDSBufferOriginal, IDirectSoundBuffer** ppDSBufferDuplicate) {
    (void)pDSBufferOriginal; (void)ppDSBufferDuplicate; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall SetCooperativeLevel(HWND hwnd, unsigned long dwLevel) { (void)hwnd; (void)dwLevel; return DS_OK; }
  virtual long __stdcall Compact() { return DS_OK; }
  virtual long __stdcall GetSpeakerConfig(unsigned long* pdwSpeakerConfig) {
    if (pdwSpeakerConfig) *pdwSpeakerConfig = DSSPEAKER_STEREO; return DS_OK;
  }
  virtual long __stdcall SetSpeakerConfig(unsigned long dwSpeakerConfig) { (void)dwSpeakerConfig; return DS_OK; }
  virtual long __stdcall Initialize(const GUID* pcGuidDevice) { (void)pcGuidDevice; return DS_OK; }
};

struct IDirectSound8 : IDirectSound {
  virtual long __stdcall VerifyCertification(unsigned long* pdwCertified) {
    if (pdwCertified) *pdwCertified = 0; return DS_OK;
  }
};

struct IDirectSound3DListener : IUnknown {
  virtual long __stdcall GetAllParameters(DS3DLISTENER* pListener) { (void)pListener; return DS_OK; }
  virtual long __stdcall GetDistanceFactor(float* pflDistanceFactor) { if (pflDistanceFactor) *pflDistanceFactor = 1.0f; return DS_OK; }
  virtual long __stdcall GetDopplerFactor(float* pflDopplerFactor) { if (pflDopplerFactor) *pflDopplerFactor = 1.0f; return DS_OK; }
  virtual long __stdcall GetRolloffFactor(float* pflRolloffFactor) { if (pflRolloffFactor) *pflRolloffFactor = 1.0f; return DS_OK; }
  virtual long __stdcall SetAllParameters(LPCDS3DLISTENER pcListener, unsigned long dwApply) { (void)pcListener; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetDistanceFactor(float flDistanceFactor, unsigned long dwApply) { (void)flDistanceFactor; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetDopplerFactor(float flDopplerFactor, unsigned long dwApply) { (void)flDopplerFactor; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetOrientation(float xFront, float yFront, float zFront, float xTop, float yTop, float zTop, unsigned long dwApply) {
    (void)xFront; (void)yFront; (void)zFront; (void)xTop; (void)yTop; (void)zTop; (void)dwApply; return DS_OK;
  }
  virtual long __stdcall SetPosition(float x, float y, float z, unsigned long dwApply) { (void)x; (void)y; (void)z; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetRolloffFactor(float flRolloffFactor, unsigned long dwApply) { (void)flRolloffFactor; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetVelocity(float x, float y, float z, unsigned long dwApply) { (void)x; (void)y; (void)z; (void)dwApply; return DS_OK; }
  virtual long __stdcall CommitDeferredSettings() { return DS_OK; }
};

struct IDirectSound3DBuffer : IUnknown {
  virtual long __stdcall SetAllParameters(LPCDS3DBUFFER pcDs3dBuffer, unsigned long dwApply) { (void)pcDs3dBuffer; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetConeAngles(unsigned long dwInsideConeAngle, unsigned long dwOutsideConeAngle, unsigned long dwApply) {
    (void)dwInsideConeAngle; (void)dwOutsideConeAngle; (void)dwApply; return DS_OK;
  }
  virtual long __stdcall SetConeOrientation(float x, float y, float z, unsigned long dwApply) { (void)x; (void)y; (void)z; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetConeOutsideVolume(long lConeOutsideVolume, unsigned long dwApply) { (void)lConeOutsideVolume; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetMaxDistance(float flMaxDistance, unsigned long dwApply) { (void)flMaxDistance; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetMinDistance(float flMinDistance, unsigned long dwApply) { (void)flMinDistance; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetMode(unsigned long dwMode, unsigned long dwApply) { (void)dwMode; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetPosition(float x, float y, float z, unsigned long dwApply) { (void)x; (void)y; (void)z; (void)dwApply; return DS_OK; }
  virtual long __stdcall SetVelocity(float x, float y, float z, unsigned long dwApply) { (void)x; (void)y; (void)z; (void)dwApply; return DS_OK; }
};

struct IDirectSoundNotify : IUnknown {};
struct IDirectSoundCapture : IUnknown {
  virtual long __stdcall CreateCaptureBuffer(const DSCBUFFERDESC* pcDSCBufferDesc, IDirectSoundCaptureBuffer** ppDSCBuffer, IUnknown* pUnkOuter) {
    (void)pcDSCBufferDesc; (void)ppDSCBuffer; (void)pUnkOuter; return DSERR_UNSUPPORTED;
  }
};
struct IDirectSoundCapture8 : IDirectSoundCapture {};
struct IDirectSoundCaptureBuffer : IUnknown {
  virtual long __stdcall GetCurrentPosition(unsigned long* pdwCapturePosition, unsigned long* pdwReadPosition) {
    (void)pdwCapturePosition; (void)pdwReadPosition; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall Lock(unsigned long dwOffset, unsigned long dwBytes, void** ppvAudioPtr1, unsigned long* pdwAudioBytes1, void** ppvAudioPtr2, unsigned long* pdwAudioBytes2, unsigned long dwFlags) {
    (void)dwOffset; (void)dwBytes; (void)ppvAudioPtr1; (void)pdwAudioBytes1; (void)ppvAudioPtr2; (void)pdwAudioBytes2; (void)dwFlags; return DSERR_UNSUPPORTED;
  }
  virtual long __stdcall Start(unsigned long dwFlags) { (void)dwFlags; return DSERR_UNSUPPORTED; }
  virtual long __stdcall Stop() { return DSERR_UNSUPPORTED; }
  virtual long __stdcall Unlock(void* pvAudioPtr1, unsigned long dwAudioBytes1, void* pvAudioPtr2, unsigned long dwAudioBytes2) {
    (void)pvAudioPtr1; (void)dwAudioBytes1; (void)pvAudioPtr2; (void)dwAudioBytes2; return DSERR_UNSUPPORTED;
  }
};
struct IDirectSoundCaptureBuffer8 : IDirectSoundCaptureBuffer {};
struct IDirectSoundFXI3DL2Reverb8 : IUnknown {
  virtual long __stdcall SetAllParameters(LPCDSFXI3DL2Reverb pcDsFxI3DL2Reverb) { (void)pcDsFxI3DL2Reverb; return DS_OK; }
};

#endif  /* C++ only */
