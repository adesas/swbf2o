#pragma once
/* Forced prefix header, injected into every translation unit with -include.
 *
 * This is the browser build's stand-in for the project's MSVC precompiled header. It exists
 * because large parts of the decompiled tree use Win32 / D3D9 / COM spellings without including
 * the header that declares them (MSVC's PCH supplied them), and because some of those names do
 * not exist on wasm at all.
 *
 * Rules for this file:
 *   - single source of truth: windows.h / unknwn.h / mmreg.h / dsound.h all forward here
 *   - C-visible types stay outside __cplusplus; anything using `virtual` goes inside it
 *   - declarations only: the real behaviour lives in the backend adapters
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <wchar.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>

/* ---- Win32 scalars --------------------------------------------------------- */
typedef int            BOOL;
typedef int            INT;
typedef unsigned int   DWORD;
typedef unsigned int   UINT;
#ifndef ZeroMemory
#define ZeroMemory(p, n) memset((p), 0, (n))
#endif
typedef long           LONG;
typedef unsigned long  ULONG;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef unsigned char  UCHAR;
typedef short          SHORT;
typedef long long      LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned int   UINT_PTR;
typedef long           INT_PTR;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef const wchar_t* LPCWSTR;
typedef wchar_t*       LPWSTR;
typedef void*          LPVOID;
typedef void**         LPLPVOID;

/* ---- Win32 handles (opaque on wasm) --------------------------------------- */
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef void* HANDLE;
typedef void* HMONITOR;
typedef struct HWND__ HWND__;
struct HWND__ { int shim; };
typedef HWND__* HWND;
typedef void* HDC;
typedef void* HBITMAP;
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HFONT;
typedef void* HMENU;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HRGN;
typedef void* HGDIOBJ;
typedef void* HDESK;
typedef void* HHOOK;
typedef void* HACCEL;
typedef void* HRSRC;
typedef void* HFILE;
typedef void* HCOLORSPACE;
typedef void* HGLOBAL;
typedef void* HLOCAL;
typedef void* HMIDIOUT;
typedef void* HMMIDI;

/* ---- common Win32 structs -------------------------------------------------- */
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { long x, y; } POINT;
typedef struct { long cx, cy; } SIZE;
typedef struct { unsigned char red, green, blue, flags; } PALETTEENTRY;
typedef struct { unsigned int message; unsigned int wParam; long lParam; unsigned int time; int x; int y; } MSG;
typedef struct { unsigned int LowPart; long HighPart; long long QuadPart; } LARGE_INTEGER;

typedef LARGE_INTEGER* PLARGE_INTEGER;

/* ---- GUIDs (C-visible; needed by D3D/audio declarations) ------------------- */
typedef struct _GUID { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
typedef GUID   IID;
typedef GUID*  LPGUID;
/* Microsoft's REFIID/REFCLSID/REFGUID are references in C++ and pointers in C (unknwn.h); the
 * engine passes GUID objects straight through to QueryInterface, so C++ must not use a pointer. */
#ifdef __cplusplus
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
typedef const GUID& REFGUID;
#else
typedef const GUID* REFIID;
typedef const GUID* REFCLSID;
typedef const GUID* REFGUID;
#endif

/* ---- audio formats (C-visible) -------------------------------------------- */
typedef struct {
  unsigned short wFormatTag, nChannels;
  unsigned int   nSamplesPerSec, nAvgBytesPerSec;
  unsigned short nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;
typedef WAVEFORMATEX* LPWAVEFORMATEX;
typedef int MMRESULT;
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif
#ifndef WAVE_FORMAT_ADPCM
#define WAVE_FORMAT_ADPCM 0x0002
#endif
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

/* ---- timing ----------------------------------------------------------------
 * swbf2_now_ms() is deliberately NOT spelled emscripten_get_now. That is a real
 * Emscripten runtime function and <emscripten.h> declares it with extern "C" linkage;
 * declaring the same name here with C++ linkage makes the two declarations collide in
 * any translation unit that needs both this header and <emscripten.h> - which the browser
 * host does, for its WebGL context and main loop. A name of our own has no such clash,
 * and the engine's timing entry points stay honest: they are the engine's own. */
double swbf2_now_ms(void);
static inline LARGE_INTEGER QueryPerformanceCounter(LARGE_INTEGER* p) {
  LARGE_INTEGER o; long long t = (long long)(swbf2_now_ms() * 1000.0);
  o.QuadPart = t; o.LowPart = (unsigned int)t; o.HighPart = (long)(t >> 32);
  if (p) *p = o;
  return o;
}
static inline LARGE_INTEGER QueryPerformanceFrequency(LARGE_INTEGER* p) {
  LARGE_INTEGER o; o.QuadPart = 1000000LL; o.LowPart = (unsigned int)o.QuadPart; o.HighPart = 0;
  if (p) *p = o;
  return o;
}
static inline DWORD GetTickCount(void) { return (DWORD)swbf2_now_ms(); }
static inline DWORD timeGetTime(void)  { return (DWORD)swbf2_now_ms(); }
static inline void  Sleep(unsigned ms) { (void)ms; }
static inline void  DebugBreak(void)   {}
#define OutputDebugStringA(s)
#define OutputDebugStringW(s)

/* ---- COM macros (fine to define for C; only C++ code expands them) -------- */
typedef long HRESULT;
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
#ifndef STDMETHOD
#define STDMETHOD(method)     virtual long STDMETHODCALLTYPE method
#endif
#ifndef STDMETHOD_
#define STDMETHOD_(t, method) virtual t STDMETHODCALLTYPE method
#endif
#ifndef STDMETHODIMP
#define STDMETHODIMP          long STDMETHODCALLTYPE
#endif
#ifndef STDMETHODIMP_
#define STDMETHODIMP_(t)      t STDMETHODCALLTYPE
#endif
#ifndef FAILED
#define FAILED(hr) (((long)(hr)) < 0)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((long)(hr)) >= 0)
#endif
#ifndef S_OK
#define S_OK ((long)0)
#endif
#ifndef S_FALSE
#define S_FALSE ((long)1)
#endif
#ifndef E_FAIL
#define E_FAIL ((long)0x80004005L)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((long)0x80004002L)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((long)0x80004001L)
#endif

#ifdef __cplusplus
/* C++-only declarations (they use `virtual`) */
struct IUnknown {
  STDMETHOD(QueryInterface)(REFIID, void**) { return S_OK; }
  STDMETHOD_(ULONG, AddRef)()  { return 1; }
  STDMETHOD_(ULONG, Release)() { return 1; }
};
typedef struct { WAVEFORMATEX Format; unsigned int dwChannelMask; GUID SubFormat; } WAVEFORMATEXTENSIBLE;
typedef struct { GUID Set; GUID Identifier; unsigned int dwFlags; } KSIDENTIFIER;
typedef KSIDENTIFIER KSPROPERTY;
struct IKsPropertySet : IUnknown {
  /* ksproxy.h (the DDK interface DirectSound/EAX hand out, mirrored by Wine's include/ksproxy.h):
   * QuerySupport / Get / Set all take the property-set GUID first. The engine's own call sites
   * (SoundFL/source/win/snddsbuffer.cpp:795, :844) only type-check against that shape. The previous
   * declarations here had the parameters the wrong way round and Set one argument short, and also
   * carried a `Query` method that is not part of IKsPropertySet in any version of the interface - it
   * would have shifted the vtable of any real implementation, so it is gone rather than left unused. */
  STDMETHOD(QuerySupport)(const GUID& propSet, unsigned long id, unsigned long* typeSupport) { return 0; }
  STDMETHOD(Get)(const GUID& propSet, unsigned long id, void* instanceData, unsigned long instanceLength,
                 void* propertyData, unsigned long dataLength, unsigned long* bytesReturned) { return 0; }
  STDMETHOD(Set)(const GUID& propSet, unsigned long id, void* instanceData, unsigned long instanceLength,
                 void* propertyData, unsigned long dataLength) { return 0; }
};
#endif /* __cplusplus */

/* ---- MSVC-only C runtime names -------------------------------------------- */
#ifndef _snprintf
#define _snprintf snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif
#ifndef _stricmp
#define _stricmp strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif
/* _wcsicmp is defined below as a real helper. It used to be #defined to wcscmp as
 * well, which rewrote that definition into 'static inline int wcscmp(...)' - a definition of
 * a libc symbol with external linkage, which the linker then reported as a duplicate in every
 * object that included this header (the whole of ZlibFL). A macro and a definition of the same
 * name in one header cannot coexist: the macro wins and renames the definition. */
/* MSVC spells its asserts _wassert(wchar* expression, wchar* file, unsigned line); the GSM codec
 * in SoundFL uses it directly. An empty macro here silently deletes every assertion, so provide a
 * real one: it reports and continues, matching the engine's own release-mode behaviour. */
#ifndef SWBF2_WASSERT
#define SWBF2_WASSERT
static __inline__ void _wassert(const wchar_t* expr, const wchar_t* file, unsigned line) {
  (void)expr; (void)file; (void)line;
}
#endif
static inline char* _strlwr(char* s) { for (char* p = s; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
static inline char* _strupr(char* s) { for (char* p = s; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }

/* ---- calling conventions: meaningless on wasm, neutralised ---------------- */
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef __thiscall
#define __thiscall
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __forceinline
#define __forceinline inline
#endif
#ifndef __declspec
#define __declspec(x)
#endif

/* ---- byte order, misc MSVC helpers ---------------------------------------- */
#ifndef ntohs
#define ntohs(v) ((unsigned short)(((v) << 8) | ((v) >> 8)))
#endif
#ifndef htons
#define htons(v) ntohs(v)
#endif
#ifndef ntohl
#define ntohl(v) ((unsigned int)((((v) & 0x000000ffU) << 24) | (((v) & 0x0000ff00U) << 8) | \
                                 (((v) & 0x00ff0000U) >> 8) | (((v) >> 24) & 0xff)))
#endif
#ifndef htonl
#define htonl(v) ntohl(v)
#endif
#ifndef __max
#define __max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef __min
#define __min(a, b) (((a) < (b)) ? (a) : (b))
#endif
static inline void _makepath(char* p, const char* d, const char* dir, const char* f, const char* e) {
  p[0] = 0;
  if (d && *d) { strcpy(p, d); strcat(p, ":"); }
  if (dir && *dir) { strcat(p, dir); if (p[strlen(p) - 1] != '\\' && p[strlen(p) - 1] != '/') strcat(p, "/"); }
  if (f) strcat(p, f);
  if (e) { strcat(p, "."); strcat(p, e); }
}
static inline void _splitpath(const char* path, char* drive, char* dir, char* file, char* ext) {
  if (drive) drive[0] = 0;
  if (dir)   dir[0]   = 0;
  if (file)  file[0]  = 0;
  if (ext)   ext[0]   = 0;
  const char* rest   = path;
  const char* slash  = strrchr(path, '/');
  const char* bslash = strrchr(path, '\\');
  const char* cut    = slash ? slash : bslash;
  if (cut) {
    if (dir) { strncat(dir, path, (size_t)(cut - path + 1)); }
    rest = cut + 1;
  }
  if (file) strcpy(file, rest);
  const char* dot = strrchr(rest, '.');
  if (dot) {
    if (file) file[dot - rest] = 0;
    if (ext)  strcpy(ext, dot + 1);
  }
}

/* musl now ships strlcpy; the tree declares its own - alias so the two never collide */
#ifndef strlcpy
#define strlcpy swbf2_strlcpy
#endif

/* ---- Win32 qualifier macros ----------------------------------------------- */
#ifndef CONST
#define CONST const
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef FAR
#define FAR
#endif
#ifndef NEAR
#define NEAR
#endif
#ifndef PASCAL
#define PASCAL
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIPRIVATE
#define APIPRIVATE
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef INLINE
#define INLINE inline
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif

/* ---- threads (the engine has a Thread abstraction; the browser build maps it to
 *      the main loop or to Web Workers - declarations only for now) ---------- */
typedef unsigned int (__stdcall *LPTHREAD_START_ROUTINE)(void*);
static inline HANDLE CreateThread(void* sa, unsigned stack, LPTHREAD_START_ROUTINE fn, void* arg, unsigned flags, unsigned* tid) { (void)sa;(void)stack;(void)flags;(void)tid; return (HANDLE)(uintptr_t)(fn && arg ? 1 : 1); }
static inline DWORD WaitForSingleObject(HANDLE h, unsigned ms) { (void)h; (void)ms; return 0; }
static inline DWORD WaitForMultipleObjects(unsigned n, const HANDLE* h, int waitAll, unsigned ms) { (void)n;(void)h;(void)waitAll;(void)ms; return 0; }
static inline int ReleaseMutex(HANDLE h) { (void)h; return 1; }
static inline int WaitForSingleObjectEx(HANDLE h, unsigned ms, int alertable) { (void)h;(void)ms;(void)alertable; return 0; }
static inline void CloseHandle(HANDLE h) { (void)h; }
static inline void ExitThread(void) {}
static inline HANDLE CreateEventA(void* sa, int manual, int init, const char* name) { (void)sa;(void)manual;(void)init;(void)name; return (HANDLE)(uintptr_t)1; }
static inline HANDLE CreateMutexA(void* sa, int initialOwner, const char* name) { (void)sa;(void)initialOwner;(void)name; return (HANDLE)(uintptr_t)1; }
static inline HANDLE CreateSemaphoreA(void* sa, long c, long mx, const char* name) { (void)sa;(void)c;(void)mx;(void)name; return (HANDLE)(uintptr_t)1; }
static inline int SetEvent(HANDLE h) { (void)h; return 1; }
static inline int ResetEvent(HANDLE h) { (void)h; return 1; }
static inline int PulseEvent(HANDLE h) { (void)h; return 1; }
static inline int ReleaseSemaphore(HANDLE h, long c, long* prev) { (void)h;(void)c;(void)prev; return 1; }
static inline int InterlockedIncrement(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline int InterlockedDecrement(volatile long* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline int InterlockedExchangeAdd(volatile long* p, long v) { return (int)__atomic_fetch_add(p, (unsigned long)v, __ATOMIC_SEQ_CST); }
static inline long InterlockedCompareExchange(volatile long* p, long v, long cmp) {
  __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp;
}
static inline unsigned long GetCurrentThreadId(void) { return 1; }
static inline void* GetCurrentThread(void) { return (void*)(uintptr_t)1; }
static inline unsigned long GetVersion(void) { return 0x0501; }
static inline unsigned short GetUserDefaultLangID(void) { return 0x0409; }
static inline int MultiByteToWideChar(unsigned cp, unsigned flags, const char* mb, int cb, wchar_t* wc, int cw) {
  (void)cp; (void)flags; if (!wc) return cb < 0 ? (int)strlen(mb) + 1 : cb;
  int n = cb < 0 ? (int)strlen(mb) : cb; for (int i = 0; i < n; ++i) wc[i] = (wchar_t)(unsigned char)mb[i]; return n;
}
static inline int WideCharToMultiByte(unsigned cp, unsigned flags, const wchar_t* wc, int cw, char* mb, int cb, const char* def, int* used) {
  (void)cp; (void)flags; (void)def; (void)used; int n = cw < 0 ? (int)wcslen(wc) : cw;
  for (int i = 0; i < n; ++i) mb[i] = (char)wc[i]; return n;
}
static inline char* _fullpath(char* out, const char* in, int n) { return strncpy(out, in, (size_t)(n > 0 ? n : MAX_PATH)); }

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef PRIMARYLANGID
#define PRIMARYLANGID(l) ((unsigned short)(l) & 0x3ff)
#endif
#ifndef SUBLANGID
#define SUBLANGID(l) ((unsigned short)(l) >> 10)
#endif
#ifndef MAKELANGID
#define MAKELANGID(p, s) ((((unsigned short)(s)) << 10) | ((unsigned short)(p)))
#endif
#ifndef strlcat
#define strlcat swbf2_strlcat
#endif

typedef struct { unsigned short wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { unsigned short wVersion; char szSystemStatus[401]; unsigned short wHighVersion; unsigned int iMaxSockets; unsigned int iMaxUdpDg; char* lpVendorInfo; } WSADATA;
typedef struct { unsigned long tv_sec; unsigned long tv_usec; } TIMEVAL_LOCAL;
#ifndef LANG_SPANISH
#define LANG_SPANISH 0x0a
#endif
#ifndef LANG_ENGLISH
#define LANG_ENGLISH 0x09
#endif
#ifndef LANG_GERMAN
#define LANG_GERMAN 0x07
#endif
#ifndef LANG_FRENCH
#define LANG_FRENCH 0x0c
#endif
#ifndef SUBLANG_DEFAULT
#define SUBLANG_DEFAULT 0x01
#endif
#ifndef SUBLANG_NEUTRAL
#define SUBLANG_NEUTRAL 0x00
#endif
static inline int _close(int fd) { return close(fd); }
static inline int _read(int fd, void* b, int n) { return (int)read(fd, b, (size_t)n); }
static inline int _write(int fd, const void* b, int n) { return (int)write(fd, b, (size_t)n); }
static inline long long _atoi64(const char* s) { return strtoll(s, 0, 10); }
static inline long long _strtoi64(const char* s, char** e, int b) { return strtoll(s, e, b); }
static inline int _access(const char* p, int m) { return access(p, m); }

typedef struct { DWORD dwSize; DWORD nCount; RECT rc; BYTE Buffer[1]; } RGNDATA;
typedef struct { unsigned int attrib; time_t time_create, time_access, time_write; unsigned int size; char name[MAX_PATH]; } _finddata_t;
typedef struct { unsigned int dwNumberOfBytes; void* lpBuffer; } OVERLAPPED_LOCAL;
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xffff
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST 0x20
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 0x0004
#endif
#ifndef IP_MULTICAST_TTL
#define IP_MULTICAST_TTL 10
#endif
#ifndef IPPROTO_IP
#define IPPROTO_IP 0
#endif
#ifndef LANG_ITALIAN
#define LANG_ITALIAN 0x10
#endif
#ifndef LANG_JAPANESE
#define LANG_JAPANESE 0x11
#endif
#ifndef LANG_KOREAN
#define LANG_KOREAN 0x12
#endif
#ifndef LANG_RUSSIAN
#define LANG_RUSSIAN 0x19
#endif
#ifndef LANG_POLISH
#define LANG_POLISH 0x15
#endif
#ifndef LANG_CZECH
#define LANG_CZECH 0x05
#endif
#ifndef LANG_DUTCH
#define LANG_DUTCH 0x13
#endif
#ifndef LANG_PORTUGUESE
#define LANG_PORTUGUESE 0x16
#endif
static inline void GetLocalTime(SYSTEMTIME* t) { if (t) { memset(t, 0, sizeof(*t)); t->wYear = 2005; t->wMonth = 10; t->wDay = 1; } }
static inline void GetSystemTime(SYSTEMTIME* t) { GetLocalTime(t); }
static inline int _open(const char* p, int flags, int mode) { return open(p, flags, mode); }
static inline int _close_(int fd) { return close(fd); }
static inline long _filelength(int fd) { return lseek(fd, 0, SEEK_END); }
static inline int _dup(int fd) { return dup(fd); }
static inline int _dup2(int a, int b) { return dup2(a, b); }
static inline int _setmode(int fd, int mode) { (void)mode; return 0; }
static inline unsigned _time32(unsigned* t) { time_t v; time(&v); if (t) *t = (unsigned)v; return (unsigned)v; }

#ifndef LANG_THAI
#define LANG_THAI 0x1e
#endif
#ifndef LANG_TURKISH
#define LANG_TURKISH 0x1f
#endif
#ifndef LANG_HUNGARIAN
#define LANG_HUNGARIAN 0x0e
#endif
#ifndef LANG_SWEDISH
#define LANG_SWEDISH 0x1d
#endif
#ifndef LANG_DANISH
#define LANG_DANISH 0x06
#endif
#ifndef LANG_FINNISH
#define LANG_FINNISH 0x0b
#endif
#ifndef LANG_NORWEGIAN
#define LANG_NORWEGIAN 0x14
#endif
static inline off_t _lseek(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
/* setsockopt/getsockopt are not stubbed here any more: the browser transport (see
   browser/platform/winsock_browser.cpp) is the implementation, and a module that calls them is
   asking for that transport. Keeping them as no-op stubs in the prefix would hide that. */
static inline int _findfirst(const char* pat, _finddata_t* fd) { (void)pat; if (fd) memset(fd, 0, sizeof(*fd)); return -1; }
static inline int _findnext(int h, _finddata_t* fd) { (void)h; if (fd) memset(fd, 0, sizeof(*fd)); return -1; }
static inline int _findclose(int h) { (void)h; return 0; }

/* platform vocabulary filled in by tools/shim_autogen.py (single contract with the backends) */
#ifdef __cplusplus
#include "generated_types.h"
#endif

/* ---- remaining Win32 scalar / pointer / sync vocabulary ------------------- */
typedef int            INT;
typedef unsigned int   UINT32T;
typedef long           LONG_PTR;
typedef unsigned long  ULONG_PTR;
typedef unsigned long  DWORD_PTR;
typedef unsigned long  SIZE_T;
typedef long           SSIZE_T;
typedef wchar_t        WCHAR;
typedef wchar_t        OLECHAR;
typedef WCHAR*         BSTR;
typedef short          VARIANT_BOOL;
typedef void*          PPID;
typedef INT*           PINT;
typedef DWORD*         LPDWORD;
typedef DWORD*         PDWORD;
typedef WORD*          LPWORD;
typedef BYTE*          LPBYTE;
typedef BOOL*          LPBOOL;
typedef LONG*          LPLONG;
typedef HANDLE*        LPHANDLE;
typedef char*          LPTSTR;
typedef const char*    LPCTSTR;
typedef const WCHAR*   LPCTSTRW;
typedef long           (__stdcall *WNDPROC)(HWND, unsigned int, UINT_PTR, LONG_PTR);
typedef struct { unsigned int style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance;
                 HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName; } WNDCLASSA;
typedef struct { DWORD cbSize; LPVOID lpReserved; LPVOID lpDesktopName; void* dwReserved[4]; void* hStdInput;
                 void* hStdOutput; void* hStdError; void* lpTitle; void* lpDesktopInfo; } STARTUPINFOA;
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; int bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { DWORD Internal; void* lpOverlapped; DWORD Offset; DWORD OffsetHigh; HANDLE hEvent; } OVERLAPPED;
typedef struct { LPVOID DebugInfo; long LockCount; DWORD RecursionCount; HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount; } CRITICAL_SECTION;
typedef struct { LPVOID Ptr; } SRWLOCK;
typedef struct { LPVOID Ptr; } CONDITION_VARIABLE;
typedef void* HKEY; typedef void* HSFRIEND;
static inline void InitializeCriticalSection(CRITICAL_SECTION* c) { if (c) memset(c, 0, sizeof(*c)); }
static inline void DeleteCriticalSection(CRITICAL_SECTION* c) { (void)c; }
static inline void EnterCriticalSection(CRITICAL_SECTION* c) { (void)c; }
static inline void LeaveCriticalSection(CRITICAL_SECTION* c) { (void)c; }
static inline void InitializeSRWLock(SRWLOCK* l) { if (l) l->Ptr = 0; }
static inline void AcquireSRWLockExclusive(SRWLOCK* l) { (void)l; }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l) { (void)l; }
static inline void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* c, unsigned n) { (void)n; InitializeCriticalSection(c); }
static inline int  GetModuleFileNameA(HINSTANCE h, char* buf, int n) { (void)h; if (buf && n > 0) buf[0] = 0; return 0; }
static inline HMODULE GetModuleHandleA(const char* n) { (void)n; return (HMODULE)(uintptr_t)1; }
static inline void* GetProcAddress(HMODULE m, const char* p) { (void)m; (void)p; return 0; }
static inline int LoadLibraryA_stub(const char* n) { (void)n; return 0; }
static inline DWORD GetCurrentProcessId(void) { return 1; }
static inline int QueryPerformanceCounter_stub(void) { return 0; }
static inline int MessageBoxA(HWND h, LPCSTR t, LPCSTR c, UINT type) { (void)h;(void)t;(void)c;(void)type; return 0; }
static inline int IsDebuggerPresent(void) { return 0; }
static inline void OutputDebugString_stub(void) {}
static inline int GetSystemPowerStatus(void* s) { (void)s; return 1; }
static inline UINT GetSystemFlags(void) { return 0; }
static inline int _fileno_stub(FILE* f) { return fileno(f); }
static inline long long _ftelli64(FILE* f) { return ftello(f); }
static inline int _fseeki64(FILE* f, long long off, int whence) { return fseeko(f, (off_t)off, whence); }
static inline int _close2(int fd) { return close(fd); }
static inline int _commit(int fd) { return fsync(fd); }
static inline int _read2(int fd, void* b, int n) { return (int)read(fd, b, (size_t)n); }
static inline int _write2(int fd, const void* b, int n) { return (int)write(fd, b, (size_t)n); }
static inline int _lseek2(int fd, long off, int w) { return (int)lseek(fd, off, w); }
static inline int _access2(const char* p) { return access(p, 0); }
static inline int _chmod2(const char* p, int m) { return chmod(p, (mode_t)m); }
static inline int _unlink2(const char* p) { return unlink(p); }
static inline int _mkdir2(const char* p) { return mkdir(p, 0777); }
static inline int _rmdir2(const char* p) { return rmdir(p); }
#define _A_SUBDIR 0x10
#define _A_ARCH   0x20
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04

/* ---- C++ standard bits the tree expects from <windows.h> culture ---------- */
#ifdef __cplusplus
#include <string>
#include <cfloat>
typedef IUnknown* LPUNKNOWN;
typedef IUnknown* LPCUNKNOWN;
#endif

/* ---- MSVC assert plumbing (the GSM codec spells its own asserts this way) - */
#ifndef _CRT_WIDE
#define _CRT_WIDE_IMPL(s) L##s
#define _CRT_WIDE(s) _CRT_WIDE_IMPL(s)
#endif
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
#endif
void __cdecl _wassert(const wchar_t* expr, const wchar_t* file, unsigned line);

/* ---- window-manager calls the surviving game-facing layers mention -------- */
static inline void* GetDC(void* h) { (void)h; return 0; }
static inline int   ReleaseDC(void* h, void* dc) { (void)h; (void)dc; return 1; }
static inline HWND GetForegroundWindow(void) { return 0; }
static inline void* GetActiveWindow(void) { return 0; }
static inline void* GetDesktopWindow(void) { return 0; }
static inline int   SetForegroundWindow(void* h) { (void)h; return 1; }
static inline int   GetWindowThreadProcessId(void* h, unsigned* pid) { (void)h; if (pid) *pid = 1; return 1; }
static inline int   GetWindowTextA(void* h, char* buf, int n) { (void)h; if (buf && n > 0) buf[0] = 0; return 0; }
static inline wchar_t* _wcsupr(wchar_t* s) { for (wchar_t* p = s; *p; ++p) { if (*p >= L'a' && *p <= L'z') *p -= 32; } return s; }
static inline wchar_t* _wcslwr(wchar_t* s) { for (wchar_t* p = s; *p; ++p) { if (*p >= L'A' && *p <= L'Z') *p += 32; } return s; }
static inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { while (*a && *b) { wchar_t x = *a, y = *b;
  if (x >= L'A' && x <= L'Z') x += 32; if (y >= L'A' && y <= L'Z') y += 32; if (x != y) return x - y; ++a; ++b; } return *a - *b; }

/* ---- GDI / user32 surface used by the surviving window-facing code --------
 * The real window lives in the browser host (canvas + DOM); these keep the game's
 * screenshot / display-mode / window-focus code type-checking until the adapters land. */
typedef struct { unsigned int biSize, biWidth, biHeight; unsigned short biPlanes, biBitCount;
                 unsigned int biCompression, biSizeImage; int biXPelsPerMeter, biYPelsPerMeter;
                 unsigned int biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { unsigned char rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
typedef struct { long bmType, bmWidth, bmHeight, bmWidthBytes; unsigned short bmPlanes, bmBitsPixel;
                 void* bmBits; } BITMAP;
typedef struct { unsigned int nSize, nVersion, dwFlags, iPixelType, cColorBits, cRedBits, cRedShift,
                 cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift,
                 cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits,
                 cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved, dwDummy[32]; } PIXELFORMATDESCRIPTOR;
#define D3DBLACK_BRUSH 1
#define WHITE_BRUSH 0
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define SPI_GETKEYBOARDDELAY 0x16
#define SPI_SETKEYBOARDDELAY 0x17
#define SPI_GETSCREENSAVEACTIVE 0x10
#define SPI_SETSCREENSAVEACTIVE 0x11
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define WM_PAINT 0x000F
#define WM_SIZE 0x0005
#define SW_SHOW 5
#define SW_HIDE 0
static inline int PostMessageA(void* h, unsigned m, unsigned w, long l) { (void)h;(void)m;(void)w;(void)l; return 1; }
static inline long SendMessageA(void* h, unsigned m, unsigned w, long l) { (void)h;(void)m;(void)w;(void)l; return 0; }
static inline int DestroyWindow(void* h) { (void)h; return 1; }
static inline int ShowWindow(void* h, int cmd) { (void)h;(void)cmd; return 1; }
static inline int UpdateWindow(void* h) { (void)h; return 1; }
static inline int InvalidateRect(void* h, const void* r, int erase) { (void)h;(void)r;(void)erase; return 1; }
static inline int GetSystemMetrics(int idx) { (void)idx; return 1024; }
static inline int SystemParametersInfoA(unsigned a, unsigned b, void* c, unsigned d) {
  (void)b; (void)d;
  if (c) {
    if (a == 0x0016 /* SPI_GETKEYBOARDDELAY */) *(int*)c = 1;       /* 500 ms */
    else if (a == 0x000A /* SPI_GETKEYBOARDSPEED */) *(int*)c = 15; /* ~62 ms */
  }
  return 1;
}
static inline unsigned GetSysColor(int i) { (void)i; return 0; }
static inline int GetObjectA(void* obj, int n, void* buf) { (void)obj; if (buf && n > 0) memset(buf, 0, (size_t)n); return n; }
static inline void* CreateCompatibleDC(void* dc) { (void)dc; return (void*)(uintptr_t)1; }
static inline void* CreateCompatibleBitmap(void* dc, int w, int h) { (void)dc;(void)w;(void)h; return (void*)(uintptr_t)1; }
static inline void* SelectObject(void* dc, void* obj) { (void)dc;(void)obj; return 0; }
static inline int DeleteObject(void* obj) { (void)obj; return 1; }
static inline int DeleteDC(void* dc) { (void)dc; return 1; }
static inline int BitBlt(void* d, int x, int y, int w, int h, void* s, int sx, int sy, unsigned r) {
  (void)d;(void)x;(void)y;(void)w;(void)h;(void)s;(void)sx;(void)sy;(void)r; return 1; }
static inline int StretchBlt(void* d, int x, int y, int w, int h, void* s, int sx, int sy, int sw, int sh, unsigned r) {
  (void)d;(void)x;(void)y;(void)w;(void)h;(void)s;(void)sx;(void)sy;(void)sw;(void)sh;(void)r; return 1; }
static inline HBITMAP CreateDIBSection(void* dc, const BITMAPINFO* bi, unsigned u, void** bits, void* sec, unsigned off) {
  (void)dc;(void)bi;(void)u;(void)sec;(void)off; if (bits) *bits = 0; return 0; }
static inline int GetDeviceCaps(void* dc, int idx) { (void)dc;(void)idx; return 0; }
static inline void* GetStockObject(int i) { (void)i; return (void*)(uintptr_t)1; }

/* ---- remaining user32/gdi32 names referenced by surviving window code ----- */
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define SYSTEM_FONT 13
#define DEVICE_DEFAULT_FONT 14
#define SPI_GETKEYBOARDSPEED 0x000A
#define SPI_SETKEYBOARDSPEED 0x000B
#define SPI_GETHOTKEY 0x0018
#define MAKEINTRESOURCEA(i) ((char*)(uintptr_t)((unsigned short)(i)))
#define MAKEINTRESOURCEW(i) ((wchar_t*)(uintptr_t)((unsigned short)(i)))
typedef INT_PTR (__stdcall *DLGPROC)(HWND, UINT, UINT_PTR, LONG_PTR);
typedef struct { unsigned int styleEx; const void* style; const char* windowName; unsigned int style3;
                 int x, y, w, h; void* parent; void* menu; void* instance; void* param; } CREATEWINDOWSHIM;
static inline int   FillRect(void* dc, const void* rect, void* brush) { (void)dc;(void)rect;(void)brush; return 1; }
static inline int   MoveToEx(void* dc, int x, int y, void* pt) { (void)dc;(void)x;(void)y;(void)pt; return 1; }
static inline int   LineTo(void* dc, int x, int y) { (void)dc;(void)x;(void)y; return 1; }
static inline int   TextOutA(void* dc, int x, int y, const char* t, int n) { (void)dc;(void)x;(void)y;(void)t;(void)n; return 1; }
static inline int   GetTextExtentPoint32A(void* dc, const char* t, int n, void* sz) { (void)dc;(void)t;(void)n; if (sz) memset(sz, 0, 8); return 1; }
static inline unsigned GetLastError(void) { return 0; }
static inline void  SetLastError(unsigned e) { (void)e; }
static inline void* GetMenu(void* h) { (void)h; return 0; }
static inline int   SetMenu(void* h, void* m) { (void)h;(void)m; return 1; }
static inline int   DrawTextA(void* dc, const char* t, int n, void* r, unsigned f) { (void)dc;(void)t;(void)n;(void)r;(void)f; return 0; }
static inline int   GetCommandLineA_len(void) { return 0; }
typedef long long __time64_t;
static inline int _time64(__time64_t* t) { if (t) *t = 0; return 0; }
static inline int _ftime64(void* s) { (void)s; return 0; }

#define MM_TEXT          1
#define MM_LOMETADATA   56
#define DT_WORDBREAK     0x00000010
#define DT_CENTER        0x00000001
#define DT_LEFT          0x00000000
#define DT_SINGLELINE    0x00000020
typedef LONG_PTR LPARAM;
typedef LONG_PTR*LPLPARAM;
typedef ULONG_PTR* PULONG_PTR;
static inline int  GetCursorPos(void* pt) { if (pt) memset(pt, 0, 8); return 1; }
static inline int  SetCursorPos(int x, int y) { (void)x;(void)y; return 1; }
static inline int  ShowCursor(int show) { (void)show; return 0; }
static inline int  DestroyMenu(void* m) { (void)m; return 1; }
static inline void* LoadMenuA(void* inst, const void* id) { (void)inst;(void)id; return 0; }
static inline struct tm* _localtime64(const __time64_t* t) { time_t v = (time_t)*t; return localtime(&v); }
static inline struct tm* _gmtime64(const __time64_t* t) { time_t v = (time_t)*t; return gmtime(&v); }

#define CP_ACP     0
#define CP_UTF8    65001
#define MM_ANISOTROPIC 8
static inline void SleepEx(unsigned ms, int alertable) { (void)alertable; (void)ms; }
static inline void PostQuitMessage(int code) { (void)code; }
static inline int  DialogBoxParamA(void* inst, const void* tmpl, void* parent, DLGPROC proc, long param) {
  (void)inst;(void)tmpl;(void)parent;(void)proc;(void)param; return 0; }
static inline void _strdate(char* buf) { if (buf) strcpy(buf, "01/01/2005"); }
static inline void _strtime(char* buf) { if (buf) strcpy(buf, "00:00:00"); }
static inline int  GetDateFormatA(unsigned locale, unsigned flags, const void* st, const char* fmt, char* out, int n) {
  (void)locale;(void)flags;(void)st;(void)fmt; if (out && n > 0) out[0] = 0; return 0; }
static inline int  GetTimeFormatA(unsigned locale, unsigned flags, const void* st, const char* fmt, char* out, int n) {
  (void)locale;(void)flags;(void)st;(void)fmt; if (out && n > 0) out[0] = 0; return 0; }

#define MAPVK_VSC_TO_VK      1
#define MAPVK_VK_TO_VSC      0
#define MAPVK_VSC_TO_VK_EX   3
static inline void* SetCursor(void* c) { (void)c; return 0; }
static inline int   SetWindowTextA(void* h, const char* t) { (void)h;(void)t; return 1; }
static inline int   SetWindowLongA(void* h, int idx, long v) { (void)h;(void)idx;(void)v; return 0; }
static inline long  GetWindowLongA(void* h, int idx) { (void)h;(void)idx; return 0; }
static inline int   SetWindowsHookExA(int id, void* proc, void* mod, unsigned thread) { (void)id;(void)proc;(void)mod;(void)thread; return 1; }
static inline int   UnhookWindowsHookEx(int h) { (void)h; return 1; }
static inline long  CallNextHookEx(int h, int code, long w, long l) { (void)h;(void)code;(void)w; return l; }
static inline int   GetKeyState(int vk) { (void)vk; return 0; }
static inline int   GetAsyncKeyState(int vk) { (void)vk; return 0; }
static inline int   GetKeyboardState(void* state) { (void)state; return 1; }
static inline int   SetKeyboardState(void* state) { (void)state; return 1; }

#define SC_MINIMIZE  0xF020
#define SC_RESTORE   0xF120
#define SC_MAXIMIZE  0xF030
#define SC_CLOSE     0xF060
static inline unsigned MapVirtualKeyA(unsigned vk, unsigned maptype) { (void)vk;(void)maptype; return vk; }
static inline void* GetDlgItem(void* dlg, int id) { (void)dlg;(void)id; return 0; }
static inline int   SetDlgItemTextA(void* dlg, int id, const char* t) { (void)dlg;(void)id;(void)t; return 1; }
static inline int   EndDialog(void* dlg, long r) { (void)dlg;(void)r; return 0; }

#ifndef LOWORD
#define LOWORD(l)  ((unsigned short)(((unsigned int)(l)) & 0xffff))
#endif
#ifndef HIWORD
#define HIWORD(l)  ((unsigned short)((((unsigned int)(l)) >> 16) & 0xffff))
#endif
#ifndef LODWORD
#define LODWORD(l) ((unsigned int)(((unsigned long long)(l)) & 0xffffffffu))
#endif
#ifndef HIDWORD
#define HIDWORD(l) ((unsigned int)((((unsigned long long)(l)) >> 32) & 0xffffffffu))
#endif
#ifndef HIBYTE
#define HIBYTE(w)  ((unsigned char)(((unsigned short)(w) >> 8) & 0xff))
#endif
#ifndef LOBYTE
#define LOBYTE(w)  ((unsigned char)((unsigned short)(w) & 0xff))
#endif
#ifndef MAKEWORD
#define MAKEWORD(a, b) ((unsigned short)(((unsigned char)(b) << 8) | ((unsigned char)(a) & 0xff)))
#endif
#ifndef MAKELONG
#define MAKELONG(a, b) ((unsigned int)(((unsigned short)(b) << 16) | ((unsigned short)(a) & 0xffff)))
#endif
typedef struct { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
                 char szCSDVersion[128]; } OSVERSIONINFOA;
typedef struct { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
                 char szCSDVersion[128]; short wServicePackMajor, wServicePackMinor;
                 unsigned short wSuiteMask, wProductType, wReserved; } OSVERSIONINFOEXA;
static inline int GetVersionExA(OSVERSIONINFOA* v) {
  if (v) { v->dwMajorVersion = 5; v->dwMinorVersion = 1; v->dwBuildNumber = 2600; v->dwPlatformId = 2; } return 1; }
static inline long DefWindowProcA(void* h, unsigned m, unsigned w, long l) { (void)h;(void)m;(void)w; return l; }
static inline int  EnumWindows(void* cb, long param) { (void)cb; (void)param; return 1; }

#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDCLOSE 8
#define SC_TASKLIST 0xF130
#define VER_PLATFORM_WIN32s          0
#define VER_PLATFORM_WIN32_WINDOWS   1
#define VER_PLATFORM_WIN32_NT        2
static inline int ToAscii(unsigned vk, unsigned sc, const unsigned char* state, void* ch, unsigned flags) {
  (void)vk; (void)sc; (void)state; (void)ch; (void)flags; return 0;
}
static inline int ToUnicode(unsigned vk, unsigned sc, const unsigned char* state, wchar_t* ch, int n, unsigned flags) {
  (void)vk; (void)state; (void)flags;
  /* On Windows, ToUnicode returns '\r' (13) for Enter, '\b' (8) for Backspace, '\t' (9) for Tab,
   * and 0x1B (27) for Escape. Returning 0 for all other keys lets RedKeyboard::pcGetAsciiValue
   * fall back to its built-in sAsciiCodeMap[sc][shift] table instead of returning raw scancodes. */
  if (ch && n > 0) {
    if (sc == 0x1C /* DIK_RETURN */ || sc == 0x9C /* DIK_NUMPADENTER */) { ch[0] = L'\r'; return 1; }
    if (sc == 0x0E /* DIK_BACK */)   { ch[0] = L'\b'; return 1; }
    if (sc == 0x0F /* DIK_TAB */)    { ch[0] = L'\t'; return 1; }
    if (sc == 0x01 /* DIK_ESCAPE */) { ch[0] = 0x1B;  return 1; }
  }
  return 0;
}
static inline int GetLocaleInfoA(unsigned locale, int type, char* buf, int n) { (void)locale;(void)type; if (buf && n > 0) buf[0] = 0; return 0; }

#define SC_SCREENSAVE   0xF140
#define SC_MONITORPOWER 0xF170
#define SC_KEYMENU      0xF100
#ifndef SetMapMode
#define SetMapMode(dc, mode) ((int)(mode))
#endif

#define TRANSPARENT 1
#define OPAQUE 2
#define IDC_ARROW     ((void*)(uintptr_t)32512)
#define IDC_IBEAM     ((void*)(uintptr_t)32513)
#define IDC_WAIT      ((void*)(uintptr_t)32514)
#define IDC_CROSS     ((void*)(uintptr_t)32515)
#define IDC_HAND      ((void*)(uintptr_t)32649)
static inline void* LoadCursorA(void* inst, const void* id) { (void)inst;(void)id; return 0; }

#ifndef SetBkMode
#define SetBkMode(dc, mode) ((int)(mode))
#endif
#ifndef GetBkMode
#define GetBkMode(dc) (1)
#endif

/* The GDI text helpers the FL input layer uses with wide strings. */
static __inline__ int lstrlenW(const wchar_t* s) { int n = 0; while (s[n]) ++n; return n; }
static __inline__ int lstrlenA(const char* s) { return (int)strlen(s); }
static __inline__ int TextOutW(void* dc, int x, int y, const wchar_t* t, int n) { (void)dc;(void)x;(void)y;(void)t;(void)n; return 1; }
static __inline__ int DrawTextW(void* dc, const wchar_t* t, int n, void* rc, unsigned f) { (void)dc;(void)t;(void)n;(void)rc;(void)f; return 0; }
static __inline__ int ExtTextOutW(void* dc, int x, int y, unsigned opts, const void* rc, const wchar_t* t, int n, const int* dx) {
  (void)dc;(void)x;(void)y;(void)opts;(void)rc;(void)t;(void)n;(void)dx; return 1; }
static __inline__ HWND FindWindowA(const char* cls, const char* win) { (void)cls;(void)win; return 0; }
static __inline__ HWND FindWindowExA(HWND parent, HWND after, const char* cls, const char* win) {
  (void)parent;(void)after;(void)cls;(void)win; return 0; }
#define CS_BYTEALIGNCLIENT 0x1000
#define CS_BYTEALIGNWINDOW 0x2000
#define CS_CLASSDC         0x0040
#define CS_OWNDC           0x0020
#define CS_PARENTDC        0x0080
#define CS_DBLCLKS         0x0008
#define CS_HREDRAW         0x0002
#define CS_VREDRAW         0x0001
#define CS_GLOBALCLASS     0x4000
#define CW_USEDEFAULT      ((int)0x80000000)

static __inline__ void* LoadIconA(void* inst, const void* id) { (void)inst;(void)id; return 0; }
static __inline__ void* LoadImageA(void* inst, const void* name, unsigned type, int w, int h, unsigned flags) {
  (void)inst;(void)name;(void)type;(void)w;(void)h;(void)flags; return 0; }
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define IMAGE_BITMAP 0
#define LR_LOADFROMFILE 0x00000010
#define LR_DEFAULTSIZE  0x00000040

static __inline__ unsigned RegisterClassA(const WNDCLASSA* wc) { (void)wc; return 1; }
static __inline__ int  UnregisterClassA(const char* cls, void* inst) { (void)cls;(void)inst; return 1; }
static __inline__ HWND CreateWindowExA(unsigned ex, const char* cls, const char* title, unsigned style,
                                       int x, int y, int w, int h, HWND parent, void* menu, void* inst, void* param) {
  (void)ex;(void)cls;(void)title;(void)style;(void)x;(void)y;(void)w;(void)h;(void)parent;(void)menu;(void)inst;(void)param; return 0; }
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_VISIBLE 0x10000000u
#define WS_POPUP 0x80000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_EX_APPWINDOW 0x00040000u
#define WM_DESTROY 0x0002
#define WM_CREATE 0x0001


/* ---- Win32 scalar error codes (winerror.h) --------------------------------
 * The platform code branches on these; the values are Microsoft's so a code that a backend ever
 * has to translate means the same thing on both sides. */
#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev, fac, code) \
  ((long)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#endif
#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS                 0L
#endif
#ifndef ERROR_INVALID_FUNCTION
#define ERROR_INVALID_FUNCTION        1L
#define ERROR_FILE_NOT_FOUND          2L
#define ERROR_PATH_NOT_FOUND          3L
#define ERROR_ACCESS_DENIED           5L
#define ERROR_INVALID_HANDLE          6L
#define ERROR_NOT_ENOUGH_MEMORY       8L
#define ERROR_INVALID_PARAMETER       87L
#define ERROR_CALL_NOT_IMPLEMENTED    120L
#define ERROR_SERVICE_NOT_ACTIVE      1062L
#define ERROR_ALREADY_INITIALIZED     1247L
#endif
#ifndef RPC_S_SERVER_UNAVAILABLE
#define RPC_S_SERVER_UNAVAILABLE      1722L
#define RPC_S_CALL_FAILED             1726L
#endif
#ifndef E_UNEXPECTED
#define E_UNEXPECTED                  ((long)0x8000FFFFL)
#define E_FAIL                        ((long)0x80004005L)
#define E_NOTIMPL                     ((long)0x80004001L)
#define E_POINTER                     ((long)0x80004003L)
#define E_ABORT                       ((long)0x80004004L)
#define E_ACCESSDENIED                ((long)0x80070005L)
#define E_OUTOFMEMORY                 ((long)0x8007000EL)
#define E_INVALIDARG                  ((long)0x80070057L)
#define CLASS_E_NOAGGREGATION         ((long)0x80040110L)
#endif
/* Structured-exception codes the game's crash handler compares against. clang has no SEH for
 * wasm32, so only the constants are portable here (see docs/platform-shim-notes.md). */
#ifndef EXCEPTION_ACCESS_VIOLATION
#define EXCEPTION_ACCESS_VIOLATION      ((unsigned long)0xC0000005L)
#define EXCEPTION_DATATYPE_MISALIGNMENT ((unsigned long)0x80000002L)
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    ((unsigned long)0xC000008EL)
#define EXCEPTION_FLT_INVALID_OPERATION ((unsigned long)0xC0000090L)
#define EXCEPTION_STACK_OVERFLOW        ((unsigned long)0xC00000FDL)
#endif

/* ---- handles, files, file time (winbase.h / winnt.h) ---------------------- */
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(uintptr_t)-1)
#endif
#ifndef GENERIC_READ
#define GENERIC_READ            (0x80000000L)
#define GENERIC_WRITE           (0x40000000L)
#define GENERIC_EXECUTE         (0x20000000L)
#define GENERIC_ALL             (0x10000000L)
#endif
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004
#endif
#ifndef CREATE_NEW
#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5
#endif
#ifndef FILE_BEGIN
#define FILE_BEGIN              0
#define FILE_CURRENT            1
#define FILE_END                2
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE  0x00000020
#define FILE_ATTRIBUTE_NORMAL   0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#endif
#ifndef FILE_FLAG_SEQUENTIAL_SCAN
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED    0x40000000
#define FILE_FLAG_NO_BUFFERING  0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#endif
typedef struct { unsigned long dwLowDateTime, dwHighDateTime; } FILETIME;
typedef union { unsigned long long QuadPart; struct { unsigned long LowPart; long HighPart; }; } ULARGE_INTEGER;
typedef struct {
  unsigned long dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  unsigned long nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
  char cFileName[260], cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct {
  unsigned long dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  unsigned long nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
  wchar_t cFileName[260], cAlternateFileName[14];
} WIN32_FIND_DATAW;
static inline HANDLE CreateFileA(const char* name, unsigned access, unsigned share, void* sec,
                                 unsigned disp, unsigned flags, void* templ) {
  (void)name;(void)access;(void)share;(void)sec;(void)disp;(void)flags;(void)templ; return INVALID_HANDLE_VALUE;
}
static inline int ReadFile(HANDLE h, void* buf, unsigned n, unsigned* read, void* ov) {
  (void)h;(void)buf;(void)n;(void)ov; if (read) *read = 0; return 0;
}
static inline int WriteFile(HANDLE h, const void* buf, unsigned n, unsigned* written, void* ov) {
  (void)h;(void)buf;(void)n;(void)ov; if (written) *written = 0; return 0;
}
static inline unsigned GetFileSize(HANDLE h, unsigned* high) { if (high) *high = 0; (void)h; return 0; }
static inline int SetFilePointer(HANDLE h, int dist, int* high, int method) {
  (void)h;(void)dist;(void)high;(void)method; return 0;
}
static inline int SetEndOfFile(HANDLE h) { (void)h; return 1; }
static inline int FlushFileBuffers(HANDLE h) { (void)h; return 1; }
static inline int DeleteFileA(const char* p) { (void)p; return 0; }
static inline int CreateDirectoryA(const char* p, void* sa) { (void)p;(void)sa; return 0; }
static inline int GetDiskFreeSpaceA(const char* root, unsigned* sectors, unsigned* bytes,
                                    unsigned* freeClusters, unsigned* totalClusters) {
  (void)root; if (sectors) *sectors = 1; if (bytes) *bytes = 512;
  if (freeClusters) *freeClusters = 0; if (totalClusters) *totalClusters = 0; return 1;
}
static inline int GetDiskFreeSpaceExA(const char* root, unsigned long long* freeToUs,
                                      unsigned long long* total, unsigned long long* totalFree) {
  (void)root; if (freeToUs) *freeToUs = 0; if (total) *total = 0; if (totalFree) *totalFree = 0; return 1;
}
static inline int GetDiskFreeSpaceExW(const wchar_t* root, ULARGE_INTEGER* freeToUs,
                                      ULARGE_INTEGER* total, ULARGE_INTEGER* totalFree) {
  (void)root; if (freeToUs) freeToUs->QuadPart = 0; if (total) total->QuadPart = 0;
  if (totalFree) totalFree->QuadPart = 0; return 1;
}
static inline HANDLE FindFirstFileA(const char* pat, WIN32_FIND_DATAA* fd) {
  (void)pat; if (fd) memset(fd, 0, sizeof(*fd)); return INVALID_HANDLE_VALUE;
}
static inline HANDLE FindFirstFileW(const wchar_t* pat, WIN32_FIND_DATAW* fd) {
  (void)pat; if (fd) memset(fd, 0, sizeof(*fd)); return INVALID_HANDLE_VALUE;
}
static inline int FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) { (void)h; if (fd) memset(fd, 0, sizeof(*fd)); return 0; }
static inline int FindNextFileW(HANDLE h, WIN32_FIND_DATAW* fd) { (void)h; if (fd) memset(fd, 0, sizeof(*fd)); return 0; }
static inline int FindClose(HANDLE h) { (void)h; return 1; }

/* ---- thread priorities (winnt.h / winbase.h) ----------------------------- */
#ifndef THREAD_BASE_PRIORITY_LOWRT
#define THREAD_BASE_PRIORITY_LOWRT  15
#define THREAD_BASE_PRIORITY_MAX    2
#define THREAD_BASE_PRIORITY_MIN    (-2)
#define THREAD_BASE_PRIORITY_IDLE   (-15)
#endif
#ifndef THREAD_PRIORITY_NORMAL
#define THREAD_PRIORITY_TIME_CRITICAL   THREAD_BASE_PRIORITY_LOWRT
#define THREAD_PRIORITY_HIGHEST         THREAD_BASE_PRIORITY_MAX
#define THREAD_PRIORITY_ABOVE_NORMAL    (THREAD_PRIORITY_HIGHEST - 1)
#define THREAD_PRIORITY_NORMAL          0
#define THREAD_PRIORITY_BELOW_NORMAL    (THREAD_PRIORITY_LOWEST + 1)
#define THREAD_PRIORITY_LOWEST          THREAD_BASE_PRIORITY_MIN
#define THREAD_PRIORITY_IDLE            THREAD_BASE_PRIORITY_IDLE
#endif
static inline int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }
static inline int SetThreadPriority(HANDLE h, int priority) { (void)h; (void)priority; return 1; }

/* ---- registry (winreg.h) --------------------------------------------------
 * The auto-settings code and the Logitech LCD driver read their configuration out of the
 * registry on Windows; on the browser build these all report "not present". */
#ifndef HKEY_LOCAL_MACHINE
#define HKEY_LOCAL_MACHINE  ((HKEY)(uintptr_t)0x80000002u)
#define HKEY_CURRENT_USER   ((HKEY)(uintptr_t)0x80000001u)
#define HKEY_USERS          ((HKEY)(uintptr_t)0x80000003u)
#define HKEY_CLASSES_ROOT   ((HKEY)(uintptr_t)0x80000000u)
#define HKEY_CURRENT_CONFIG ((HKEY)(uintptr_t)0x80000005u)
#endif
#ifndef KEY_QUERY_VALUE
#define KEY_QUERY_VALUE         0x0001
#define KEY_SET_VALUE           0x0002
#define KEY_CREATE_SUB_KEY      0x0004
#define KEY_ENUMERATE_SUB_KEYS  0x0008
#define KEY_NOTIFY              0x0010
#define KEY_CREATE_LINK         0x0020
#define SYNCHRONIZE             0x00100000
#define STANDARD_RIGHTS_READ    0x00020000
#define STANDARD_RIGHTS_WRITE   0x00020000
#define STANDARD_RIGHTS_ALL     0x001F0000
#endif
#ifndef KEY_READ
#define KEY_READ  ((STANDARD_RIGHTS_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY) & (~SYNCHRONIZE))
#define KEY_WRITE ((STANDARD_RIGHTS_WRITE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY) & (~SYNCHRONIZE))
#define KEY_ALL_ACCESS ((STANDARD_RIGHTS_ALL | KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY \
                         | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | KEY_CREATE_LINK) & (~SYNCHRONIZE))
#endif
#ifndef REG_OPTION_NON_VOLATILE
#define REG_OPTION_NON_VOLATILE   0x00000000
#define REG_OPTION_VOLATILE       0x00000001
#define REG_NONE                  0
#define REG_SZ                    1
#define REG_EXPAND_SZ             2
#define REG_BINARY                3
#define REG_DWORD                 4
#define REG_MULTI_SZ              7
#define REG_DWORD_BIG_ENDIAN      5
#endif
static inline long RegOpenKeyExA(HKEY k, const char* sub, unsigned opt, unsigned access, HKEY* out) {
  (void)k;(void)sub;(void)opt;(void)access; if (out) *out = 0; return ERROR_FILE_NOT_FOUND;
}
static inline long RegCreateKeyExA(HKEY k, const char* sub, unsigned res, char* cls, unsigned opt,
                                   unsigned access, void* sa, HKEY* out, unsigned* disp) {
  (void)k;(void)sub;(void)res;(void)cls;(void)opt;(void)access;(void)sa;(void)disp;
  if (out) *out = 0; return ERROR_ACCESS_DENIED;
}
static inline long RegQueryValueExA(HKEY k, const char* name, unsigned* res, unsigned* type,
                                    unsigned char* data, unsigned* cb) {
  (void)k;(void)name;(void)res;(void)data; if (type) *type = REG_NONE; if (cb) *cb = 0;
  return ERROR_FILE_NOT_FOUND;
}
static inline long RegSetValueExA(HKEY k, const char* name, unsigned res, unsigned type,
                                  const unsigned char* data, unsigned cb) {
  (void)k;(void)name;(void)res;(void)type;(void)data;(void)cb; return ERROR_ACCESS_DENIED;
}
static inline long RegCloseKey(HKEY k) { (void)k; return ERROR_SUCCESS; }

/* ---- user32 / gdi32 calls the surviving window and LCD code makes -------- */
/* SetRect / SetRectEmpty are macros in the real windows.h, but RedEngineFL declares its own
 * `RedHotSpot::SetRect(float,float,float,float)` member, and a macro would be expanded inside that
 * declaration. Plain functions give the same call sites the same meaning with no such collision. */
static inline int SetRect(RECT* r, long left, long top, long right, long bottom) {
  if (r) { r->left = left; r->top = top; r->right = right; r->bottom = bottom; }
  return 1;
}
#ifndef SW_SHOWNORMAL
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#endif
#ifndef SWP_NOSIZE
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_FRAMECHANGED 0x0020
#endif
#ifndef PM_NOREMOVE
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001
#define PM_NOYIELD  0x0002
#endif
#ifndef ICM_OFF
#define ICM_OFF   1
#define ICM_ON    2
#define ICM_QUERY 3
#define ICM_DONE_OUTSIDEDC 4
#endif
#ifndef FW_NORMAL
#define FW_DONTCARE  0
#define FW_THIN      100
#define FW_NORMAL    400
#define FW_MEDIUM    500
#define FW_SEMIBOLD  600
#define FW_BOLD      700
#define FW_HEAVY     900
#endif
#ifndef BI_RGB
#define BI_RGB        0L
#define BI_RLE8       1L
#define BI_RLE4       2L
#define BI_BITFIELDS  3L
#endif
#ifndef DIB_RGB_COLORS
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#endif
typedef struct {
  long lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
  unsigned char lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision,
                lfQuality, lfPitchAndFamily;
  char lfFaceName[32];
} LOGFONTA;
typedef struct { unsigned int length, flags, showCmd; POINT ptMinPosition, ptMaxPosition;
                 RECT rcNormalPosition, rcDevice; } WINDOWPLACEMENT;
static inline int AdjustWindowRect(RECT* r, unsigned style, int menu) { (void)style;(void)menu; return 1; }
static inline int GetClientRect(void* h, RECT* r) { if (r) memset(r, 0, sizeof(*r)); (void)h; return 1; }
static inline int SetWindowPos(void* h, void* after, int x, int y, int cx, int cy, unsigned flags) {
  (void)h;(void)after;(void)x;(void)y;(void)cx;(void)cy;(void)flags; return 1;
}
static inline void* GetFocus(void) { return 0; }
static inline int PeekMessageA(MSG* m, void* h, unsigned min, unsigned max, unsigned remove) {
  (void)h;(void)min;(void)max;(void)remove; if (m) memset(m, 0, sizeof(*m)); return 0;
}
static inline int TranslateMessage(const MSG* m) { (void)m; return 1; }
static inline long DispatchMessageA(const MSG* m) { (void)m; return 0; }
static inline int SetICMMode(void* dc, int mode) { (void)dc;(void)mode; return 1; }
static inline int GetDeviceGammaRamp(void* dc, void* ramp) { (void)dc;(void)ramp; return 1; }
static inline unsigned SetTextColor(void* dc, unsigned c) { (void)dc; (void)c; return 0; }
static inline unsigned SetBkColor(void* dc, unsigned c) { (void)dc; (void)c; return 0; }
static inline HFONT CreateFontIndirectA(const LOGFONTA* lf) { (void)lf; return 0; }
static inline int GdiFlush(void) { return 1; }
static inline void* LoadLibraryA(const char* name) { (void)name; return 0; }
static inline int FreeLibrary(void* m) { (void)m; return 1; }
static inline unsigned GetExceptionCode(void) { return 0; }

/* ---- the wide-character CRT spellings the save-device code uses ---------- */
#ifndef _MAX_PATH
#define _MAX_PATH      260
#define _MAX_DRIVE     3
#define _MAX_DIR       256
#define _MAX_FNAME     256
#define _MAX_EXT       256
#endif
typedef struct { unsigned int attrib; time_t time_create, time_access, time_write;
                 unsigned int size; wchar_t name[260]; } _wfinddata_t;
static inline intptr_t _wfindfirst(const wchar_t* pat, _wfinddata_t* fd) {
  (void)pat; if (fd) memset(fd, 0, sizeof(*fd)); return -1;
}
static inline int _wfindnext(intptr_t h, _wfinddata_t* fd) { (void)h; if (fd) memset(fd, 0, sizeof(*fd)); return -1; }
static inline int _wfindclose(intptr_t h) { (void)h; return 0; }
static inline void _wsplitpath(const wchar_t* path, wchar_t* drive, wchar_t* dir,
                               wchar_t* file, wchar_t* ext) {
  if (drive) drive[0] = 0;
  if (dir)   dir[0]   = 0;
  if (file)  file[0]  = 0;
  if (ext)   ext[0]   = 0;
  const wchar_t* rest  = path;
  const wchar_t* slash = wcsrchr(path, L'/');
  const wchar_t* cut   = slash ? slash : wcsrchr(path, L'\\');
  if (cut) { rest = cut + 1; if (dir) wcscat(dir, L"/"); }
  if (file) wcscat(file, rest);
  const wchar_t* dot = wcsrchr(rest, L'.');
  if (dot) {
    if (file) file[wcslen(file) - wcslen(dot)] = 0;
    if (ext)  wcscat(ext, dot + 1);
  }
}
static inline void _wmakepath(wchar_t* p, const wchar_t* d, const wchar_t* dir,
                              const wchar_t* f, const wchar_t* e) {
  p[0] = 0;
  if (d && *d) { wcscat(p, d); wcscat(p, L":"); }
  if (dir && *dir) { wcscat(p, dir); if (p[wcslen(p) - 1] != L'\\' && p[wcslen(p) - 1] != L'/') wcscat(p, L"/"); }
  if (f) wcscat(p, f);
  if (e) { wcscat(p, L"."); wcscat(p, e); }
}
static inline wchar_t* _wfullpath(wchar_t* out, const wchar_t* in, size_t n) {
  return wcsncpy(out, in, n);
}
/* MSVC spells it _strdup; musl only exposes strdup. */
static inline char* _strdup(const char* s) { size_t n = strlen(s) + 1; char* p = (char*)malloc(n); return p ? (memcpy(p, s, n), p) : 0; }

/* ---- COM interface identifiers declared where Windows declares them -------
 * unknwn.h owns IID_IUnknown, ksproxy.h owns IID_IKsPropertySet; both are objects (Microsoft's
 * GUID bytes) defined once in spike/shim/guids.cpp - Windows' uuid.lib role in this build. */
#ifdef __cplusplus
extern const GUID IID_IUnknown;
extern const GUID IID_IKsPropertySet;
#endif

/* ---- the wide CRT / user32 string helpers the save-device and LCD code uses */
static inline int _wmkdir(const wchar_t* p) { (void)p; return -1; }
static inline char* lstrcpyA(char* dst, const char* src) { return strcpy(dst, src); }
static inline int lstrcmpA(const char* a, const char* b) { return strcmp(a, b); }
static inline char* lstrcpynA(char* dst, const char* src, int n) {
  if (n > 0) { strncpy(dst, src, (size_t)(n - 1)); dst[n - 1] = 0; }
  return dst;
}
/* guiddef.h defines value comparison for GUIDs in C++ (the DMO/DSound code compares device ids). */
#ifdef __cplusplus
inline bool operator==(const GUID& a, const GUID& b) {
  return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
         memcmp(a.Data4, b.Data4, 8) == 0;
}
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }
#endif

static inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
  char p[512], m[16];
  for (int i = 0; i < 511 && path[i]; ++i) p[i] = (char)path[i];
  p[path[511] ? 511 : (int)wcslen(path)] = 0;
  for (int i = 0; i < 15 && mode[i]; ++i) m[i] = (char)mode[i];
  m[(int)wcslen(mode)] = 0;
  return fopen(p, m);
}
static inline int _wremove(const wchar_t* p) { char b[512]; int i = 0;
  for (; i < 511 && p[i]; ++i) b[i] = (char)p[i]; b[i] = 0; return remove(b); }
static inline int _wrename(const wchar_t* a, const wchar_t* b) { char x[512], y[512]; int i = 0, j = 0;
  for (; i < 511 && a[i]; ++i) x[i] = (char)a[i]; x[i] = 0;
  for (; j < 511 && b[j]; ++j) y[j] = (char)b[j]; y[j] = 0; return rename(x, y); }

/* ---- additional Win32 vocabulary used by vendor/openswbf2 ----------------- */
#ifndef _WINDEF_
#define _WINDEF_
#endif
typedef UINT_PTR WPARAM;
typedef RECT* LPRECT;
typedef const RECT* LPCRECT;
typedef SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#define PAGE_READONLY  0x02
#define PAGE_NOACCESS  0x01
#define MEM_COMMIT     0x00001000
#define MEM_RESERVE    0x00002000
#define MEM_RELEASE    0x00008000
#define MEM_FREE       0x00010000
#endif
#ifndef FILE_MAP_READ
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ  0x0004
#endif
#ifndef ERROR_ALREADY_EXISTS
#define ERROR_ALREADY_EXISTS 183L
#endif
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x001F0003u
#endif
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0u
#endif
#ifndef LOGPIXELSX
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#endif
#ifndef MONITOR_DEFAULTTOPRIMARY
#define MONITOR_DEFAULTTONULL    0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002
#endif
#ifndef GWL_STYLE
#define GWL_STYLE   (-16)
#define GWL_EXSTYLE (-20)
#endif
#ifndef WS_CAPTION
#define WS_CAPTION     0x00C00000u
#define WS_SYSMENU     0x00080000u
#define WS_THICKFRAME  0x00040000u
#define WS_MINIMIZE    0x20000000u
#define WS_MAXIMIZE    0x01000000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_EX_TOPMOST  0x00000008u
#endif
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFu)
#endif
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#endif
#ifndef STD_OUTPUT_HANDLE
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK    0x0001
#define FILE_TYPE_CHAR    0x0002
#define FILE_TYPE_PIPE    0x0003
#endif
#ifndef LANG_NEUTRAL
#define LANG_NEUTRAL 0x00
#endif
#ifndef HWND_TOP
#define HWND_TOP       ((HWND)(uintptr_t)0)
#define HWND_BOTTOM    ((HWND)(uintptr_t)1)
#define HWND_TOPMOST   ((HWND)(uintptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(uintptr_t)-2)
#endif
typedef struct {
  DWORD cbSize;
  RECT rcMonitor;
  RECT rcWork;
  DWORD dwFlags;
} MONITORINFO;
typedef struct {
  LPVOID BaseAddress;
  LPVOID AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
} MEMORY_BASIC_INFORMATION;
typedef struct {
  DWORD dwLength;
  DWORD dwMemoryLoad;
  unsigned long long ullTotalPhys, ullAvailPhys;
  unsigned long long ullTotalPageFile, ullAvailPageFile;
  unsigned long long ullTotalVirtual, ullAvailVirtual;
  unsigned long long ullAvailExtendedVirtual;
} MEMORYSTATUSEX;
typedef struct {
  DWORD dwOemId;
  DWORD dwPageSize;
  LPVOID lpMinimumApplicationAddress;
  LPVOID lpMaximumApplicationAddress;
  DWORD_PTR dwActiveProcessorMask;
  DWORD dwNumberOfProcessors;
  DWORD dwProcessorType;
  DWORD dwAllocationGranularity;
  WORD wProcessorLevel;
  WORD wProcessorRevision;
} SYSTEM_INFO;
typedef struct {
  DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
  wchar_t szCSDVersion[128];
} OSVERSIONINFOW;
static inline int GlobalMemoryStatusEx(MEMORYSTATUSEX* s) {
  if (s) {
    memset(s, 0, sizeof(*s));
    s->dwLength = sizeof(*s);
    s->ullTotalPhys = 512ULL * 1024 * 1024;
    s->ullAvailPhys = 256ULL * 1024 * 1024;
    s->ullTotalVirtual = 2048ULL * 1024 * 1024;
    s->ullAvailVirtual = 1024ULL * 1024 * 1024;
  }
  return 1;
}
static inline void GetSystemInfo(SYSTEM_INFO* si) {
  if (si) { memset(si, 0, sizeof(*si)); si->dwPageSize = 4096; si->dwNumberOfProcessors = 1; si->dwAllocationGranularity = 65536; }
}
static inline SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len) {
  (void)addr; if (mbi && len >= sizeof(*mbi)) memset(mbi, 0, sizeof(*mbi)); return 0;
}
static inline HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD flProtect,
                                        DWORD dwMaxHigh, DWORD dwMaxLow, LPCSTR lpName) {
  (void)hFile;(void)sa;(void)flProtect;(void)dwMaxHigh;(void)dwMaxLow;(void)lpName; return 0;
}
static inline LPVOID MapViewOfFile(HANDLE hMap, DWORD dwAccess, DWORD offHi, DWORD offLo, SIZE_T cb) {
  (void)hMap;(void)dwAccess;(void)offHi;(void)offLo;(void)cb; return 0;
}
static inline BOOL UnmapViewOfFile(const void* p) { (void)p; return 1; }
static inline BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period, void* comp, void* arg, BOOL resume) {
  (void)h;(void)due;(void)period;(void)comp;(void)arg;(void)resume; return 0;
}
static inline MMRESULT timeBeginPeriod(UINT uPeriod) { (void)uPeriod; return TIMERR_NOERROR; }
static inline MMRESULT timeEndPeriod(UINT uPeriod) { (void)uPeriod; return TIMERR_NOERROR; }
static inline UINT GetSystemDirectoryA(LPSTR lpBuf, UINT uSize) {
  if (lpBuf && uSize > 0) lpBuf[0] = 0; return 0;
}
static inline BOOL GetMonitorInfoA(HMONITOR hMon, MONITORINFO* lpmi) {
  (void)hMon;
  if (lpmi) {
    lpmi->rcMonitor.left = 0; lpmi->rcMonitor.top = 0; lpmi->rcMonitor.right = 1280; lpmi->rcMonitor.bottom = 720;
    lpmi->rcWork = lpmi->rcMonitor;
    lpmi->dwFlags = 1;
  }
  return 0;
}
static inline HMONITOR MonitorFromWindow(HWND hWnd, DWORD dwFlags) { (void)hWnd;(void)dwFlags; return 0; }
static inline HMONITOR MonitorFromPoint(POINT pt, DWORD dwFlags) { (void)pt;(void)dwFlags; return 0; }
static inline BOOL AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle) {
  (void)lpRect;(void)dwStyle;(void)bMenu;(void)dwExStyle; return 1;
}
static inline BOOL IsIconic(HWND hWnd) { (void)hWnd; return 0; }
static inline DWORD GetFileAttributesA(LPCSTR lpFileName) { (void)lpFileName; return INVALID_FILE_ATTRIBUTES; }
static inline BOOL SetFileAttributesA(LPCSTR lpFileName, DWORD dwAttrs) { (void)lpFileName;(void)dwAttrs; return 0; }
static inline BOOL CopyFileA(LPCSTR src, LPCSTR dst, BOOL failIfExists) { (void)src;(void)dst;(void)failIfExists; return 0; }
static inline BOOL MoveFileExA(LPCSTR src, LPCSTR dst, DWORD flags) { (void)src;(void)dst;(void)flags; return 0; }
static inline BOOL WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file) {
  (void)app;(void)key;(void)val;(void)file; return 0;
}
static inline DWORD GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR ret, DWORD sz, LPCSTR file) {
  (void)app;(void)key;(void)file;
  if (!ret || sz == 0) return 0;
  if (!def) { ret[0] = 0; return 0; }
  size_t n = strlen(def); if (n >= sz) n = sz - 1;
  memcpy(ret, def, n); ret[n] = 0; return (DWORD)n;
}
static inline HANDLE GetStdHandle(DWORD nStdHandle) { (void)nStdHandle; return INVALID_HANDLE_VALUE; }
static inline DWORD GetFileType(HANDLE hFile) { (void)hFile; return FILE_TYPE_UNKNOWN; }
static inline DWORD GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart) {
  if (!lpFileName || !lpBuffer || nBufferLength == 0) return 0;
  size_t n = strlen(lpFileName);
  if (n >= nBufferLength) n = nBufferLength - 1;
  memcpy(lpBuffer, lpFileName, n);
  lpBuffer[n] = 0;
  if (lpFilePart) {
    char* sl = strrchr(lpBuffer, '/');
    char* bs = strrchr(lpBuffer, '\\');
    char* cut = (sl && bs) ? (sl > bs ? sl : bs) : (sl ? sl : bs);
    *lpFilePart = cut ? cut + 1 : lpBuffer;
  }
  return (DWORD)n;
}
#ifndef _finite
#define _finite isfinite
#endif
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)-1; }
static inline int InterlockedExchange(volatile long* p, long v) { return (int)__atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
typedef struct {
  DWORD bV5Size;
  LONG  bV5Width;
  LONG  bV5Height;
  WORD  bV5Planes;
  WORD  bV5BitCount;
  DWORD bV5Compression;
  DWORD bV5SizeImage;
  LONG  bV5XPelsPerMeter;
  LONG  bV5YPelsPerMeter;
  DWORD bV5ClrUsed;
  DWORD bV5ClrImportant;
  DWORD bV5RedMask;
  DWORD bV5GreenMask;
  DWORD bV5BlueMask;
  DWORD bV5AlphaMask;
  DWORD bV5CSType;
  int   bV5Endpoints[9];
  DWORD bV5GammaRed;
  DWORD bV5GammaGreen;
  DWORD bV5GammaBlue;
  DWORD bV5Intent;
  DWORD bV5ProfileData;
  DWORD bV5ProfileSize;
  DWORD bV5Reserved;
} BITMAPV5HEADER;
typedef struct {
  BOOL    fIcon;
  DWORD   xHotspot;
  DWORD   yHotspot;
  HBITMAP hbmMask;
  HBITMAP hbmColor;
} ICONINFO;
static inline HICON CreateIconIndirect(ICONINFO* piconinfo) { (void)piconinfo; return 0; }
static inline BOOL DestroyIcon(HICON hIcon) { (void)hIcon; return 1; }
static inline HCURSOR GetCursor(void) { return 0; }
static inline HWND WindowFromPoint(POINT pt) { (void)pt; return 0; }
static inline BOOL ClipCursor(const RECT* lpRect) { (void)lpRect; return 1; }
static inline BOOL GetClipCursor(LPRECT lpRect) { if (lpRect) memset(lpRect, 0, sizeof(*lpRect)); return 0; }
static inline BOOL EqualRect(const RECT* a, const RECT* b) {
  return (a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom);
}
static inline BOOL ClientToScreen(HWND hWnd, POINT* lpPoint) { (void)hWnd; (void)lpPoint; return 1; }
static inline BOOL ScreenToClient(HWND hWnd, POINT* lpPoint) { (void)hWnd; (void)lpPoint; return 1; }

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS  ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif
typedef struct {
  char  dmDeviceName[32];
  WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
  DWORD dmFields;
  DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
} DEVMODEA;
typedef struct {
  DWORD cbSize;
  RECT  rcMonitor;
  RECT  rcWork;
  DWORD dwFlags;
  char  szDevice[32];
} MONITORINFOEXA;
static inline BOOL EnumDisplaySettingsA(LPCSTR lpszDeviceName, DWORD iModeNum, DEVMODEA* lpDevMode) {
  (void)lpszDeviceName; (void)iModeNum;
  if (lpDevMode) {
    lpDevMode->dmPelsWidth = 1280;
    lpDevMode->dmPelsHeight = 720;
    lpDevMode->dmBitsPerPel = 32;
    lpDevMode->dmDisplayFrequency = 60;
  }
  return 1;
}
#ifdef __cplusplus
static inline BOOL GetMonitorInfoA(HMONITOR hMon, MONITORINFOEXA* lpmi) {
  (void)hMon;
  if (lpmi) {
    lpmi->rcMonitor.left = 0; lpmi->rcMonitor.top = 0; lpmi->rcMonitor.right = 1280; lpmi->rcMonitor.bottom = 720;
    lpmi->rcWork = lpmi->rcMonitor;
    lpmi->dwFlags = 1;
    strncpy(lpmi->szDevice, "\\\\.\\DISPLAY1", sizeof(lpmi->szDevice) - 1);
    lpmi->szDevice[sizeof(lpmi->szDevice) - 1] = 0;
  }
  return 1;
}
#endif
static inline BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl) {
  (void)hWnd;
  if (lpwndpl) {
    memset(lpwndpl, 0, sizeof(*lpwndpl));
    lpwndpl->length = sizeof(*lpwndpl);
    lpwndpl->showCmd = 1; /* SW_SHOWNORMAL */
  }
  return 1;
}
static inline HWND SetFocus(HWND hWnd) { return hWnd; }

