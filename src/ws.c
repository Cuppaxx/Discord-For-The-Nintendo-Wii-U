#include "ws.h"
#include "nxnet.h"
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mbedtls_entropy_context ent; static mbedtls_ctr_drbg_context drbg; static bool seeded;
static SDL_mutex *rmx;
void ws_random(unsigned char *out, size_t n) {
  if (!rmx) rmx = SDL_CreateMutex(); SDL_LockMutex(rmx);
  if (!seeded) { mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg); mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"mingus-ws", 9); seeded = true; }
  mbedtls_ctr_drbg_random(&drbg, out, n); SDL_UnlockMutex(rmx);
}

static bool send_raw(Ws *w, const unsigned char *d, size_t n) {
  size_t off = 0; int spins = 0;
  while (off < n) { size_t sent = 0; CURLcode rc = curl_easy_send(w->c, d + off, n - off, &sent);
    if (rc == CURLE_OK) { off += sent; spins = 0; } else if (rc == CURLE_AGAIN) { SDL_Delay(5); if (++spins > 1000) return false; } else return false; }
  return true;
}
static bool send_frame(Ws *w, int opcode, const char *payload, size_t n) {
  unsigned char hdr[14]; int h = 0; hdr[h++] = 0x80 | opcode;
  if (n < 126) hdr[h++] = 0x80 | (unsigned char)n;
  else if (n < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (unsigned char)(n >> 8); hdr[h++] = (unsigned char)n; }
  else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)n >> (8 * i)); }
  unsigned char mask[4]; ws_random(mask, 4); memcpy(hdr + h, mask, 4); h += 4;
  unsigned char *buf = malloc(h + n); if (!buf) return false; memcpy(buf, hdr, h); for (size_t i = 0; i < n; i++) buf[h + i] = (unsigned char)payload[i] ^ mask[i & 3];
  bool ok = send_raw(w, buf, h + n); free(buf); return ok;
}
bool ws_send_text(Ws *w, const char *s, size_t n) { return w->c && send_frame(w, 1, s, n); }

static bool fill(Ws *w) {                 /* read whatever is available; false = closed/error */
  if (w->rn + 4096 > w->rcap) { size_t nc = w->rcap ? w->rcap * 2 : 65536; while (nc < w->rn + 4096) nc *= 2; if (nc > 1u << 22) nc = 1u << 22; if (nc <= w->rn + 16) return false; unsigned char *nb = realloc(w->rb, nc); if (!nb) return false; w->rb = nb; w->rcap = nc; }
  size_t got = 0; CURLcode rc = curl_easy_recv(w->c, w->rb + w->rn, w->rcap - w->rn, &got);
  if (rc == CURLE_OK) { if (got == 0) return false; w->rn += got; return true; }
  return rc == CURLE_AGAIN;
}
int ws_recv2(Ws *w, char *out, size_t cap, bool *is_bin, size_t *outlen) {
  if (!w->c) return -1;
  for (;;) {
    if (w->skip) {                        /* discarding an oversized frame */
      if (!w->rn) { if (!fill(w)) return -1; if (!w->rn) return 0; }
      size_t d = w->rn < w->skip ? w->rn : w->skip; w->skipped_total += d; if (w->skip_cb) w->skip_cb(w->skip_ud, w->rb, d); memmove(w->rb, w->rb + d, w->rn - d); w->rn -= d; w->skip -= d; if (!w->skip && w->skip_cb) w->skip_cb(w->skip_ud, NULL, 0); continue; }
    if (w->rn >= 2) {
      unsigned char op = w->rb[0] & 0x0F; uint64_t len = w->rb[1] & 0x7F; size_t hl = 2;
      if (len == 126) { if (w->rn < 4) goto more; len = ((uint64_t)w->rb[2] << 8) | w->rb[3]; hl = 4; }
      else if (len == 127) { if (w->rn < 10) goto more; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | w->rb[2 + i]; hl = 10; }
      if (len + hl > w->rcap - 16 || len >= cap) {           /* too big to keep: swallow it */
        size_t have = w->rn > hl ? w->rn - hl : 0; size_t total = (size_t)len; if (have > total) have = total; w->skip_op = op;
        if (w->skip_cb) w->skip_cb(w->skip_ud, w->rb + hl, have);
        if (have == total) { memmove(w->rb, w->rb + hl + total, w->rn - hl - total); w->rn -= hl + total; w->skip = 0; if (w->skip_cb) w->skip_cb(w->skip_ud, NULL, 0); }   /* whole frame was buffered: keep what follows it */
        else { w->skip = total - have; w->rn = 0; }
        continue; }
      if (w->rn < hl + len) goto more;
      int result = 0;
      if (op == 1 || op == 0 || op == 2) { memcpy(out, w->rb + hl, (size_t)len); out[len] = 0; result = 1; if (is_bin) *is_bin = (op == 2); if (outlen) *outlen = (size_t)len; }
      else if (op == 8) { if (len >= 2) snprintf(w->err, sizeof w->err, "close %d %.*s", w->rb[hl] << 8 | w->rb[hl + 1], (int)(len > 2 ? (len - 2 > 120 ? 120 : len - 2) : 0), (char *)w->rb + hl + 2); result = -1; }
      else if (op == 9) send_frame(w, 0xA, (const char *)w->rb + hl, (size_t)len);
      memmove(w->rb, w->rb + hl + len, w->rn - hl - (size_t)len); w->rn -= hl + (size_t)len;
      if (result) return result; continue;
    }
    more: { size_t before = w->rn; if (!fill(w)) return -1; if (w->rn == before) return 0; }
  }
}

int ws_recv(Ws *w, char *out, size_t cap) {      /* text frames only: binary frames are skipped */
  for (;;) { bool bin = false; size_t n = 0; int r = ws_recv2(w, out, cap, &bin, &n); if (r == 1 && bin) continue; return r; }
}
bool ws_send_binary(Ws *w, const void *d, size_t n) { return w->c && send_frame(w, 2, (const char *)d, n); }

bool ws_open(Ws *w, const char *url, const char *origin, int timeout_ms) {
  memset(w, 0, sizeof *w); w->rcap = 65536; w->rb = malloc(w->rcap); if (!w->rb) return false;
  CURL *c = curl_easy_init(); if (!c) return false; w->c = c;
  curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CAPATH, "/vol/content/certs");   /* curl's default CApath points into the system title and breaks the blob */
  const char *ca = nxnet_ca_data(); if (ca) { struct curl_blob bl = {(void *)ca, (size_t)nxnet_ca_len(), CURL_BLOB_COPY}; curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &bl); }
  char eb[CURL_ERROR_SIZE] = ""; curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) { snprintf(w->err, sizeof w->err, "connect: %s %s", curl_easy_strerror(rc), eb); ws_close(w); return false; }
  const char *p = strstr(url, "://"); p = p ? p + 3 : url; const char *sl = strchr(p, '/'); char host[200]; snprintf(host, sizeof host, "%.*s", (int)(sl ? sl - p : (int)strlen(p)), p); const char *path = sl ? sl : "/";
  unsigned char kb[16]; ws_random(kb, 16); unsigned char key[40]; size_t kl = 0; mbedtls_base64_encode(key, sizeof key, &kl, kb, 16); key[kl] = 0;
  char req[1200]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\nPragma: no-cache\r\nCache-Control: no-cache\r\n\r\n", path, host, key, origin);
  if (!send_raw(w, (unsigned char *)req, n)) { snprintf(w->err, sizeof w->err, "handshake send failed"); ws_close(w); return false; }
  char resp[4096]; size_t rn = 0; uint32_t t0 = SDL_GetTicks(); bool done = false;
  while (rn < sizeof resp - 1 && SDL_GetTicks() - t0 < (uint32_t)timeout_ms) {
    size_t got = 0; CURLcode r2 = curl_easy_recv(c, resp + rn, sizeof resp - 1 - rn, &got);
    if (r2 == CURLE_OK && got) { rn += got; resp[rn] = 0; char *e = strstr(resp, "\r\n\r\n"); if (e) { size_t used = (e + 4) - resp; if (rn > used) { memcpy(w->rb, resp + used, rn - used); w->rn = rn - used; } done = true; break; } }
    else if (r2 == CURLE_AGAIN || (r2 == CURLE_OK && !got)) SDL_Delay(10); else { snprintf(w->err, sizeof w->err, "closed during handshake"); ws_close(w); return false; }
  }
  if (!done || strncmp(resp, "HTTP/1.1 101", 12)) { snprintf(w->err, sizeof w->err, "upgrade refused: %.80s", done ? resp : "timeout"); char *nl = strpbrk(w->err, "\r\n"); if (nl) *nl = 0; ws_close(w); return false; }
  return true;
}
void ws_close(Ws *w) { ws_close_code(w, 1000); }
void ws_close_code(Ws *w, int code) { if (w->c) { char cb[2] = {(char)(code >> 8), (char)(code & 255)}; send_frame(w, 8, cb, 2); curl_easy_cleanup(w->c); w->c = NULL; } free(w->rb); w->rb = NULL; w->rn = w->rcap = 0; }
