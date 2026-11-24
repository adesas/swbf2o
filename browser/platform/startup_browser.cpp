// Browser replacement for the startup hooks that lived in pcMain.cpp.
//
// pcMain.cpp is excluded from the build: it is the Win32 entry point (WinMain, the message
// pump, the D3D9 device, and a __try/__except crash guard that wasm32 clang cannot lower).
// But two of its functions are not platform code at all - they are the engine's own memory
// bootstrap, called from RedEngineFL/Memory/RedMemory.cpp, and the game cannot initialise
// without them. Their bodies are reproduced here from the decompilation (0x006171d0 and
// 0x00617200), with one deliberate difference: the application arena comes from malloc
// instead of a 64 MB static array in the BSS, because the wasm linear memory is grown by the
// host and a static array would reserve it before the module even validates.

#include <stdlib.h>

#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Memory/RedMemoryMi.h"
#include "Battlefront2/Source/GameMemory.h"

// The debug heap handle, defined here because the only file that used it (pcMain.cpp) is
// excluded. RedMemory.h exposes the heap ids as ints; this is just the bookkeeping slot the
// original startup kept so the debug heap could be re-selected after a device reset.
int PCDebugHeap = 0;

static void Startup_InitApplicationMemory(void)
{
    // 0x00617190: the original reserved 64 MB of application memory and handed the range to
    // GameMemory, which builds its pools out of it.
    static const unsigned int uiArenaSize = 0x04000000;
    char* pBase = (char*)malloc(uiArenaSize);
    if (!pBase)
    {
        return;
    }
    GameMemory::InitApplicationMemory(pBase, pBase + uiArenaSize);
}

static void Startup_DebugHeap(void)
{
    // 0x006171d0
    void* p = malloc(0x300000);
    if (!p)
    {
        return;
    }
    PCDebugHeap = RedCreateHeap(p, (char*)p + 0x300000, 0x10, "Debug");
    RedSetDebugHeap(PCDebugHeap);
}

// FUNCTION: SWBF2 0x00617200
extern "C" void Startup_RedInitHeap(int, int)
{
    RedMiConfigure(0);
    Startup_InitApplicationMemory();
    Startup_DebugHeap();
}
