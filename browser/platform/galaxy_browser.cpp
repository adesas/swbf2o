/* The browser's GameSpy Galaxy layer: there is no Galaxy client in a browser page.
 *
 * Galaxy is GameSpy's platform SDK - it carries the multiplayer session and the peer-to-peer
 * networking the engine's online code talks to. The three entry points the retained tree needs
 * are:
 *
 *   - galaxy::api::GalaxyFactory::GetInstance: the running Galaxy client. Null, because there is
 *     no client: the engine's own callers all test it for null before using it (see
 *     pcMain.cpp:522, where GetGalaxyNetworking returns 0 when there is no instance).
 *   - galaxy::api::GalaxyFactory::CreateInstance: the engine asks for a client at startup. There
 *     is nothing to create, so it returns null and the engine stays offline.
 *   - galaxy::api::GalaxyFactory::GetErrorManager: the error sink for Galaxy's own errors. With
 *     no client there are no Galaxy errors to report, so it returns null.
 *
 * The two networking getters (GetGalaxyNetworking, GetGalaxyHostNetworking) are the engine's own
 * wrappers around the Galaxy client, and they live in pcMain.cpp - a file this build does not
 * include because it is the Win32 entry point. They are reproduced here with the same bodies,
 * because the multiplayer stack is bypassed in this build and the honest answer is "no
 * networking object exists".
 */

#include "Steam/galaxy_api.h"

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
        Log("galaxy: the GameSpy Galaxy client is not present in a browser page; "
            "the engine stays offline (said once)");
    }
}
}  // namespace

namespace galaxy
{
namespace api
{

IGalaxy* GalaxyFactory::GetInstance()
{
    LogNoClient();
    return 0;
}

IErrorManager* GalaxyFactory::GetErrorManager()
{
    LogNoClient();
    return 0;
}

IGalaxy* GalaxyFactory::CreateInstance()
{
    LogNoClient();
    return 0;
}

/* The client is torn down and re-created when the engine changes the login state
 * (galaxy_api.h:15). There is no client instance to release, so there is nothing to do; the
 * factory stays in the state it started in, which is "no client". */
void GalaxyFactory::ResetInstance()
{
    LogNoClient();
}

}  // namespace api
}  // namespace galaxy

// pcMain.cpp:522 and :533 - the engine's wrappers around the Galaxy client's networking objects.
void* GetGalaxyNetworking()
{
    if (!galaxy::api::GalaxyFactory::GetInstance())
    {
        return 0;
    }
    return 0;
}

void* GetGalaxyHostNetworking()
{
    if (!galaxy::api::GalaxyFactory::GetInstance())
    {
        return 0;
    }
    return 0;
}
