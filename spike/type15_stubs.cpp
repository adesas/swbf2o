/* Stand-ins for the few symbols NetGame.cpp's global constructors reach for. The check calls four
 * readers and four writers from NetGame.cpp; none of them need these, but the file's globals are
 * constructed when the module starts, and a constructor that calls a missing function traps.
 *
 * Nothing here is a model of engine behaviour: the clock is a clock, and the check never reads it. */
#include <emscripten/emscripten.h>

double swbf2_now_ms(void)
{
    return (double)emscripten_get_now();
}

/* The packet layer hands finished packets back to the game layer. The check does not run the game,
 * so the callback releases nothing. */
namespace NetGameP { void FreePackets() {} }
