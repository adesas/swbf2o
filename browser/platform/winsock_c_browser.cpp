/* The C-linkage closesocket that the engine's NetGame.cpp declares for itself.
 *
 * NetGame.cpp:8473 declares the Win32 socket call with C linkage and an unsigned int parameter:
 *
 *     extern "C" int __stdcall closesocket(unsigned int s);
 *
 * That is a different symbol from the C++-linkage closesocket(int) in winsock_browser.cpp, which
 * is the one the shim's winsock2.h declares. On Windows both spellings are the same C function;
 * here they are two symbols, so both are provided and the C one does the same work.
 *
 * The work is not repeated here: winsock_browser.cpp exposes its implementation under a second
 * name, because one translation unit cannot declare the same name with two different linkages.
 */

extern "C" int __stdcall closesocket(unsigned int s);

// winsock_browser.cpp: the C++-linkage implementation, under a second name so that this
// translation unit can call it without declaring "closesocket" twice.
extern int net_close_socket(int s);

extern "C" int __stdcall closesocket(unsigned int s)
{
    return net_close_socket((int)s);
}
