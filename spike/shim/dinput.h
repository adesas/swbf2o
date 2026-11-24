#pragma once
/* This header models C++ COM/DirectInput8 APIs: never include it from C. */
#ifdef __cplusplus  /* C++ only */
#include "swbf2_prefix.h"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

/* ---- DirectInput 8 scancodes (Microsoft dinput.h exact values, 0x01..0xED) ---- */
#define DIK_ESCAPE          0x01
#define DIK_1               0x02
#define DIK_2               0x03
#define DIK_3               0x04
#define DIK_4               0x05
#define DIK_5               0x06
#define DIK_6               0x07
#define DIK_7               0x08
#define DIK_8               0x09
#define DIK_9               0x0A
#define DIK_0               0x0B
#define DIK_MINUS           0x0C
#define DIK_EQUALS          0x0D
#define DIK_BACK            0x0E
#define DIK_TAB             0x0F
#define DIK_Q               0x10
#define DIK_W               0x11
#define DIK_E               0x12
#define DIK_R               0x13
#define DIK_T               0x14
#define DIK_Y               0x15
#define DIK_U               0x16
#define DIK_I               0x17
#define DIK_O               0x18
#define DIK_P               0x19
#define DIK_LBRACKET        0x1A
#define DIK_RBRACKET        0x1B
#define DIK_RETURN          0x1C
#define DIK_LCONTROL        0x1D
#define DIK_A               0x1E
#define DIK_S               0x1F
#define DIK_D               0x20
#define DIK_F               0x21
#define DIK_G               0x22
#define DIK_H               0x23
#define DIK_J               0x24
#define DIK_K               0x25
#define DIK_L               0x26
#define DIK_SEMICOLON       0x27
#define DIK_APOSTROPHE      0x28
#define DIK_GRAVE           0x29
#define DIK_LSHIFT          0x2A
#define DIK_BACKSLASH       0x2B
#define DIK_Z               0x2C
#define DIK_X               0x2D
#define DIK_C               0x2E
#define DIK_V               0x2F
#define DIK_B               0x30
#define DIK_N               0x31
#define DIK_M               0x32
#define DIK_COMMA           0x33
#define DIK_PERIOD          0x34
#define DIK_SLASH           0x35
#define DIK_RSHIFT          0x36
#define DIK_MULTIPLY        0x37
#define DIK_LMENU           0x38
#define DIK_SPACE           0x39
#define DIK_CAPITAL         0x3A
#define DIK_F1              0x3B
#define DIK_F2              0x3C
#define DIK_F3              0x3D
#define DIK_F4              0x3E
#define DIK_F5              0x3F
#define DIK_F6              0x40
#define DIK_F7              0x41
#define DIK_F8              0x42
#define DIK_F9              0x43
#define DIK_F10             0x44
#define DIK_NUMLOCK         0x45
#define DIK_SCROLL          0x46
#define DIK_NUMPAD7         0x47
#define DIK_NUMPAD8         0x48
#define DIK_NUMPAD9         0x49
#define DIK_SUBTRACT        0x4A
#define DIK_NUMPAD4         0x4B
#define DIK_NUMPAD5         0x4C
#define DIK_NUMPAD6         0x4D
#define DIK_ADD             0x4E
#define DIK_NUMPAD1         0x4F
#define DIK_NUMPAD2         0x50
#define DIK_NUMPAD3         0x51
#define DIK_NUMPAD0         0x52
#define DIK_DECIMAL         0x53
#define DIK_OEM_102         0x56
#define DIK_F11             0x57
#define DIK_F12             0x58
#define DIK_F13             0x64
#define DIK_F14             0x65
#define DIK_F15             0x66
#define DIK_KANA            0x70
#define DIK_ABNT_C1         0x73
#define DIK_CONVERT         0x79
#define DIK_NOCONVERT       0x7B
#define DIK_YEN             0x7D
#define DIK_ABNT_C2         0x7E
#define DIK_NUMPADEQUALS    0x8D
#define DIK_PREVTRACK       0x90
#define DIK_AT              0x91
#define DIK_COLON           0x92
#define DIK_UNDERLINE       0x93
#define DIK_KANJI           0x94
#define DIK_STOP            0x95
#define DIK_AX              0x96
#define DIK_UNLABELED       0x97
#define DIK_NEXTTRACK       0x99
#define DIK_NUMPADENTER     0x9C
#define DIK_RCONTROL        0x9D
#define DIK_MUTE            0xA0
#define DIK_CALCULATOR      0xA1
#define DIK_PLAYPAUSE       0xA2
#define DIK_MEDIASTOP       0xA4
#define DIK_VOLUMEDOWN      0xAE
#define DIK_VOLUMEUP        0xB0
#define DIK_WEBHOME         0xB2
#define DIK_NUMPADCOMMA     0xB3
#define DIK_DIVIDE          0xB5
#define DIK_SYSRQ           0xB7
#define DIK_RMENU           0xB8
#define DIK_PAUSE           0xC5
#define DIK_HOME            0xC7
#define DIK_UP              0xC8
#define DIK_PRIOR           0xC9
#define DIK_LEFT            0xCB
#define DIK_RIGHT           0xCD
#define DIK_END             0xCF
#define DIK_DOWN            0xD0
#define DIK_NEXT            0xD1
#define DIK_INSERT          0xD2
#define DIK_DELETE          0xD3
#define DIK_LWIN            0xDB
#define DIK_RWIN            0xDC
#define DIK_APPS            0xDD
#define DIK_POWER           0xDE
#define DIK_SLEEP           0xDF
#define DIK_WAKE            0xE3
#define DIK_WEBSEARCH       0xE5
#define DIK_WEBFAVORITES    0xE6
#define DIK_WEBREFRESH      0xE7
#define DIK_WEBSTOP         0xE8
#define DIK_WEBFORWARD      0xE9
#define DIK_WEBBACK         0xEA
#define DIK_MYCOMPUTER      0xEB
#define DIK_MAIL            0xEC
#define DIK_MEDIASELECT     0xED

/* ---- DirectInput 8 flags & error codes ---- */
#define DISCL_EXCLUSIVE                  0x00000001
#define DISCL_NONEXCLUSIVE               0x00000002
#define DISCL_FOREGROUND                 0x00000004
#define DISCL_BACKGROUND                 0x00000008
#define DISCL_NOWINKEY                   0x00000010
#define DIPH_DEVICE                      0
#define DIPH_BYOFFSET                    1
#define DIPH_BYID                        2
#define DIPH_BYUSAGE                     3
#define DIEDFL_ATTACHEDONLY              0x00000001
#define DIEDFL_INCLUDEALIASES            0x01000000
#define DIEDFL_INCLUDEPHANTOMS           0x02000000
#define DI8DEVCLASS_ALL                  0
#define DI8DEVCLASS_DEVICE               1
#define DI8DEVCLASS_POINTER              2
#define DI8DEVCLASS_KEYBOARD             3
#define DI8DEVCLASS_GAMECTRL             4
#define DI8DEVTYPE_DEVICE                0x11
#define DIDFT_ALL                        0x00000000
#define DIDFT_RELAXIS                    0x00000001
#define DIDFT_ABSAXIS                    0x00000002
#define DIDFT_AXIS                       0x00000003
#define DI_OK                            0
#define DI_NOTATTACHED                   1
#define DI_BUFFEROVERFLOW                1
#define DI_PROPNOEFFECT                  1
#define DI_NOEFFECT                      1
#define DI_POLLEDDEVICE                  2
#define DI_GETDEVICESPECIFICDATA_DONE    0x00000200
#define DIERR_UNSUPPORTED                ((int)0x80004001)
#define DIERR_NOTINITIALIZED             ((int)0x80070015)
#define DIERR_DEVICENOTREG               ((int)0x80040154)
#define DIERR_INPUTLOST                  ((int)0x8007001E)
#define DIERR_NOTACQUIRED                ((int)0x8007000C)
#define DIERR_UNPLUGGED                  ((int)0x800701EA)
#define DIENUM_STOP                      0
#define DIENUM_CONTINUE                  1
#define DIENUM_STOP_OR_CONTINUE          int

/* ---- DirectInput 8 structs ---- */
struct DIOBJECTDATAFORMAT { DWORD ogType, ogOfs, ogBytes, dwFlags; };

struct DIDATAFORMAT {
  DWORD dwSize, dwDataSize, dwNumDataRanges, dwFlags;
  const DIOBJECTDATAFORMAT* rgordf;
  void** rgdid;
};

struct DIDEVICEOBJECTDATA {
  unsigned int dw_offset, dw_sequence, dw_timestamp;
  unsigned char dw_data;
};

struct DIMOUSESTATE   { int lX, lY, lZ; unsigned char rgbButtons[4]; };
struct DIMOUSESTATE2  { int lX, lY, lZ; unsigned char rgbButtons[8]; };
struct DIMOUSEX       { DWORD dwButtons, lX, lY, lRX, lRY, lZ; };
struct DIJOYSTATE     { int lX, lY, lZ, lRX, lRY, lRZ, lSliderX, lSliderY; int lVrSlider, lSliderV, lHAT[4]; int rglDummy[2]; DWORD rgbButtons[32]; };
struct DIJOYSTATE2    { int lX, lY, lZ, lRX, lRY, lRZ, lSliderX, lSliderY; int lVrSlider, lSliderV, lHAT[4]; int rglDummy[2]; DWORD rgbButtons[128]; int rgdwPOV[4]; int rglSlider[2]; int lRx, lRy, lRz, lUgl, lVgl; int lUGl[2], lVGl[2]; };

struct DIDEVICEINSTANCEA {
  DWORD dwSize, dwFlags, dwDevType;
  GUID guidInstance, guidProduct, guidClass;
  DWORD  dwTimeStamp;
  char   tszInstanceName[260], tszProductName[260];
  GUID   guidFFDriver;
  unsigned short wIdVendor, wIdProduct;
};

struct DIDEVICEOBJECTINSTANCEA {
  DWORD dwSize, dwType, dwFlags;
  GUID oguid;
  DWORD dwTS, dwDataSize;
  char tszName[260];
};

struct DIPROPHEADER { DWORD dwSize; DWORD dwHeaderSize; DWORD dwObj; DWORD dwHow; };
struct DIPROPDWORD  { DIPROPHEADER diph; DWORD dwData; };
struct DIPROPRANGE  { DIPROPHEADER diph; int lMin; int lMax; };
struct DIPROPCAL    { DIPROPHEADER diph; int lMin; int lMax; };
struct DIPROPVOID   { DIPROPHEADER diph; DWORD dwData; LPVOID lplvData; };
struct DIPROPSTRING { DIPROPHEADER diph; wchar_t wsz[260]; };

struct DIPERFORMANCE { DWORD dwSize; DWORD dwFlags; DWORD dwInstanceDataSize; DWORD dwDeviceDataSize; DWORD dwDataSize; LPVOID lpData; };
struct DIPERFORMANCEStructDummy { int unused; };

struct DIEFFECT {
  DWORD dwSize; DWORD dwFlags; DWORD dwDuration; DWORD dwSamplePeriod; DWORD dwGain;
  DWORD dwTriggerType; DWORD dwDirection; LPVOID lpwszName; LPVOID lpwszComment; DWORD dwChannel;
  DWORD dwDevice; DWORD dwTriggerReport; DWORD dwDurationUsecs; DWORD dwStartDelay; DWORD dwInitialPeriod;
  void* rgpwszNames[8];
};
struct DIEFFECTINFOA {
  DWORD dwSize; GUID guid; DWORD dwEffType, dwStaticParams, dwDynamicParams; char tszName[260];
};
struct DIEFFESCAPE {
  DWORD dwSize, dwCommand; LPVOID lpvInBuffer; DWORD cbInBuffer; LPVOID lpvOutBuffer; DWORD cbOutBuffer;
};
struct DIFILEEFFECT {
  DWORD dwSize; GUID GuidEffect; const DIEFFECT* lpDiEffect; char szFriendlyName[260];
};
struct DIACTIONFORMATA {
  DWORD dwSize, dwActionSize, dwDataSize, dwNumActions; void* rgoAction; GUID guidActionMap;
  DWORD dwGenre, dwBufferSize; LONG lAxisMin, lAxisMax; HINSTANCE hInstString;
  unsigned long long ftTimeStamp; DWORD dwCRC; char tszActionMap[260];
};
struct DIDEVICEIMAGEINFOHEADERA {
  DWORD dwSize, dwSizeImageInfo, dwcViews, dwcButtons, dwcAxes, dwcPOVs, dwBufferSize, dwBufferUsed;
  void* lprgImageInfoArray;
};
struct DICONFIGUREDEVICESPARAMSA {
  DWORD dwSize, dwcUsers; char* lptszUserNames; DWORD dwcFormats; DIACTIONFORMATA* lprgFormats;
  HWND hwnd; DWORD dics; IUnknown* lpUnkDDSTarget;
};
struct DIOBJECTINFORMATIONStructDummy { int unused; };
struct DIDEVCAPS {
  DWORD dwSize; DWORD dwFlags; DWORD dwVersion; DWORD dwDriverVersion;
  DWORD dwAxes; DWORD dwButtons; DWORD dwPOVs; DWORD dwFFSamplePeriod;
};

struct IDirectInputEffect;
struct IDirectInputDevice8A;
struct IDirectInput8A;

typedef DIPROPSTRING*               LPDIPROPSTRING;
typedef const DIPROPHEADER*         LPCDIPROPHEADER;
typedef DIPROPHEADER*               LPDIPROPHEADER;
typedef const DIPERFORMANCE*        LPCDIPERFORMANCE;
typedef DIPERFORMANCE*              LPDIPERFORMANCE;
typedef DIDEVCAPS*                  LPDIDEVCAPS;
typedef const DIDEVCAPS*            LPCDIDEVCAPS;
typedef const DIDATAFORMAT*         LPCDIDATAFORMAT;
typedef DIDEVICEOBJECTDATA*         LPDIDEVICEOBJECTDATA;
typedef const DIDEVICEOBJECTDATA*   LPCDIDEVICEOBJECTDATA;
typedef DIDEVICEOBJECTINSTANCEA*    LPDIDEVICEOBJECTINSTANCEA;
typedef DIDEVICEINSTANCEA*          LPDIDEVICEINSTANCEA;
typedef const DIEFFECT*             LPCDIEFFECT;
typedef IDirectInputEffect*         LPDIRECTINPUTEFFECT;
typedef DIEFFECTINFOA*              LPDIEFFECTINFOA;
typedef DIEFFESCAPE*                LPDIEFFESCAPE;
typedef DIFILEEFFECT*               LPDIFILEEFFECT;
typedef DIACTIONFORMATA*            LPDIACTIONFORMATA;
typedef DIDEVICEIMAGEINFOHEADERA*   LPDIDEVICEIMAGEINFOHEADERA;
typedef DICONFIGUREDEVICESPARAMSA*  LPDICONFIGUREDEVICESPARAMSA;

typedef BOOL (CALLBACK *LPDIENUMDEVICEOBJECTSCALLBACKA)(const DIDEVICEOBJECTINSTANCEA*, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICESCALLBACKA)(const DIDEVICEINSTANCEA*, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMEFFECTSCALLBACKA)(const DIEFFECTINFOA*, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMCREATEDEFFECTOBJECTSCALLBACK)(LPDIRECTINPUTEFFECT, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMEFFECTSINFILECALLBACK)(const DIFILEEFFECT*, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICESBYSEMANTICSCBA)(const DIDEVICEINSTANCEA*, IDirectInputDevice8A*, DWORD, DWORD, LPVOID);
typedef BOOL (CALLBACK *LPDICONFIGUREDEVICESCALLBACK)(IUnknown*, LPVOID);

typedef IDirectInputDevice8A*       LPDIRECTINPUTDEVICE8A;
typedef IDirectInputDevice8A*       LPCDIRECTINPUTDEVICE8A;
typedef IDirectInput8A*             LPDIRECTINPUT8A;

/* ---- Real virtual COM interfaces (matching FakeInputDevice / FakeDirectInput vtables) ---- */
struct IDirectInputDevice8A : IUnknown {
  STDMETHOD(GetCapabilities)(LPDIDEVCAPS) { return DI_OK; }
  STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(GetProperty)(REFGUID, LPDIPROPHEADER) { return DI_OK; }
  STDMETHOD(SetProperty)(REFGUID, LPCDIPROPHEADER) { return DI_OK; }
  STDMETHOD(Acquire)() { return DI_OK; }
  STDMETHOD(Unacquire)() { return DI_OK; }
  STDMETHOD(GetDeviceState)(DWORD, LPVOID) { return DI_OK; }
  STDMETHOD(GetDeviceData)(DWORD, LPDIDEVICEOBJECTDATA, DWORD*, DWORD) { return DI_OK; }
  STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT) { return DI_OK; }
  STDMETHOD(SetEventNotification)(HANDLE) { return DI_OK; }
  STDMETHOD(SetCooperativeLevel)(HWND, DWORD) { return DI_OK; }
  STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) { return DI_OK; }
  STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA) { return DI_OK; }
  STDMETHOD(RunControlPanel)(HWND, DWORD) { return DI_OK; }
  STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) { return DI_OK; }
  STDMETHOD(CreateEffect)(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, IUnknown*) { return DI_OK; }
  STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA, REFGUID) { return DI_OK; }
  STDMETHOD(GetForceFeedbackState)(DWORD*) { return DI_OK; }
  STDMETHOD(SendForceFeedbackCommand)(DWORD) { return DI_OK; }
  STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(Escape)(LPDIEFFESCAPE) { return DI_OK; }
  STDMETHOD(Poll)() { return DI_OK; }
  STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, DWORD*, DWORD) { return DI_OK; }
  STDMETHOD(EnumEffectsInFile)(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(WriteEffectToFile)(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) { return DI_OK; }
  STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) { return DI_OK; }
  STDMETHOD(SetActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) { return DI_OK; }
  STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA) { return DI_OK; }
};

struct IDirectInput8A : IUnknown {
  STDMETHOD(CreateDevice)(REFGUID, LPDIRECTINPUTDEVICE8A*, IUnknown*) { return DI_OK; }
  STDMETHOD(EnumDevices)(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(GetDeviceStatus)(REFGUID) { return DI_OK; }
  STDMETHOD(RunControlPanel)(HWND, DWORD) { return DI_OK; }
  STDMETHOD(Initialize)(HINSTANCE, DWORD) { return DI_OK; }
  STDMETHOD(FindDevice)(REFGUID, LPCSTR, LPGUID) { return DI_OK; }
  STDMETHOD(EnumDevicesBySemantics)(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) { return DI_OK; }
  STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) { return DI_OK; }
};

typedef IDirectInputDevice8A IDirectInputDevice8;
typedef IDirectInput8A       IDirectInput8;
typedef IDirectInputDevice8  LPDIRECTINPUTDEVICE8_;

/* ---- DirectInput 8 well-known GUIDs, properties & data formats ---- */
static const IID IID_IDirectInput8A_local = { 0xBF798030, 0x483A, 0x4DA2, {0} };
#define IID_IDirectInput8A IID_IDirectInput8A_local

static const GUID GUID_SysKeyboard_local  = { 1, 0, 0, {0} };
static const GUID GUID_SysMouse_local     = { 2, 0, 0, {0} };
static const GUID GUID_Joystick_local     = { 3, 0, 0, {0} };
#define GUID_SysKeyboard GUID_SysKeyboard_local
#define GUID_SysMouse    GUID_SysMouse_local
#define GUID_Joystick    GUID_Joystick_local

static const GUID DIPROP_RANGE_local      = { 4, 0, 0, {0} };
static const GUID DIPROP_DEADZONE_local   = { 5, 0, 0, {0} };
static const GUID DIPROP_KEYNAME_local    = { 20, 0, 0, {0} };
#define DIPROP_RANGE     DIPROP_RANGE_local
#define DIPROP_DEADZONE  DIPROP_DEADZONE_local
#define DIPROP_KEYNAME   DIPROP_KEYNAME_local

static const DIDATAFORMAT c_dfDIKeyboard_local  = {0};
static const DIDATAFORMAT c_dfDIMouse_local     = {0};
static const DIDATAFORMAT c_dfDIMouse2_local    = {0};
static const DIDATAFORMAT c_dfDIMouseEx_local   = {0};
static const DIDATAFORMAT c_dfDIJoystick_local  = {0};
static const DIDATAFORMAT c_dfDIJoystick2_local = {0};
#define c_dfDIKeyboard  c_dfDIKeyboard_local
#define c_dfDIMouse     c_dfDIMouse_local
#define c_dfDIMouse2    c_dfDIMouse2_local
#define c_dfDIMouseEx   c_dfDIMouseEx_local
#define c_dfDIJoystick  c_dfDIJoystick_local
#define c_dfDIJoystick2 c_dfDIJoystick2_local

long __cdecl DirectInput8Create(void* hinst, unsigned version, const IID& riid, void** ppv, void* punk);
#endif  /* C++ only */
