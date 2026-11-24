/* The browser's Steam layer: there is no Steam client in a browser page, and the engine has to be
 * told so in the shape it checks.
 *
 * The Steam API is a DLL the engine loads on the PC. The retained tree calls into it in a handful
 * of places:
 *
 *   - SteamAPI_RunCallbacks (gfSteamAuth.cpp:313) pumps Steam's callbacks. With no client there
 *     are no callbacks, so the call does nothing.
 *   - SteamAPI_GetHSteamPipe / SteamAPI_GetHSteamUser are the handles the engine uses to decide
 *     whether Steam is running at all (steamworks.h:149-150, :273). Zero is the answer for "no
 *     client", and the engine's own code treats zero as exactly that.
 *   - SteamInternal_ContextInit / SteamInternal_CreateInterface hand back the ISteamClient and
 *     the callback context. There is no client, so there is nothing to hand back: null.
 *   - SteamAPI_RegisterCallResult / SteamAPI_UnregisterCallResult pair a callback object with an
 *     API call. There are no API calls, so there is nothing to register or unregister.
 *
 * Every answer here is the one the engine already handles for "Steam is not running". Nothing
 * fabricates a handle or a client interface, because a fake client would be driven by code that
 * expects the real one's semantics.
 */

#include "Steam/steamworks.h"

// Shared with browser/main.cpp: the engine-facing log ring the page shows.
extern void Log(const char* fmt, ...);

namespace
{
bool gLoggedNoClient = false;

void LogNoClient()
{
    if (!gLoggedNoClient)
    {
        gLoggedNoClient = true;
        Log("steam: the Steam client is not present in a browser page; "
            "the engine's Steam paths report 'not running' (said once)");
    }
}
}  // namespace

/* The client's start-up and shut-down (steamworks.h:109-111). The engine asks Steam to start
 * before it asks anything else, and reads the answer: false means "there is no Steam client",
 * which is the state this build is in and the state the engine's own callers handle (the
 * overlay and the auth paths are skipped when the init fails). RestartAppIfNecessary asks the
 * client to re-launch the game under its own launcher; with no client there is no launcher to
 * re-launch under, so the answer is false and the game carries on. */
extern "C" bool __cdecl SteamAPI_Init()
{
    LogNoClient();
    return false;
}

extern "C" bool __cdecl SteamAPI_RestartAppIfNecessary(unsigned int unOwnAppID)
{
    (void)unOwnAppID;
    LogNoClient();
    return false;
}

extern "C" void __cdecl SteamAPI_Shutdown()
{
    LogNoClient();
}

extern "C" void __cdecl SteamAPI_RunCallbacks()
{
    LogNoClient();
}

extern "C" void __cdecl SteamAPI_RegisterCallResult(CCallbackBase* pCallback, SteamAPICall_t hAPICall)
{
    (void)pCallback;
    (void)hAPICall;
    LogNoClient();
}

extern "C" void __cdecl SteamAPI_UnregisterCallResult(CCallbackBase* pCallback,
                                                     SteamAPICall_t hAPICall)
{
    (void)pCallback;
    (void)hAPICall;
    LogNoClient();
}

extern "C" HSteamPipe __cdecl SteamAPI_GetHSteamPipe()
{
    LogNoClient();
    return 0;
}

extern "C" HSteamUser __cdecl SteamAPI_GetHSteamUser()
{
    LogNoClient();
    return 0;
}

extern "C" void* __cdecl SteamInternal_ContextInit(void* pContextInitData)
{
    (void)pContextInitData;
    LogNoClient();
    return 0;
}

extern "C" void* __cdecl SteamInternal_CreateInterface(const char* ver)
{
    (void)ver;
    LogNoClient();
    return 0;
}
