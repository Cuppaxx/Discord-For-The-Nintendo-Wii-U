#include "voice.h"
#include "ws.h"
#include "dlog.h"
#include "dave_c.h"
#include "vrecv.h"
#include "golive.h"
#include "vsend.h"
#include "gw.h"
#include "netmem.h"
#include <errno.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <jansson.h>
#include <opus/opus.h>
#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>
#include <coreinit/thread.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <mic/mic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define FRAME 960            /* 20 ms @ 48 kHz */
#define JB 64                /* jitter buffer slots per peer (by RTP sequence number) */
#define JB_START 3           /* frames buffered before a speaker starts playing (60 ms) */
#define MAXF 5760            /* longest Opus packet: 120 ms @ 48 kHz (some clients send 40/60 ms packets) */
#define JB_MAX 10            /* more than this many frames ahead (200 ms): drop one to keep latency down */
#define RING (48000 * 2)     /* output ring: 1 s of stereo samples */
#define RING_TARGET (960 * 2 * 5)   /* keep ~100 ms mixed ahead of the speaker */

#define PKT_MAX 3000         /* one Opus packet: a 40/60 ms packet holds several frames and can exceed the 1275-byte single-frame limit */
typedef struct { uint16_t seq, len; bool have; unsigned char data[PKT_MAX]; } JSlot;
typedef struct { VcPeer p; dave_decryptor_t *dd; OpusDecoder *dec; JSlot jb[JB]; uint16_t play_seq; bool playing, seen; int plc_run;  unsigned probe; int last_dur, bad_dec; int pend_n, pend_off; int16_t pend[(MAXF + FRAME) * 2]; } Peer;

static VcInfo I; static SDL_mutex *mx; static SDL_Thread *th, *mic_th; static volatile bool stop_flag;
static char server_id[24], user_id[24], session_id[80], vtoken[120], endpoint[160];
static int sock = -1; static struct sockaddr_in peer_addr; static uint32_t my_ssrc; static bool udp_ready;
static int enc_mode;          /* 0 none, 1 aes256-gcm, 2 xchacha20-poly1305 */
static unsigned char key[32]; static mbedtls_gcm_context gcm; static mbedtls_chachapoly_context cpoly; static bool crypto_ready;
static uint32_t tx_nonce, tx_ts; static uint16_t tx_seq; static SDL_mutex *txmx;
static Peer peers[VC_MAX_PEERS]; static int npeers;
static SDL_AudioDeviceID adev; static OpusEncoder *enc;
static volatile bool muted; static volatile bool speaking_sent; static volatile int want_speaking = -1;
static Ws *g_ws;
static volatile uint32_t st_dec, st_mix, st_mic, st_sent, st_peak, st_raw, st_aead_bad, st_rxerr; static volatile int st_lasterr;   /* stats for the log */
static char chan_id[24];

#define LOCK() SDL_LockMutex(mx)
#define UNLOCK() SDL_UnlockMutex(mx)
static const char *js(json_t *o, const char *k) { json_t *v = json_object_get(o, k); return json_is_string(v) ? json_string_value(v) : ""; }
static void set_state(int s, const char *err) { LOCK(); I.state = s; if (err) snprintf(I.err, sizeof I.err, "%s", err); UNLOCK(); }
static void send_json(Ws *w, json_t *o) { char *s = json_dumps(o, JSON_COMPACT); if (s) { ws_send_text(w, s, strlen(s)); free(s); } json_decref(o); }


/* ---------- DAVE (E2EE) ----------
 * All libdave/OpenSSL calls are serialised by dmx (OpenSSL is built no-threads). The heavy MLS work runs on its own
 * 1 MB-stack thread fed by a job queue; it never touches the websocket: replies go through an outbox the voice thread drains.
 * Lock order: dmx -> mx (never the other way). Audio paths only TRY dmx and drop the frame if the MLS thread holds it. */
enum { D_INIT, D_EPOCH, D_PREP, D_EXEC, D_EXTSENDER, D_PROPOSALS, D_COMMIT, D_WELCOME, D_PEERS };
typedef struct DJob { struct DJob *next; int kind, a, b; uint8_t *data; size_t len; } DJob;
typedef struct OMsg { struct OMsg *next; bool bin; uint8_t *data; size_t len; } OMsg;
static SDL_mutex *dmx, *qmx, *umx; static SDL_sem *dsem; static SDL_Thread *dth; static volatile bool dstop;
static DJob *dq_h, *dq_t; static OMsg *ob_h, *ob_t;
static dave_session_t *dsess; static dave_encryptor_t *denc;
static bool d_inited, d_group; static int d_pend_valid, d_pend_tid, d_pend_ver; static uint8_t *d_ext; static size_t d_ext_len;
static volatile int dave_ver;           /* current media protocol version (0 = passthrough) */
static volatile bool tx_encrypt;        /* outgoing audio goes through dave_encrypt_opus */
static bool rx_pass = true;             /* decryptors accept non-DAVE frames (until the first v>0 transition executes) */
static char known[64][24]; static int nknown; static int dlog_budget;
#define DLOCK() SDL_LockMutex(dmx)
#define DUNLOCK() SDL_UnlockMutex(dmx)

static int dave_entropy(void *b, size_t n) { ws_random((unsigned char *)b, n); return 0; }
static void dave_log(int sev, const char *file, int line, const char *msg) { (void)file; (void)line; if (sev >= 2 && dlog_budget > 0) { dlog_budget--; dlog("[dave] %s", msg); } }
static void dave_fail(const char *src, const char *why, void *u) { (void)u; dlog("[dave] failure: %s: %s", src ? src : "?", why ? why : "?"); }

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
static void ob_flush(Ws *w) {            /* voice thread only */
  for (;;) { SDL_LockMutex(qmx); OMsg *m = ob_h; if (m) { ob_h = m->next; if (!ob_h) ob_t = NULL; } SDL_UnlockMutex(qmx); if (!m) return;
    if (m->bin) ws_send_binary(w, m->data, m->len); else ws_send_text(w, (const char *)m->data, m->len); free(m->data); free(m); }
}
static void queues_clear(void) {
  DJob *j; while ((j = dq_pop())) { free(j->data); free(j); }
  SDL_LockMutex(qmx); OMsg *m = ob_h; ob_h = ob_t = NULL; SDL_UnlockMutex(qmx); while (m) { OMsg *n = m->next; free(m->data); free(m); m = n; }
}
static void send_tid(int op, int tid) { char b[64]; int n = snprintf(b, sizeof b, "{\"op\":%d,\"d\":{\"transition_id\":%d}}", op, tid); ob_push(false, (const uint8_t *)b, n); }
static void user_add(const char *id) { if (!id || !id[0] || strlen(id) >= 24) return; SDL_LockMutex(umx); bool have = false; for (int i = 0; i < nknown; i++) if (!strcmp(known[i], id)) have = true; if (!have && nknown < 64) snprintf(known[nknown++], 24, "%s", id); SDL_UnlockMutex(umx); }
static void user_del(const char *id) { SDL_LockMutex(umx); for (int i = 0; i < nknown; i++) if (!strcmp(known[i], id)) { memmove(known[i], known[i + 1], (nknown - i - 1) * 24); nknown--; break; } SDL_UnlockMutex(umx); }

static bool ensure_session(void) {       /* dmx held */
  if (!dsess) { dlog("[dave] > session_new"); dsess = dave_session_new(dave_fail, NULL); dlog("[dave] < session_new %p", (void *)dsess); }
  if (!denc) { dlog("[dave] > encryptor_new"); denc = dave_encryptor_new(); dlog("[dave] < encryptor_new %p", (void *)denc); }
  if (!dsess || !denc) { dlog("[dave] session alloc failed"); return false; }
  return true;
}
static void publish_state(void) { LOCK(); I.dave_version = dave_ver; I.dave_group = d_group; I.dave_active = tx_encrypt; UNLOCK(); }
static void send_key_package(void) {     /* dmx held */
  uint8_t *kp = NULL; size_t kn = 0; dlog("[dave] > get_key_package"); int krc = dave_session_get_key_package(dsess, &kp, &kn); dlog("[dave] < get_key_package rc=%d", krc); if (krc != 0 || !kp) { dlog("[dave] key package failed"); return; }
  uint8_t *m = malloc(kn + 1); if (m) { m[0] = 26; memcpy(m + 1, kp, kn); ob_push(true, m, kn + 1); free(m); } dave_free(kp); dlog("[dave] sent key package (%u bytes)", (unsigned)kn);
}
static void session_init(int ver) {      /* dmx held: (re)start the MLS state for this channel */
  dlog("[dave] > dave_session_init ver=%d", ver); dave_session_init(dsess, (uint16_t)ver, strtoull(chan_id, NULL, 10), user_id); dlog("[dave] < dave_session_init ok"); d_inited = true; d_group = false;
  if (d_ext) dave_session_set_external_sender(dsess, d_ext, d_ext_len);
  send_key_package();
}
static void vdec_rekey(void); static void vdec_passthrough(bool on); static void vdec_free_all(void);   /* video decryptors (video block below) */
static void set_all_passthrough(bool on) {   /* dmx held */
  rx_pass = on; for (int i = 0; i < npeers; i++) if (peers[i].dd) dave_decryptor_set_passthrough(peers[i].dd, on ? 1 : 0);
  vdec_passthrough(on);
}
static void apply_ratchets(void) {       /* dmx held: (re)key the decryptor of every peer whose user id we know */
  if (!d_group) return; char ids[VC_MAX_PEERS][24]; Peer *pp[VC_MAX_PEERS]; int n = 0;
  LOCK(); for (int i = 0; i < npeers; i++) if (peers[i].p.user_id[0]) { snprintf(ids[n], 24, "%s", peers[i].p.user_id); pp[n++] = &peers[i]; } UNLOCK();
  for (int k = 0; k < n; k++) { if (!pp[k]->dd) { pp[k]->dd = dave_decryptor_new(); if (!pp[k]->dd) continue; dave_decryptor_set_passthrough(pp[k]->dd, rx_pass ? 1 : 0); }
    if (dave_decryptor_set_ratchet(pp[k]->dd, dsess, ids[k]) != 0) dlog("[dave] no ratchet for peer %s", ids[k]); }
  vdec_rekey();
}
static void do_execute(int ver) {        /* dmx held */
  if (ver == 0) { if (denc) dave_encryptor_set_passthrough(denc, 1); tx_encrypt = false; dave_ver = 0; set_all_passthrough(true); }
  else if (d_group && denc && dave_encryptor_set_ratchet(denc, dsess, user_id) == 0) { dave_encryptor_set_passthrough(denc, 0); dave_ver = ver; tx_encrypt = true; set_all_passthrough(false); }
  else { dlog("[dave] execute v%d but no ratchet yet", ver); return; }
  dlog("[dave] transition executed: v%d", ver); publish_state(); vr_set_dave(VR_CONN_VOICE, dave_ver > 0);
}
static void finish_transition(int tid, int ver) {   /* dmx held, after a successful commit/welcome */
  d_pend_valid = 1; d_pend_tid = tid; d_pend_ver = ver; apply_ratchets();
  if (tid == 0) { do_execute(ver); d_pend_valid = 0; } else send_tid(23, tid);
  publish_state();
}
static void bad_commit(int tid) {        /* dmx held: tell the gateway, restart our MLS state, offer a fresh key package */
  send_tid(31, tid); session_init(dsess ? dave_session_get_protocol_version(dsess) : 1); publish_state();
}
static int recognized(char ids[][24], const char **p) {   /* known call members + ourselves */
  int n = 0; SDL_LockMutex(umx); for (int i = 0; i < nknown && n < 64; i++) { snprintf(ids[n], 24, "%s", known[i]); p[n] = ids[n]; n++; } SDL_UnlockMutex(umx);
  bool self = false; for (int i = 0; i < n; i++) if (!strcmp(p[i], user_id)) self = true;
  if (!self) { snprintf(ids[n], 24, "%s", user_id); p[n] = ids[n]; n++; } return n;
}
static void dave_job(DJob *j) {
  DLOCK();
  if (j->kind == D_PEERS) { apply_ratchets(); DUNLOCK(); return; }
  if (j->kind == D_EXTSENDER) { free(d_ext); d_ext = malloc(j->len ? j->len : 1); if (d_ext) { memcpy(d_ext, j->data, j->len); d_ext_len = j->len; }
    if (d_inited && d_ext && dsess) dave_session_set_external_sender(dsess, d_ext, d_ext_len); dlog("[dave] external sender (%u bytes)", (unsigned)j->len); DUNLOCK(); return; }
  if (!ensure_session()) { DUNLOCK(); return; }
  switch (j->kind) {
  case D_INIT: dlog("[dave] init v%d channel %s", j->a, chan_id); session_init(j->a); break;
  case D_EPOCH: dlog("[dave] prepare epoch v%d epoch %d", j->a, j->b); if (j->b == 1 || !d_inited) session_init(j->a); else dave_session_set_protocol_version(dsess, (uint16_t)j->a); break;
  case D_PREP: dlog("[dave] prepare transition v%d id %d", j->a, j->b); d_pend_valid = 1; d_pend_ver = j->a; d_pend_tid = j->b;
    if (j->b == 0) { do_execute(j->a); d_pend_valid = 0; } else send_tid(23, j->b); break;
  case D_EXEC: if (d_pend_valid && d_pend_tid == j->a) { d_pend_valid = 0; do_execute(d_pend_ver); } else dlog("[dave] execute for unknown transition %d", j->a); break;
  case D_PROPOSALS: { if (!d_inited) break; static char ids[65][24]; const char *p[65]; int n = recognized(ids, p);
    uint8_t *out = NULL; size_t on = 0; dave_session_process_proposals(dsess, j->data, j->len, p, (size_t)n, &out, &on); dlog("[dave] proposals processed, reply %u bytes", (unsigned)on);
    if (out && on) { uint8_t *m = malloc(on + 1); if (m) { m[0] = 28; memcpy(m + 1, out, on); ob_push(true, m, on + 1); free(m); } } if (out) dave_free(out); break; }
  case D_COMMIT: { if (!d_inited) break; uint64_t *ro = NULL; size_t rn = 0; int rc = dave_session_process_commit(dsess, j->data, j->len, &ro, &rn); if (ro) dave_free(ro);
    dlog("[dave] commit tid %d -> %d", j->a, rc); if (rc == 0) { d_group = true; finish_transition(j->a, dave_session_get_protocol_version(dsess)); } else if (rc == 1) bad_commit(j->a); break; }
  case D_WELCOME: { if (!d_inited) break; static char ids[65][24]; const char *p[65]; int n = recognized(ids, p);
    uint64_t *ro = NULL; size_t rn = 0; int rc = dave_session_process_welcome(dsess, j->data, j->len, p, (size_t)n, &ro, &rn); if (ro) dave_free(ro);
    dlog("[dave] welcome tid %d -> %d", j->a, rc); if (rc == 0) { d_group = true; finish_transition(j->a, dave_session_get_protocol_version(dsess)); } else bad_commit(j->a); break; }
  }
  DUNLOCK();
}
static int dave_thread(void *arg) {
  (void)arg; while (!dstop) { SDL_SemWaitTimeout(dsem, 250); DJob *j; while ((j = dq_pop())) { if (!dstop) dave_job(j); free(j->data); free(j); } } return 0;
}
static void dave_start(void) {
  if (!dmx) { dmx = SDL_CreateMutex(); qmx = SDL_CreateMutex(); umx = SDL_CreateMutex(); }
  if (dsem) SDL_DestroySemaphore(dsem); dsem = SDL_CreateSemaphore(0); dstop = false; dlog_budget = 200;
  dave_set_entropy_source(dave_entropy); dave_set_log_callback(dave_log);
  nknown = 0; d_inited = d_group = false; d_pend_valid = 0; dave_ver = 0; tx_encrypt = false; rx_pass = true; free(d_ext); d_ext = NULL; d_ext_len = 0;
  dth = SDL_CreateThreadWithStackSize(dave_thread, "dave", 1024 * 1024, NULL);
}
static void dave_stop(void) {            /* after the voice + mic threads are gone */
  if (!dth) return; dstop = true; SDL_SemPost(dsem); SDL_WaitThread(dth, NULL); dth = NULL; queues_clear();
  DLOCK(); for (int i = 0; i < npeers; i++) if (peers[i].dd) { dave_decryptor_free(peers[i].dd); peers[i].dd = NULL; }
  vdec_free_all();
  if (denc) { dave_encryptor_free(denc); denc = NULL; } if (dsess) { dave_session_free(dsess); dsess = NULL; } d_inited = d_group = false; dave_ver = 0; tx_encrypt = false; DUNLOCK();
}

/* ---------- XChaCha20 helper: HChaCha20 subkey derivation (mbedtls only has the IETF 12-byte-nonce variant) ---------- */
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
/* nonce for rtpsize modes: 4-byte big-endian counter, zero padded (12 bytes for GCM, 24 for XChaCha) */
static int aead_encrypt(const unsigned char *aad, size_t aadlen, const unsigned char *in, size_t n, unsigned char *out, uint32_t counter) {
  unsigned char nonce[24] = {0}; nonce[0] = counter >> 24; nonce[1] = counter >> 16; nonce[2] = counter >> 8; nonce[3] = counter;
  if (enc_mode == 1) return mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, nonce, 12, aad, aadlen, in, out, 16, out + n);
  unsigned char sub[32]; hchacha20(key, nonce, sub); mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c); mbedtls_chachapoly_setkey(&c, sub); unsigned char n12[12] = {0}; memcpy(n12 + 4, nonce + 16, 8);
  int r = mbedtls_chachapoly_encrypt_and_tag(&c, n, n12, aad, aadlen, in, out, out + n); mbedtls_chachapoly_free(&c); return r;
}
static int aead_decrypt(const unsigned char *aad, size_t aadlen, const unsigned char *in, size_t n, const unsigned char *tag, unsigned char *out, uint32_t counter) {
  unsigned char nonce[24] = {0}; nonce[0] = counter >> 24; nonce[1] = counter >> 16; nonce[2] = counter >> 8; nonce[3] = counter;
  if (enc_mode == 1) return mbedtls_gcm_auth_decrypt(&gcm, n, nonce, 12, aad, aadlen, tag, 16, in, out);
  unsigned char sub[32]; hchacha20(key, nonce, sub); mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c); mbedtls_chachapoly_setkey(&c, sub); unsigned char n12[12] = {0}; memcpy(n12 + 4, nonce + 16, 8);
  int r = mbedtls_chachapoly_auth_decrypt(&c, n, n12, aad, aadlen, tag, in, out); mbedtls_chachapoly_free(&c); return r;
}

/* ---------- video (receive cameras; see vrecv.c) ----------
 * Signalling: IDENTIFY video:true + one (inactive) stream, SELECT_PROTOCOL codecs (opus 120, H264 103 / RTX 104),
 * after op 4 one op 12 with no video of our own (required before receiving video) and op 15 media sink wants.
 * RTP with a payload type other than Opus goes to vrecv (never to the audio peers). Each remote user gets a
 * video DAVE decryptor separate from their audio one (same key ratchet, separate replay window). */
typedef struct { char uid[24]; dave_decryptor_t *dd; } VDec;
static VDec vdec[VC_MAX_PEERS]; static int nvdec;                     /* dmx held */
static uint32_t my_video_ssrc, my_rtx_ssrc; static int sink_q = -1; static char sink_last[300]; static volatile uint32_t st_vid;
static void video_tick(Ws *w);
SDL_mutex *voice_dave_mutex(void) { if (!dmx) { dmx = SDL_CreateMutex(); qmx = SDL_CreateMutex(); umx = SDL_CreateMutex(); } return dmx; }
void voice_video_ssrcs(uint32_t *audio, uint32_t *video, uint32_t *rtx) { if (audio) *audio = my_ssrc; if (video) *video = my_video_ssrc; if (rtx) *rtx = my_rtx_ssrc; }
json_t *voice_codecs_json(void) {
  json_t *a = json_array(), *o = json_object();
  json_object_set_new(o, "name", json_string("opus")); json_object_set_new(o, "type", json_string("audio")); json_object_set_new(o, "priority", json_integer(1000)); json_object_set_new(o, "payload_type", json_integer(120)); json_array_append_new(a, o);
  o = json_object(); json_object_set_new(o, "name", json_string("H264")); json_object_set_new(o, "type", json_string("video")); json_object_set_new(o, "priority", json_integer(1000));
  json_object_set_new(o, "payload_type", json_integer(VR_PT_H264)); json_object_set_new(o, "rtx_payload_type", json_integer(VR_PT_H264_RTX)); json_object_set_new(o, "encode", json_true()); json_object_set_new(o, "decode", json_true()); json_array_append_new(a, o);
  return a;
}
static void vdec_rekey(void) { if (!d_group) return; for (int i = 0; i < nvdec; i++) if (dave_decryptor_set_ratchet(vdec[i].dd, dsess, vdec[i].uid) != 0) dlog("[vc] no video ratchet for %s", vdec[i].uid); }
static void vdec_passthrough(bool on) { for (int i = 0; i < nvdec; i++) dave_decryptor_set_passthrough(vdec[i].dd, on ? 1 : 0); }
static void vdec_free_all(void) { for (int i = 0; i < nvdec; i++) dave_decryptor_free(vdec[i].dd); nvdec = 0; }
static int video_decrypt(int conn, const char *uid, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {   /* vrecv decode thread */
  (void)conn; DLOCK(); VDec *v = NULL; for (int i = 0; i < nvdec; i++) if (!strcmp(vdec[i].uid, uid)) v = &vdec[i];
  if (!v && nvdec < VC_MAX_PEERS) { v = &vdec[nvdec]; snprintf(v->uid, 24, "%s", uid); v->dd = dave_decryptor_new();
    if (!v->dd) v = NULL; else { nvdec++; dave_decryptor_set_passthrough(v->dd, rx_pass ? 1 : 0); if (d_group && dave_decryptor_set_ratchet(v->dd, dsess, uid) != 0) dlog("[vc] no video ratchet for %s yet", uid); } }
  int rc = v ? dave_decrypt_video(v->dd, in, n, out, cap, w) : -1; DUNLOCK(); return rc;
}
static void video_pli(int conn, uint32_t media) {   /* RTCP PSFB PLI; rtpsize AEAD: 8-byte header clear, body encrypted, nonce suffix */
  (void)conn; if (!crypto_ready || !udp_ready || sock < 0) return; unsigned char p[12 + 16 + 4];
  p[0] = 0x81; p[1] = 206; p[2] = 0; p[3] = 2; p[4] = my_ssrc >> 24; p[5] = my_ssrc >> 16; p[6] = my_ssrc >> 8; p[7] = my_ssrc;
  unsigned char body[4] = {media >> 24, media >> 16, media >> 8, media}, ct[4 + 16];
  SDL_LockMutex(txmx); uint32_t ctr = ++tx_nonce;
  if (aead_encrypt(p, 8, body, 4, ct, ctr) == 0) { memcpy(p + 8, ct, 20); p[28] = ctr >> 24; p[29] = ctr >> 16; p[30] = ctr >> 8; p[31] = ctr; sendto(sock, p, 32, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr); }
  SDL_UnlockMutex(txmx);
}
static void video_rtcp(const unsigned char *pkt, int len);
static bool video_rx_conn(unsigned char *pkt, int len, int conn);
static bool video_rx(unsigned char *pkt, int len) { return video_rx_conn(pkt, len, VR_CONN_VOICE); }
static bool video_rx_conn(unsigned char *pkt, int len, int conn) {   /* true = handled here (video, RTCP or other non-audio RTP) */
  if (len < 12 + 16 + 4 || (pkt[0] & 0xC0) != 0x80) return false;
  int pt = pkt[1] & 0x7F; if (pt >= 72 && pt <= 78) { video_rtcp(pkt, len); return true; }   /* RTCP (the audio path ignored it anyway) */
  if (pt == 120) return false;                                                                 /* Opus: audio path */
  int hl = 12 + 4 * (pkt[0] & 0x0F); bool ext = pkt[0] & 0x10; if (ext) hl += 4; if (len < hl + 16 + 4) return true;
  uint32_t ssrc = (uint32_t)pkt[8] << 24 | pkt[9] << 16 | pkt[10] << 8 | pkt[11]; uint16_t seq = pkt[2] << 8 | pkt[3]; uint32_t ts = (uint32_t)pkt[4] << 24 | pkt[5] << 16 | pkt[6] << 8 | pkt[7];
  uint32_t ctr = (uint32_t)pkt[len - 4] << 24 | pkt[len - 3] << 16 | pkt[len - 2] << 8 | pkt[len - 1];
  int clen = len - hl - 16 - 4; unsigned char plain[2048]; if (clen > (int)sizeof plain) return true;   /* local: the dry-run loopback calls this on the encoder thread */
  if (aead_decrypt(pkt, hl, pkt + hl, clen, pkt + hl + clen, plain, ctr) != 0) { static int bad; if (bad++ < 5) dlog("[vc] video packet failed transport decrypt (pt %d ssrc %u)", pt, (unsigned)ssrc); return true; }
  unsigned char *pl = plain; int n = clen;
  if (ext) { int words = pkt[hl - 2] << 8 | pkt[hl - 1]; int skip = words * 4; if (skip > n) return true; pl += skip; n -= skip; }
  if ((pkt[0] & 0x20) && n > 0) { int pad = pl[n - 1]; if (pad > n) return true; n -= pad; }
  st_vid++; if (st_vid == 1) dlog("[vc] first video RTP packet: pt %d ssrc %u seq %u len %d", pt, (unsigned)ssrc, seq, n);
  vr_rtp(conn, ssrc, pt, seq, ts, pkt[1] & 0x80, pl, n); return true;
}
static void video_after_session(Ws *w, json_t *d) {   /* op 4 received */
  dlog("[vc] video: video_codec=%s audio_codec=%s keyframe_interval=%d media_session_id=%s", js(d, "video_codec"), js(d, "audio_codec"), (int)json_integer_value(json_object_get(d, "keyframe_interval")), js(d, "media_session_id"));
  char b[200]; snprintf(b, sizeof b, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":0,\"rtx_ssrc\":0,\"streams\":[]}}", (unsigned)my_ssrc); ws_send_text(w, b, strlen(b));
  dlog("[vc] sent op 12 (no own video)"); sink_q = 0; sink_last[0] = 0; video_tick(w);
}
static void video_tick(Ws *w) {   /* op 15 MEDIA_SINK_WANTS whenever the wanted layers change (tile vs focused video) */
  if (!crypto_ready || sink_q < 0) return; char d[300]; vr_sink_wants(VR_CONN_VOICE, d, sizeof d); if (!strcmp(d, sink_last)) return;
  snprintf(sink_last, sizeof sink_last, "%s", d); char b[340]; snprintf(b, sizeof b, "{\"op\":15,\"d\":%s}", d); ws_send_text(w, b, strlen(b)); dlog("[vc] op 15 %s", d);
}

/* ---------- video send (our camera; capture + encoder in vsend.c) ----------
 * voice_set_camera(on): starts vsend, announces the stream with op 12 (our video/rtx ssrcs from READY, active)
 * and self_video in the main-gateway voice state. Every encoded access unit goes through voice_send_video_au():
 * DAVE encrypt (whole frame, H.264-aware) -> RFC 6184 packetiser -> RTP (PT 103, 90 kHz clock with a random offset,
 * own sequence numbers, marker on the last packet, header extension playout-delay (id 5, as in Discord's extmap))
 * -> aead rtpsize -> UDP. RTCP PLI/FIR for our ssrc -> IDR; Sender Report every 5 s.
 * DRY RUN (test): with SD:/wiiu/apps/MingusDiscord/vsend_dry.txt nothing video-related is sent to Discord (no op 12,
 * no self_video, no RTP/RTCP); each finished UDP packet is instead parsed back through the receive path on
 * VR_CONN_TEST (transport decrypt -> depacketise -> DAVE decrypt with our own key ratchet -> hardware decode),
 * shown in our own tile. */
static volatile bool cam_on, cam_dry; static volatile int want_op12 = -1;   /* -1 nothing, 0 clear, 1 announce */
static uint16_t v_seq; static uint32_t v_ts_off, v_last_ts, v_pkts, v_octets, sr_t; static int kf_ms;
static uint8_t *v_ebuf; static size_t v_ecap; static dave_decryptor_t *loop_dd;
static void rtcp_send(unsigned char *p, int n) {   /* p: plain RTCP, room for +20; first 8 bytes stay clear */
  if (!crypto_ready || !udp_ready || sock < 0 || n < 8) return; unsigned char ct[1500 + 16];
  SDL_LockMutex(txmx); uint32_t ctr = ++tx_nonce;
  if (aead_encrypt(p, 8, p + 8, n - 8, ct, ctr) == 0) { memcpy(p + 8, ct, n - 8 + 16); unsigned char *e = p + n + 16; e[0] = ctr >> 24; e[1] = ctr >> 16; e[2] = ctr >> 8; e[3] = ctr; if (!cam_dry) sendto(sock, p, n + 20, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr); }
  SDL_UnlockMutex(txmx);
}
static void video_rtcp(const unsigned char *pkt, int len) {   /* incoming RTCP: PLI / FIR for our video -> keyframe */
  static unsigned char plain[1500]; static int logged; int clen = len - 8 - 16 - 4; if (clen < 0 || clen > (int)sizeof plain) return;
  uint32_t ctr = (uint32_t)pkt[len - 4] << 24 | pkt[len - 3] << 16 | pkt[len - 2] << 8 | pkt[len - 1];
  if (aead_decrypt(pkt, 8, pkt + 8, clen, pkt + 8 + clen, plain, ctr) != 0) { if (logged++ < 5) dlog("[vc] RTCP pt %d failed to decrypt", pkt[1]); return; }
  static unsigned char c[1508]; memcpy(c, pkt, 8); memcpy(c + 8, plain, clen); int n = 8 + clen;
  for (int o = 0; o + 4 <= n;) { int fmt = c[o] & 31, pt = c[o + 1], words = c[o + 2] << 8 | c[o + 3]; int sz = (words + 1) * 4; if (o + sz > n) break;
    uint32_t media = o + 12 <= n ? (uint32_t)c[o + 8] << 24 | c[o + 9] << 16 | c[o + 10] << 8 | c[o + 11] : 0;
    if (logged < 30) { logged++; dlog("[vc] RTCP in: pt %d fmt %d len %d media ssrc %u", pt, fmt, sz, (unsigned)media); }
    if (pt == 206 && fmt == 1 && cam_on && media == my_video_ssrc) { dlog("[vc] PLI for our camera -> IDR"); vs_request_idr(); }
    else if (pt == 206 && fmt == 4 && cam_on) { for (int q = o + 12; q + 8 <= o + sz; q += 8) if (((uint32_t)c[q] << 24 | c[q + 1] << 16 | c[q + 2] << 8 | c[q + 3]) == my_video_ssrc) { dlog("[vc] FIR for our camera -> IDR"); vs_request_idr(); } }
    o += sz; }
}
static int loop_decrypt(int conn, const char *uid, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {   /* dry run: our own frames */
  (void)conn; (void)uid; DLOCK(); if (!loop_dd) { loop_dd = dave_decryptor_new(); if (loop_dd) { dave_decryptor_set_passthrough(loop_dd, rx_pass ? 1 : 0); if (d_group && dave_decryptor_set_ratchet(loop_dd, dsess, user_id) != 0) dlog("[vc] loopback: no own ratchet"); } }
  int rc = loop_dd ? dave_decrypt_video(loop_dd, in, n, out, cap, w) : -1; DUNLOCK(); return rc;
}
typedef struct { uint32_t ts; int sent, bytes; } PktCtx;
static void send_rtp_cb(void *u, const uint8_t *pl, int len, bool marker) {   /* encoder thread: one RTP packet */
  PktCtx *c = u; unsigned char pkt[16 + 8 + 1500 + 16 + 4]; if (len > 1500) return;
  pkt[0] = 0x90; pkt[1] = (uint8_t)(VR_PT_H264 | (marker ? 0x80 : 0)); pkt[4] = c->ts >> 24; pkt[5] = c->ts >> 16; pkt[6] = c->ts >> 8; pkt[7] = c->ts;
  pkt[8] = my_video_ssrc >> 24; pkt[9] = my_video_ssrc >> 16; pkt[10] = my_video_ssrc >> 8; pkt[11] = my_video_ssrc;
  pkt[12] = 0xBE; pkt[13] = 0xDE; pkt[14] = 0; pkt[15] = 1;                              /* one-byte header extensions, 1 word */
  unsigned char pt[4 + 1500]; pt[0] = 0x52; pt[1] = pt[2] = pt[3] = 0;                  /* Discord's extmap: id 5 = playout-delay (min = max = 0), like the official clients.
     (Was id 6 + id 11: those are video-content-type and *repaired*-rtp-stream-id in Discord's map - phones dropped our video as repair packets.) */
  memcpy(pt + 4, pl, len);
  SDL_LockMutex(txmx); pkt[2] = v_seq >> 8; pkt[3] = (uint8_t)v_seq; v_seq++; uint32_t ctr = ++tx_nonce;
  int n = -1; if (aead_encrypt(pkt, 16, pt, 4 + len, pkt + 16, ctr) == 0) { unsigned char *e = pkt + 16 + 4 + len + 16; e[0] = ctr >> 24; e[1] = ctr >> 16; e[2] = ctr >> 8; e[3] = ctr; n = 16 + 4 + len + 16 + 4;
    if (!cam_dry) sendto(sock, pkt, n, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr); v_pkts++; v_octets += len; }
  SDL_UnlockMutex(txmx);
  if (n > 0) { c->sent++; c->bytes += n; if (cam_dry) video_rx_conn(pkt, n, VR_CONN_TEST); }   /* loopback: parse our own wire bytes */
}
int voice_send_video_au(const uint8_t *au, size_t n, uint32_t ts90k, bool key) {   /* encoder thread (vsend.c) */
  if (!cam_on || !crypto_ready || !udp_ready || !my_video_ssrc) return -1;
  const uint8_t *d = au; size_t dn = n;
  if (tx_encrypt) {                                        /* DAVE: never send a frame in the clear once E2EE is on */
    size_t need = n + 64 + 1024; if (v_ecap < need) { free(v_ebuf); v_ecap = need * 2; v_ebuf = malloc(v_ecap); if (!v_ebuf) { v_ecap = 0; return -3; } }
    DLOCK(); size_t w = 0; int rc = denc ? dave_encrypt_video(denc, my_video_ssrc, DAVE_C_CODEC_H264, au, n, v_ebuf, v_ecap, &w) : -1; DUNLOCK();
    if (rc != 0 || !w) { static int bad; if (bad++ < 5) dlog("[vc] DAVE video encrypt failed rc=%d", rc); return -2; }
    d = v_ebuf; dn = w; }
  PktCtx c = {v_ts_off + ts90k, 0, 0}; v_last_ts = c.ts;
  int np = vr_h264_packetize(d, dn, 1200, send_rtp_cb, &c);
  static uint32_t nau; nau++; if (nau <= 3 || (key && nau < 300)) dlog("[vc] video AU #%u %s %u -> %u bytes (DAVE %d), %d packets, ts %u%s", (unsigned)nau, key ? "IDR" : "", (unsigned)n, (unsigned)dn, tx_encrypt ? 1 : 0, np, (unsigned)c.ts, cam_dry ? " [dry run: not sent]" : "");
  return c.sent == np && np > 0 ? 0 : -4;
}
static void send_sr(void) {   /* RTCP Sender Report for our video ssrc */
  unsigned char p[28 + 20]; time_t now = time(NULL); uint32_t ntp_s = (uint32_t)now + 2208988800u, ntp_f = (uint32_t)((SDL_GetTicks() % 1000) * 4294967.296);
  p[0] = 0x80; p[1] = 200; p[2] = 0; p[3] = 6; uint32_t v[6] = {my_video_ssrc, ntp_s, ntp_f, v_last_ts, v_pkts, v_octets};
  for (int i = 0; i < 6; i++) { p[4 + 4 * i] = v[i] >> 24; p[5 + 4 * i] = v[i] >> 16; p[6 + 4 * i] = v[i] >> 8; p[7 + 4 * i] = v[i]; }
  rtcp_send(p, 28);
}
static void video_send_tick(Ws *w) {   /* voice thread: op 12 announce / clear, periodic SR */
  int want = want_op12; if (want >= 0 && crypto_ready) { want_op12 = -1; char b[600];
    if (want == 1) snprintf(b, sizeof b, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":%u,\"rtx_ssrc\":%u,\"active\":true,\"quality\":100,\"max_bitrate\":%d,\"max_framerate\":%d,\"max_resolution\":{\"type\":\"fixed\",\"width\":%d,\"height\":%d}}]}}",
                        (unsigned)my_ssrc, (unsigned)my_video_ssrc, (unsigned)my_rtx_ssrc, (unsigned)my_video_ssrc, (unsigned)my_rtx_ssrc, VS_KBPS * 1200, VS_FPS, VS_W, VS_H);
    else snprintf(b, sizeof b, "{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":0,\"rtx_ssrc\":0,\"streams\":[]}}", (unsigned)my_ssrc);
    if (!cam_dry) ws_send_text(w, b, strlen(b)); dlog("[vc] op 12 %s%s", b, cam_dry ? " [dry run: NOT sent]" : ""); }
  if (cam_on && !cam_dry && SDL_GetTicks() - sr_t > 5000 && v_pkts) { sr_t = SDL_GetTicks(); send_sr(); }
}
static void video_sink_from_server(json_t *d) {   /* op 15 from the server: may ask us to pause / reduce our stream */
  char *s = json_dumps(d, JSON_COMPACT); dlog("[vc] op 15 from server: %.300s", s ? s : "?"); free(s);
  if (!cam_on || !my_video_ssrc) return; char k[16]; snprintf(k, sizeof k, "%u", (unsigned)my_video_ssrc);
  json_t *q = json_object_get(d, k); if (!json_is_number(q)) q = json_object_get(d, "any"); if (json_is_number(q)) vs_set_paused(json_number_value(q) <= 0);
}
static bool file_exists(const char *name) { char p[200]; snprintf(p, sizeof p, "/vol/external01/wiiu/apps/MingusDiscord/%s", name); FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
void voice_set_camera(bool on) {   /* UI thread */
  if (on == cam_on) return;
  if (on) {
    if (!th || !crypto_ready || !my_video_ssrc) { dlog("[vc] camera: not connected yet"); return; }
    cam_dry = file_exists("vsend_dry.txt");
    uint32_t r[2]; ws_random((unsigned char *)r, sizeof r); SDL_LockMutex(txmx); v_ts_off = r[0]; v_seq = (uint16_t)r[1]; v_pkts = v_octets = 0; SDL_UnlockMutex(txmx);
    if (cam_dry) { vr_set_conn(VR_CONN_TEST, loop_decrypt, NULL); vr_set_dave(VR_CONN_TEST, dave_ver > 0); vr_map(VR_CONN_TEST, user_id, &my_video_ssrc, &my_rtx_ssrc, 1); }
    cam_on = true;
    if (!vs_start(user_id, kf_ms > 0 ? (kf_ms + 999) / 1000 : 0, !cam_dry)) { cam_on = false; if (cam_dry) vr_map(VR_CONN_TEST, user_id, NULL, NULL, 0); return; }
    want_op12 = 1; if (!cam_dry) gw_set_self_video(true); sr_t = SDL_GetTicks();
    dlog("[vc] camera ON (ssrc %u rtx %u)%s", (unsigned)my_video_ssrc, (unsigned)my_rtx_ssrc, cam_dry ? " DRY RUN: nothing is sent to Discord, loopback into our own tile" : "");
  } else {
    vs_stop(); cam_on = false; want_op12 = 0; if (!cam_dry) gw_set_self_video(false);
    if (cam_dry) { vr_map(VR_CONN_TEST, user_id, NULL, NULL, 0); DLOCK(); if (loop_dd) { dave_decryptor_free(loop_dd); loop_dd = NULL; } DUNLOCK(); }
    dlog("[vc] camera OFF");
  }
}
bool voice_camera_on(void) { return cam_on; }
const char *voice_camera_status(void) { return cam_on ? vs_status() : ""; }

/* ---- camera send self test (SD:/wiiu/apps/MingusDiscord/vsend_selftest.txt, only while NOT in a call) ----
 * Runs the real send code above (DAVE encrypt with a real MLS key ratchet, packetiser, RTP + extensions + AEAD)
 * in dry-run mode with a local random transport key, and parses every packet back through the receive path
 * (transport decrypt -> depacketise -> DAVE decrypt with our own ratchet -> hardware decode). Nothing is sent. */
static char st_uid[24];
static int cam_selftest(void *arg) {
  (void)arg; SDL_Delay(8000);
  if (th) { dlog("[vsend-test] skipped: a call is active"); return 0; }
  if (!mx) { mx = SDL_CreateMutex(); txmx = SDL_CreateMutex(); } voice_dave_mutex();
  dave_session_t *A = NULL, *B = NULL; DLOCK(); int rc = dave_test_make_pair(55555ULL, st_uid, "424242424242424242", &A, &B);
  if (rc == 0) { denc = dave_encryptor_new(); if (!denc || dave_encryptor_set_ratchet(denc, A, st_uid) != 0) rc = 60; else dave_encryptor_set_passthrough(denc, 0); }
  dsess = A; d_group = true; rx_pass = false; dave_ver = 1; tx_encrypt = rc == 0; DUNLOCK();
  dlog("[vsend-test] MLS pair rc=%d", rc); if (rc) return 0;
  snprintf(user_id, sizeof user_id, "%s", st_uid); ws_random(key, 32); enc_mode = 1; mbedtls_gcm_init(&gcm); mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  my_ssrc = 1000; my_video_ssrc = 1001; my_rtx_ssrc = 1002; crypto_ready = udp_ready = true; cam_dry = true;
  uint32_t r[2]; ws_random((unsigned char *)r, sizeof r); v_ts_off = r[0]; v_seq = (uint16_t)r[1]; v_pkts = v_octets = 0;
  vr_set_conn(VR_CONN_TEST, loop_decrypt, NULL); vr_set_dave(VR_CONN_TEST, true); vr_map(VR_CONN_TEST, user_id, &my_video_ssrc, &my_rtx_ssrc, 1);
  cam_on = true; bool ok = vs_start(user_id, 2, true);   /* preview on: also exercises the local self-preview path (VR_CONN_SELF) */ dlog("[vsend-test] camera dry run started: %d", ok);
  if (ok) { SDL_Delay(6000); vs_request_idr(); SDL_Delay(9000); }
  VrStats s, sp; vr_stats(VR_CONN_TEST, user_id, &s); vr_stats(VR_CONN_SELF, user_id, &sp); dlog("[vsend-test] self preview: %u pictures converted (%dx%d)", (unsigned)sp.n_conv, sp.w, sp.h);
  vs_stop(); cam_on = false;
  dlog("[vsend-test] RESULT rtp packets %u (%u payload bytes) -> received %u, frames %u, drops %u, DAVE decrypt ok %u fail %u, decoded %u (%dx%d), errors %u, PLIs %u | %s",
       (unsigned)v_pkts, (unsigned)v_octets, (unsigned)s.pkts, (unsigned)s.frames, (unsigned)s.drops, (unsigned)s.decrypt_ok, (unsigned)s.decrypt_fail, (unsigned)s.decoded, s.w, s.h, (unsigned)s.errors, (unsigned)s.plis,
       ok && s.decoded > 0 && s.decrypt_fail == 0 && s.decrypt_ok > 0 && s.pkts == v_pkts ? "PASS" : "FAIL");
  vr_map(VR_CONN_TEST, user_id, NULL, NULL, 0);
  crypto_ready = udp_ready = false; mbedtls_gcm_free(&gcm); enc_mode = 0; cam_dry = false; my_video_ssrc = my_rtx_ssrc = my_ssrc = 0;
  DLOCK(); if (loop_dd) { dave_decryptor_free(loop_dd); loop_dd = NULL; } if (denc) { dave_encryptor_free(denc); denc = NULL; } dave_session_free(A); dave_session_free(B); dsess = NULL; d_group = false; rx_pass = true; dave_ver = 0; tx_encrypt = false; DUNLOCK();
  return 0;
}
void voice_camera_selftest_start(const char *self_uid) {
  static bool once; if (once || !file_exists("vsend_selftest.txt")) return; once = true; snprintf(st_uid, sizeof st_uid, "%s", self_uid && self_uid[0] ? self_uid : "123456789");
  dlog("[vsend-test] scheduled"); SDL_CreateThreadWithStackSize(cam_selftest, "vstest", 1024 * 1024, NULL);
}

/* ---------- send one opus packet ---------- */
static void send_opus(const unsigned char *op, int n) {
  if (!crypto_ready || !udp_ready || sock < 0) return;
  unsigned char pkt[12 + 1500 + 16 + 4]; if (n > 1200) return;
  static unsigned char ebuf[1400];
  if (tx_encrypt) {                                        /* DAVE: encrypt the Opus frame first; if the MLS thread is busy drop the frame (never send it in the clear) */
    DLOCK(); size_t w = 0; int rc = denc ? dave_encrypt_opus(denc, my_ssrc, op, (size_t)n, ebuf, sizeof ebuf, &w) : -1; DUNLOCK();
    if (rc != 0 || w == 0 || w > 1400) return; op = ebuf; n = (int)w; }
  SDL_LockMutex(txmx);
  pkt[0] = 0x80; pkt[1] = 0x78; pkt[2] = tx_seq >> 8; pkt[3] = tx_seq; tx_seq++;
  pkt[4] = tx_ts >> 24; pkt[5] = tx_ts >> 16; pkt[6] = tx_ts >> 8; pkt[7] = tx_ts; tx_ts += FRAME;
  pkt[8] = my_ssrc >> 24; pkt[9] = my_ssrc >> 16; pkt[10] = my_ssrc >> 8; pkt[11] = my_ssrc;
  uint32_t ctr = ++tx_nonce;
  if (aead_encrypt(pkt, 12, op, n, pkt + 12, ctr) == 0) { unsigned char *e = pkt + 12 + n + 16; e[0] = ctr >> 24; e[1] = ctr >> 16; e[2] = ctr >> 8; e[3] = ctr;
    sendto(sock, pkt, 12 + n + 16 + 4, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr); LOCK(); I.tx_packets++; UNLOCK(); }
  SDL_UnlockMutex(txmx);
}

/* ---------- receive: decrypt + decode into the peer's jitter queue ---------- */
static Peer *peer_for(uint32_t ssrc, bool create) {
  for (int i = 0; i < npeers; i++) if (peers[i].p.ssrc == ssrc) return &peers[i];
  if (!create || npeers >= VC_MAX_PEERS) return NULL; Peer *p = &peers[npeers++]; memset(p, 0, sizeof *p); p->p.ssrc = ssrc; int e; p->dec = opus_decoder_create(48000, 2, &e); return p;
}
static void rx_packet(unsigned char *pkt, int len) {
  if (len < 12 + 16 + 4 || (pkt[0] & 0xC0) != 0x80) return;
  int pt = pkt[1] & 0x7F; if (pt >= 72 && pt <= 78) return;                       /* RTCP (200-206) */
  int hl = 12 + 4 * (pkt[0] & 0x0F); bool ext = pkt[0] & 0x10; if (ext) hl += 4; if (len < hl + 16 + 4) return;
  uint32_t ssrc = (uint32_t)pkt[8] << 24 | pkt[9] << 16 | pkt[10] << 8 | pkt[11];
  uint32_t ctr = (uint32_t)pkt[len - 4] << 24 | pkt[len - 3] << 16 | pkt[len - 2] << 8 | pkt[len - 1];
  int clen = len - hl - 16 - 4; static unsigned char plain[4096]; if (clen > (int)sizeof plain) return;
  if (aead_decrypt(pkt, hl, pkt + hl, clen, pkt + hl + clen, plain, ctr) != 0) { if (st_aead_bad++ < 3) dlog("[vc] audio packet failed transport decrypt (ssrc %u len %d)", (unsigned)ssrc, len); return; }
  unsigned char *op = plain; int on = clen;
  if (ext) { int words = pkt[hl - 2] << 8 | pkt[hl - 1]; int skip = words * 4; if (skip > on) return; op += skip; on -= skip; }
  if ((pkt[0] & 0x20) && on > 0) { int pad = op[on - 1]; if (pad <= 0 || pad > on) return; on -= pad; }   /* RTP padding (inside the encrypted payload) */
  LOCK(); Peer *p = peer_for(ssrc, true); if (p) { p->p.last_pkt = SDL_GetTicks(); I.rx_packets++; } UNLOCK();
  if (p && dave_ver > 0) {                                 /* DAVE: strip the E2EE layer with this peer's decryptor */
    static unsigned char dbuf[4096]; bool ok = false; size_t w = 0;
    DLOCK();                                               /* wait for the MLS thread rather than dropping audio (drops = pops) */
    if (p->dd) { size_t mxs = dave_decryptor_max_size(p->dd, (size_t)on); if (mxs <= sizeof dbuf) ok = dave_decrypt_opus(p->dd, op, (size_t)on, dbuf, sizeof dbuf, &w) == 0; }
    else { ok = !(on >= 2 && op[on - 1] == 0xFA && op[on - 2] == 0xFA);   /* no keys yet: only pass frames that do not look E2EE-wrapped */
      if (!ok && d_group && !p->p.user_id[0] && (p->probe++ % 25) == 0) {   /* whose audio is this? nobody told us (no op 5): try each person's key */
        char cand[64][24]; int nc = 0; SDL_LockMutex(umx); for (int i = 0; i < nknown && nc < 64; i++) { bool taken = !strcmp(known[i], user_id); LOCK(); for (int k = 0; k < npeers && !taken; k++) if (!strcmp(peers[k].p.user_id, known[i])) taken = true; UNLOCK(); if (!taken) snprintf(cand[nc++], 24, "%s", known[i]); } SDL_UnlockMutex(umx);
        for (int i = 0; i < nc && !ok; i++) { void *td = dave_decryptor_new(); if (!td) break; dave_decryptor_set_passthrough(td, 0);
          if (dave_decryptor_set_ratchet(td, dsess, cand[i]) == 0 && dave_decrypt_opus(td, op, (size_t)on, dbuf, sizeof dbuf, &w) == 0) { ok = true; p->dd = td; dave_decryptor_set_passthrough(td, rx_pass ? 1 : 0); LOCK(); snprintf(p->p.user_id, 24, "%s", cand[i]); UNLOCK(); dlog("[vc] audio ssrc %u belongs to %s (matched by key)", (unsigned)ssrc, cand[i]); }
          else dave_decryptor_free(td); } } }
    DUNLOCK(); if (!ok) return; if (p->dd) { op = dbuf; on = (int)w; } }
  if (!p || on <= 0 || on > PKT_MAX) { if (p && on > PKT_MAX && p->bad_dec++ < 3) dlog("[vc] audio packet too big (%d bytes, ssrc %u)", on, (unsigned)ssrc); return; }
  uint16_t seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
  LOCK();
  if (p->playing) { int16_t d = (int16_t)(seq - p->play_seq); if (d < 0) { UNLOCK(); return; }              /* too late: already played/concealed */
    if (d >= JB) { p->playing = false; for (int k = 0; k < JB; k++) p->jb[k].have = false; } }          /* far ahead: resync */
  if (!p->playing) { if (!p->seen || (int16_t)(seq - p->play_seq) < 0 || (int16_t)(seq - p->play_seq) >= JB) p->play_seq = seq; p->seen = true; }
  JSlot *s = &p->jb[seq % JB]; s->seq = seq; s->len = (uint16_t)on; s->have = true; memcpy(s->data, op, on); st_dec++;
  UNLOCK();
}

/* ---------- playback: jitter buffers -> mixer thread -> ring -> SDL audio callback ----------
 * The speaker pulls (SDL callback) from a ring the mixer keeps ~100 ms ahead. The mixer always produces audio
 * (silence when nobody talks), so the device never starves; each speaker plays in RTP-sequence order with Opus
 * FEC/PLC for lost packets. */
static int16_t ring[RING]; static int ring_r, ring_w, ring_n; static SDL_mutex *ringmx; static SDL_Thread *mix_th; static volatile uint32_t st_under, st_plc;
static void __attribute__((unused)) audio_cb(void *ud, Uint8 *stream, int len) {
  (void)ud; int16_t *o = (int16_t *)stream; int n = len / 2, got = 0;
  SDL_LockMutex(ringmx); while (got < n && ring_n > 0) { o[got++] = ring[ring_r]; ring_r = (ring_r + 1) % RING; ring_n--; } SDL_UnlockMutex(ringmx);
  if (got < n) { memset(o + got, 0, (n - got) * 2); if (got || st_mix) st_under++; }
}
static int peer_decode(Peer *p, int16_t *pcm) {        /* LOCK held: one packet (or concealment) -> samples per channel (pcm holds MAXF), 0 = nothing */
  if (!p->dec) return 0;
  if (!p->playing) {                                     /* (re)start at the earliest buffered packet - it used to stay at the old position and drop new speech */
    for (int k = 0; k < JB; k++) { JSlot *s0 = &p->jb[(uint16_t)(p->play_seq + k) % JB]; if (s0->have && s0->seq == (uint16_t)(p->play_seq + k)) { p->play_seq += k; break; } }
    int buf = 0; for (int k = 0; k < JB_START; k++) { JSlot *s = &p->jb[(uint16_t)(p->play_seq + k) % JB]; if (s->have && s->seq == (uint16_t)(p->play_seq + k)) buf++; }
    if (buf < JB_START) return 0; p->playing = true; p->plc_run = 0; }
  int ahead = 0; for (int k = 0; k < JB; k++) { JSlot *s = &p->jb[(uint16_t)(p->play_seq + k) % JB]; if (s->have && s->seq == (uint16_t)(p->play_seq + k)) ahead = k + 1; }
  if (ahead > JB_MAX) { JSlot *s = &p->jb[p->play_seq % JB]; s->have = false; p->play_seq++; }                /* latency creeping up: skip one frame */
  JSlot *s = &p->jb[p->play_seq % JB], *nx = &p->jb[(uint16_t)(p->play_seq + 1) % JB]; int got;
  if (s->have && s->seq == p->play_seq) { got = opus_decode(p->dec, s->data, s->len, pcm, MAXF, 0); s->have = false; p->plc_run = 0;
    if (got >= 480) { if (got != p->last_dur && got != FRAME) dlog("[vc] ssrc %u sends %d ms packets", (unsigned)p->p.ssrc, got / 48); p->last_dur = got; }   /* <10 ms: junk that happened to parse, keep the real length */ else if (p->bad_dec++ < 3) dlog("[vc] opus decode error %d (ssrc %u, %u bytes)", got, (unsigned)p->p.ssrc, (unsigned)s->len); }
  else { bool later = false; for (int k = 1; k < JB; k++) { JSlot *s2 = &p->jb[(uint16_t)(p->play_seq + k) % JB]; if (s2->have && s2->seq == (uint16_t)(p->play_seq + k)) { later = true; break; } }
    if (!later || ++p->plc_run > 10) { p->playing = false; return 0; }   /* nothing queued after the hole: they stopped talking - stop cleanly instead of concealing */
    if (nx->have && nx->seq == (uint16_t)(p->play_seq + 1)) got = opus_decode(p->dec, nx->data, nx->len, pcm, p->last_dur > 0 ? p->last_dur : FRAME, 1);   /* in-band FEC from the next packet */
    else got = opus_decode(p->dec, NULL, 0, pcm, p->last_dur > 0 ? p->last_dur : FRAME, 0);                                                    /* packet loss concealment */
    st_plc++; }
  p->play_seq++;
  return got >= 480 ? got : 0;   /* under 10 ms = not a real Discord packet: drop instead of playing noise */
}
static bool peer_next(Peer *p, int16_t *pcm) {          /* LOCK held: next 20 ms of this speaker, false = nothing to play */
  if (!p->dec) return false;
  int have = p->pend_n - p->pend_off;
  while (have < FRAME) {                                 /* decode packets until 20 ms is ready (a 60 ms packet covers three ticks) */
    if (p->pend_off) { memmove(p->pend, p->pend + p->pend_off * 2, (size_t)have * 4); p->pend_n = have; p->pend_off = 0; }
    if (p->pend_n + MAXF > MAXF + FRAME) break;
    int got = peer_decode(p, p->pend + p->pend_n * 2); if (got <= 0) break;
    p->pend_n += got; have += got; }
  if (have <= 0) return false;
  int take = have < FRAME ? have : FRAME;
  memcpy(pcm, p->pend + p->pend_off * 2, (size_t)take * 4); if (take < FRAME) memset(pcm + take * 2, 0, (size_t)(FRAME - take) * 4);
  p->pend_off += take; if (p->pend_off >= p->pend_n) p->pend_off = p->pend_n = 0;
  return true;
}
/* per-user playback volume (0 = muted for me, 1 = normal, up to 3 = amplified) and local deafen */
static struct { char uid[24]; float gain; } ugain[96]; static int nugain; static volatile bool deafened;
void voice_set_user_gain(const char *uid, float g) {
  if (!uid || !uid[0]) return; if (g < 0) g = 0; if (g > 3) g = 3; if (mx) LOCK();
  int i = 0; for (; i < nugain; i++) if (!strcmp(ugain[i].uid, uid)) break;
  if (i == nugain && nugain < 96) { snprintf(ugain[nugain].uid, 24, "%s", uid); nugain++; } if (i < nugain) ugain[i].gain = g;
  if (mx) UNLOCK();
}
float voice_get_user_gain(const char *uid) { for (int i = 0; i < nugain; i++) if (!strcmp(ugain[i].uid, uid)) return ugain[i].gain; return 1.0f; }
void voice_set_deaf(bool d) { deafened = d; }
/* ---- mic gain: user volume (x2 base = 100%), optional auto gain, soft limiter ---- */
static volatile float mic_gain = 1.0f; static volatile bool mic_agc; static volatile int mic_lvl; static float agc_mul = 1.0f, agc_env;
void voice_set_mic_gain(float g) { if (g < 0.5f) g = 0.5f; if (g > 4.0f) g = 4.0f; mic_gain = g; }
float voice_get_mic_gain(void) { return mic_gain; }
void voice_set_mic_agc(bool on) { mic_agc = on; if (!on) agc_mul = 1.0f; }
bool voice_get_mic_agc(void) { return mic_agc; }
int voice_mic_level(void) { return mic_lvl; }
static inline int16_t soft_clip(int v) {            /* gentle knee above ~50% so amplified speech doesn't crackle */
  int a = v < 0 ? -v : v; if (a > 16384) a = 16384 + (a - 16384) / 3; if (a > 32767) a = 32767; return (int16_t)(v < 0 ? -a : a);
}
static VoiceSettings vs = {true, true, false, false, true, 0, 64, 1.0f}; static volatile bool enc_dirty = true, ptt_down; static volatile int out_peak, vad_thr = 600; static float noise_floor = 300;
void voice_get_settings(VoiceSettings *s) { *s = vs; }
void voice_set_settings(const VoiceSettings *s) { bool re = s->music != vs.music || s->kbps != vs.kbps || s->fec != vs.fec; vs = *s; if (vs.kbps < 16) vs.kbps = 16; if (vs.kbps > 128) vs.kbps = 128; if (vs.out_gain < 0) vs.out_gain = 0; if (vs.out_gain > 2) vs.out_gain = 2; if (re) enc_dirty = true; }
void voice_ptt(bool down) { ptt_down = down; }
int voice_vad_threshold(void) { return vad_thr; }
static OpusEncoder *enc_make(void) {               /* (re)create the encoder for the current settings */
  int e; OpusEncoder *o = opus_encoder_create(48000, 2, vs.music ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &e); if (!o) return NULL;
  opus_encoder_ctl(o, OPUS_SET_BITRATE(vs.kbps * 1000)); opus_encoder_ctl(o, OPUS_SET_SIGNAL(vs.music ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(o, OPUS_SET_INBAND_FEC(vs.fec ? 1 : 0)); opus_encoder_ctl(o, OPUS_SET_PACKET_LOSS_PERC(vs.fec ? 10 : 0)); if (vs.music) opus_encoder_ctl(o, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
  dlog("[mic] opus: %s, %d kbps, fec %d", vs.music ? "music" : "voice", vs.kbps, vs.fec); return o;
}
static void mic_pre(int16_t *pcm, int n) {          /* low-cut (~100 Hz) when noise suppression is on; stereo-duplicated mono */
  static float px, py; if (!vs.ns) return;
  for (int i = 0; i < n; i += 2) { float x = pcm[i], y = 0.987f * (py + x - px); px = x; py = y; int v = (int)y; if (v > 32767) v = 32767; if (v < -32768) v = -32768; pcm[i] = pcm[i + 1] = (int16_t)v; }
}
static int mic_post(int16_t *pcm, int n) {          /* gate / echo duck / threshold; returns the frame peak after processing */
  int peak = 0; for (int i = 0; i < n; i += 2) { int a = abs(pcm[i]); if (a > peak) peak = a; }
  if (peak < noise_floor * 1.6f) noise_floor = noise_floor * 0.95f + peak * 0.05f; else noise_floor *= 1.0008f;   /* follows the background noise */
  if (noise_floor < 80) noise_floor = 80; if (noise_floor > 8000) noise_floor = 8000;
  vad_thr = vs.sens > 0 ? vs.sens * 250 : (int)fmaxf(600.0f, noise_floor * 3.0f);
  float att = 1.0f;
  if (vs.ns && peak < vad_thr) att *= 0.15f;                                    /* gate: hush the noise between words */
  if (vs.echo && out_peak > 1500) att *= 0.3f;                                  /* call audio is playing: duck the mic */
  if (att < 0.999f) { int ai = (int)(att * 256); for (int i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[i] * ai) >> 8); peak = (int)(peak * att); }
  return peak;
}
static void mic_process(int16_t *pcm, int n) {      /* n stereo samples; pcm already at the old fixed x2 level */
  float g = mic_gain * (mic_agc ? agc_mul : 1.0f); int gi = (int)(g * 256), peak = 0;
  for (int i = 0; i < n; i++) { int s = (pcm[i] * gi) >> 8; pcm[i] = soft_clip(s); int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > peak) peak = a; }
  mic_lvl = peak;
  if (mic_agc && peak > 900) {                        /* only adapt while talking */
    agc_env = agc_env * 0.97f + peak * 0.03f;
    if (agc_env < 9000 && agc_mul < 4.0f) agc_mul *= 1.01f; else if (agc_env > 22000 && agc_mul > 0.5f) agc_mul *= 0.97f;
  }
}
/* mic test loopback: processed mic frames -> a small ring the mixer plays */
static volatile bool mic_test, test_standalone; static int16_t lb[960 * 2 * 10]; static int lb_r, lb_w, lb_n; static SDL_mutex *lbmx;
static void lb_push(const int16_t *pcm) { if (!lbmx) return; SDL_LockMutex(lbmx); for (int i = 0; i < FRAME * 2; i++) { if (lb_n >= (int)(sizeof lb / 2)) { lb_r = (lb_r + 1) % (int)(sizeof lb / 2); lb_n--; } lb[lb_w] = pcm[i]; lb_w = (lb_w + 1) % (int)(sizeof lb / 2); lb_n++; } SDL_UnlockMutex(lbmx); }
static bool lb_pop(int32_t *mix) { if (!lbmx) return false; bool any = false; SDL_LockMutex(lbmx); if (lb_n >= FRAME * 2) { for (int i = 0; i < FRAME * 2; i++) { mix[i] += lb[lb_r]; lb_r = (lb_r + 1) % (int)(sizeof lb / 2); } lb_n -= FRAME * 2; any = true; } SDL_UnlockMutex(lbmx); return any; }
static int mixer_worker(void *arg);
static int mic_worker(void *arg);
bool voice_mic_testing(void) { return mic_test; }
void voice_mic_test(bool on) {
  if (!lbmx) lbmx = SDL_CreateMutex(); if (!mx) { mx = SDL_CreateMutex(); txmx = SDL_CreateMutex(); }
  SDL_LockMutex(lbmx); lb_r = lb_w = lb_n = 0; SDL_UnlockMutex(lbmx);
  if (on && !mic_test) { mic_test = true;
    if (!th && !mic_th) {                               /* not in a call: run the mic + mixer on their own */
      if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
      if (!adev) { SDL_AudioSpec want = {0}, have; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); if (adev) SDL_PauseAudioDevice(adev, 0); }
      stop_flag = false; test_standalone = true;
      mic_th = SDL_CreateThreadWithStackSize(mic_worker, "mic", 512 * 1024, NULL); mix_th = SDL_CreateThreadWithStackSize(mixer_worker, "mixer", 256 * 1024, NULL); dlog("[mic] test started (standalone)"); } }
  else if (!on && mic_test) { mic_test = false;
    if (test_standalone) { stop_flag = true; if (mic_th) { SDL_WaitThread(mic_th, NULL); mic_th = NULL; } if (mix_th) { SDL_WaitThread(mix_th, NULL); mix_th = NULL; } test_standalone = false; stop_flag = false; dlog("[mic] test stopped"); } }
}
static int mixer_worker(void *arg) {
  (void)arg; dlog("[vc] mixer thread start");
  static int32_t mix[FRAME * 2]; static int16_t out[FRAME * 2], pcm[FRAME * 2];
  while (!stop_flag) {
    if (!adev) { SDL_Delay(10); continue; }
    int fill = (int)(SDL_GetQueuedAudioSize(adev) / 2);   /* samples queued in SDL (push model: a custom SDL callback crashes Cemu) */
    if (fill >= RING_TARGET) { SDL_Delay(4); continue; }
    if (fill == 0 && st_mix) st_under++;
    memset(mix, 0, sizeof mix); uint32_t now = SDL_GetTicks();
    LOCK();
    for (int i = 0; i < npeers; i++) { Peer *p = &peers[i]; if (!peer_next(p, pcm) || deafened) continue;   /* always decode (keeps Opus state), then apply the volume */
      float g = p->p.user_id[0] ? voice_get_user_gain(p->p.user_id) : 1.0f; if (g <= 0.001f) continue;
      if (g > 0.999f && g < 1.001f) { for (int k = 0; k < FRAME * 2; k++) mix[k] += pcm[k]; } else { int gi = (int)(g * 256); for (int k = 0; k < FRAME * 2; k++) mix[k] += (pcm[k] * gi) >> 8; } }
    for (int i = 0; i < npeers; i++) I.peers[i] = peers[i].p, I.peers[i].speaking = now - peers[i].p.last_pkt < 300; I.npeers = npeers;
    UNLOCK();
    if (mic_test) lb_pop(mix);
    { int og = (int)(vs.out_gain * 256), op2 = 0; for (int k = 0; k < FRAME * 2; k++) { mix[k] = (mix[k] * og) >> 8; int a = mix[k] < 0 ? -mix[k] : mix[k]; if (a > op2) op2 = a; } out_peak = op2 > out_peak ? op2 : (out_peak * 7) / 8; }
    for (int k = 0; k < FRAME * 2; k++) { int32_t s = mix[k]; out[k] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s; }
    SDL_QueueAudio(adev, out, sizeof out); ring_n = (int)(SDL_GetQueuedAudioSize(adev) / 2);
    st_mix++;
  }
  return 0;
}

/* ---------- GamePad microphone thread: 32 kHz mono -> 48 kHz -> Opus ---------- */
static int mic_worker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(OSGetCurrentThread(), 14);
  { FILE *nf = fopen("/vol/external01/wiiu/apps/MingusDiscord/nomic.txt", "rb"); if (nf) { fclose(nf); dlog("[mic] nomic.txt present: mic disabled (receive only, test switch)"); LOCK(); I.mic_ok = false; UNLOCK(); return 0; } }
  MICWorkMemory wm; wm.sampleMaxCount = 0x2800; wm.sampleBuffer = memalign(0x40, wm.sampleMaxCount * 2); MICError err = MIC_ERROR_OK;
  MICHandle h = MICInit(MIC_INSTANCE_0, 0, &wm, &err);
  if (h < 0 || err != MIC_ERROR_OK) { dlog("[mic] init failed (%d) - receive only", (int)err); LOCK(); I.mic_ok = false; UNLOCK(); free(wm.sampleBuffer); return 0; }
  if (MICOpen(h) != MIC_ERROR_OK) { dlog("[mic] open failed"); MICUninit(h); free(wm.sampleBuffer); return 0; }
  LOCK(); I.mic_ok = true; UNLOCK(); dlog("[mic] open");
  enc = enc_make(); enc_dirty = false;
  static int16_t acc[4096]; int accn = 0; static int16_t pcm[FRAME * 2]; static unsigned char op[1500]; int16_t *mb = wm.sampleBuffer; int silent_frames = 0;
  while (!stop_flag) {
    MICStatus st; if (MICGetStatus(h, &st) != MIC_ERROR_OK) break;
    int n = st.availableData; if (n > 0) { if (n > 2048) n = 2048; int pos = st.bufferPos; DCInvalidateRange(mb, wm.sampleMaxCount * 2);   /* the mic DMA-writes this buffer: drop stale cache lines (silent mic on real hardware) */
      for (int i = 0; i < n && accn < 4096; i++) acc[accn++] = mb[(pos + i) % wm.sampleMaxCount];
      MICSetDataConsumed(h, n); }
    while (accn >= 640) {                                   /* 640 @32k = 960 @48k = 20 ms */
      for (int i = 0; i < FRAME; i++) { float s = i * 640.0f / FRAME; int i0 = (int)s; float fr = s - i0; int i1 = i0 + 1 < 640 ? i0 + 1 : i0; int v = (int)((1 - fr) * acc[i0] + fr * acc[i1]) * 2; if (v > 32767) v = 32767; if (v < -32768) v = -32768; pcm[2 * i] = pcm[2 * i + 1] = (int16_t)v; }
      memmove(acc, acc + 640, (accn - 640) * 2); accn -= 640;
      if (enc_dirty) { enc_dirty = false; OpusEncoder *ne = enc_make(); if (ne) { if (enc) opus_encoder_destroy(enc); enc = ne; } }
      mic_pre(pcm, FRAME * 2); mic_process(pcm, FRAME * 2); int ppeak = mic_post(pcm, FRAME * 2); LOCK(); I.mic_level = mic_lvl; UNLOCK();
      if (mic_test) lb_push(pcm);
      if (muted || !crypto_ready) { silent_frames = 0; continue; }
      int peak = 0; for (int i = 0; i < FRAME * 2; i += 7) { int a = abs(pcm[i]); if (a > peak) peak = a; } st_mic++; if ((uint32_t)peak > st_peak) st_peak = peak;
      bool talk = vs.ptt ? ptt_down : ppeak > vad_thr; (void)peak; if (talk) silent_frames = 0; else silent_frames++;
      if (silent_frames > 25) { LOCK(); I.speaking = false; UNLOCK(); SDL_LockMutex(txmx); tx_ts += FRAME; SDL_UnlockMutex(txmx); continue; }   /* not sending, but the RTP clock keeps time */
      if (silent_frames == 25) { static const unsigned char sil[3] = {0xF8, 0xFF, 0xFE}; for (int k = 0; k < 5; k++) send_opus(sil, 3); st_sent += 5; continue; }   /* end of speech: 5 Opus silence frames */
      LOCK(); I.speaking = true; UNLOCK();
      int on = opus_encode(enc, pcm, FRAME, op, sizeof op); if (on > 0) { send_opus(op, on); st_sent++; }
    }
    SDL_Delay(5);
  }
  MICClose(h); MICUninit(h); free(wm.sampleBuffer); if (enc) { opus_encoder_destroy(enc); enc = NULL; } return 0;
}

/* ---------- UDP: IP discovery ---------- */
static bool udp_discover(const char *ip, int port, char *out_ip, int *out_port) {
  sock = socket(AF_INET, SOCK_DGRAM, 0); if (sock < 0) return false;
  memset(&peer_addr, 0, sizeof peer_addr); peer_addr.sin_family = AF_INET; peer_addr.sin_port = htons((uint16_t)port); peer_addr.sin_addr.s_addr = inet_addr(ip);
  int one = 1; setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &one, sizeof one);
  netmem_udp_setup(sock, "voice");   /* Wii U: receive into the app's own socket memory pool */
  unsigned char pkt[74] = {0}; pkt[1] = 1; pkt[3] = 70; pkt[4] = my_ssrc >> 24; pkt[5] = my_ssrc >> 16; pkt[6] = my_ssrc >> 8; pkt[7] = my_ssrc;
  for (int attempt = 0; attempt < 5 && !stop_flag; attempt++) {
    sendto(sock, pkt, 74, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr);
    for (int i = 0; i < 100 && !stop_flag; i++) { unsigned char r[128]; int n = recvfrom(sock, r, sizeof r, 0, NULL, NULL);
      if (n >= 74 && r[1] == 2) { snprintf(out_ip, 64, "%.63s", (char *)r + 8); *out_port = r[72] << 8 | r[73]; return true; } SDL_Delay(10); }
  }
  return false;
}

/* ---------- voice gateway thread ---------- */
static int worker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(OSGetCurrentThread(), 16);
  static char msg[65536]; Ws w; char host[170]; snprintf(host, sizeof host, "%s", endpoint); char *c = strchr(host, ':'); int eport = c ? atoi(c + 1) : 0; if (c) *c = 0;
  char url[260]; if (eport > 0 && eport != 443 && eport != 80) snprintf(url, sizeof url, "wss://%s:%d/?v=8", host, eport); else snprintf(url, sizeof url, "wss://%s/?v=8", host);   /* newer endpoints carry a real port (e.g. :2096) */
  dlog("[vc] voice url %s", url);
  set_state(VC_CONNECTING, "");
  if (!ws_open(&w, url, "https://discord.com", 15000)) { dlog("[vc] %s", w.err); set_state(VC_ERROR, w.err); return 0; }
  g_ws = &w; dlog("[vc] connected to %s", host);
  json_t *id = json_object(); json_object_set_new(id, "server_id", json_string(server_id)); json_object_set_new(id, "user_id", json_string(user_id)); json_object_set_new(id, "session_id", json_string(session_id)); json_object_set_new(id, "token", json_string(vtoken)); json_object_set_new(id, "max_dave_protocol_version", json_integer(dave_max_protocol_version()));
  { json_object_set_new(id, "video", json_true()); json_t *st = json_object(); json_object_set_new(st, "type", json_string("video")); json_object_set_new(st, "rid", json_string("100")); json_object_set_new(st, "quality", json_integer(100)); json_object_set_new(st, "active", json_false());
    json_t *sa = json_array(); json_array_append_new(sa, st); json_object_set_new(id, "streams", sa); }   /* video: we receive cameras (our own stream stays inactive) */
  json_t *o = json_object(); json_object_set_new(o, "op", json_integer(0)); json_object_set_new(o, "d", id); send_json(&w, o);
  uint32_t hb_iv = 13750, last_hb = SDL_GetTicks(), hb_sent_t = 0, next_mix = SDL_GetTicks(); long long seq = -1; bool hello = false;
  while (!stop_flag) {
    ob_flush(&w);
    bool is_bin = false; size_t mlen = 0; int r = ws_recv2(&w, msg, sizeof msg, &is_bin, &mlen);
    if (r < 0) { dlog("[vc] closed: %s", w.err); set_state(VC_ERROR, w.err[0] ? w.err : "Voice connection closed"); break; }
    if (r == 1 && is_bin) {                                   /* DAVE binary: u16 seq, u8 opcode, payload */
      if (mlen >= 3) { const uint8_t *m = (const uint8_t *)msg; seq = m[0] << 8 | m[1]; int bop = m[2]; const uint8_t *pl = m + 3; size_t pn = mlen - 3;
        if (bop == 25) dq_push(D_EXTSENDER, 0, 0, pl, pn); else if (bop == 27) dq_push(D_PROPOSALS, 0, 0, pl, pn);
        else if ((bop == 29 || bop == 30) && pn >= 2) dq_push(bop == 29 ? D_COMMIT : D_WELCOME, pl[0] << 8 | pl[1], 0, pl + 2, pn - 2);
        else dlog("[vc] binary opcode %d (%u bytes) ignored", bop, (unsigned)pn); }
    } else if (r == 1) {
      json_t *j = json_loads(msg, 0, NULL); if (!j) continue;
      int op = (int)json_integer_value(json_object_get(j, "op")); json_t *d = json_object_get(j, "d"); json_t *sq = json_object_get(j, "seq"); if (json_is_integer(sq)) seq = json_integer_value(sq);
      if (op == 8) { hello = true; json_t *hi = d ? json_object_get(d, "heartbeat_interval") : NULL; if (json_is_number(hi)) hb_iv = (uint32_t)json_number_value(hi); last_hb = SDL_GetTicks(); dlog("[vc] hello hb=%u", hb_iv); }
      else if (op == 2 && d) {
        my_ssrc = (uint32_t)json_integer_value(json_object_get(d, "ssrc")); const char *ip = js(d, "ip"); int port = (int)json_integer_value(json_object_get(d, "port"));
        json_t *modes = json_object_get(d, "modes"); bool gcm_ok = false, xc_ok = false; size_t k; json_t *m; json_array_foreach(modes, k, m) { if (json_is_string(m)) { if (!strcmp(json_string_value(m), "aead_aes256_gcm_rtpsize")) gcm_ok = true; if (!strcmp(json_string_value(m), "aead_xchacha20_poly1305_rtpsize")) xc_ok = true; } }
        dlog("[vc] ready ssrc=%u %s:%d gcm=%d xchacha=%d", my_ssrc, ip, port, gcm_ok, xc_ok); set_state(VC_UDP, "");
        { json_t *vs = json_object_get(d, "streams"); json_t *s0 = json_array_get(vs, 0); if (s0) { my_video_ssrc = (uint32_t)json_integer_value(json_object_get(s0, "ssrc")); json_t *rx = json_object_get(s0, "rtx_ssrc"); my_rtx_ssrc = json_is_integer(rx) ? (uint32_t)json_integer_value(rx) : my_video_ssrc + 1; }
          char *sd = json_dumps(vs, JSON_COMPACT); dlog("[vc] ready video streams: %s", sd ? sd : "none"); free(sd); }
        char ext_ip[64]; int ext_port = 0;
        if (!udp_discover(ip, port, ext_ip, &ext_port)) { dlog("[vc] ip discovery failed"); set_state(VC_ERROR, "UDP to the voice server failed (firewall?)"); json_decref(j); break; }
        udp_ready = true; dlog("[vc] external %s:%d", ext_ip, ext_port);
        const char *mode = gcm_ok ? "aead_aes256_gcm_rtpsize" : xc_ok ? "aead_xchacha20_poly1305_rtpsize" : NULL;
        if (!mode) { set_state(VC_ERROR, "Server offers no supported encryption mode"); json_decref(j); break; }
        json_t *data = json_object(); json_object_set_new(data, "address", json_string(ext_ip)); json_object_set_new(data, "port", json_integer(ext_port)); json_object_set_new(data, "mode", json_string(mode));
        json_t *sd = json_object(); json_object_set_new(sd, "protocol", json_string("udp")); json_object_set_new(sd, "data", data); json_object_set_new(sd, "codecs", voice_codecs_json());
        json_t *so = json_object(); json_object_set_new(so, "op", json_integer(1)); json_object_set_new(so, "d", sd); send_json(&w, so);
      }
      else if (op == 4 && d) {
        const char *mode = js(d, "mode"); json_t *sk = json_object_get(d, "secret_key"); int dv = (int)json_integer_value(json_object_get(d, "dave_protocol_version"));
        if (json_is_array(sk) && json_array_size(sk) == 32) { for (int i = 0; i < 32; i++) key[i] = (unsigned char)json_integer_value(json_array_get(sk, i)); }
        enc_mode = !strcmp(mode, "aead_aes256_gcm_rtpsize") ? 1 : 2; if (enc_mode == 1) { mbedtls_gcm_init(&gcm); mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256); }
        crypto_ready = true; LOCK(); snprintf(I.mode, sizeof I.mode, "%s", mode); I.dave_version = dv; UNLOCK(); set_state(VC_CONNECTED, "");
        dlog("[vc] session description mode=%s dave=%d", mode, dv); if (dv > 0) { dave_ver = dv; dq_push(D_INIT, dv, 0, NULL, 0); }
        want_speaking = muted ? 0 : 1;
        if (dv > 0) vr_set_dave(VR_CONN_VOICE, true); kf_ms = (int)json_integer_value(json_object_get(d, "keyframe_interval")); video_after_session(&w, d);
      }
      else if (op == 12 && d) { const char *uid = vr_on_video_op(VR_CONN_VOICE, d); if (uid) { user_add(uid); uint32_t as = (uint32_t)json_integer_value(json_object_get(d, "audio_ssrc"));
          if (as) { bool fresh = false; LOCK(); Peer *p = peer_for(as, true); if (p && strcmp(p->p.user_id, uid)) { snprintf(p->p.user_id, 24, "%s", uid); fresh = true; } UNLOCK(); if (fresh && dave_ver > 0) dq_push(D_PEERS, 0, 0, NULL, 0); } } }   /* op 12 also names the audio ssrc */
      else if (op == 5 && d) { uint32_t ssrc = (uint32_t)json_integer_value(json_object_get(d, "ssrc")); const char *uid = js(d, "user_id"); LOCK(); Peer *p = peer_for(ssrc, true); if (p) snprintf(p->p.user_id, 24, "%s", uid); UNLOCK(); user_add(uid); if (dave_ver > 0) dq_push(D_PEERS, 0, 0, NULL, 0); }
      else if (op == 15 && d) video_sink_from_server(d);
      else if (op == 11 && d) { if (cam_on) vs_request_idr();   /* new viewer: send a keyframe */
        json_t *ids = json_object_get(d, "user_ids"); size_t k; json_t *u; json_array_foreach(ids, k, u) if (json_is_string(u)) user_add(json_string_value(u)); }
      else if (op == 21 && d) dq_push(D_PREP, (int)json_integer_value(json_object_get(d, "protocol_version")), (int)json_integer_value(json_object_get(d, "transition_id")), NULL, 0);
      else if (op == 22 && d) dq_push(D_EXEC, (int)json_integer_value(json_object_get(d, "transition_id")), 0, NULL, 0);
      else if (op == 24 && d) dq_push(D_EPOCH, (int)json_integer_value(json_object_get(d, "protocol_version")), (int)json_integer_value(json_object_get(d, "epoch")), NULL, 0);
      else if (op == 6) { LOCK(); I.ping_ms = (int)(SDL_GetTicks() - hb_sent_t); UNLOCK(); }
      else if (op == 13 && d) { const char *uid = js(d, "user_id"); vr_map(VR_CONN_VOICE, uid, NULL, NULL, 0); user_del(uid); SDL_LockMutex(dmx); LOCK(); for (int i = 0; i < npeers; i++) if (!strcmp(peers[i].p.user_id, uid)) { if (peers[i].dec) opus_decoder_destroy(peers[i].dec); if (peers[i].dd) dave_decryptor_free(peers[i].dd); memmove(&peers[i], &peers[i + 1], (npeers - i - 1) * sizeof peers[0]); npeers--; break; } UNLOCK(); SDL_UnlockMutex(dmx); }
      json_decref(j);
    } else {
      if (hello && SDL_GetTicks() - last_hb >= hb_iv) { char b[96]; snprintf(b, sizeof b, "{\"op\":3,\"d\":{\"t\":%u,\"seq_ack\":%lld}}", (unsigned)SDL_GetTicks(), seq); ws_send_text(&w, b, strlen(b)); last_hb = hb_sent_t = SDL_GetTicks(); }
      if (want_speaking >= 0 && crypto_ready) { char b[96]; snprintf(b, sizeof b, "{\"op\":5,\"d\":{\"speaking\":%d,\"delay\":0,\"ssrc\":%u}}", want_speaking ? 1 : 0, my_ssrc); ws_send_text(&w, b, strlen(b)); want_speaking = -1; }
      if (udp_ready) { static NetRx vrx; unsigned char *pkt = vrx.buf; for (int i = 0; i < 64; i++) { int n = netmem_recv(sock, &vrx, "voice"); if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) { st_lasterr = errno; if (st_rxerr++ < 3) dlog("[vc] UDP receive error %d", errno); } if (n <= 0) break; if (st_raw++ == 0) dlog("[vc] first UDP packet after discovery: %d bytes, pt %d", n, n > 1 ? pkt[1] & 0x7F : -1); if (crypto_ready && !video_rx(pkt, n)) rx_packet(pkt, n); } }
      video_tick(&w); video_send_tick(&w);
      uint32_t now = SDL_GetTicks();
      if (udp_ready) { static uint32_t ka_t, ka_n; if (now - ka_t > 5000) { ka_t = now; unsigned char ka[8] = {0}; ka_n++; ka[0] = ka_n; ka[1] = ka_n >> 8; ka[2] = ka_n >> 16; ka[3] = ka_n >> 24; sendto(sock, ka, 8, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr); } }   /* UDP keepalive (like discord.js): keeps the NAT path open while we are silent */
      { static uint32_t st_t; if (now - st_t > 5000) { st_t = now; dlog("[vc] stats: udp %u bad %u rxq %d rxerr %u/%d | rx %u buffered %u mixed %u ring %d | underruns %u plc %u | mic frames %u peak %u sent %u | dev %d", (unsigned)st_raw, (unsigned)st_aead_bad, netmem_rxdata(sock), (unsigned)st_rxerr, st_lasterr, (unsigned)I.rx_packets, (unsigned)st_dec, (unsigned)st_mix, ring_n, (unsigned)st_under, (unsigned)st_plc, (unsigned)st_mic, (unsigned)st_peak, (unsigned)st_sent, (int)adev); st_peak = 0; } }
      SDL_Delay(3);
    }
  }
  g_ws = NULL; ws_close(&w); return 0;
}

void voice_start(const char *sid, const char *cid, const char *uid, const char *sess, const char *tok, const char *ep) {
  if (test_standalone) { bool keep = mic_test; voice_mic_test(false); mic_test = keep; }   /* the call takes over the mic (test keeps playing inside the call) */
  if (!mx) { mx = SDL_CreateMutex(); txmx = SDL_CreateMutex(); } if (th) voice_stop();
  snprintf(server_id, 24, "%s", sid); snprintf(chan_id, 24, "%s", cid); snprintf(user_id, 24, "%s", uid); snprintf(session_id, sizeof session_id, "%s", sess); snprintf(vtoken, sizeof vtoken, "%s", tok); snprintf(endpoint, sizeof endpoint, "%s", ep);
  stop_flag = false; crypto_ready = udp_ready = false; enc_mode = 0; tx_nonce = tx_seq = 0; tx_ts = 0; npeers = 0; memset(&I, 0, sizeof I); I.muted = muted;
  if (!adev) {   /* the app's SDL_Init has no AUDIO: without this the device never opened and every call was silent */
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) dlog("[vc] audio init failed: %s", SDL_GetError());
    if (!ringmx) ringmx = SDL_CreateMutex();
    SDL_AudioSpec want = {0}, have; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (adev) { SDL_PauseAudioDevice(adev, 0); dlog("[vc] audio out: %d Hz, %d ch, fmt 0x%x", have.freq, have.channels, have.format); } else dlog("[vc] audio open failed: %s", SDL_GetError()); }
  if (adev) SDL_ClearQueuedAudio(adev); ring_n = 0; st_under = st_plc = 0; st_raw = st_aead_bad = st_rxerr = 0; st_lasterr = 0;
  dave_start(); vr_init(); vr_set_conn(VR_CONN_VOICE, video_decrypt, video_pli); vr_set_dave(VR_CONN_VOICE, false); sink_q = -1; st_vid = 0; my_video_ssrc = my_rtx_ssrc = 0;
  th = SDL_CreateThreadWithStackSize(worker, "voice", 512 * 1024, NULL);
  mix_th = SDL_CreateThreadWithStackSize(mixer_worker, "mixer", 256 * 1024, NULL);
  mic_th = SDL_CreateThreadWithStackSize(mic_worker, "mic", 512 * 1024, NULL);
}
void voice_stop(void) {
  if (!th) return; golive_stop();
  if (cam_on) { vs_stop(); cam_on = false; want_op12 = -1; if (!cam_dry) gw_set_self_video(false); vr_map(VR_CONN_TEST, user_id, NULL, NULL, 0); dlog("[vc] camera off (leaving the call)"); }
  stop_flag = true; SDL_WaitThread(mic_th, NULL); mic_th = NULL; if (mix_th) { SDL_WaitThread(mix_th, NULL); mix_th = NULL; } SDL_WaitThread(th, NULL); th = NULL; dave_stop();
  if (sock >= 0) { close(sock); sock = -1; } if (enc_mode == 1) mbedtls_gcm_free(&gcm); crypto_ready = udp_ready = false;
  for (int i = 0; i < npeers; i++) if (peers[i].dec) opus_decoder_destroy(peers[i].dec); npeers = 0; if (adev) SDL_ClearQueuedAudio(adev); ring_n = 0;
  vr_conn_reset(VR_CONN_VOICE); vr_set_dave(VR_CONN_VOICE, false);
  LOCK(); I.state = VC_OFF; I.npeers = 0; UNLOCK();
}
void voice_set_mute(bool m) { muted = m; want_speaking = m ? 0 : 1; LOCK(); I.muted = m; if (m) I.speaking = false; UNLOCK(); }
void voice_snapshot(VcInfo *out) { if (!mx) { memset(out, 0, sizeof *out); return; } LOCK(); *out = I; UNLOCK();
  out->nusers = 0; if (umx) { SDL_LockMutex(umx); for (int i = 0; i < nknown && out->nusers < VC_MAX_PEERS; i++) snprintf(out->users[out->nusers++], 24, "%s", known[i]); SDL_UnlockMutex(umx); } }
