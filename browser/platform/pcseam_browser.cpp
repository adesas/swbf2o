/* The browser's replacements for the symbols that lived in pcMain.cpp.
 *
 * pcMain.cpp is the Win32 entry point: it owns the window, the message pump, the D3D9 device and
 * a __try/__except crash guard, none of which a browser has and none of which can be ported
 * (wasm32 clang has no SEH lowering). It is held out of the core build, so the symbols it
 * defined are missing, and the engine calls them from places that are not the entry point:
 *
 *   - ToggleSoundEnabled, ToggleSoundPitch, ToggleAltMeleeHitEffectsCheat are the console
 *     commands the game binds in LuaCallbacks_IFace.cpp:337-339. They are reproduced here with
 *     the engine's own bodies and the engine's own state: the pause-menu sound flag and the
 *     pitch multiplier live in PauseMenu.cpp and sndsource3d.cpp, both of which are in the build,
 *     so these toggles really do toggle the engine's flags.
 *   - gAltMeleeHitEffects is the flag WeaponMelee.cpp:2396 reads to decide whether melee hits
 *     get their alternative effect. pcMain.cpp owned it; the browser's platform globals file
 *     owns it now (browser/platform/pc_globals.cpp:41), with the same default (false - the
 *     normal effect).
 *   - PumpWaitingMessages is the Win32 message pump. The browser's pump is the page's own
 *     requestAnimationFrame loop, which the host runs; there is nothing for this to do here, and
 *     pretending there was would mean inventing a message queue.
 *   - std::_Xbad_alloc and std::_Xlength_error are the MSVC CRT's allocation-failure and
 *     length-error helpers. The engine's own source declares and calls them (GameState.cpp:219,
 *     :1557 and :1655), because it was decompiled from an MSVC build. libc++ has its own
 *     differently-named helpers, so these two are defined here: this build has no exceptions, so
 *     both report and stop, which is what the CRT's do when there is no handler to unwind to.
 *   - __cxa_throw is the Itanium ABI's throw entry point, reached from libc++'s inline code in
 *     the two engine files that were compiled with exceptions enabled. The engine has no catch
 *     block anywhere (grep over build/src_fixed finds none), so an escaping exception ends the
 *     program; this reports the fact and stops, which is the same end state with a better
 *     message.
 */

#include "SoundFL/source/sndsource3d.h"

#include <stdlib.h>

// Shared with browser/main.cpp: the engine-facing log ring the page shows.
extern void Log(const char* fmt, ...);

// The flags these commands toggle. PauseMenu.cpp:660 defines sPauseMenuSoundEnabled; pcMain.cpp:45
// declared it extern and owned the toggle. gAltMeleeHitEffects lives with the other platform
// globals (browser/platform/pc_globals.cpp:41).
extern bool sPauseMenuSoundEnabled;
extern bool gAltMeleeHitEffects;

bool ToggleAltMeleeHitEffectsCheat()
{
    // pcMain.cpp:75 - the same body.
    gAltMeleeHitEffects = !gAltMeleeHitEffects;
    return gAltMeleeHitEffects;
}

bool ToggleSoundEnabled()
{
    // pcMain.cpp:82 - the same body.
    sPauseMenuSoundEnabled = !sPauseMenuSoundEnabled;
    return !sPauseMenuSoundEnabled;
}

bool ToggleSoundPitch()
{
    // pcMain.cpp:89 - the same body: 1.0 is normal pitch, 0.3 is the slowed-down one.
    if (Snd::Source3D::sPitchMultiplier == 1.0f)
    {
        Snd::Source3D::sPitchMultiplier = 0.3f;
        return true;
    }
    Snd::Source3D::sPitchMultiplier = 1.0f;
    return false;
}

void PumpWaitingMessages()
{
    // The browser's message pump is the page's own animation-frame loop, which the host runs
    // (browser/host.js:frame). There is no Win32 message queue to drain here.
}

namespace std
{
// GameState.cpp:219 declares these two in namespace std, with __declspec(noreturn).
__declspec(noreturn) void __cdecl _Xbad_alloc();
__declspec(noreturn) void __cdecl _Xlength_error(const char*);

void __cdecl _Xbad_alloc()
{
    Log("std: allocation failed and this build cannot throw; stopping");
    abort();
}

void __cdecl _Xlength_error(const char* msg)
{
    Log("std: length error (%s) and this build cannot throw; stopping", msg ? msg : "no message");
    abort();
}
}  // namespace std

extern "C" __declspec(noreturn) void __cxa_throw(void* thrown_exception, void* tinfo,
                                                 void (*dest)(void*))
{
    (void)thrown_exception;
    (void)tinfo;
    (void)dest;
    Log("std: an exception escaped and this build cannot unwind; stopping");
    abort();
}
