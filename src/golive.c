/* golive.c - receive-only Go Live (screen share) connection. Same voice-gateway protocol as voice.c, but its own
 * websocket, UDP socket, transport key, SSRC namespace and DAVE MLS group (group id = rtc_server_id - 1).
 * Only signalling is sent (identify, select protocol, op 12 "no video of our own", op 15 sink wants, DAVE replies,
 * heartbeats, RTCP PLI keyframe requests): no audio or video goes out. */
#include "golive.h"
#include "vrecv.h"
#include "netmem.h"
#include "voice.h"
#include "ws.h"
#include "dlog.h"
#include "dave_c.h"
#include <SDL2/SDL.h>
#include <jansson.h>
#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>
#include <coreinit/thread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_mutex *gmx, *smx;                 /* gmx: start/stop, smx: state text */
static SDL_Thread *th, *dth; static volatile bool stop_flag, dstop;
static int state; static char errs[160]; static char owner[24];
static char skey[96], srv_id[24], chan_id[24], user_id[24], sess_id[80], token[160], endpoint[160];
static int sock = -1; static struct sockaddr_in paddr; static uint32_t my_ssrc; static volatile bool crypto_ready;
static int enc_mode; static unsigned char key[32]; static mbedtls_gcm_context gcm; static SDL_mutex *txmx; static uint32_t tx_nonce;
static uint32_t st_audio, st_video, st_rtcp, st_bad;

static void set_state(int s, const char *e) { SDL_LockMutex(smx); state = s; if (e) snprintf(errs, sizeof errs, "%s", e); SDL_UnlockMutex(smx); }
static const char *js(json_t *o, const char *k) { json_t *v = json_object_get(o, k); return json_is_string(v) ? json_string_value(v) : ""; }
static void send_json(Ws *w, json_t *o) { char *s = json_dumps(o, JSON_COMPACT); if (s) { ws_send_text(w, s, strlen(s)); free(s); } json_decref(o); }

/* ---------- transport AEAD (rtpsize modes), as in voice.c ---------- */
#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))
static uint32_t ld32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void st32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void hchacha20(const unsigned char k[32], const unsigned char n[16], unsigned char out[32]) {
  uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; i++) x[4 + i] = ld32(k + 4 * i); for (int i = 0; i < 4; i++) x[12 + i] = ld32(n + 4 * i);
  for (int i = 0; i < 10; i++) { QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]); QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]); }
  for (int i = 0; i < 4; i++) st32(out + 4 * i, x[i]); for (int i = 0; i < 4; i++) st32(out + 16 + 4 * i, x[12 + i]);
}
static int aead(bool enc, const unsigned char *aad, size_t aadlen, const unsigned char *in, size_t n, unsigned char *tag, unsigned char *out, uint32_t ctr) {
  unsigned char nonce[24] = {0}; nonce[0] = ctr >> 24; nonce[1] = ctr >> 16; nonce[2] = ctr >> 8; nonce[3] = ctr;
  if (enc_mode == 1) return enc ? mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, nonce, 12, aad, aadlen, in, out, 16, tag) : mbedtls_gcm_auth_decrypt(&gcm, n, nonce, 12, aad, aadlen, tag, 16, in, out);
  unsigned char sub[32]; hchacha20(key, nonce, sub); mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c); mbedtls_chachapoly_setkey(&c, sub); unsigned char n12[12] = {0}; memcpy(n12 + 4, nonce + 16, 8);
  int r = enc ? mbedtls_chachapoly_encrypt_and_tag(&c, n, n12, aad, aadlen, in, out, tag) : mbedtls_chachapoly_auth_decrypt(&c, n, n12, aad, aadlen, tag, in, out); mbedtls_chachapoly_free(&c); return r;
}

/* ---------- DAVE for the stream's own MLS group (all libdave calls under voice.c's DAVE lock) ---------- */
enum { D_INIT, D_EPOCH, D_PREP, D_EXEC, D_EXTSENDER, D_PROPOSALS, D_COMMIT, D_WELCOME };
typedef struct DJob { struct DJob *next; int kind, a, b; uint8_t *data; size_t len; } DJob;
typedef struct OMsg { struct OMsg *next; bool bin; uint8_t *data; size_t len; } OMsg;
static SDL_mutex *qmx, *umx; static SDL_sem *dsem; static DJob *dq_h, *dq_t; static OMsg *ob_h, *ob_t;
static dave_session_t *dsess; static bool d_inited, d_group, rx_pass = true; static int d_pend_valid, d_pend_tid, d_pend_ver; static volatile int dave_ver;
static uint8_t *d_ext; static size_t d_ext_len; static uint64_t group_id;
typedef struct { char uid[24]; dave_decryptor_t *dd; } VDec; static VDec vd[16]; static int nvd;   /* under the DAVE lock */
static char known[64][24]; static int nknown;
#define DLOCK() SDL_LockMutex(voice_dave_mutex())
#define DUNLOCK() SDL_UnlockMutex(voice_dave_mutex())

static void dq_push(int kind, int a, int b, const uint8_t *data, size_t len) {
  DJob *j = calloc(1, sizeof *j); if (!j) return; j->kind = kind; j->a = a; j->b = b;
  if (len) { j->data = malloc(len); if (!j->data) { free(j); return; } memcpy(j->data, data, len); j->len = len; }
  SDL_LockMutex(qmx); if (dq_t) dq_t->next = j; else dq_h = j; dq_t = j; SDL_UnlockMutex(qmx); SDL_SemPost(dsem);
}
static DJob *dq_pop(void) { SDL_LockMutex(qmx); DJob *j = dq_h; if (j) { dq_h = j->next; if (!dq_h) dq_t = NULL; } SDL_UnlockMutex(qmx); return j; }
static void ob_push(bool bin, const uint8_t *d, size_t n) {
  OMsg *m = calloc(1, sizeof *m); if (!m) return; m->data = malloc(n + 1); if (!m->data) { free(m); return; } memcpy(m->data, d, n); m->data[n] = 0; m->len = n; m->bin = bin;
  SDL_LockMutex(qmx); if (ob_t) ob_t->next = m; else ob_h = m; ob_t = m; SDL_UnlockMutex(qmx);
}
static void ob_flush(Ws *w) { for (;;) { SDL_LockMutex(qmx); OMsg *m = ob_h; if (m) { ob_h = m->next; if (!ob_h) ob_t = NULL; } SDL_UnlockMutex(qmx); if (!m) return; if (m->bin) ws_send_binary(w, m->data, m->len); else ws_send_text(w, (const char *)m->data, m->len); free(m->data); free(m); } }
static void send_tid(int op, int tid) { char b[64]; int n = snprintf(b, sizeof b, "{\"op\":%d,\"d\":{\"transition_id\":%d}}", op, tid); ob_push(false, (const uint8_t *)b, n); }
static void user_add(const char *id) { if (!id || !id[0] || strlen(id) >= 24) return; SDL_LockMutex(umx); bool have = false; for (int i = 0; i < nknown; i++) if (!strcmp(known[i], id)) have = true; if (!have && nknown < 64) snprintf(known[nknown++], 24, "%s", id); SDL_UnlockMutex(umx); }
static int recognized(char ids[][24], const char **p) {
  int n = 0; SDL_LockMutex(umx); for (int i = 0; i < nknown && n < 64; i++) { snprintf(ids[n], 24, "%s", known[i]); p[n] = ids[n]; n++; } SDL_UnlockMutex(umx);
  bool self = false; for (int i = 0; i < n; i++) if (!strcmp(p[i], user_id)) self = true; if (!self) { snprintf(ids[n], 24, "%s", user_id); p[n] = ids[n]; n++; } return n;
}
static void key_package(void) { uint8_t *kp = NULL; size_t kn = 0; if (dave_session_get_key_package(dsess, &kp, &kn) != 0 || !kp) { dlog("[golive] dave key package failed"); return; }
  uint8_t *m = malloc(kn + 1); if (m) { m[0] = 26; memcpy(m + 1, kp, kn); ob_push(true, m, kn + 1); free(m); } dave_free(kp); dlog("[golive] dave key package sent (%u bytes)", (unsigned)kn); }
static void session_init(int ver) { dave_session_init(dsess, (uint16_t)ver, group_id, user_id); d_inited = true; d_group = false; if (d_ext) dave_session_set_external_sender(dsess, d_ext, d_ext_len); key_package(); }
static void rekey(void) { if (!d_group) return; for (int i = 0; i < nvd; i++) { if (dave_decryptor_set_ratchet(vd[i].dd, dsess, vd[i].uid) != 0) dlog("[golive] no ratchet for %s", vd[i].uid); dave_decryptor_set_passthrough(vd[i].dd, rx_pass ? 1 : 0); } }
static void do_execute(int ver) {
  if (ver == 0) { dave_ver = 0; rx_pass = true; } else if (d_group) { dave_ver = ver; rx_pass = false; } else { dlog("[golive] execute v%d without group", ver); return; }
  for (int i = 0; i < nvd; i++) dave_decryptor_set_passthrough(vd[i].dd, rx_pass ? 1 : 0);
  vr_set_dave(VR_CONN_STREAM, dave_ver > 0); dlog("[golive] dave transition executed: v%d", ver);
}
static void finish(int tid, int ver) { d_pend_valid = 1; d_pend_tid = tid; d_pend_ver = ver; rekey(); if (tid == 0) { do_execute(ver); d_pend_valid = 0; } else send_tid(23, tid); }
static void dave_job(DJob *j) {
  DLOCK();
  if (j->kind == D_EXTSENDER) { free(d_ext); d_ext = malloc(j->len ? j->len : 1); if (d_ext) { memcpy(d_ext, j->data, j->len); d_ext_len = j->len; } if (d_inited && d_ext) dave_session_set_external_sender(dsess, d_ext, d_ext_len); DUNLOCK(); return; }
  if (!dsess) dsess = dave_session_new(NULL, NULL); if (!dsess) { DUNLOCK(); return; }
  switch (j->kind) {
  case D_INIT: dlog("[golive] dave init v%d group %llu", j->a, (unsigned long long)group_id); session_init(j->a); break;
  case D_EPOCH: if (j->b == 1 || !d_inited) session_init(j->a); else dave_session_set_protocol_version(dsess, (uint16_t)j->a); break;
  case D_PREP: d_pend_valid = 1; d_pend_ver = j->a; d_pend_tid = j->b; if (j->b == 0) { do_execute(j->a); d_pend_valid = 0; } else send_tid(23, j->b); break;
  case D_EXEC: if (d_pend_valid && d_pend_tid == j->a) { d_pend_valid = 0; do_execute(d_pend_ver); } break;
  case D_PROPOSALS: { if (!d_inited) break; static char ids[65][24]; const char *p[65]; int n = recognized(ids, p); uint8_t *out = NULL; size_t on = 0;
    dave_session_process_proposals(dsess, j->data, j->len, p, (size_t)n, &out, &on);
    if (out && on) { uint8_t *m = malloc(on + 1); if (m) { m[0] = 28; memcpy(m + 1, out, on); ob_push(true, m, on + 1); free(m); } } if (out) dave_free(out); break; }
  case D_COMMIT: { if (!d_inited) break; uint64_t *ro = NULL; size_t rn = 0; int rc = dave_session_process_commit(dsess, j->data, j->len, &ro, &rn); if (ro) dave_free(ro); dlog("[golive] dave commit tid %d -> %d", j->a, rc);
    if (rc == 0) { d_group = true; finish(j->a, dave_session_get_protocol_version(dsess)); } else if (rc == 1) { send_tid(31, j->a); session_init(dave_session_get_protocol_version(dsess)); } break; }
  case D_WELCOME: { if (!d_inited) break; static char ids[65][24]; const char *p[65]; int n = recognized(ids, p); uint64_t *ro = NULL; size_t rn = 0;
    int rc = dave_session_process_welcome(dsess, j->data, j->len, p, (size_t)n, &ro, &rn); if (ro) dave_free(ro); dlog("[golive] dave welcome tid %d -> %d", j->a, rc);
    if (rc == 0) { d_group = true; finish(j->a, dave_session_get_protocol_version(dsess)); } else { send_tid(31, j->a); session_init(dave_session_get_protocol_version(dsess)); } break; }
  }
  DUNLOCK();
}
static int dave_thread(void *a) { (void)a; while (!dstop) { SDL_SemWaitTimeout(dsem, 250); DJob *j; while ((j = dq_pop())) { if (!dstop) dave_job(j); free(j->data); free(j); } } return 0; }
/* VR decrypt callback (decode thread) */
static int stream_decrypt(int conn, const char *uid, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {
  (void)conn; DLOCK(); VDec *v = NULL; for (int i = 0; i < nvd; i++) if (!strcmp(vd[i].uid, uid)) v = &vd[i];
  if (!v && nvd < 16) { v = &vd[nvd]; snprintf(v->uid, 24, "%s", uid); v->dd = dave_decryptor_new(); if (!v->dd) v = NULL; else { nvd++; dave_decryptor_set_passthrough(v->dd, rx_pass ? 1 : 0); if (d_group && dave_decryptor_set_ratchet(v->dd, dsess, uid) != 0) dlog("[golive] no ratchet for %s yet", uid); } }
  int rc = v ? dave_decrypt_video(v->dd, in, n, out, cap, w) : -1; DUNLOCK(); return rc;
}

/* ---------- RTCP PLI (keyframe request) ---------- */
static void send_pli(int conn, uint32_t media) {
  (void)conn; if (!crypto_ready || sock < 0) return; unsigned char p[12 + 16 + 4];
  p[0] = 0x81; p[1] = 206; p[2] = 0; p[3] = 2; p[4] = my_ssrc >> 24; p[5] = my_ssrc >> 16; p[6] = my_ssrc >> 8; p[7] = my_ssrc;
  unsigned char body[4] = {media >> 24, media >> 16, media >> 8, media};
  SDL_LockMutex(txmx); uint32_t ctr = ++tx_nonce; if (aead(true, p, 8, body, 4, p + 12, p + 8, ctr) == 0) { p[28] = ctr >> 24; p[29] = ctr >> 16; p[30] = ctr >> 8; p[31] = ctr; sendto(sock, p, 32, 0, (struct sockaddr *)&paddr, sizeof paddr); } SDL_UnlockMutex(txmx);
}

/* ---------- RTP in ---------- */
static void rx(unsigned char *pkt, int len) {
  if (len < 12 + 16 + 4 || (pkt[0] & 0xC0) != 0x80) { st_bad++; return; }
  int pt = pkt[1] & 0x7F; if (pt >= 72 && pt <= 76) { st_rtcp++; return; }
  int hl = 12 + 4 * (pkt[0] & 0x0F); bool ext = pkt[0] & 0x10; if (ext) hl += 4; if (len < hl + 16 + 4) { st_bad++; return; }
  uint32_t ssrc = (uint32_t)pkt[8] << 24 | pkt[9] << 16 | pkt[10] << 8 | pkt[11]; uint16_t seq = pkt[2] << 8 | pkt[3]; uint32_t ts = (uint32_t)pkt[4] << 24 | pkt[5] << 16 | pkt[6] << 8 | pkt[7];
  uint32_t ctr = (uint32_t)pkt[len - 4] << 24 | pkt[len - 3] << 16 | pkt[len - 2] << 8 | pkt[len - 1];
  int clen = len - hl - 16 - 4; static unsigned char plain[2048]; if (clen > (int)sizeof plain) { st_bad++; return; }
  if (aead(false, pkt, hl, pkt + hl, clen, pkt + hl + clen, plain, ctr) != 0) { st_bad++; return; }
  unsigned char *pl = plain; int n = clen;
  if (ext) { int words = pkt[hl - 2] << 8 | pkt[hl - 1]; int skip = words * 4; if (skip > n) return; pl += skip; n -= skip; }
  if ((pkt[0] & 0x20) && n > 0) { int pad = pl[n - 1]; if (pad > n) return; n -= pad; }
  if (vr_rtp(VR_CONN_STREAM, ssrc, pt, seq, ts, pkt[1] & 0x80, pl, n)) st_video++; else st_audio++;   /* stream audio (soundshare) is not played yet */
}

static bool discover(const char *ip, int port, char *out_ip, int *out_port) {
  sock = socket(AF_INET, SOCK_DGRAM, 0); if (sock < 0) return false;
  memset(&paddr, 0, sizeof paddr); paddr.sin_family = AF_INET; paddr.sin_port = htons((uint16_t)port); paddr.sin_addr.s_addr = inet_addr(ip);
  int one = 1; setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &one, sizeof one); netmem_udp_setup(sock, "golive");
  unsigned char pkt[74] = {0}; pkt[1] = 1; pkt[3] = 70; pkt[4] = my_ssrc >> 24; pkt[5] = my_ssrc >> 16; pkt[6] = my_ssrc >> 8; pkt[7] = my_ssrc;
  for (int a = 0; a < 5 && !stop_flag; a++) { sendto(sock, pkt, 74, 0, (struct sockaddr *)&paddr, sizeof paddr);
    for (int i = 0; i < 100 && !stop_flag; i++) { unsigned char r[128]; int n = recvfrom(sock, r, sizeof r, 0, NULL, NULL); if (n >= 74 && r[1] == 2) { snprintf(out_ip, 64, "%.63s", (char *)r + 8); *out_port = r[72] << 8 | r[73]; return true; } SDL_Delay(10); } }
  return false;
}
static void handle_op12(json_t *d) {   /* the streamer's video ssrcs */
  const char *uid = vr_on_video_op(VR_CONN_STREAM, d); if (uid) user_add(uid);
}
static int worker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0); OSSetThreadPriority(OSGetCurrentThread(), 17);
  static char msg[65536]; Ws w; char host[170]; snprintf(host, sizeof host, "%s", endpoint); char *c = strchr(host, ':'); int eport = c ? atoi(c + 1) : 0; if (c) *c = 0;
  char url[260]; if (eport > 0 && eport != 443 && eport != 80) snprintf(url, sizeof url, "wss://%s:%d/?v=8", host, eport); else snprintf(url, sizeof url, "wss://%s/?v=8", host);   /* keep a real port (e.g. :2096) */
  set_state(GL_CONNECTING, "");
  if (!ws_open(&w, url, "https://discord.com", 15000)) { dlog("[golive] %s", w.err); set_state(GL_ERROR, w.err); return 0; }
  dlog("[golive] connected to %s for %s", host, skey);
  { json_t *id = json_object(); json_object_set_new(id, "server_id", json_string(srv_id)); json_object_set_new(id, "channel_id", json_string(chan_id)); json_object_set_new(id, "user_id", json_string(user_id));
    json_object_set_new(id, "session_id", json_string(sess_id)); json_object_set_new(id, "token", json_string(token)); json_object_set_new(id, "video", json_true());
    json_t *st = json_object(); json_object_set_new(st, "type", json_string("screen")); json_object_set_new(st, "rid", json_string("100")); json_object_set_new(st, "quality", json_integer(100)); json_t *sa = json_array(); json_array_append_new(sa, st); json_object_set_new(id, "streams", sa);
    json_object_set_new(id, "max_dave_protocol_version", json_integer(dave_max_protocol_version()));
    json_t *o = json_object(); json_object_set_new(o, "op", json_integer(0)); json_object_set_new(o, "d", id); send_json(&w, o); }
  uint32_t hb_iv = 13750, last_hb = SDL_GetTicks(), st_t = SDL_GetTicks(); long long seq = -1; bool hello = false, udp = false; int last_q = -1;
  while (!stop_flag) {
    ob_flush(&w); bool bin = false; size_t mlen = 0; int r = ws_recv2(&w, msg, sizeof msg, &bin, &mlen);
    if (r < 0) { dlog("[golive] closed: %s", w.err); set_state(GL_ERROR, w.err[0] ? w.err : "stream connection closed"); break; }
    if (r == 1 && bin) { if (mlen >= 3) { const uint8_t *m = (const uint8_t *)msg; seq = m[0] << 8 | m[1]; int bop = m[2]; const uint8_t *pl = m + 3; size_t pn = mlen - 3;
        if (bop == 25) dq_push(D_EXTSENDER, 0, 0, pl, pn); else if (bop == 27) dq_push(D_PROPOSALS, 0, 0, pl, pn);
        else if ((bop == 29 || bop == 30) && pn >= 2) dq_push(bop == 29 ? D_COMMIT : D_WELCOME, pl[0] << 8 | pl[1], 0, pl + 2, pn - 2); } }
    else if (r == 1) {
      json_t *j = json_loads(msg, 0, NULL); if (!j) continue; int op = (int)json_integer_value(json_object_get(j, "op")); json_t *d = json_object_get(j, "d"); json_t *sq = json_object_get(j, "seq"); if (json_is_integer(sq)) seq = json_integer_value(sq);
      if (op == 8) { hello = true; json_t *hi = d ? json_object_get(d, "heartbeat_interval") : NULL; if (json_is_number(hi)) hb_iv = (uint32_t)json_number_value(hi); last_hb = SDL_GetTicks(); }
      else if (op == 2 && d) {
        my_ssrc = (uint32_t)json_integer_value(json_object_get(d, "ssrc")); const char *ip = js(d, "ip"); int port = (int)json_integer_value(json_object_get(d, "port"));
        bool gcm_ok = false, xc_ok = false; size_t k; json_t *m; json_array_foreach(json_object_get(d, "modes"), k, m) if (json_is_string(m)) { if (!strcmp(json_string_value(m), "aead_aes256_gcm_rtpsize")) gcm_ok = true; if (!strcmp(json_string_value(m), "aead_xchacha20_poly1305_rtpsize")) xc_ok = true; }
        { char *sd = json_dumps(json_object_get(d, "streams"), JSON_COMPACT); dlog("[golive] ready ssrc=%u %s:%d streams=%s", (unsigned)my_ssrc, ip, port, sd ? sd : "-"); free(sd); }
        char eip[64]; int eport = 0; if (!discover(ip, port, eip, &eport)) { set_state(GL_ERROR, "UDP to the stream server failed"); json_decref(j); break; } udp = true;
        const char *mode = gcm_ok ? "aead_aes256_gcm_rtpsize" : xc_ok ? "aead_xchacha20_poly1305_rtpsize" : NULL; if (!mode) { set_state(GL_ERROR, "no supported encryption mode"); json_decref(j); break; }
        json_t *data = json_object(); json_object_set_new(data, "address", json_string(eip)); json_object_set_new(data, "port", json_integer(eport)); json_object_set_new(data, "mode", json_string(mode));
        json_t *sd = json_object(); json_object_set_new(sd, "protocol", json_string("udp")); json_object_set_new(sd, "data", data); json_object_set_new(sd, "codecs", voice_codecs_json());
        json_t *so = json_object(); json_object_set_new(so, "op", json_integer(1)); json_object_set_new(so, "d", sd); send_json(&w, so); }
      else if (op == 4 && d) {
        const char *mode = js(d, "mode"); json_t *sk = json_object_get(d, "secret_key"); int dv = (int)json_integer_value(json_object_get(d, "dave_protocol_version"));
        if (json_is_array(sk) && json_array_size(sk) == 32) for (int i = 0; i < 32; i++) key[i] = (unsigned char)json_integer_value(json_array_get(sk, i));
        enc_mode = !strcmp(mode, "aead_aes256_gcm_rtpsize") ? 1 : 2; if (enc_mode == 1) { mbedtls_gcm_init(&gcm); mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256); }
        crypto_ready = true; set_state(GL_CONNECTED, "");
        dlog("[golive] session description mode=%s dave=%d video_codec=%s media_session_id=%s (MLS group %llu)", mode, dv, js(d, "video_codec"), js(d, "media_session_id"), (unsigned long long)group_id);
        if (dv > 0) { dave_ver = dv; vr_set_dave(VR_CONN_STREAM, true); dq_push(D_INIT, dv, 0, NULL, 0); }
        char b[160]; snprintf(b, sizeof b, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":0,\"rtx_ssrc\":0,\"streams\":[]}}", (unsigned)my_ssrc); ws_send_text(&w, b, strlen(b));
        last_q = 100; snprintf(b, sizeof b, "{\"op\":15,\"d\":{\"any\":%d}}", last_q); ws_send_text(&w, b, strlen(b)); }
      else if (op == 12 && d) handle_op12(d);
      else if (op == 5 && d) user_add(js(d, "user_id"));
      else if (op == 11 && d) { size_t k; json_t *u; json_array_foreach(json_object_get(d, "user_ids"), k, u) if (json_is_string(u)) user_add(json_string_value(u)); }
      else if (op == 13 && d) { const char *uid = js(d, "user_id"); vr_map(VR_CONN_STREAM, uid, NULL, NULL, 0); }
      else if (op == 21 && d) dq_push(D_PREP, (int)json_integer_value(json_object_get(d, "protocol_version")), (int)json_integer_value(json_object_get(d, "transition_id")), NULL, 0);
      else if (op == 22 && d) dq_push(D_EXEC, (int)json_integer_value(json_object_get(d, "transition_id")), 0, NULL, 0);
      else if (op == 24 && d) dq_push(D_EPOCH, (int)json_integer_value(json_object_get(d, "protocol_version")), (int)json_integer_value(json_object_get(d, "epoch")), NULL, 0);
      else if (op != 6 && op != 18 && op != 20 && op != 15 && op != 14 && op != 16 && op != 17) dlog("[golive] op %d", op);
      json_decref(j);
    } else {
      if (hello && SDL_GetTicks() - last_hb >= hb_iv) { char b[96]; snprintf(b, sizeof b, "{\"op\":3,\"d\":{\"t\":%u,\"seq_ack\":%lld}}", (unsigned)SDL_GetTicks(), seq); ws_send_text(&w, b, strlen(b)); last_hb = SDL_GetTicks(); }
      if (udp && crypto_ready) { static NetRx grx; for (int i = 0; i < 64; i++) { int n = netmem_recv(sock, &grx, "golive"); if (n <= 0) break; rx(grx.buf, n); } }
      int q = vr_sink_quality(VR_CONN_STREAM); if (crypto_ready && q != last_q) { char b[64]; snprintf(b, sizeof b, "{\"op\":15,\"d\":{\"any\":%d}}", q); ws_send_text(&w, b, strlen(b)); last_q = q; }
      if (SDL_GetTicks() - st_t > 10000) { st_t = SDL_GetTicks(); dlog("[golive] stats: video pkts %u, audio pkts %u (not played), rtcp %u, bad %u", (unsigned)st_video, (unsigned)st_audio, (unsigned)st_rtcp, (unsigned)st_bad); }
      SDL_Delay(2);
    }
  }
  ws_close(&w); return 0;
}

void golive_start(const char *key_, const char *rtc_server_id, const char *rtc_channel_id, const char *uid, const char *sess, const char *tok, const char *ep) {
  if (!gmx) { gmx = SDL_CreateMutex(); smx = SDL_CreateMutex(); txmx = SDL_CreateMutex(); qmx = SDL_CreateMutex(); umx = SDL_CreateMutex(); }
  golive_stop(); SDL_LockMutex(gmx);
  snprintf(skey, sizeof skey, "%s", key_); snprintf(srv_id, sizeof srv_id, "%s", rtc_server_id); snprintf(chan_id, sizeof chan_id, "%s", rtc_channel_id); snprintf(user_id, sizeof user_id, "%s", uid);
  snprintf(sess_id, sizeof sess_id, "%s", sess); snprintf(token, sizeof token, "%s", tok); snprintf(endpoint, sizeof endpoint, "%s", ep);
  const char *o = strrchr(skey, ':'); snprintf(owner, sizeof owner, "%s", o ? o + 1 : "");
  group_id = strtoull(srv_id, NULL, 10) - 1;   /* docs.discord.food: stream MLS group id = rtc_server_id - 1 */
  stop_flag = dstop = false; crypto_ready = false; enc_mode = 0; tx_nonce = 0; sock = -1; nknown = 0; d_inited = d_group = false; rx_pass = true; d_pend_valid = 0; dave_ver = 0; nvd = 0;
  st_audio = st_video = st_rtcp = st_bad = 0; set_state(GL_CONNECTING, "");
  user_add(owner); vr_init(); vr_set_conn(VR_CONN_STREAM, stream_decrypt, send_pli); vr_set_dave(VR_CONN_STREAM, false);
  if (dsem) SDL_DestroySemaphore(dsem); dsem = SDL_CreateSemaphore(0);
  dlog("[golive] start %s rtc_server %s rtc_channel %s endpoint %s", skey, srv_id, chan_id, endpoint);
  dth = SDL_CreateThreadWithStackSize(dave_thread, "gl_dave", 1024 * 1024, NULL);
  th = SDL_CreateThreadWithStackSize(worker, "golive", 256 * 1024, NULL);
  SDL_UnlockMutex(gmx);
}
void golive_stop(void) {
  if (!gmx) return; SDL_LockMutex(gmx);
  if (th) { stop_flag = true; SDL_WaitThread(th, NULL); th = NULL; }
  if (dth) { dstop = true; SDL_SemPost(dsem); SDL_WaitThread(dth, NULL); dth = NULL;
    DJob *j; while ((j = dq_pop())) { free(j->data); free(j); } SDL_LockMutex(qmx); OMsg *m = ob_h; ob_h = ob_t = NULL; SDL_UnlockMutex(qmx); while (m) { OMsg *n = m->next; free(m->data); free(m); m = n; }
    vr_conn_reset(VR_CONN_STREAM); vr_set_dave(VR_CONN_STREAM, false);
    DLOCK(); for (int i = 0; i < nvd; i++) dave_decryptor_free(vd[i].dd); nvd = 0; if (dsess) { dave_session_free(dsess); dsess = NULL; } free(d_ext); d_ext = NULL; d_ext_len = 0; DUNLOCK();
    dlog("[golive] stopped (%s): video pkts %u", skey, (unsigned)st_video); }
  if (sock >= 0) { close(sock); sock = -1; } if (enc_mode == 1) mbedtls_gcm_free(&gcm); enc_mode = 0; crypto_ready = false; owner[0] = 0; set_state(GL_OFF, NULL);
  SDL_UnlockMutex(gmx);
}
int golive_state(char *err, int cap) { if (!smx) return GL_OFF; SDL_LockMutex(smx); int s = state; if (err && cap > 0) snprintf(err, cap, "%s", errs); SDL_UnlockMutex(smx); return s; }
const char *golive_owner(void) { return owner; }
