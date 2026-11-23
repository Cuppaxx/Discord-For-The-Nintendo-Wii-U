#ifndef WS_H
#define WS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Minimal WebSocket client over libcurl's connect-only TLS socket (shared by the gateway and voice threads). */
typedef struct {
  void *c;                     /* CURL* */
  unsigned char *rb; size_t rn, rcap;
  size_t skip;                 /* bytes of an oversized frame still to discard */
  int skip_op;
  void (*skip_cb)(void *ud, const unsigned char *p, size_t n);   /* sees the payload of oversized frames as it streams past (n == 0: frame ended) */
  void *skip_ud;
  size_t skipped_total;        /* bytes of oversized frames consumed (progress of a big READY) */
  char err[200];
} Ws;
bool ws_open(Ws *w, const char *url, const char *origin, int timeout_ms);   /* TLS connect + HTTP upgrade */
bool ws_send_text(Ws *w, const char *s, size_t n);
/* 1 = text message copied to out (NUL terminated, truncated to cap-1), 0 = nothing yet, -1 = closed / error.
 * Frames larger than cap are consumed and dropped (the main gateway READY can be megabytes). */
int ws_recv(Ws *w, char *out, size_t cap);
/* like ws_recv but also returns binary frames (op 2): *is_bin set, *outlen = payload length (text is NUL terminated too) */
int ws_recv2(Ws *w, char *out, size_t cap, bool *is_bin, size_t *outlen);
bool ws_send_binary(Ws *w, const void *d, size_t n);
void ws_close(Ws *w);
void ws_close_code(Ws *w, int code);   /* 1000 ends a Discord session for good; 4000 keeps it resumable */
void ws_random(unsigned char *out, size_t n);
#endif
