/* vrecv.c - Discord video receive: RTP H.264 depacketiser, DAVE frame decrypt hook, hardware decode, textures. See vrecv.h. */
#include <coreinit/cache.h>
#include "vrecv.h"
#include "dlog.h"
#include "nx.h"
#include <h264/decode.h>
#include <coreinit/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <jansson.h>
#include <coreinit/time.h>
#include "yuv.h"

#define SLOTS 512                /* reorder/reassembly window per stream (packets) */
#define SLOT_MAX 1500            /* max RTP payload we keep */
#define FQ 3                     /* reassembled frames waiting for the decoder */
#define MAX_FRAME (2u << 20)     /* biggest frame we accept */
#define TILE_W 320               /* tiles are converted at <= this width; the focused stream at <= 1280 */
#define FULL_W 1280
#define BLOCK_MS 100             /* a later frame waiting this long for an earlier one = packet loss */
#define PLI_MS 400               /* min gap between keyframe requests */
#define OUTQ 3                   /* converted frames waiting for presentation */
#define BEHIND_MS 150            /* presentation clock re-anchors when it lags/leads the newest frame by more */

typedef struct { uint16_t seq; bool used, data, marker; uint32_t ts; uint16_t len; uint8_t *buf; } Slot;
typedef struct { uint8_t *d; size_t n, cap; uint32_t ts; bool key; } Frame;
typedef struct VrStream {
  bool used, kill; int conn; unsigned gen; char uid[24];
  uint32_t ssrc[4], rtx[4]; int nssrc; uint32_t cur_ssrc;
  /* depacketiser (under mx) */
  Slot *slots; bool have_emit, need_key; uint16_t last_emit; uint32_t blocked_t; uint32_t pli_t; bool pli_due;
  Frame fq[FQ]; int fqn;
  /* decoder (decode thread only) */
  void *dmem; uint8_t *fb; size_t fbsz; bool dec_open, dec_failed, lvl_logged; int dw, dh, dlevel, dprof; uint8_t sps[256], pps[256]; int spsn, ppsn;
  /* output (under omx) */
  struct { uint8_t *buf; size_t cap; int w, h; uint32_t pts, seq; int state; } out[OUTQ];   /* state 0 free, 1 writing, 2 ready, 3 uploading */
  uint32_t out_seq; int ow, oh; uint32_t frames_out;
  bool clk_ok; int32_t clk_off;                  /* presentation clock (main thread): frame pts + clk_off = due tick */
  uint64_t t_exec, t_conv; uint32_t n_exec, n_conv, presented, late_drops;
  char status[64];
  /* stats */
  uint32_t pkts, rtx_pkts, frames, drops, dec_ok, dec_fail, plis, decoded, late, fed, cbn; int last_rc;
} VrStream;

static VrStream S[VR_MAX_STREAMS];
static SDL_mutex *mx, *omx; static SDL_sem *sem; static SDL_Thread *th; static volatile bool started;
static VrDecryptFn c_dec[VR_NCONN]; static VrPliFn c_pli[VR_NCONN]; static volatile bool c_dave[VR_NCONN];
static char focus_uid[24]; static int focus_conn = -1;
static uint32_t unmapped_pkts; static uint32_t unmapped_ssrc_logged[8]; static int n_unmapped_logged;
/* main thread */
static SDL_Texture *tex[VR_MAX_STREAMS]; static unsigned tex_gen[VR_MAX_STREAMS]; static int tex_w[VR_MAX_STREAMS], tex_h[VR_MAX_STREAMS];
static unsigned gen_ctr;

static inline int sdiff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }
static void setstat(VrStream *s, const char *m) { snprintf(s->status, sizeof s->status, "%s", m); }

/* ---------------- stream table (mx held) ---------------- */
static VrStream *find_user(int conn, const char *uid) { for (int i = 0; i < VR_MAX_STREAMS; i++) if (S[i].used && !S[i].kill && S[i].conn == conn && !strcmp(S[i].uid, uid)) return &S[i]; return NULL; }
static VrStream *find_ssrc(int conn, uint32_t ssrc, bool *is_rtx) {
  for (int i = 0; i < VR_MAX_STREAMS; i++) { VrStream *s = &S[i]; if (!s->used || s->kill || s->conn != conn) continue;
    for (int k = 0; k < s->nssrc; k++) { if (s->ssrc[k] == ssrc) { *is_rtx = false; return s; } if (s->rtx[k] && s->rtx[k] == ssrc) { *is_rtx = true; return s; } } }
  return NULL;
}
static void depack_reset(VrStream *s) {
  if (s->slots) for (int i = 0; i < SLOTS; i++) { s->slots[i].used = s->slots[i].data = false; }
  s->have_emit = false; s->need_key = true; s->blocked_t = 0; s->pli_due = true;
}

void vr_init(void) {
  if (started) return; mx = SDL_CreateMutex(); omx = SDL_CreateMutex(); sem = SDL_CreateSemaphore(0);
  extern int vr_decode_thread(void *); started = true;
  th = SDL_CreateThreadWithStackSize(vr_decode_thread, "vdec", 256 * 1024, NULL);
}
void vr_set_conn(int conn, VrDecryptFn dec, VrPliFn pli) { if (conn < 0 || conn >= VR_NCONN) return; vr_init(); SDL_LockMutex(mx); c_dec[conn] = dec; c_pli[conn] = pli; SDL_UnlockMutex(mx); }
void vr_set_dave(int conn, bool on) { if (conn >= 0 && conn < VR_NCONN && c_dave[conn] != on) { c_dave[conn] = on; dlog("[vr] conn %d DAVE video decrypt %s", conn, on ? "on" : "off"); } }
void vr_conn_reset(int conn) {
  if (!started) return; SDL_LockMutex(mx); int n = 0; for (int i = 0; i < VR_MAX_STREAMS; i++) if (S[i].used && S[i].conn == conn && !S[i].kill) { S[i].kill = true; n++; } SDL_UnlockMutex(mx);
  if (n) { dlog("[vr] conn %d reset (%d streams)", conn, n); SDL_SemPost(sem); }
}
void vr_map(int conn, const char *uid, const uint32_t *ssrc, const uint32_t *rtx, int n) {
  if (!uid || !uid[0] || conn < 0 || conn >= VR_NCONN) return; vr_init(); if (n > 4) n = 4;
  SDL_LockMutex(mx); VrStream *s = find_user(conn, uid);
  if (n <= 0) { if (s) { s->kill = true; dlog("[vr] conn %d user %s video off", conn, uid); } SDL_UnlockMutex(mx); SDL_SemPost(sem); return; }
  bool same = s && s->nssrc == n; for (int k = 0; same && k < n; k++) if (s->ssrc[k] != ssrc[k] || s->rtx[k] != (rtx ? rtx[k] : 0)) same = false;
  if (same) { SDL_UnlockMutex(mx); return; }
  if (!s) { for (int i = 0; i < VR_MAX_STREAMS && !s; i++) if (!S[i].used) s = &S[i];
    if (!s) { SDL_UnlockMutex(mx); dlog("[vr] no free stream slot for user %s", uid); return; }
    memset(s, 0, sizeof *s); s->used = true; s->conn = conn; s->gen = ++gen_ctr; snprintf(s->uid, sizeof s->uid, "%s", uid);
    s->slots = calloc(SLOTS, sizeof(Slot)); if (!s->slots) { s->used = false; SDL_UnlockMutex(mx); dlog("[vr] out of memory (slots)"); return; }
    setstat(s, "waiting for video"); }
  s->nssrc = n; for (int k = 0; k < n; k++) { s->ssrc[k] = ssrc[k]; s->rtx[k] = rtx ? rtx[k] : 0; } s->cur_ssrc = 0; depack_reset(s);
  char b[160]; int o = 0; for (int k = 0; k < n; k++) o += snprintf(b + o, sizeof b - o, " %u/%u", (unsigned)s->ssrc[k], (unsigned)s->rtx[k]);
  SDL_UnlockMutex(mx); dlog("[vr] conn %d user %s video ssrc/rtx:%s", conn, uid, b);
}

const char *vr_on_video_op(int conn, json_t *d) {
  const char *uid = json_string_value(json_object_get(d, "user_id")); if (!uid || !uid[0]) return NULL;
  uint32_t ss[4], rr[4]; int n = 0; json_t *streams = json_object_get(d, "streams"); size_t k; json_t *s;
  json_array_foreach(streams, k, s) { uint32_t v = (uint32_t)json_integer_value(json_object_get(s, "ssrc")); json_t *a = json_object_get(s, "active");
    if (!v || (json_is_boolean(a) && !json_is_true(a)) || n >= 4) continue; ss[n] = v; json_t *r = json_object_get(s, "rtx_ssrc"); rr[n] = json_is_integer(r) && json_integer_value(r) ? (uint32_t)json_integer_value(r) : v + 1; n++; }
  uint32_t vs = (uint32_t)json_integer_value(json_object_get(d, "video_ssrc"));
  if (!n && vs && !json_array_size(streams)) { ss[0] = vs; json_t *r = json_object_get(d, "rtx_ssrc"); rr[0] = json_is_integer(r) && json_integer_value(r) ? (uint32_t)json_integer_value(r) : vs + 1; n = 1; }
  char *dump = json_dumps(d, JSON_COMPACT); dlog("[vr] conn %d op 12: %.600s", conn, dump ? dump : "?"); free(dump);
  vr_map(conn, uid, ss, rr, vs ? n : 0); return uid;
}

/* local picture source (our own camera preview): a stream entry with no ssrcs, fed with NV12 pictures directly */
static bool put_picture(VrStream *s, const uint8_t *yp, const uint8_t *uvp, int pitch, int w, int h, double ts_sec);
void vr_local_open(int conn, const char *uid) {
  if (!uid || !uid[0] || conn < 0 || conn >= VR_NCONN) return; vr_init(); SDL_LockMutex(mx); VrStream *s = find_user(conn, uid);
  for (int i = 0; i < VR_MAX_STREAMS && !s; i++) if (!S[i].used) { s = &S[i]; memset(s, 0, sizeof *s); s->used = true; s->conn = conn; s->gen = ++gen_ctr; snprintf(s->uid, sizeof s->uid, "%s", uid); s->pkts = 1; }
  SDL_UnlockMutex(mx); dlog("[vr] local preview %s on conn %d %s", uid, conn, s ? "open" : "FAILED (no slot)");
}
void vr_local_close(int conn, const char *uid) { vr_map(conn, uid, NULL, NULL, 0); }
bool vr_local_frame(int conn, const char *uid, const uint8_t *y, const uint8_t *uv, int pitch, int w, int h, uint32_t ts90k) {
  if (!started) return false; SDL_LockMutex(mx); VrStream *s = find_user(conn, uid); SDL_UnlockMutex(mx); if (!s || s->nssrc) return false;
  bool ok = put_picture(s, y, uv, pitch, w, h, ts90k / 90000.0); if (ok) s->decoded++; return ok;
}

/* ---------------- depacketiser ---------------- */
static Slot *slot_at(VrStream *s, uint16_t seq) { Slot *x = &s->slots[seq % SLOTS]; return x->used && x->seq == seq ? x : NULL; }
static bool pkt_is_key(const Slot *x) {
  const uint8_t *p = x->buf; int n = x->len; if (n < 1) return false; int t = p[0] & 31;
  if (t == 5) return true; if (t == 28) return n >= 2 && (p[1] & 31) == 5;
  if (t == 24) { int i = 1; while (i + 2 <= n) { int sz = p[i] << 8 | p[i + 1]; i += 2; if (i + sz > n || sz < 1) break; if ((p[i] & 31) == 5) return true; i += sz; } }
  return false;
}
static bool starts_with_sps(const Slot *x) {   /* single SPS NAL, or STAP-A whose first NAL is an SPS */
  if (!x || x->len < 2) return false; int t = x->buf[0] & 31; if (t == 7) return true; return t == 24 && x->len >= 4 && (x->buf[3] & 31) == 7;
}
static bool put(Frame *f, const uint8_t *d, size_t n) {
  if (f->n + n > f->cap) { size_t nc = f->cap ? f->cap : 65536; while (nc < f->n + n) nc *= 2; if (nc > MAX_FRAME + 65536) return false; uint8_t *nb = realloc(f->d, nc); if (!nb) return false; f->d = nb; f->cap = nc; }
  memcpy(f->d + f->n, d, n); f->n += n; return true;
}
static const uint8_t SC[4] = {0, 0, 0, 1};
/* packets b..e (all present, same ts) -> Annex-B with 4-byte start codes. false = malformed */
static bool assemble(VrStream *s, uint16_t b, uint16_t e, Frame *f) {
  f->n = 0; bool in_fu = false; bool ok = true;
  for (uint16_t q = b;; q++) { Slot *x = slot_at(s, q); if (!x) return false; const uint8_t *p = x->buf; int n = x->len; int t = n ? p[0] & 31 : 0;
    if (n < 1) ok = false;
    else if (t >= 1 && t <= 23) { if (in_fu) ok = false; ok = ok && put(f, SC, 4) && put(f, p, n); }
    else if (t == 24) { int i = 1; while (ok && i + 2 <= n) { int sz = p[i] << 8 | p[i + 1]; i += 2; if (sz < 1 || i + sz > n) { ok = false; break; } ok = put(f, SC, 4) && put(f, p + i, sz); i += sz; } }
    else if (t == 28) { if (n < 2) ok = false; else { bool st = p[1] & 0x80, en = p[1] & 0x40; uint8_t h = (p[0] & 0xE0) | (p[1] & 0x1F);
        if (st) { ok = put(f, SC, 4) && put(f, &h, 1); in_fu = true; } else if (!in_fu) ok = false;
        if (ok) ok = put(f, p + 2, n - 2); if (en) in_fu = false; } }
    else ok = false;   /* STAP-B / MTAP / FU-B: not used in packetization-mode 1 */
    if (!ok || q == e) break; }
  return ok && !in_fu && f->n > 4;
}
static void request_key(VrStream *s, const char *why) {
  if (!s->need_key) { s->need_key = true; dlog("[vr] %s: %s -> waiting for a keyframe", s->uid, why); } s->pli_due = true; setstat(s, "waiting for keyframe");
}
static void emit(VrStream *s, uint16_t b, uint16_t e, bool key) {
  for (uint16_t q = b;; q++) { Slot *x = slot_at(s, q); if (x) x->data = false; if (q == e) break; }
  s->have_emit = true; s->last_emit = e; s->blocked_t = 0;
  Slot *x = slot_at(s, e); uint32_t ts = x ? x->ts : 0;
  if (s->fqn >= FQ) { if (key) { s->drops += s->fqn; s->fqn = 0; } else { s->drops++; request_key(s, "decoder queue full"); return; } }
  /* reassemble straight into the queue slot (buffers are recycled) */
  Frame *f = &s->fq[s->fqn]; f->ts = ts; f->key = key;
  if (!assemble(s, b, e, f)) { s->drops++; request_key(s, "malformed RTP H.264 payload"); return; }
  s->fqn++; s->frames++; if (key) s->need_key = false;
  if (s->frames <= 3 || (key && s->frames < 200)) dlog("[vr] %s frame #%u ts=%u %u bytes %s (seq %u..%u)", s->uid, (unsigned)s->frames, (unsigned)ts, (unsigned)f->n, key ? "KEY" : "", b, e);
  SDL_SemPost(sem);
}
/* try to complete the frame containing seq; returns true if one was emitted */
static bool try_frame(VrStream *s, uint16_t seq) {
  Slot *x = slot_at(s, seq); if (!x || !x->data) return false; uint32_t T = x->ts;
  uint16_t e = seq; int guard = 0; while (!slot_at(s, e)->marker) { Slot *n = slot_at(s, (uint16_t)(e + 1)); if (!n || !n->data || n->ts != T || ++guard > SLOTS) return false; e++; }
  uint16_t b = seq; bool prev_known = false, prev_marker = false; guard = 0;
  for (;;) { Slot *p = slot_at(s, (uint16_t)(b - 1)); if (!p) break; if (p->ts != T) { prev_known = true; prev_marker = p->marker; break; } if (!p->data || ++guard > SLOTS) return false; b--; }
  bool contiguous = s->have_emit && (uint16_t)(b - 1) == s->last_emit;
  bool delimited = prev_known && prev_marker;
  if (!contiguous && !delimited && !s->have_emit && !prev_known && starts_with_sps(slot_at(s, b))) delimited = true;   /* very first frame: a keyframe starting with SPS is a safe start */
  if (!contiguous && !delimited) return false;                       /* start of this frame still unknown: wait */
  bool key = false; for (uint16_t q = b;; q++) { if (pkt_is_key(slot_at(s, q))) { key = true; break; } if (q == e) break; }
  if (contiguous && !s->need_key) { emit(s, b, e, key); return true; }
  if (key) { if (s->need_key) dlog("[vr] %s resync on keyframe (seq %u)", s->uid, b); emit(s, b, e, true); return true; }
  /* a later, complete frame while an earlier one is missing/incomplete */
  if (!s->need_key) { uint32_t now = SDL_GetTicks(); if (!s->blocked_t) s->blocked_t = now ? now : 1; else if (now - s->blocked_t > BLOCK_MS) { s->late++; request_key(s, "packet loss"); } }
  return false;
}
bool vr_rtp(int conn, uint32_t ssrc, int pt, uint16_t seq, uint32_t ts, bool marker, const uint8_t *pl, int n) {
  if (!started || conn < 0 || conn >= VR_NCONN) return pt == VR_PT_H264 || pt == VR_PT_H264_RTX;
  SDL_LockMutex(mx); bool rtx = false; VrStream *s = find_ssrc(conn, ssrc, &rtx);
  if (!s) { SDL_UnlockMutex(mx); bool vid = pt == VR_PT_H264 || pt == VR_PT_H264_RTX;
    if (vid) { unmapped_pkts++; bool seen = false; for (int i = 0; i < n_unmapped_logged; i++) if (unmapped_ssrc_logged[i] == ssrc) seen = true;
      if (!seen && n_unmapped_logged < 8) { unmapped_ssrc_logged[n_unmapped_logged++] = ssrc; dlog("[vr] conn %d video RTP from unmapped ssrc %u pt %d (dropped until op 12 maps it)", conn, (unsigned)ssrc, pt); } }
    return vid; }
  if (rtx || pt == VR_PT_H264_RTX) {                                 /* RFC 4588: OSN + original payload; empty = padding/probe */
    s->rtx_pkts++; if (n <= 2) { SDL_UnlockMutex(mx); return true; } seq = (uint16_t)(pl[0] << 8 | pl[1]); pl += 2; n -= 2;
    if (rtx) { for (int k = 0; k < s->nssrc; k++) if (s->rtx[k] == ssrc) ssrc = s->ssrc[k]; } }
  if (ssrc != s->cur_ssrc) { if (s->cur_ssrc) dlog("[vr] %s switched ssrc %u -> %u", s->uid, (unsigned)s->cur_ssrc, (unsigned)ssrc); s->cur_ssrc = ssrc; depack_reset(s); }
  s->pkts++;
  if (n > 0 && n <= SLOT_MAX && !(s->have_emit && sdiff(seq, s->last_emit) <= 0)) {
    Slot *x = &s->slots[seq % SLOTS];
    if (!(x->used && x->seq == seq && x->data)) {
      if (!x->buf) x->buf = malloc(SLOT_MAX);
      if (x->buf) { x->used = true; x->data = true; x->seq = seq; x->ts = ts; x->marker = marker; x->len = (uint16_t)n; memcpy(x->buf, pl, n);
        if (try_frame(s, seq)) { for (int g = 0; g < 64; g++) { uint16_t nx = (uint16_t)(s->last_emit + 1); Slot *y = slot_at(s, nx); if (!y || !y->data || !try_frame(s, nx)) break; } } }
    }
  } else if (n > SLOT_MAX) s->drops++;
  uint32_t now = SDL_GetTicks(); uint32_t pli_ssrc = 0; VrPliFn pf = c_pli[conn];
  if (s->need_key && s->pli_due && now - s->pli_t >= PLI_MS) { s->pli_t = now; s->pli_due = false; s->plis++; pli_ssrc = s->cur_ssrc; }
  else if (s->need_key && now - s->pli_t >= 2000) s->pli_due = true;   /* keep asking while no keyframe comes */
  char uid[24]; snprintf(uid, sizeof uid, "%s", s->uid); uint32_t np = s->plis;
  SDL_UnlockMutex(mx);
  if (pli_ssrc && pf) { if (np <= 5 || np % 20 == 0) dlog("[vr] %s request keyframe (PLI #%u, ssrc %u)", uid, (unsigned)np, (unsigned)pli_ssrc); pf(conn, pli_ssrc); }
  return true;
}

/* ---------------- H.264 helpers ---------------- */
typedef struct { const uint8_t *p; size_t n, bit; } Bits;   /* over RBSP (emulation bytes removed) */
static int rb(Bits *b) { if (b->bit >= b->n * 8) { b->bit++; return 0; } int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1; b->bit++; return v; }
static uint32_t rbits(Bits *b, int k) { uint32_t v = 0; while (k--) v = v << 1 | rb(b); return v; }
static uint32_t ue(Bits *b) { int z = 0; while (!rb(b) && z < 32) z++; if (z >= 32) return 0; return ((1u << z) - 1) + rbits(b, z); }
static int32_t se(Bits *b) { uint32_t k = ue(b); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
/* SPS NAL (with header) -> coded size, display (cropped) size, profile, level. false if unparsable */
static bool sps_parse(const uint8_t *nal, size_t n, int *cw, int *ch, int *w, int *h, int *prof, int *lvl) {
  static uint8_t r[256]; size_t rn = 0; for (size_t i = 1; i < n && rn < sizeof r; i++) { if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue; r[rn++] = nal[i]; }
  if (rn < 4) return false; Bits b = {r, rn, 0};
  int pidc = rbits(&b, 8); rbits(&b, 8); int lidc = rbits(&b, 8); ue(&b);
  int cfi = 1;
  if (pidc == 100 || pidc == 110 || pidc == 122 || pidc == 244 || pidc == 44 || pidc == 83 || pidc == 86 || pidc == 118 || pidc == 128 || pidc == 138 || pidc == 139 || pidc == 134 || pidc == 135) {
    cfi = ue(&b); if (cfi == 3) rb(&b); ue(&b); ue(&b); rb(&b);
    if (rb(&b)) { for (int i = 0; i < (cfi != 3 ? 8 : 12); i++) if (rb(&b)) { int sz = i < 6 ? 16 : 64, last = 8, next = 8; for (int j = 0; j < sz; j++) { if (next) { next = (last + se(&b) + 256) % 256; } last = next ? next : last; } } } }
  ue(&b); int poct = ue(&b);
  if (poct == 0) ue(&b); else if (poct == 1) { rb(&b); se(&b); se(&b); int nc = ue(&b); for (int i = 0; i < nc && i < 256; i++) se(&b); }
  ue(&b); rb(&b); int wm = ue(&b) + 1, hm = ue(&b) + 1; int fmo = rb(&b); if (!fmo) rb(&b); rb(&b);
  int cl = 0, cr = 0, ct = 0, cbt = 0; if (rb(&b)) { cl = ue(&b); cr = ue(&b); ct = ue(&b); cbt = ue(&b); }
  if (b.bit > rn * 8) return false;
  int cux = cfi == 0 || cfi == 3 ? 1 : 2, cuy = (cfi == 1 ? 2 : 1) * (2 - fmo);
  *cw = wm * 16; *ch = hm * 16 * (2 - fmo); *w = *cw - (cl + cr) * cux; *h = *ch - (ct + cbt) * cuy; *prof = pidc; *lvl = lidc;
  return *w > 0 && *h > 0 && *cw <= 4096 && *ch <= 4096;
}
/* iterate Annex-B NALs: returns pointer to NAL (after start code), sets *len; NULL at end */
static const uint8_t *next_nal(const uint8_t *d, size_t n, size_t *pos, size_t *len) {
  size_t i = *pos; while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) i++; if (i + 3 > n) return NULL; i += 3;
  size_t j = i; while (j + 3 <= n && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (d[j + 2] == 0 && j + 3 < n && d[j + 3] == 1)))) j++; if (j + 3 > n) j = n;
  *len = j - i; *pos = j; return d + i;
}

/* ---------------- decoder ---------------- */
static VrStream *volatile cur_dec;
static inline uint64_t now_us(void) { return (uint64_t)OSTicksToMicroseconds(OSGetSystemTime()); }
/* one picture (NV12) -> a free output slot, converted at display size; the producer never waits for the UI */
static bool put_picture(VrStream *s, const uint8_t *yp, const uint8_t *uvp, int pitch, int w, int h, double ts_sec) {
  bool foc = focus_conn == s->conn && !strcmp(focus_uid, s->uid); int maxw = foc ? FULL_W : TILE_W;
  int ow = w, oh = h; if (ow > maxw) { oh = (int)((int64_t)h * maxw / w); ow = maxw; } if (oh < 2) oh = 2;
  SDL_LockMutex(omx); int k = -1; for (int q = 0; q < OUTQ && k < 0; q++) if (s->out[q].state == 0) k = q;
  if (k < 0) { uint32_t best = 0; for (int q = 0; q < OUTQ; q++) if (s->out[q].state == 2 && (k < 0 || (int32_t)(s->out[q].seq - best) < 0)) { k = q; best = s->out[q].seq; } if (k >= 0) s->late_drops++; }
  if (k >= 0) s->out[k].state = 1; SDL_UnlockMutex(omx); if (k < 0) return false;
  size_t need = (size_t)ow * oh * 4; if (s->out[k].cap < need) { free(s->out[k].buf); s->out[k].buf = malloc(need); s->out[k].cap = s->out[k].buf ? need : 0; }
  if (!s->out[k].buf) { SDL_LockMutex(omx); s->out[k].state = 0; SDL_UnlockMutex(omx); return false; }
  uint64_t t0 = now_us();
  yuv_nv12_to_rgba(yp, uvp, pitch, w, h, s->out[k].buf, ow * 4, ow, oh, (size_t)ow * oh >= 300000 ? 2 : 1);
  s->t_conv += now_us() - t0; s->n_conv++;
  SDL_LockMutex(omx); s->out[k].w = ow; s->out[k].h = oh; s->out[k].pts = (uint32_t)(int64_t)(ts_sec * 1000.0); s->out[k].seq = ++s->out_seq; s->out[k].state = 2; s->ow = ow; s->oh = oh; s->frames_out++; SDL_UnlockMutex(omx);
  return true;
}
static void frame_cb(H264DecodeOutput *out) {
  if (!out) return; VrStream *s = NULL;
  for (int i = 0; i < VR_MAX_STREAMS; i++) if (out->userMemory == (void *)&S[i]) s = &S[i];
  if (!s && out->frameCount > 0 && out->decodeResults[0]) { const uint8_t *fbp = out->decodeResults[0]->framebuffer; for (int i = 0; i < VR_MAX_STREAMS && !s; i++) if (S[i].fb && fbp >= S[i].fb && fbp < S[i].fb + S[i].fbsz) s = &S[i]; }
  if (!s) s = cur_dec; if (!s) return; s->cbn++;
  for (int i = 0; i < out->frameCount; i++) { H264DecodeResult *r = out->decodeResults[i]; if (!r || !r->framebuffer || (r->status != 0 && r->status != 100)) { s->dec_fail++; continue; }
    int w = r->width, h = r->height, cl = 0, ct = 0; if (r->cropEnableFlag) { w -= r->cropLeft + r->cropRight; h -= r->cropTop + r->cropBottom; cl = r->cropLeft; ct = r->cropTop; }
    if (w <= 0 || h <= 0 || w > 1920 || h > 1088) continue;
    DCInvalidateRange(r->framebuffer, (uint32_t)r->nextLine * r->height * 3 / 2);   /* the decoder DMA-writes this: drop stale cache lines */
    if (!put_picture(s, (const uint8_t *)r->framebuffer + ct * r->nextLine + cl, (const uint8_t *)r->framebuffer + (size_t)r->nextLine * r->height + (ct / 2) * r->nextLine + (cl & ~1), r->nextLine, w, h, r->timestamp)) continue;
    int ow = s->ow, oh = s->oh;
    if (s->status[0] && !s->need_key) s->status[0] = 0;
    s->decoded++; if (s->decoded == 1 || s->decoded % 300 == 0) dlog("[vr] %s decoded frame #%u %dx%d (shown %dx%d, routed %s)", s->uid, (unsigned)s->decoded, w, h, ow, oh, out->userMemory == (void *)s ? "user ptr" : "fallback"); }
}
static int decoders_open(void) { int n = 0; for (int i = 0; i < VR_MAX_STREAMS; i++) if (S[i].dec_open) n++; return n; }
static void dec_close(VrStream *s) {
  if (s->dec_open) { H264DECFlush(s->dmem); H264DECEnd(s->dmem); H264DECClose(s->dmem); s->dec_open = false; }
  free(s->dmem); s->dmem = NULL; free(s->fb); s->fb = NULL; s->fbsz = 0;
}
static bool dec_open(VrStream *s, int cw, int ch, int prof, int lvl) {
  if (decoders_open() >= VR_MAX_DECODERS) { setstat(s, "too many videos at once"); return false; }
  int p = prof == 66 || prof == 77 || prof == 100 ? prof : 100; uint32_t need = 0;
  /* memory: the decoder sizes its picture buffers by level. Browsers/Discord often signal a high level (5.2 seen live),
   * which costs ~35 MB per decoder; use the lowest level whose max frame size fits this picture (WebRTC encoders use
   * 1-2 reference frames, well inside that level's DPB). */
  int mbs = (cw / 16) * (ch / 16); int l = mbs <= 1620 ? 30 : mbs <= 3600 ? 31 : mbs <= 5120 ? 32 : mbs <= 8192 ? 40 : 42; if (lvl > 0 && lvl < l) l = lvl < 30 ? 30 : lvl;
  H264Error e = H264DECMemoryRequirement(p, l, cw, ch, &need);
  if (e != H264_ERROR_OK || !need) { l = 41; e = H264DECMemoryRequirement(100, l, cw, ch, &need); p = 100; }
  dlog("[vr] %s decoder memreq prof=%d lvl=%d %dx%d -> err=%d need=%u (open: %d)", s->uid, p, l, cw, ch, (int)e, (unsigned)need, decoders_open());
  if (e != H264_ERROR_OK || !need) { setstat(s, "video format not supported"); return false; }
  s->dmem = memalign(256, need + 256); s->fbsz = (size_t)((cw + 255) & ~255) * ((ch + 63) & ~63) * 2 + 65536; s->fb = memalign(256, s->fbsz);
  if (!s->dmem || !s->fb) { dlog("[vr] out of memory for decoder"); free(s->dmem); free(s->fb); s->dmem = NULL; s->fb = NULL; setstat(s, "out of memory"); return false; }
  if (H264DECInitParam((int32_t)need, s->dmem) != H264_ERROR_OK) { dec_close(s); setstat(s, "decoder init failed"); return false; }
  H264DECSetParam_FPTR_OUTPUT(s->dmem, frame_cb); uint32_t one = 1; H264DECSetParam(s->dmem, H264_PARAMETER_OUTPUT_PER_FRAME, &one); void *um = s; H264DECSetParam_USER_MEMORY(s->dmem, &um);
  H264Error e2 = H264DECOpen(s->dmem), e3 = e2 == H264_ERROR_OK ? H264DECBegin(s->dmem) : e2;
  if (e2 != H264_ERROR_OK || e3 != H264_ERROR_OK) { dlog("[vr] decoder open err=%d begin err=%d", (int)e2, (int)e3); if (e2 == H264_ERROR_OK) H264DECClose(s->dmem); free(s->dmem); free(s->fb); s->dmem = NULL; s->fb = NULL; setstat(s, "decoder open failed"); return false; }
  s->dec_open = true; s->dw = cw; s->dh = ch; s->dlevel = l; s->dprof = p; s->fed = 0; s->decoded = 0; setstat(s, ""); return true;
}
static void teardown(VrStream *s) {
  dlog("[vr] %s stream closed: pkts %u (rtx %u) frames %u drops %u decrypt ok %u fail %u decoded %u errors %u PLIs %u", s->uid, (unsigned)s->pkts, (unsigned)s->rtx_pkts, (unsigned)s->frames, (unsigned)s->drops, (unsigned)s->dec_ok, (unsigned)s->dec_fail, (unsigned)s->decoded, (unsigned)s->late, (unsigned)s->plis);
  dec_close(s);
  SDL_LockMutex(mx); if (s->slots) { for (int i = 0; i < SLOTS; i++) free(s->slots[i].buf); free(s->slots); s->slots = NULL; } for (int i = 0; i < FQ; i++) { free(s->fq[i].d); s->fq[i].d = NULL; s->fq[i].cap = 0; }
  SDL_LockMutex(omx); for (int q = 0; q < OUTQ; q++) { free(s->out[q].buf); s->out[q].buf = NULL; s->out[q].cap = 0; s->out[q].state = 0; } SDL_UnlockMutex(omx);
  s->used = false; s->kill = false; SDL_UnlockMutex(mx);
}
static void decode_one(VrStream *s, Frame *f, uint8_t **dbuf, size_t *dcap, uint8_t **abuf, size_t *acap) {
  const uint8_t *d = f->d; size_t n = f->n;
  if (c_dave[s->conn] && c_dec[s->conn]) {                    /* E2EE: frame -> plaintext Annex-B (drop on failure, ask for a keyframe) */
    if (*dcap < n + 64) { *dcap = n + 65536; free(*dbuf); *dbuf = malloc(*dcap); if (!*dbuf) { *dcap = 0; return; } }
    size_t w = 0; int rc = c_dec[s->conn](s->conn, s->uid, d, n, *dbuf, *dcap, &w);
    if (rc != 0 || !w) { s->dec_fail++; if (s->dec_fail <= 5 || s->dec_fail % 100 == 0) dlog("[vr] %s DAVE decrypt failed (rc %d, %u bytes, #%u)", s->uid, rc, (unsigned)n, (unsigned)s->dec_fail);
      SDL_LockMutex(mx); request_key(s, "DAVE decrypt failed"); SDL_UnlockMutex(mx); setstat(s, "can't decrypt (E2EE)"); return; }
    s->dec_ok++; if (s->dec_ok == 1) dlog("[vr] %s first DAVE video frame decrypted (%u -> %u bytes)", s->uid, (unsigned)n, (unsigned)w); d = *dbuf; n = w; }
  /* scan NALs: cache SPS/PPS, find IDR */
  size_t pos = 0, len; const uint8_t *nal; bool has_sps = false, has_idr = false, has_vcl = false; const uint8_t *sps = NULL; size_t spsn = 0;
  while ((nal = next_nal(d, n, &pos, &len))) { int t = len ? nal[0] & 31 : 0;
    if (t == 7 && len <= sizeof s->sps) { memcpy(s->sps, nal, len); s->spsn = (int)len; has_sps = true; sps = nal; spsn = len; }
    else if (t == 8 && len <= sizeof s->pps) { memcpy(s->pps, nal, len); s->ppsn = (int)len; }
    else if (t == 5) { has_idr = true; has_vcl = true; } else if (t == 1) has_vcl = true; }
  if (!has_vcl) return;
  if (!sps && s->spsn) { sps = s->sps; spsn = s->spsn; }
  if (has_idr && sps) { int cw, ch, w, h, pr, lv; if (sps_parse(sps, spsn, &cw, &ch, &w, &h, &pr, &lv)) {
      if (s->dec_open && (cw != s->dw || ch != s->dh)) { dlog("[vr] %s resolution change %dx%d -> %dx%d: reopening decoder", s->uid, s->dw, s->dh, cw, ch); dec_close(s); }
      if (!s->dec_open && !s->dec_failed) {
        dlog("[vr] %s SPS: profile %d level %d coded %dx%d display %dx%d", s->uid, pr, lv, cw, ch, w, h);
        if (!dec_open(s, cw, ch, pr, lv)) s->dec_failed = true; } } }
  if (!s->dec_open) return;
  /* an IDR without parameter sets in this frame: put the cached ones in front */
  if (has_idr && !has_sps && s->spsn && s->ppsn) { size_t need = n + s->spsn + s->ppsn + 8; if (*acap < need) { *acap = need + 65536; free(*abuf); *abuf = malloc(*acap); if (!*abuf) { *acap = 0; return; } }
    size_t o = 0; memcpy(*abuf, SC, 4); o += 4; memcpy(*abuf + o, s->sps, s->spsn); o += s->spsn; memcpy(*abuf + o, SC, 4); o += 4; memcpy(*abuf + o, s->pps, s->ppsn); o += s->ppsn; memcpy(*abuf + o, d, n); o += n; d = *abuf; n = o; }
  { size_t p3 = 0, l3; const uint8_t *q3; while ((q3 = next_nal(d, n, &p3, &l3))) if (l3 >= 4 && (q3[0] & 31) == 7 && q3[3] > s->dlevel && (q3[1] || q3[2])) { if (!s->lvl_logged) { s->lvl_logged = true; dlog("[vr] %s SPS level %d -> %d (decoder memory is sized for %d)", s->uid, q3[3], s->dlevel, s->dlevel); } ((uint8_t *)q3)[3] = (uint8_t)s->dlevel; }
    if (s->spsn >= 4 && s->sps[3] > s->dlevel && (s->sps[1] || s->sps[2])) s->sps[3] = (uint8_t)s->dlevel; }   /* the cached copy too (prepended to IDRs) */
  uint64_t t0 = now_us(); uint64_t c0 = s->t_conv;
  cur_dec = s; H264Error e1 = H264DECSetBitstream(s->dmem, (uint8_t *)d, (uint32_t)n, f->ts / 90000.0); H264Error e2 = e1 == H264_ERROR_OK ? H264DECExecute(s->dmem, s->fb) : e1; cur_dec = NULL;
  s->t_exec += (now_us() - t0) - (s->t_conv - c0); s->n_exec++;   /* decode time without the conversion done in the callback */
  if (e1 != H264_ERROR_OK || (e2 & 0xFF000000)) { s->late++; if (s->late <= 5) dlog("[vr] %s decode error %d/%d (%u bytes)", s->uid, (int)e1, (int)e2, (unsigned)n); }
  else if ((int)e2 != s->last_rc) { s->last_rc = (int)e2; dlog("[vr] %s H264DECExecute status %d", s->uid, (int)e2); }   /* small positive values are status, not errors (228 seen in Cemu) */
  if (s->conn != VR_CONN_TEST && s->fed < 40) {   /* first frames after (re)opening: NAL types, sizes, pictures out */
    char ty[64]; int o = 0; size_t p2 = 0, l2; const uint8_t *q; while (o < 56 && (q = next_nal(d, n, &p2, &l2))) o += snprintf(ty + o, sizeof ty - o, "%d,", l2 ? q[0] & 31 : -1);
    dlog("[vr] %s feed #%u ts %u %u bytes nals %s rc %d decoded %u callbacks %u", s->uid, (unsigned)s->fed, (unsigned)f->ts, (unsigned)n, ty, (int)e2, (unsigned)s->decoded, (unsigned)s->cbn); }
  s->fed++; if (s->decoded == 0 && s->fed == 4) { dlog("[vr] %s no picture after 4 frames: asking for a new keyframe", s->uid); SDL_LockMutex(mx); request_key(s, "decoder produced no picture"); SDL_UnlockMutex(mx); }
}
int vr_decode_thread(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0); OSSetThreadPriority(OSGetCurrentThread(), 20);
  uint8_t *dbuf = NULL, *abuf = NULL; size_t dcap = 0, acap = 0; Frame work = {0};
  for (;;) {
    SDL_SemWaitTimeout(sem, 100);
    for (int i = 0; i < VR_MAX_STREAMS; i++) { VrStream *s = &S[i];
      SDL_LockMutex(mx); bool used = s->used, kill = s->kill; bool got = false;
      if (used && !kill && s->fqn > 0) { Frame t = s->fq[0]; memmove(&s->fq[0], &s->fq[1], (FQ - 1) * sizeof(Frame)); s->fqn--; s->fq[FQ - 1] = work; work = t; got = true; }   /* swap buffers, no copy */
      SDL_UnlockMutex(mx);
      if (used && kill) { teardown(s); continue; }
      if (got) { decode_one(s, &work, &dbuf, &dcap, &abuf, &acap); i--; }   /* drain this stream before moving on */
    }
  }
  return 0;
}

/* ---------------- UI side ---------------- */
/* one-off renderer measurements for the self test (main thread): does the Wii U SDL renderer take NV12 natively? */
static volatile bool ui_bench_req; void vr_request_ui_bench(void) { ui_bench_req = true; }
static void ui_bench(SDL_Renderer *ren) {
  SDL_RendererInfo ri; char fm[300]; int o = 0; fm[0] = 0;
  if (SDL_GetRendererInfo(ren, &ri) == 0) for (Uint32 k = 0; k < ri.num_texture_formats && o < 260; k++) o += snprintf(fm + o, sizeof fm - o, "%s ", SDL_GetPixelFormatName(ri.texture_formats[k]));
  const int W = 1280, H = 720; uint8_t *buf = malloc(W * H * 4); if (!buf) return; memset(buf, 100, W * H * 4);
  double nv = -1, rgba = -1; uint64_t t;
  SDL_Texture *tn = SDL_CreateTexture(ren, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, W, H);
  if (tn) { t = now_us(); for (int k = 0; k < 5; k++) SDL_UpdateNVTexture(tn, NULL, buf, W, buf + W * H, W); nv = (now_us() - t) / 5000.0; SDL_DestroyTexture(tn); }
  SDL_Texture *tr = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, W, H);
  if (tr) { t = now_us(); for (int k = 0; k < 5; k++) SDL_UpdateTexture(tr, NULL, buf, W * 4); rgba = (now_us() - t) / 5000.0; SDL_DestroyTexture(tr); }
  dlog("[vr] renderer '%s' native formats: %s| 1280x720 upload: NV12 texture %s %.2f ms, RGBA %.2f ms", ri.name ? ri.name : "?", fm, tn ? "ok" : "unsupported", nv, rgba);
  free(buf);
}
void vr_tick(void) {
  if (!started) return; SDL_Renderer *ren = nx_renderer(); uint32_t now = SDL_GetTicks();
  if (ui_bench_req) { ui_bench_req = false; ui_bench(ren); }
  for (int i = 0; i < VR_MAX_STREAMS; i++) { VrStream *s = &S[i]; bool live = s->used && !s->kill;
    if (tex[i] && (!live || tex_gen[i] != s->gen)) { SDL_DestroyTexture(tex[i]); tex[i] = NULL; }
    if (!live) continue;
    /* pacing: show the newest frame that is due by its timestamp; re-anchor the clock when it drifts too far */
    SDL_LockMutex(omx); int pick = -1, newest = -1;
    for (int q = 0; q < OUTQ; q++) if (s->out[q].state == 2) { if (newest < 0 || (int32_t)(s->out[q].seq - s->out[newest].seq) > 0) newest = q; }
    if (newest >= 0) {
      if (!s->clk_ok) { s->clk_off = (int32_t)(now - s->out[newest].pts); s->clk_ok = true; }
      for (int q = 0; q < OUTQ; q++) if (s->out[q].state == 2 && (int32_t)(s->out[q].pts + s->clk_off - now) <= 4 && (pick < 0 || (int32_t)(s->out[q].seq - s->out[pick].seq) > 0)) pick = q;
      int32_t lead = (int32_t)(s->out[newest].pts + s->clk_off - now);
      if (pick < 0 && lead > BEHIND_MS) { s->clk_off = (int32_t)(now - s->out[newest].pts); pick = newest; }        /* timestamps jumped ahead */
      if (pick >= 0) { int32_t lag = (int32_t)(now - (s->out[pick].pts + s->clk_off)); if (lag > BEHIND_MS) s->clk_off += lag - 20; }   /* running late: catch up */
      if (pick >= 0) { for (int q = 0; q < OUTQ; q++) if (q != pick && s->out[q].state == 2 && (int32_t)(s->out[q].seq - s->out[pick].seq) < 0) { s->out[q].state = 0; s->late_drops++; }
        s->out[pick].state = 3; } }
    SDL_UnlockMutex(omx);
    if (pick >= 0) {                                   /* upload outside the lock: the decoder keeps going */
      int w = s->out[pick].w, h = s->out[pick].h;
      if (!tex[i] || tex_w[i] != w || tex_h[i] != h) { if (tex[i]) SDL_DestroyTexture(tex[i]); tex[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h); tex_w[i] = w; tex_h[i] = h; tex_gen[i] = s->gen; }
      if (tex[i]) SDL_UpdateTexture(tex[i], NULL, s->out[pick].buf, w * 4); s->presented++;
      SDL_LockMutex(omx); s->out[pick].state = 0; SDL_UnlockMutex(omx); }
  }
  { static uint32_t st; static uint32_t pd[VR_MAX_STREAMS], pp[VR_MAX_STREAMS], pe[VR_MAX_STREAMS], pc[VR_MAX_STREAMS]; static uint64_t pte[VR_MAX_STREAMS], ptc[VR_MAX_STREAMS];
    if (now - st > 10000) { float secs = st ? (now - st) / 1000.0f : 10.0f; st = now;
      for (int i = 0; i < VR_MAX_STREAMS; i++) { VrStream *s = &S[i]; if (!s->used || s->kill || !s->pkts) continue;
        uint32_t de = s->n_exec - pe[i], dc = s->n_conv - pc[i];
        dlog("[vr] stats %s conn %d: pkts %u rtx %u frames %u drops %u decrypt %u/%u fail decoded %u errors %u PLIs %u%s | %.1f fps decoded, %.1f fps shown, decode %.2f ms, convert %.2f ms (%dx%d), late drops %u",
             s->uid, s->conn, (unsigned)s->pkts, (unsigned)s->rtx_pkts, (unsigned)s->frames, (unsigned)s->drops, (unsigned)s->dec_ok, (unsigned)s->dec_fail, (unsigned)s->decoded, (unsigned)s->late, (unsigned)s->plis, s->need_key ? " (need key)" : "",
             (s->n_conv - pc[i]) / secs, (s->presented - pp[i]) / secs, de ? (s->t_exec - pte[i]) / 1000.0 / de : 0.0, dc ? (s->t_conv - ptc[i]) / 1000.0 / dc : 0.0, s->ow, s->oh, (unsigned)s->late_drops);
        pd[i] = s->decoded; pp[i] = s->presented; pe[i] = s->n_exec; pc[i] = s->n_conv; pte[i] = s->t_exec; ptc[i] = s->t_conv; }
      if (unmapped_pkts) dlog("[vr] unmapped video packets so far: %u", (unsigned)unmapped_pkts); } }
}
static int idx_user(int conn, const char *uid) { for (int i = 0; i < VR_MAX_STREAMS; i++) if (S[i].used && !S[i].kill && S[i].conn == conn && uid && !strcmp(S[i].uid, uid)) return i; return -1; }
bool vr_active(int conn, const char *uid) { return started && idx_user(conn, uid) >= 0; }
SDL_Texture *vr_texture(int conn, const char *uid, int *w, int *h) { if (!started) return NULL; int i = idx_user(conn, uid); if (i < 0 || !tex[i] || tex_gen[i] != S[i].gen) return NULL; if (w) *w = tex_w[i]; if (h) *h = tex_h[i]; return tex[i]; }
const char *vr_status(int conn, const char *uid) { if (!started) return ""; int i = idx_user(conn, uid); return i < 0 ? "" : S[i].status; }
void vr_set_focus(int conn, const char *uid) { if (uid && uid[0]) { snprintf(focus_uid, sizeof focus_uid, "%s", uid); focus_conn = conn; } else { focus_uid[0] = 0; focus_conn = -1; } }
bool vr_stats(int conn, const char *uid, VrStats *o) {
  memset(o, 0, sizeof *o); if (!started) return false; SDL_LockMutex(mx); int i = idx_user(conn, uid); if (i >= 0) { VrStream *s = &S[i];
    o->pkts = s->pkts; o->rtx_pkts = s->rtx_pkts; o->frames = s->frames; o->drops = s->drops; o->decrypt_ok = s->dec_ok; o->decrypt_fail = s->dec_fail; o->decoded = s->decoded; o->errors = s->late; o->plis = s->plis; o->need_key = s->need_key; o->w = s->ow; o->h = s->oh;
    o->exec_us = s->t_exec; o->conv_us = s->t_conv; o->n_exec = s->n_exec; o->n_conv = s->n_conv; o->presented = s->presented; o->late_drops = s->late_drops; }
  SDL_UnlockMutex(mx); return i >= 0;
}
int vr_h264_packetize(const uint8_t *d, size_t n, int mtu, void (*cb)(void *, const uint8_t *, int, bool), void *u) {
  const uint8_t *nals[256]; size_t lens[256]; int k = 0; size_t pos = 0, len; const uint8_t *nal;
  while (k < 256 && (nal = next_nal(d, n, &pos, &len))) if (len) { nals[k] = nal; lens[k] = len; k++; }
  uint8_t pkt[1500]; if (mtu > (int)sizeof pkt) mtu = sizeof pkt; if (mtu < 64) mtu = 64; int np = 0;
  for (int i = 0; i < k;) {
    if ((int)lens[i] > mtu) {                                   /* FU-A */
      const uint8_t *p = nals[i] + 1; size_t rem = lens[i] - 1; bool first = true;
      while (rem) { size_t c = rem > (size_t)(mtu - 2) ? (size_t)(mtu - 2) : rem; pkt[0] = (nals[i][0] & 0xE0) | 28; pkt[1] = (nals[i][0] & 0x1F) | (first ? 0x80 : 0) | (c == rem ? 0x40 : 0);
        memcpy(pkt + 2, p, c); p += c; rem -= c; first = false; cb(u, pkt, (int)c + 2, i == k - 1 && !rem); np++; }
      i++; continue; }
    int j = i, tot = 1; while (j < k && (int)lens[j] <= mtu && tot + 2 + (int)lens[j] <= mtu) { tot += 2 + (int)lens[j]; j++; }
    if (j - i >= 2) {                                           /* STAP-A */
      int o = 1; uint8_t nri = 0; for (int q = i; q < j; q++) { if ((nals[q][0] & 0x60) > nri) nri = nals[q][0] & 0x60; pkt[o++] = (uint8_t)(lens[q] >> 8); pkt[o++] = (uint8_t)lens[q]; memcpy(pkt + o, nals[q], lens[q]); o += (int)lens[q]; }
      pkt[0] = nri | 24; cb(u, pkt, o, j == k); np++; i = j; }
    else { memcpy(pkt, nals[i], lens[i]); cb(u, pkt, (int)lens[i], i == k - 1); np++; i++; }
  }
  return np;
}
int vr_sink_wants(int conn, char *b, int cap) {   /* op 15 "d": tiles get a low layer, the focused video the top one */
  int any = conn == VR_CONN_STREAM ? 100 : 50; int o = snprintf(b, cap, "{\"any\":%d", any); char px[200]; int po = 0;
  if (started && focus_conn == conn && focus_uid[0]) { SDL_LockMutex(mx); VrStream *s = find_user(conn, focus_uid);
    if (s) for (int k = 0; k < s->nssrc && o < cap - 40; k++) { o += snprintf(b + o, cap - o, ",\"%u\":100", (unsigned)s->ssrc[k]); po += snprintf(px + po, sizeof px - po, "%s\"%u\":921600", po ? "," : "", (unsigned)s->ssrc[k]); }
    SDL_UnlockMutex(mx); }
  if (po && o < cap - po - 20) o += snprintf(b + o, cap - o, ",\"pixelCounts\":{%s}", px);
  if (o < cap - 1) { b[o++] = '}'; b[o] = 0; } return o;
}
int vr_sink_quality(int conn) { return focus_conn == conn ? 100 : (conn == VR_CONN_STREAM ? 100 : 50); }
