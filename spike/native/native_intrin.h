/* The host target's intrinsics are built into clang; wasm's are not.
 *
 * spike/shim/intrin.h defines __rdtsc and __cpuid so that the engine's files that include the
 * MSVC intrinsic headers parse. On wasm32 those are ordinary names and the shim's definitions
 * are the only ones; on x86-64 clang has them as builtins, and a file that defines a builtin
 * with a different type is an error ("functions that differ only in their return type cannot be
 * overloaded").
 *
 * The shim is not ours to edit, so the native build renames the two names before the shim sees
 * them. The semantics are the same as the wasm build's: the intrinsics do nothing on either
 * target - the shim's bodies return zero - and the engine's own use of them is a timing
 * measurement that the browser does not rely on.
 */
#define __rdtsc swbf2_host_no_rdtsc
#define __cpuid swbf2_host_no_cpuid

/* The same clash for the bit-scan intrinsics: on x86 (i386 and x86-64) _BitScanForward and
 * _BitScanReverse are clang builtins, and spike/shim/intrin.h defines them as ordinary
 * functions, which is an error ("definition of builtin function"). wasm32 has neither builtin,
 * so the wasm build parses the shim's definitions as the only ones. */
#define _BitScanForward swbf2_host_no_BitScanForward
#define _BitScanReverse swbf2_host_no_BitScanReverse

/* The Windows va_list convention, for the one engine file that needs it.
 *
 * Battlefront2/Source/SaveFsm.cpp calls the engine's own PblUnicode::uvsnprintf and passes a
 * va_list the way Windows passes one: as a char*. On x86-64 a va_list is an array of one struct,
 * so that call does not match the engine's declaration and the file does not compile for the
 * host. The declaration is here; spike/native/native_uni_va.cpp defines it. */
#if defined(__x86_64__) || defined(_WIN64)
/* Only where va_list is not itself a char*. The native build is 32-bit (-m32), and on i386
 * va_list is a char*, which is what the engine passes - so on the target the oracle actually
 * uses this overload is neither needed nor compilable (it would have the same signature as the
 * engine's own declaration). The 64-bit case is kept so the same source still builds if
 * someone tries a 64-bit native build and wants to see the same engine behaviour. */
namespace PblUnicode
{
int __cdecl uvsnprintf(unsigned short* pusDest, unsigned int uiCount,
                       unsigned short const* pusFormat, char* pArgs);
}
#endif
