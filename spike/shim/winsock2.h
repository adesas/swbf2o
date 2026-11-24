#pragma once
/* No raw UDP in the browser: pcNetSocket.cpp is replaced by a WebSocket/WebRTC adapter that
 * implements the same NetSocket interface. Types here only need to satisfy declarations. */
#include "swbf2_prefix.h"
#ifdef __cplusplus
extern "C" {
#endif
struct hostent { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; };
struct hostent* gethostbyname(const char* name);
const char* inet_ntop(int af, const void* src, char* dst, size_t size);
#ifdef __cplusplus
}
#endif
/* This header models C++ COM/D3D APIs: keep remaining C++ helpers inside __cplusplus. */
#ifdef __cplusplus  /* C++ only */
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
struct in_addr { unsigned int s_addr; };
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in { unsigned short sin_family; unsigned short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
struct timeb { long time; unsigned short millitm; short timezone; short dstflag; };
int socket(int af, int type, int proto);
int closesocket(int s);
int sendto(int s, const void* buf, int len, int flags, const struct sockaddr* to, int tolen);
int recvfrom(int s, void* buf, int len, int flags, struct sockaddr* from, int* fromlen);
int bind_(int s, const struct sockaddr* a, int len);
int setsockopt(int s, int level, int opt, const void* v, int len);
int getsockopt(int s, int level, int opt, void* v, int* len);
int ioctlsocket(int s, long cmd, unsigned long* arg);
int WSAStartup(unsigned short version, void* data);
void WSACleanup(void);
char* inet_ntoa(struct in_addr a);
#define FD_READ 1
#define FD_WRITE 2
#define FD_OOB 4
#define FD_ACCEPT 8
#define FD_CONNECT 16
#define FD_CLOSE 32
#define FIONREAD 100
#define FIONBIO 101
#ifndef inet_addr
#define inet_addr(a) ((unsigned int)0)
#endif


#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif
typedef struct { unsigned long fd_array[2]; } fd_set_dummy;

int connect(int s, const struct sockaddr* a, int len);
int bind_socket(int s, const struct sockaddr* a, int len);
int send(int s, const void* b, int len, int flags);
int recv(int s, void* b, int len, int flags);
int listen(int s, int backlog);
int accept(int s, struct sockaddr* a, int* len);
int getsockname(int s, struct sockaddr* a, int* len);
int getpeername(int s, struct sockaddr* a, int* len);
int select(int nfds, void* r, void* w, void* e, struct timeval* t);
int __stdcall WSAAsyncSelect(int s, HWND h, unsigned int msg, long evt);
#ifndef INADDR_ANY
#define INADDR_ANY 0u
#endif
#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST 0xffffffffu
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001u
#endif

#endif  /* C++ only */

int bind(int s, const struct sockaddr* a, int len);
int WSARecvFrom(int s, void* buf, unsigned n, unsigned* nr, unsigned* flags, struct sockaddr* from, int* fromlen, void* ov, void* cf);
int __stdcall WSAEventSelect(int s, void* e, long netevents);
void* WSACreateEvent(void);
int WSACloseEvent(void* e);
int WSAWaitForMultipleEvents(unsigned n, void* const* e, int waitAll, unsigned ms, int alertable);

int WSAGetLastError(void);
void WSASetLastError(int err);
#ifndef WSAEWOULDBLOCK
#define WSAEWOULDBLOCK 10035
#endif
#ifndef WSAECONNREFUSED
#define WSAECONNREFUSED 10061
#endif
