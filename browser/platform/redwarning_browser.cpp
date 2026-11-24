/* The engine's log plumbing, three functions of which the decompilation never produced bodies.
 *
 * RedWarning.h declares them; RedWarning.cpp does not define them:
 *
 *   - SetLogCallback (RedWarning.h:67)
 *   - GetFormatToDestination / SetFormatToDestination (RedWarning.h:64-65)
 *
 * They are the engine's own accessors over its own globals - g_fnLogCallback and
 * g_auiFormatToDestination, both defined in RedWarning.cpp with external linkage - so the bodies
 * here are the obvious ones, not invented behaviour.
 *
 * The browser needs them because RedWarning::LogMessage writes to the callback when one is set
 * (RedWarning.cpp:340), and without it the engine's warnings go to the console destination - a
 * RedConsole instance a browser does not have - or to a log file that is not there. A fatal
 * message ("Could not initialize sound engine!", GameState.cpp:440) would then be lost exactly
 * when it matters.
 */

#include "RedEngineFL/Util/RedWarning.h"

namespace RedWarning
{
// Defined in RedWarning.cpp (not static, so they have external linkage).
extern void(__cdecl* g_fnLogCallback)(char const*, ...);
extern unsigned int g_auiFormatToDestination[5];

void __cdecl SetLogCallback(void(__cdecl* pFunc)(char const*, ...))
{
    g_fnLogCallback = pFunc;
}

unsigned int __cdecl GetFormatToDestination(RED_LOG_DESTINATION_INDEX eIndex)
{
    return g_auiFormatToDestination[eIndex];
}

void __cdecl SetFormatToDestination(RED_LOG_DESTINATION_INDEX eIndex, unsigned int uiFormat)
{
    g_auiFormatToDestination[eIndex] = uiFormat;
}

}  // namespace RedWarning

/* Set the engine's log up for a browser.
 *
 * RedWarning::Init is what the PC's entry point does before anything else (pcMain.cpp:353): it
 * gives each log destination its severity threshold and its format mask. The browser has no
 * pcMain, so it runs the engine's own initialiser here.
 *
 * Then the callback destination is given the "include the message" bit (0x00010000), which the
 * file destination has and the callback's mask does not (RedWarning.cpp:232-236). Without it
 * RedWarning::Format writes nothing for the callback destination - it only writes anything at
 * all when that bit is set - and the browser would receive empty strings exactly when the engine
 * is reporting a fatal error.
 */
void RedWarningBrowserInit(void(__cdecl* pCallback)(char const*, ...))
{
    RedWarning::Init();

    /* The callback destination's severity threshold is RED_SEVERITY_NONE after Init (RedWarning.cpp:
     * 229), and RedWarning::LogMessage skips any destination whose threshold the message does not
     * reach (RedWarning.cpp:294) - NONE is the highest value, so the callback would never be
     * called. The browser wants the engine's messages, so the threshold is lowered to VERBOSE
     * with the engine's own setter. */
    RedWarning::SetSeverityToDestination(RED_LOG_DESTINATION_INDEX_CALLBACK, RedWarning::RED_SEVERITY_VERBOSE);

    const unsigned int uiFormat =
        RedWarning::GetFormatToDestination(RED_LOG_DESTINATION_INDEX_CALLBACK);
    RedWarning::SetFormatToDestination(RED_LOG_DESTINATION_INDEX_CALLBACK, uiFormat | 0x00010000u);

    RedWarning::SetLogCallback(pCallback);
}
