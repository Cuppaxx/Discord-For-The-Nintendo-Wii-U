#ifndef NETMEM_H
#define NETMEM_H
#include <stdbool.h>
/* Wii U network memory: give nsysnet its own receive-buffer pool (somemopt) so UDP media isn't dropped
 * while HTTPS downloads use the small default pool, and set UDP sockets up to use it (SO_RUSRBUF). */
void netmem_start(void);                 /* call once at startup */
bool netmem_ok(void);                    /* the pool is active */
void netmem_udp_setup(int sock, const char *tag);   /* SO_RUSRBUF + the biggest SO_RCVBUF the console accepts */
int netmem_rxdata(int sock);
void netmem_stop(void);                 /* at exit: shut the socket library down so the pool thread returns (else the console hangs leaving the app) */             /* bytes waiting in the socket's receive buffer (-1 unknown) */
/* UDP receive for the console: 64-byte aligned buffer; if reads fail with EMSGSIZE the read size steps down
 * (2048, 1472, 1024, 512) until one works. Returns bytes, 0 nothing waiting, -1 error (errno kept). */
typedef struct { unsigned char buf[2048] __attribute__((aligned(64))); int size, step, fails; unsigned ok; } NetRx;
int netmem_recv(int sock, NetRx *rx, const char *tag);
#endif
