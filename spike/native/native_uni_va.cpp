/* The Windows va_list convention, for the one engine file that needs it.
 *
 * Battlefront2/Source/SaveFsm.cpp calls the engine's own PblUnicode::uvsnprintf and passes a
 * va_list the way Windows passes one: as a char* pointing at the argument. On x86-64 a va_list
 * is an array of one struct, so that call does not match the engine's declaration and the file
 * does not compile for the host. (wasm32 agrees with Windows here - both pass a va_list as a
 * pointer - which is why the file compiles for wasm and not for the host.)
 *
 * This overload makes the file compile. It cannot format faithfully: the Windows calling
 * convention and the System V one put the arguments in different places, so a char* from the
 * first is not a va_list for the second. Rather than format something wrong, it stops. The
 * oracle never reaches it: the call is made only when the localisation database has the string,
 * and the native oracle build loads no localisation chunk.
 */

#include <stdio.h>
#include <stdlib.h>

#if defined(__x86_64__) || defined(_WIN64)
namespace PblUnicode
{
int uvsnprintf(unsigned short*, unsigned int, unsigned short const*, char*)
{
    fprintf(stderr, "the native build cannot format a Windows-style va_list\n");
    abort();
}
}
#endif
