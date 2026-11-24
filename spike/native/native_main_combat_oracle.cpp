/* The native side of the combat oracle.
 *
 * The browser build runs the same harness (spike/combat_harness.cpp) inside the wasm module and
 * the same script (CombatHarnessRun, which lives in the harness), so the two builds fight the
 * same fight by construction rather than by two scripts agreeing. The numbers they print are
 * compared by tools/combat_oracle_check.py.
 *
 * The preamble is the input/camera oracle's preamble, step for step (see
 * spike/native/native_main_input_camera.cpp), because the comparison is only worth anything if
 * both sides start from the same engine's start-up, the same level and the same classes.
 *
 * The data root is the environment variable SWBF2_DATA_ROOT, because the browser's file layer
 * reads the game's data from the VFS at /data while the native build reads it from a directory.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../spike/combat_harness.h"
#include "Battlefront2/Source/Entity.h"
#include "RedEngineFL/Graphics/RedCamera.h"

/* The host's own two functions, which the browser build gets from browser/main.cpp: the log and
   the scene camera the renderer seam asks for. */
void __cdecl Log(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

RedCamera* BrowserSceneCamera()
{
    static RedCamera sCamera;
    return &sCamera;
}

/* The display mode the browser platform layer keeps (browser/platform/pc_globals.cpp). The
   renderer seam's screen aspect ratio is computed from it (pcRedRenderer_browser.cpp:814-821),
   and the chase camera's field of view is computed from that, so the oracle gives it the same
   size the browser side gives it. */
extern int pcWidth;
extern int pcHeight;

static const unsigned int ORACLE_VIEWPORT_WIDTH = 1280;
static const unsigned int ORACLE_VIEWPORT_HEIGHT = 720;

static void __cdecl NativeLog(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

int main(int argc, char** argv)
{
    const char* pcLevel = (argc > 1) ? argv[1] : "tat/tat2.lvl";
    const char* pcPlayerClass = (argc > 2) ? argv[2] : "rep_inf_ep3_rifleman";
    const char* pcPlayerClassFile = (argc > 3) ? argv[3] : "side/rep.lvl;rep_inf_ep3_rifleman";
    const char* pcTargetClass = (argc > 4) ? argv[4] : "cis_inf_rifleman";
    const char* pcTargetClassFile = (argc > 5) ? argv[5] : "side/cis.lvl;cis_inf_rifleman";
    const char* pcRoot = (argc > 6) ? argv[6] : 0;
    const int iTargets = (argc > 7) ? atoi(argv[7]) : 4;
    if (pcRoot && pcRoot[0])
    {
        setenv("SWBF2_DATA_ROOT", pcRoot, 1);
    }

    pcWidth = (int)ORACLE_VIEWPORT_WIDTH;
    pcHeight = (int)ORACLE_VIEWPORT_HEIGHT;
    /* The browser module's canvas size, so that the chase camera's own field of view
       (ChaseCamera.cpp:306-309) is the field of view the browser gives it. */
    LevelHarnessSetViewport(ORACLE_VIEWPORT_WIDTH, ORACLE_VIEWPORT_HEIGHT);

    CombatConfig config;
    CombatHarnessDefaultConfig(&config);
    config.pcLevel = pcLevel;
    config.pcPlayerClass = pcPlayerClass;
    config.pcPlayerClassFile = pcPlayerClassFile;
    config.pcTargetClass = pcTargetClass;
    config.pcTargetClassFile = pcTargetClassFile;
    config.iTargetCount = iTargets;

    CombatResult result;
    const bool bRan = CombatHarnessRun(&config, NativeLog, &result);

    /* The harness builds the report, so both builds print the same text for the same fight. */
    printf("COMBAT_ORACLE_JSON %s\n", CombatHarnessReport());
    return bRan ? 0 : 2;
}
