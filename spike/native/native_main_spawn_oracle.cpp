/* The native side of the spawn oracle.
 *
 * The browser build runs the same harness (spike/level_harness.cpp) inside the wasm module; this
 * program runs it in a native build of the same engine sources (build/native_core.a, compiled by
 * tools/compile_native.sh with the same clang front-end, the same shim headers and the same
 * flags, for the host target instead of wasm32).
 *
 * Both read the same files through the same engine loader, spawn the same soldier class at the
 * same point and then run the same number of engine updates; the numbers they print are
 * compared by tools/spawn_oracle_check.py.
 *
 * The data root is the environment variable SWBF2_DATA_ROOT, because the browser's file layer
 * reads the game's data from the VFS at /data while the native build reads it from a directory.
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

    /* The base class the soldier class is derived from. The engine's own chain is
       soldier -> com_inf_default -> cis_inf_default -> cis_inf_default_rifleman ->
       cis_inf_rifleman, and the first link of it lives in ingame.lvl (the engine's own file
       for the classes the game always has). */
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


    /* The point the soldier is spawned at: the origin of the level, on the ground the engine's
       own terrain reports there. The drop probe is the engine's own ray query, so the spawn
       point is the engine's own ground rather than a number the harness made up. */
    GroundProbeResult ground;
    LevelHarnessDrop(0.0f, 0.0f, 0.35f, 0.85f, &ground);

    SpawnResult spawn;
    const bool bSpawned = LevelHarnessSpawn(pcClass, 0.0f, ground.restY, 0.0f, 0, &spawn);

    /* The engine's own updates: the soldier's physics runs on the turn thread, and the game's
       own loop advances those threads with Thread::UpdateAll. */
    const int iUpdates = 60;
    for (int i = 0; i < iUpdates; ++i)
    {
        LevelHarnessUpdate(1.0f / 30.0f);
    }

    /* The marker tells the check script which line is the result. */
    printf("SPAWN_ORACLE_JSON {\"level\":\"%s\",\"class\":\"%s\",\"class_file\":\"%s\","
           "\"spawned\":%s,\"ground_rest\":%.17g,\"x\":%.17g,\"y\":%.17g,\"z\":%.17g,"
           "\"health\":%.17g,\"max_health\":%.17g,\"class_weapons\":%d,\"built_weapons\":%d,"
           "\"collision_bodies\":%d,\"updates\":%d}\n",
           pcLevel, pcClass, pcClassFile, bSpawned ? "true" : "false", (double)ground.restY,
           (double)spawn.x, (double)spawn.y, (double)spawn.z, (double)spawn.health,
           (double)spawn.maxHealth, spawn.classWeaponCount, spawn.builtWeaponCount,
           spawn.collisionBodyCount, iUpdates);
    return 0;
}
