// Windows SDK interface identifiers and effect GUIDs that the engine's Windows backends
// reference.
//
// Every one of these belongs to a subsystem the browser build replaces: DirectSound and its
// effects (WebAudio owns mixing), DirectMediaObject (the media element owns decoding), and
// IKsPropertySet (kernel streaming). The code that references them still has to link, because
// it is part of the engine and the engine's symbol demands are what say whether game logic can
// actually run - so the identifiers are declared here.
//
// They are all zero, on purpose. Zero names no resolvable interface, which is the truth for a
// browser build. Copying the published values would advertise interfaces nothing implements,
// and a plausible-but-wrong GUID is harder to notice than an obviously empty one. If a backend
// is ever wired to a real object here, it should replace this file with the actual constants.
#include "windows.h"
#include "dsound.h"
#include "mediaobj.h"

extern const GUID DS3DALG_DEFAULT = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID DSDEVID_DefaultPlayback = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID DSDEVID_DefaultVoicePlayback = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_CHORUS = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_COMPRESSOR = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_DISTORTION = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_ECHO = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_FLANGER = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_GARGLE = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_I3DL2REVERB = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_STANDARD_PARAMEQ = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID GUID_DSFX_WAVES_REVERB = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IDirectSound3DBuffer = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IDirectSound3DListener = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IDirectSound8 = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IDirectSoundFXI3DL2Reverb8 = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IKsPropertySet = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IMediaBuffer = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IMediaObject = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IMediaObjectInPlace = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
extern const GUID IID_IUnknown = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
