/* The native side of the engine's memory bootstrap.
 *
 * The browser's seam (browser/platform/startup_browser.cpp) reproduces the two startup hooks
 * from pcMain.cpp: the application arena and the debug heap. The native oracle does the same
 * thing, for the same reason and with the same calls: the arena has to come from somewhere the
 * engine can use, and the engine's memory code expects the 32-bit world it was built for
 * (RedMemory.cpp:306, :307, :381 and :382 cast the arena's start and end to unsigned int).
 *
 * The native build is 32-bit (-m32, see tools/compile_native.sh), so an ordinary mapping is
 * already in the address range the engine's casts can carry, and the browser's seam takes the
 * arena from malloc for the same reason on wasm32. The calls the engine makes, the sizes it
 * uses and the layout are the same on both sides.
 */

#include <stddef.h>
#include <sys/mman.h>

#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Memory/RedMemoryMi.h"
#include "Battlefront2/Source/GameMemory.h"

// The debug heap handle, defined here because the only file that used it (pcMain.cpp) is
// excluded. RedMemory.h exposes the heap ids as ints; this is the bookkeeping slot the
// original startup kept so the debug heap could be re-selected after a device reset.
int PCDebugHeap = 0;

namespace
{
// The sizes the original startup used (pcMain.cpp:0x00617190 and :0x006171d0).
const size_t kArenaSize = 0x04000000;
const size_t kDebugHeapSize = 0x300000;

void* MapArena(size_t uiSize)
{
    void* p = mmap(0, uiSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
}  // namespace

// FUNCTION: SWBF2 0x00617200
extern "C" void Startup_RedInitHeap(int, int)
{
    RedMiConfigure(0);
    char* pArena = (char*)MapArena(kArenaSize);
    if (!pArena)
    {
        return;
    }
    GameMemory::InitApplicationMemory(pArena, pArena + kArenaSize);

    char* pDebug = (char*)MapArena(kDebugHeapSize);
    if (!pDebug)
    {
        return;
    }
    PCDebugHeap = RedCreateHeap(pDebug, pDebug + kDebugHeapSize, 0x10, "Debug");
    RedSetDebugHeap(PCDebugHeap);
}
