#include "netmem.h"
#include "dlog.h"
#include <SDL2/SDL.h>
#include <nn/nets2/somemopt.h>
#include <sys/socket.h>
#include <nsysnet/_socket.h>
#include <malloc.h>
#include <errno.h>
#include <stdio.h>

#define NETMEM_SIZE (0x200000)           /* 2 MB (allowed: 128 KB .. 3 MB) */
static volatile int state;               /* 0 off, 1 init running (= active), 2 init failed / returned */
static volatile int init_rc;
static int worker(void *arg) {
  (void)arg; void *buf = memalign(0x40, NETMEM_SIZE); if (!buf) { state = 2; return 0; }
  state = 1; init_rc = somemopt(SOMEMOPT_REQUEST_INIT, buf, NETMEM_SIZE, SOMEMOPT_FLAGS_NONE);   /* blocks for the app's lifetime when it works */
  state = 2; dlog("[net] somemopt init returned %d (errno %d)", init_rc, errno);
  free(buf); return 0;
}
void netmem_start(void) {
  if (state) return; SDL_Thread *t = SDL_CreateThreadWithStackSize(worker, "somemopt", 64 * 1024, NULL); if (!t) { state = 2; dlog("[net] somemopt thread failed"); return; }
  SDL_DetachThread(t);
  for (int i = 0; i < 50 && state != 2; i++) SDL_Delay(4);   /* a quick failure shows up within a few ms */
  if (state == 1) { int used = somemopt(SOMEMOPT_REQUEST_GET_BYTES_USED, NULL, 0, SOMEMOPT_FLAGS_NONE); dlog("[net] socket memory pool active (%d KB, %d bytes in use)", NETMEM_SIZE / 1024, used); }
}
bool netmem_ok(void) { return state == 1; }
void netmem_stop(void) {
  if (state != 1) return; dlog("[exit] stopping socket library (pool thread)"); socket_lib_finish();
  for (int i = 0; i < 100 && state == 1; i++) SDL_Delay(10);
  dlog("[exit] pool thread %s", state == 1 ? "still blocked" : "done");
}
void netmem_udp_setup(int sock, const char *tag) {
  int on = 1, rus = -1; if (netmem_ok()) rus = setsockopt(sock, SOL_SOCKET, SO_RUSRBUF, &on, sizeof on);
  int got = 0; for (int sz = 512 * 1024; sz >= 8 * 1024; sz /= 2) if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz) == 0) { got = sz; break; }
  int cur = 0; socklen_t cl = sizeof cur; getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &cur, &cl);
  dlog("[net] %s udp: pool %s, RUSRBUF %s, RCVBUF set %d (now %d)", tag, netmem_ok() ? "on" : "off", rus == 0 ? "ok" : rus < 0 && !netmem_ok() ? "skipped" : "refused", got, cur);
}
int netmem_rxdata(int sock) { int v = -1; socklen_t l = sizeof v; if (getsockopt(sock, SOL_SOCKET, SO_RXDATA, &v, &l) != 0) return -1; return v; }

static const int RX_SIZES[] = {2048, 1472, 1024, 512};
int netmem_recv(int sock, NetRx *rx, const char *tag) {
  if (!rx->size) rx->size = RX_SIZES[0];
  int n = recvfrom(sock, rx->buf, rx->size, 0, NULL, NULL);
  if (n > 0) { if (rx->ok++ == 0) dlog("[net] %s: first UDP packet read (%d bytes, read size %d)", tag, n, rx->size); rx->fails = 0; return n; }
  if (n < 0 && errno == EMSGSIZE) {
    if (++rx->fails >= 20 && rx->step + 1 < (int)(sizeof RX_SIZES / sizeof RX_SIZES[0])) { rx->step++; rx->size = RX_SIZES[rx->step]; rx->fails = 0; dlog("[net] %s: reads refused (EMSGSIZE), trying read size %d", tag, rx->size); }
    return -1; }
  if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return 0;
  return n;
}
