/* The wasm side of the combat oracle.
 *
 * The browser module (build/browser/swbf2.js) cannot call this harness yet: its source list and
 * its exported-function list are both fixed by tools/build_browser.sh, and adding to them is a
 * change to a file this track does not own. So the wasm half of this oracle is a standalone
 * module: the same engine sources, the same platform seams, the same harness
 * (spike/combat_harness.cpp) and the same fight script (CombatHarnessRun), linked for wasm32
 * with tools/build_browser.sh's flags. The integration this module needs in the browser module
 * is three functions and is written down in docs/combat-oracle.md.
 *
 * The three exports are the same names the browser module uses for the same jobs
 * (browser/main.cpp:178-193), so the probe script reads one module or the other:
 *
 *   swbf2_combat_run(level, player_class, player_class_file, target_class, target_class_file,
 *                    target_count) -> 1 when the fight ran, 0 when it did not
 *   swbf2_combat_report() -> the fight's JSON report, as the harness built it
 *   swbf2_log_line(i) -> the i'th line of the engine's own log ring
 *
 * The data root is not a parameter here: in the wasm build the engine's file layer reads the
 * game's data from the VFS at /data (browser/platform/pblfile_browser.cpp:79-103), which is
 * where the probe stages it. The native build reads the same bytes from a directory, named by
 * SWBF2_DATA_ROOT.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../spike/combat_harness.h"
#include "Battlefront2/Source/Entity.h"
#include "RedEngineFL/Graphics/RedCamera.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* The display mode the browser platform layer keeps (browser/platform/pc_globals.cpp). The
   renderer seam's screen aspect ratio is computed from it (pcRedRenderer_browser.cpp:814-821),
   and the chase camera's field of view is computed from that, so the oracle gives it the same
   size the browser side gives it. */
extern int pcWidth;
extern int pcHeight;

static const unsigned int ORACLE_VIEWPORT_WIDTH = 1280;
static const unsigned int ORACLE_VIEWPORT_HEIGHT = 720;

/* The engine's own log ring, the same shape as the browser module's (browser/main.cpp:139-193):
   the engine's messages and the harness's arrive in one place, in one order, and the probe can
   pull them out one at a time. */
#define LOG_LINES 64
static char g_log[LOG_LINES][256];
static int g_logHead = 0;
static int g_logCount = 0;

void __cdecl Log(char const* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char* line = g_log[g_logHead];
    vsnprintf(line, sizeof(g_log[0]), fmt, ap);
    va_end(ap);

    g_logCount++;
    g_logHead = (g_logHead + 1) % LOG_LINES;

    fflush(stdout);
    fputc('\n', stdout);
    fputs(line, stdout);
    fflush(stdout);
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

/* The fight itself: the harness's own script, the harness's own report. Both builds call this
   same function, so the two builds fight the same fight by construction. */
static int RunFight(const char* pcLevel, const char* pcPlayerClass,
                    const char* pcPlayerClassFile, const char* pcTargetClass,
                    const char* pcTargetClassFile, int iTargets)
{
    pcWidth = (int)ORACLE_VIEWPORT_WIDTH;
    pcHeight = (int)ORACLE_VIEWPORT_HEIGHT;
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
    return bRan ? 1 : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int swbf2_combat_run(const char* pcLevel,
                                                     const char* pcPlayerClass,
                                                     const char* pcPlayerClassFile,
                                                     const char* pcTargetClass,
                                                     const char* pcTargetClassFile,
                                                     int iTargets)
{
    if (!pcLevel || !pcPlayerClass || !pcPlayerClassFile || !pcTargetClass ||
        !pcTargetClassFile)
    {
        return 0;
    }
    return RunFight(pcLevel, pcPlayerClass, pcPlayerClassFile, pcTargetClass, pcTargetClassFile,
                    iTargets);
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_combat_report(void)
{
    return CombatHarnessReport();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* swbf2_log_line(int i)
{
    if (i < 0 || i >= LOG_LINES || i >= g_logCount)
    {
        return "";
    }
    return g_log[(g_logHead + i) % LOG_LINES];
}

/* The module's own main, for running it under node without the probe script:
 *   node build/combat_oracle/swbf2_combat.js tat/tat2.lvl rep_inf_ep3_rifleman
 *       "side/rep.lvl;rep_inf_ep3_rifleman" cis_inf_rifleman "side/cis.lvl;cis_inf_rifleman" 4
 * The data comes from the VFS, which the probe script stages before calling main. */
int main(int argc, char** argv)
{
    const char* pcLevel = (argc > 1) ? argv[1] : "tat/tat2.lvl";
    const char* pcPlayerClass = (argc > 2) ? argv[2] : "rep_inf_ep3_rifleman";
    const char* pcPlayerClassFile =
        (argc > 3) ? argv[3] : "side/rep.lvl;rep_inf_ep3_rifleman";
    const char* pcTargetClass = (argc > 4) ? argv[4] : "cis_inf_rifleman";
    const char* pcTargetClassFile =
        (argc > 5) ? argv[5] : "side/cis.lvl;cis_inf_rifleman";
    const int iTargets = (argc > 6) ? atoi(argv[6]) : 4;

    const int rc = RunFight(pcLevel, pcPlayerClass, pcPlayerClassFile, pcTargetClass,
                            pcTargetClassFile, iTargets);
    printf("COMBAT_ORACLE_JSON %s\n", CombatHarnessReport());
    return rc ? 0 : 2;
}
