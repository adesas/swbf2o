/* The native side of the ground oracle.
 *
 * The browser build runs the same harness (spike/level_harness.cpp) inside the wasm module; this
 * program runs it in a native build of the same engine sources (build/native_core.a, compiled by
 * tools/compile_native.sh with the same clang front-end, the same shim headers and the same
 * flags, for the host target instead of wasm32).
 *
 * Both read the same level through the same engine loader and then drop the same capsules; the
 * numbers they print are compared by tools/ground_oracle_check.py.
 *
 * The data root is the environment variable SWBF2_DATA_ROOT, because the browser's file layer
 * reads the game's data from the VFS at /data while the native build reads it from a directory.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../spike/level_harness.h"
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

/* The sample points: a grid over the level, plus the origin. They are the same in both builds
 * because both read them from the command line. */
int main(int argc, char** argv)
{
    const char* pcLevel = (argc > 1) ? argv[1] : "tat/tat2.lvl";
    const char* pcRoot = (argc > 2) ? argv[2] : 0;
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

    // The marker tells the check script which line is the result.
    printf("GROUND_ORACLE_JSON {\"level\":\"%s\",\"collision_meshes\":%d,\"samples\":[", pcLevel,
           LevelHarnessCollisionMeshCount());

    /* The same points the browser build is asked for, by default: a grid across the world's
       extent. */
    static const float afPoints[] = {
        0.0f, 0.0f, 10.0f, 0.0f, -10.0f, 0.0f, 0.0f, 10.0f, 0.0f, -10.0f,
        25.0f, 25.0f, -25.0f, 25.0f, 25.0f, -25.0f, -25.0f, -25.0f,
        50.0f, 0.0f, 0.0f, 50.0f, -50.0f, 0.0f, 0.0f, -50.0f,
        100.0f, 100.0f, -100.0f, 100.0f, 100.0f, -100.0f, -100.0f, -100.0f,
    };
    const int iCount = (int)(sizeof(afPoints) / sizeof(afPoints[0]) / 2);

    for (int i = 0; i < iCount; ++i)
    {
        GroundProbeResult result;
        const bool bOk = LevelHarnessDrop(afPoints[i * 2], afPoints[i * 2 + 1], 0.35f, 0.85f, &result);
        /* %.17g: the values are floats, and a float promoted to a double needs seventeen
           significant digits to round-trip exactly. The browser prints the same numbers as JSON,
           which also round-trips exactly, so the comparison in tools/ground_oracle_check.py is
           between the two builds' exact values and not between their rounded forms. */
        printf("%s{\"x\":%.6f,\"z\":%.6f,\"ok\":%s,\"terrain\":%.17g,\"ground\":%.17g,\"rest\":%.17g,"
               "\"steps\":%d}",
               i ? "," : "", afPoints[i * 2], afPoints[i * 2 + 1], bOk ? "true" : "false",
               (double)result.terrainY, (double)result.groundY, (double)result.restY, result.steps);
    }
    printf("]}\n");
    return 0;
}
