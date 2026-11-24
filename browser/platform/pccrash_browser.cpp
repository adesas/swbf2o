// Browser / portable implementation of OpenSWBF2's pcCrash diagnostics namespace.
// Replaces Battlefront2/Source/pcCrash.cpp (Win32 SEH + MiniDumpWriteDump + watchdog thread)
// with a safe non-blocking implementation for wasm32 and 32-bit native oracle builds.

#include "Battlefront2/Source/pcCrash.h"

#include <stdio.h>
#include <stdlib.h>

extern void Log(const char* fmt, ...);

namespace pcCrash
{
void Install() {}
void Reinstall() {}
long Filter(struct _EXCEPTION_POINTERS*) { return 0; }
void __cdecl Exit(int iCode) { exit(iCode); }
void SetLogPath(char const*) {}
void SetCrashTest(char const*) {}
void RunCrashTest() {}
void RunMissionCrashTest() {}
void FatalInit(char const* pcWhat, unsigned int uiSize)
{
    Log("pcCrash::FatalInit(%s, %u)", pcWhat ? pcWhat : "?", uiSize);
}
bool SimulateAllocFailure(char const*) { return false; }
bool FindCommandArg(char const*, char const**, int*) { return false; }
void ReserveLowAddressSpace() {}
bool IsLargeAddressAware() { return true; }
void __cdecl Check(char const* pcExpr, char const* pcFile, int iLine)
{
    Log("pcCrash::Check failed: %s (%s:%d)", pcExpr ? pcExpr : "?", pcFile ? pcFile : "?", iLine);
}
void Tick() {}
bool SetHangTimeout(int) { return true; }
bool Supervised() { return false; }
void StartWatchdog() {}
char const* GetBuildId() { return "swbf2-web-openswbf2"; }
char const* GetCrashTest() { return 0; }
void FatalReset(long hr, unsigned int uiFailures, unsigned int uiElapsedMs)
{
    Log("pcCrash::FatalReset(hr=0x%lx, failures=%u, elapsed=%ums)", hr, uiFailures, uiElapsedMs);
}
}  // namespace pcCrash
