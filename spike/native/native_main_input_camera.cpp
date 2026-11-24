/* The native side of the movement oracle.

 * The browser build runs the same harness (spike/level_harness.cpp) inside the wasm module and the
 * same script (LevelHarnessMovementScript, which lives in the harness), so the two builds drive the
 * trooper with the same injected input by construction rather than by two scripts agreeing. The
 * numbers they print are compared by tools/input_camera_check.py.

 * The preamble is the spawn oracle's preamble, step for step (see
 * spike/native/native_main_spawn_oracle.cpp), so that what is compared is a trooper the engine
 * built, put on the ground the engine's own ray found, and then driven.

 * The data root is the environment variable SWBF2_DATA_ROOT, because the browser's file layer reads
 * the game's data from the VFS at /data while the native build reads it from a directory.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../spike/level_harness.h"
#include "Battlefront2/Source/Entity.h"
#include "RedEngineFL/Graphics/RedCamera.h"

/* The host's own two functions, which the browser build gets from browser/main.cpp: the log and
 * the scene camera the renderer seam asks for. */
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
 * renderer seam's screen aspect ratio is computed from it (pcRedRenderer_browser.cpp:814-821), and
 * the chase camera's field of view is computed from that, so the oracle gives it the same size the
 * browser side gives it. */
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

/* One record per driven turn. The order is the order of TrooperState's fields; the check script
 * reads the same names on both sides. */
static void print_state(int i, const TrooperState* pState)
{
    /* %.9g is enough to round-trip a float exactly (FLT_DECIMAL_DIG is 9), which is what an exact
       comparison between the two builds needs; the ground oracle uses %.17g for the same reason. */
    printf("%s[%d,%d,%d,%d,%u,%u,", i ? "," : "", pState->updates, pState->iState,
           pState->iFireDown, pState->iSpecialMoveDown, pState->uiHostButtons,
           pState->uiHostMouseMode);
    printf("%.9g,%.9g,%.9g,", (double)pState->x, (double)pState->y, (double)pState->z);
    printf("%.9g,%.9g,%.9g,", (double)pState->dirX, (double)pState->dirY, (double)pState->dirZ);
    printf("%.9g,%.9g,%.9g,", (double)pState->velX, (double)pState->velY, (double)pState->velZ);
    printf("%.9g,%.9g,%.9g,%.9g,", (double)pState->controlMove, (double)pState->controlStrafe,
           (double)pState->controlTurn, (double)pState->controlPitch);
    printf("%.9g,", (double)pState->fHealth);
    printf("%.9g,%.9g,%.9g,", (double)pState->camX, (double)pState->camY, (double)pState->camZ);
    printf("%.9g,%.9g,%.9g,", (double)pState->camDirX, (double)pState->camDirY, (double)pState->camDirZ);
    printf("%.9g,%.9g,", (double)pState->camFov, (double)pState->camZoom);
    printf("%.9g,%.9g]", (double)pState->fRollSpeedFactor, (double)pState->fEnergy);
}

int main(int argc, char** argv)
{
    const char* pcLevel = (argc > 1) ? argv[1] : "tat/tat2.lvl";
    const char* pcClass = (argc > 2) ? argv[2] : "cis_inf_rifleman";
    const char* pcClassFile = (argc > 3) ? argv[3] : "side/cis.lvl;cis_inf_rifleman";
    const char* pcRoot = (argc > 4) ? argv[4] : 0;
    if (pcRoot && pcRoot[0])
    {
        setenv("SWBF2_DATA_ROOT", pcRoot, 1);
    }

    if (!LevelHarnessInit(NativeLog))
    {
        fprintf(stderr, "harness init failed\n");
        return 1;
    }

    if (!LevelHarnessLoad(pcLevel))
    {
        fprintf(stderr, "level %s failed to load\n", pcLevel);
        return 1;
    }

    if (!LevelHarnessLoadClass("ingame.lvl;com_inf_default"))
    {
        fprintf(stderr, "base class com_inf_default failed to load\n");
        return 1;
    }

    if (!LevelHarnessLoadClass(pcClassFile))
    {
        fprintf(stderr, "class %s failed to load from %s\n", pcClass, pcClassFile);
        return 1;
    }

    GroundProbeResult ground;
    LevelHarnessDrop(0.0f, 0.0f, 0.35f, 0.85f, &ground);

    SpawnResult spawn;
    const bool bSpawned = LevelHarnessSpawn(pcClass, 0.0f, ground.restY, 0.0f, 0, &spawn);
    if (!bSpawned)
    {
        fprintf(stderr, "spawn failed\n");
        return 1;
    }

    /* The engine's own view and screen dimensions, and the display mode the platform layer keeps.
       Without them the camera's field of view is 0/0 (ChaseCamera.cpp:306-309). */
    pcWidth = (int)ORACLE_VIEWPORT_WIDTH;
    pcHeight = (int)ORACLE_VIEWPORT_HEIGHT;
    LevelHarnessSetViewport(ORACLE_VIEWPORT_WIDTH, ORACLE_VIEWPORT_HEIGHT);

    /* The engine's own player-control chain: find the PlayerController the engine made for this
       trooper and put the chase camera on it. */
    const bool bAttached = LevelHarnessControlAttach();
    const bool bReady = LevelHarnessControlReady();

    const int iCapacity = LevelHarnessMovementScriptLength();
    TrooperState* pStates = (TrooperState*)calloc((size_t)(iCapacity > 0 ? iCapacity : 1),
                                                  sizeof(TrooperState));
    int iDriven = 0;
    const bool bRan = LevelHarnessMovementScript(pStates, iCapacity, &iDriven);

    printf("INPUT_CAMERA_ORACLE_JSON {\"level\":\"%s\",\"class\":\"%s\",\"class_file\":\"%s\","
           "\"attached\":%s,\"ready\":%s,\"ground_rest\":%.17g,\"x\":%.17g,\"y\":%.17g,\"z\":%.17g,"
           "\"script_turns\":%d,\"recorded\":%d,\"states\":[",
           pcLevel, pcClass, pcClassFile, bAttached ? "true" : "false", bReady ? "true" : "false",
           (double)ground.restY, (double)spawn.x, (double)spawn.y, (double)spawn.z, iDriven,
           bRan ? iDriven : 0);

    for (int i = 0; i < iDriven && i < iCapacity; ++i)
    {
        print_state(i, &pStates[i]);
    }
    printf("]}\n");

    free(pStates);
    return bRan && bReady ? 0 : 2;
}
