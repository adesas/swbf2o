// Phase 5 Browser DirectInput8 backend for RedInputManager (Keyboard + Mouse + Gamepad).
//
// Subclasses FakeDirectInput / FakeInputDevice (defined in pcRedInputManager.h) so the COM vtable
// layout matches the engine's exact ABI. Dispatches GetDeviceState by cbData:
//   - cbData == 256                       -> keyboard (256-byte DIK_* state array)
//   - cbData == sizeof(DIMOUSESTATE2) /16 -> mouse (relative lX/lY/lZ + buttons[0..7])
//   - cbData == sizeof(DIJOYSTATE2)       -> gamepad (axes lX..lRz, 32 buttons, POV hat)
//
// Also enumerates one browser gamepad device in EnumDevices(DI8DEVCLASS_GAMECTRL) so
// RedInputManager::m_joysticks[0] is wired up for Web Gamepad API controllers.

#include <dinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "RedEngineFL/input/pc/pcRedInputManager.h"

namespace {

unsigned char g_dikKeys[256] = {};
int g_mouseDx = 0;
int g_mouseDy = 0;
int g_mouseDz = 0;
unsigned char g_mouseButtons[8] = {};

bool g_gamepadConnected = false;
int g_gpLx = 0, g_gpLy = 0, g_gpLz = 0;
int g_gpRx = 0, g_gpRy = 0, g_gpRz = 0;
uint32_t g_gpButtons = 0;
uint32_t g_gpPov = 0xFFFFFFFFu;

unsigned long g_diCreations = 0;
unsigned long g_deviceCreations = 0;
unsigned long g_keyboardStates = 0;
unsigned long g_keyboardDownBytes = 0;
unsigned long g_mouseStates = 0;
unsigned long g_gamepadStates = 0;

class BrowserInputDevice final : public FakeInputDevice {
 public:
  static void* operator new(size_t n) { return malloc(n); }
  static void operator delete(void* p) noexcept { free(p); }
  static void operator delete(void* p, size_t) noexcept { free(p); }

  STDMETHOD(GetCapabilities)(LPDIDEVCAPS pCaps) override {
    if (pCaps) {
      pCaps->dwAxes = 6;
      pCaps->dwButtons = 16;
      pCaps->dwPOVs = 1;
    }
    return DI_OK;
  }

  STDMETHOD(GetDeviceState)(DWORD cbData, LPVOID lpvData) override {
    if (!lpvData) return DIERR_UNSUPPORTED;
    if (cbData == 256) {
      memcpy(lpvData, g_dikKeys, 256);
      ++g_keyboardStates;
      for (int i = 0; i < 256; ++i) {
        if (g_dikKeys[i]) ++g_keyboardDownBytes;
      }
      return DI_OK;
    }
    if (cbData == sizeof(DIMOUSESTATE2)) {
      auto* ms = reinterpret_cast<DIMOUSESTATE2*>(lpvData);
      ms->lX = g_mouseDx;
      ms->lY = g_mouseDy;
      ms->lZ = g_mouseDz;
      memcpy(ms->rgbButtons, g_mouseButtons, 8);
      g_mouseDx = 0;
      g_mouseDy = 0;
      g_mouseDz = 0;
      ++g_mouseStates;
      return DI_OK;
    }
    if (cbData == sizeof(DIMOUSESTATE)) {
      auto* ms = reinterpret_cast<DIMOUSESTATE*>(lpvData);
      ms->lX = g_mouseDx;
      ms->lY = g_mouseDy;
      ms->lZ = g_mouseDz;
      memcpy(ms->rgbButtons, g_mouseButtons, 4);
      g_mouseDx = 0;
      g_mouseDy = 0;
      g_mouseDz = 0;
      ++g_mouseStates;
      return DI_OK;
    }
    if (cbData == sizeof(DIJOYSTATE2)) {
      auto* js = reinterpret_cast<DIJOYSTATE2*>(lpvData);
      memset(js, 0, sizeof(*js));
      for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFFu;
      if (g_gamepadConnected) {
        js->lX = g_gpLx;
        js->lY = g_gpLy;
        js->lZ = g_gpLz;
        js->lRx = g_gpRx;
        js->lRy = g_gpRy;
        js->lRz = g_gpRz;
        js->rgdwPOV[0] = g_gpPov;
        for (int b = 0; b < 32; ++b) {
          js->rgbButtons[b] = (g_gpButtons & (1u << b)) ? 0x80 : 0x00;
        }
      }
      ++g_gamepadStates;
      return DI_OK;
    }
    memset(lpvData, 0, cbData);
    return DI_OK;
  }

  STDMETHOD(GetDeviceData)(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD pdwInOut, DWORD) override {
    if (pdwInOut) *pdwInOut = 0;
    return DI_OK;
  }
};

class BrowserDirectInput8 final : public FakeDirectInput {
 public:
  static void* operator new(size_t n) { return malloc(n); }
  static void operator delete(void* p) noexcept { free(p); }
  static void operator delete(void* p, size_t) noexcept { free(p); }

  STDMETHOD(CreateDevice)(REFGUID, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN) override {
    ++g_deviceCreations;
    if (!lplpDirectInputDevice) return DIERR_UNSUPPORTED;
    *lplpDirectInputDevice = new BrowserInputDevice();
    return DI_OK;
  }

  STDMETHOD(EnumDevices)(DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD) override {
    if (dwDevType == DI8DEVCLASS_GAMECTRL && lpCallback) {
      DIDEVICEINSTANCEA inst{};
      inst.dwSize = sizeof(inst);
      inst.guidInstance = GUID_Joystick;
      inst.guidProduct = GUID_Joystick;
      inst.dwDevType = DI8DEVCLASS_GAMECTRL;
      strncpy(inst.tszInstanceName, "Browser Gamepad", sizeof(inst.tszInstanceName) - 1);
      strncpy(inst.tszProductName, "Web Gamepad API", sizeof(inst.tszProductName) - 1);
      lpCallback(&inst, pvRef);
    }
    return DI_OK;
  }
};

}  // namespace

long __cdecl DirectInput8Create(void*, unsigned, const IID&, void** ppv, void*) {
  ++g_diCreations;
  if (!ppv) return DIERR_UNSUPPORTED;
  *ppv = new BrowserDirectInput8();
  return DI_OK;
}

extern "C" {

void swbf2_input_set_key(int dik, int down) {
  if (dik >= 0 && dik < 256) {
    g_dikKeys[dik] = down ? 0x80 : 0x00;
  }
}

void swbf2_input_set_mouse_delta(int dx, int dy, int dz, int buttonsMask) {
  g_mouseDx += dx;
  g_mouseDy += dy;
  g_mouseDz += dz;
  for (int i = 0; i < 8; ++i) {
    g_mouseButtons[i] = (buttonsMask & (1 << i)) ? 0x80 : 0x00;
  }
}

void swbf2_input_set_cursor(int x, int y, int buttonsMask) {
  for (int i = 0; i < 8; ++i) {
    g_mouseButtons[i] = (buttonsMask & (1 << i)) ? 0x80 : 0x00;
  }
  if (RedMouse* m = RedInputManager::GetMouse(0)) {
    m->SetCursorPos(x, y);
  }
}

void swbf2_input_set_gamepad(int connected, int lx, int ly, int lz, int rx, int ry, int rz,
                             unsigned int buttonsMask, unsigned int povDeg) {
  g_gamepadConnected = connected != 0;
  g_gpLx = lx;
  g_gpLy = ly;
  g_gpLz = lz;
  g_gpRx = rx;
  g_gpRy = ry;
  g_gpRz = rz;
  g_gpButtons = buttonsMask;
  g_gpPov = povDeg;
}

// Single-call per-frame input sync from browser/input_bridge.mjs's SharedArrayBuffer state.
void swbf2_input_sync_state(const unsigned char* keys256, int dx, int dy, int dz,
                            int absX, int absY, int useAbs, int mouseButtonsMask,
                            int gpConnected, int lx, int ly, int lz, int rx, int ry, int rz,
                            unsigned int gpButtonsMask, unsigned int gpPov) {
  if (keys256) {
    memcpy(g_dikKeys, keys256, 256);
  }
  for (int i = 0; i < 8; ++i) {
    g_mouseButtons[i] = (mouseButtonsMask & (1 << i)) ? 0x80 : 0x00;
  }
  if (useAbs) {
    if (RedMouse* m = RedInputManager::GetMouse(0)) {
      m->SetCursorPos(absX, absY);
    }
    g_mouseDz += dz;
  } else {
    g_mouseDx += dx;
    g_mouseDy += dy;
    g_mouseDz += dz;
  }
  g_gamepadConnected = gpConnected != 0;
  g_gpLx = lx;
  g_gpLy = ly;
  g_gpLz = lz;
  g_gpRx = rx;
  g_gpRy = ry;
  g_gpRz = rz;
  g_gpButtons = gpButtonsMask;
  g_gpPov = gpPov;
}

void swbf2_input_seam_counters(unsigned long* pCreations, unsigned long* pDevices,
                               unsigned long* pKeyboardStates, unsigned long* pKeyboardDownBytes) {
  if (pCreations) *pCreations = g_diCreations;
  if (pDevices) *pDevices = g_deviceCreations;
  if (pKeyboardStates) *pKeyboardStates = g_keyboardStates;
  if (pKeyboardDownBytes) *pKeyboardDownBytes = g_keyboardDownBytes;
}

void swbf2_input_clear_all(void) {
  memset(g_dikKeys, 0, sizeof(g_dikKeys));
  g_mouseDx = g_mouseDy = g_mouseDz = 0;
  memset(g_mouseButtons, 0, sizeof(g_mouseButtons));
  g_gamepadConnected = false;
  g_gpLx = g_gpLy = g_gpLz = g_gpRx = g_gpRy = g_gpRz = 0;
  g_gpButtons = 0;
  g_gpPov = 0xFFFFFFFFu;
}

}  // extern "C"
