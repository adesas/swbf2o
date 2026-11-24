/* The native side of the same seam the browser adapter fills. Kept tiny on purpose: a clock and one
 * flag. Everything else is the engine's own code, compiled without emscripten. */
#include <sys/time.h>
double swbf2_now_ms(void)
{
    struct timeval tv; gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}
bool netInShell = false;

namespace NetSocket { void StartSockets() {} }
