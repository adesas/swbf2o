// Platform globals that pcMain.cpp used to own.
//
// pcMain.cpp is the one translation unit that cannot be ported at all: WinMain, the Win32
// message pump, D3D9 device creation and a __try/__except crash guard. Its *data* is still
// referenced across the engine (the window handle, the display-mode settings the ini parser
// fills in), so the browser host supplies it here rather than leaving a hole the engine would
// hit the moment game logic runs.
//
// Deliberate choice: hGlobalWnd stays null. There is no Win32 window to hand out. Code that
// passes it to a Win32 API will fail visibly instead of acting on a fabricated handle, which is
// the only honest option while those call sites are being replaced by the browser backend.

#include "windows.h"

HWND hGlobalWnd = 0;

// A page is always windowed; there is no exclusive-mode display to switch to.
bool pcWindowed = true;

// Kept in sync with the canvas by the host (swbf2_init / resize), so settings and anything
// reading the "desktop resolution" see the drawing-buffer size rather than zero.
int pcWidth = 0;
int pcHeight = 0;

// No Win32 instance handle exists. Read by pcRedInputManager's device creation, which the
// browser input backend replaces; until that call site is gone it reads a null handle and
// fails visibly rather than acting on an invented one.
HINSTANCE hGlobalInstance = 0;

// Lifecycle state the message loop used to maintain. The host updates gAppActive from the
// page's visibility, so backgrounded tabs stop simulating.
bool gAppActive = true;
bool gIsQuitting = false;

// Settings the ini loader and the hotkey in pcMain used to write. The defaults are the
// engine's own shipped values; the browser has no post-install step and no alternate melee
// hit-effect data to toggle on.
bool pcForceFixedFunction = false;
bool pcPostInstall = false;
bool pcEnableLogging = true;
bool gAltMeleeHitEffects = false;

// The single-instance mutex is a named Win32 kernel object. Two tabs playing at once is the
// browser's normal behaviour, so there is nothing here to model.
bool gbMutexValid = false;
