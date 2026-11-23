/* venc - software H.264 encoder (x264) for the GamePad camera. API and behaviour: venc.h.
 * x264 is GPL v2+: an .rpx linking this file is a GPL'd work (third_party/x264/BUILD-NOTES.md). */
#include "venc.h"
#include "dlog.h"
#include <x264.h>
#include <SDL2/SDL.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>

#define OUTQ 4          /* output ring for venc_poll: 1 held by the caller + up to 3 queued */
#define RATEWIN 32      /* frames remembered for the kbps estimate */

typedef struct { uint8_t *buf; size_t cap; VencOut o; } OutSlot;

struct Venc {
  int w, h, fps, kbps;
  x264_t *x; x264_param_t par;
  uint32_t frame_no; int64_t pts;
  OSTime t0;
  uint8_t *au; size_t aucap;                 /* assembled access unit (encoder-private) */
  uint8_t *nv12;                             /* scratch for venc_encode_rgba */
  /* async */
  SDL_Thread *th; SDL_mutex *mx; SDL_cond *cv; volatile bool quit;
  int core, prio; int vbv_ms;
  uint8_t *in[2]; uint32_t in_ts[2]; int pending, enc_slot;
  VencCb cb; void *cbu;
  OutSlot oq[OUTQ]; int oq_head, oq_n, oq_held;
  /* control (any thread) */
  volatile int want_idr; volatile int new_kbps;
  /* stats */
  VencStats st; double ms_sum;
  struct { uint32_t t90; uint32_t bytes; } rw[RATEWIN]; int rw_i, rw_n;
};

static void xlog(void *u, int lvl, const char *fmt, va_list ap) {
  (void)u; char b[300]; vsnprintf(b, sizeof b, fmt, ap); size_t n = strlen(b); while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
  dlog("[venc] x264 %s: %s", lvl == X264_LOG_ERROR ? "error" : lvl == X264_LOG_WARNING ? "warning" : "info", b);
}

uint32_t venc_now90k(const Venc *v) { return (uint32_t)(OSTicksToMicroseconds(OSGetSystemTime() - v->t0) * 9 / 100); }
void venc_size(const Venc *v, int *w, int *h) { if (w) *w = v->w; if (h) *h = v->h; }

Venc *venc_open(int w, int h, int fps, int kbps) {
  VencOpts o = { w, h, fps, kbps, VENC_SLICE_MAX, VENC_IDR_SECONDS, 1000 }; return venc_open_ex(&o);
}

Venc *venc_open_ex(const VencOpts *op) {
  int w = op->w, h = op->h, fps = op->fps, kbps = op->kbps;
  int slice = op->slice_max > 0 ? op->slice_max : 0, idrs = op->idr_secs > 0 ? op->idr_secs : VENC_IDR_SECONDS, vbv = op->vbv_ms > 0 ? op->vbv_ms : 1000;
  if (w < 16 || h < 16 || (w & 1) || (h & 1) || w > 1920 || h > 1088 || fps < 1 || fps > 60 || kbps < 20) { dlog("[venc] bad args %dx%d %d fps %d kbps", w, h, fps, kbps); return NULL; }
  Venc *v = calloc(1, sizeof *v); if (!v) return NULL;
  v->w = w; v->h = h; v->fps = fps; v->kbps = kbps; v->pending = v->enc_slot = -1; v->oq_held = -1; v->t0 = OSGetSystemTime();
  x264_param_t *p = &v->par;
  if (x264_param_default_preset(p, "ultrafast", "zerolatency") < 0) { dlog("[venc] preset failed"); free(v); return NULL; }
  p->cpu = 0;                                    /* C only (built --disable-asm) */
  p->i_threads = 1; p->b_sliced_threads = 0; p->i_lookahead_threads = 1; p->i_sync_lookahead = 0;
  p->i_width = w; p->i_height = h; p->i_csp = X264_CSP_NV12;
  p->i_fps_num = fps; p->i_fps_den = 1; p->i_timebase_num = 1; p->i_timebase_den = fps; p->b_vfr_input = 0;
  p->i_level_idc = 31;                           /* 3.1 = profile-level-id ..1f */
  p->i_keyint_max = fps * idrs; p->i_keyint_min = p->i_keyint_max; p->i_scenecut_threshold = 0; p->b_intra_refresh = 0;
  p->i_bframe = 0; p->i_frame_reference = 1;
  p->b_repeat_headers = 1; p->b_annexb = 1; p->b_aud = 0;
  p->i_slice_max_size = slice;
  p->rc.i_rc_method = X264_RC_ABR; p->rc.i_bitrate = kbps; p->rc.i_vbv_max_bitrate = kbps; p->rc.i_vbv_buffer_size = kbps * vbv / 1000; /* default 1 s VBV: room for IDRs */ v->vbv_ms = vbv;
  p->rc.f_vbv_buffer_init = 0.9f; p->rc.b_mb_tree = 0; p->rc.i_lookahead = 0;
  p->i_nal_hrd = X264_NAL_HRD_NONE;
  p->i_log_level = X264_LOG_WARNING; p->pf_log = xlog; p->p_log_private = v;
  if (x264_param_apply_profile(p, "baseline") < 0) { dlog("[venc] baseline profile rejected"); free(v); return NULL; }
  v->x = x264_encoder_open(p);
  if (!v->x) { dlog("[venc] x264_encoder_open failed (%dx%d)", w, h); free(v); return NULL; }
  dlog("[venc] open %dx%d %d fps %d kbps: x264 core %d, ultrafast/zerolatency baseline L3.1, keyint %d, slice-max %d", w, h, fps, kbps, X264_BUILD, p->i_keyint_max, slice);
  return v;
}

/* x264 NALs -> one Annex-B AU with 4-byte start codes everywhere (x264 uses 3-byte ones for later slices);
 * drops x264's informational SEI (version/options string, ~600 bytes on the first IDR). */
static int assemble(Venc *v, x264_nal_t *nal, int nn, int *maxnal) {
  size_t need = 0; for (int i = 0; i < nn; i++) need += (size_t)nal[i].i_payload + 1;
  if (need > v->aucap) { free(v->au); v->aucap = need + 16384; v->au = malloc(v->aucap); if (!v->au) { v->aucap = 0; return -1; } }
  size_t o = 0; int cnt = 0; *maxnal = 0;
  for (int i = 0; i < nn; i++) {
    const uint8_t *d = nal[i].p_payload; int n = nal[i].i_payload;
    int sc = (n >= 4 && !d[0] && !d[1] && !d[2] && d[3] == 1) ? 4 : (n >= 3 && !d[0] && !d[1] && d[2] == 1) ? 3 : 0;
    d += sc; n -= sc; if (n <= 0) continue;
    if ((d[0] & 31) == 6) continue;                                   /* SEI */
    v->au[o++] = 0; v->au[o++] = 0; v->au[o++] = 0; v->au[o++] = 1; memcpy(v->au + o, d, n); o += n; cnt++;
    if (n > *maxnal) *maxnal = n;
  }
  (void)cnt; return (int)o;
}

static void note_rate(Venc *v, uint32_t t90, uint32_t bytes) {
  v->rw[v->rw_i].t90 = t90; v->rw[v->rw_i].bytes = bytes; v->rw_i = (v->rw_i + 1) % RATEWIN; if (v->rw_n < RATEWIN) v->rw_n++;
  /* sum frames inside the last 2 s */
  uint64_t sum = 0; uint32_t oldest = t90; int k = 0;
  for (int i = 0; i < v->rw_n; i++) { int j = (v->rw_i - 1 - i + RATEWIN) % RATEWIN; if ((uint32_t)(t90 - v->rw[j].t90) > 180000) break; sum += v->rw[j].bytes; oldest = v->rw[j].t90; k++; }
  double span = k > 1 ? (double)(uint32_t)(t90 - oldest) / 90000.0 * k / (k - 1) : 1.0 / v->fps;
  v->st.kbps = span > 0 ? (float)(sum * 8 / span / 1000.0) : 0;
}

/* encode one NV12 frame; fills out (data -> v->au) */
static int enc_frame(Venc *v, const uint8_t *y, const uint8_t *uv, int yp, int uvp, bool force, uint32_t ts, VencOut *out) {
  int nk = v->new_kbps;
  if (nk > 0) { v->new_kbps = 0; x264_param_t p = v->par; p.rc.i_bitrate = nk; p.rc.i_vbv_max_bitrate = nk; p.rc.i_vbv_buffer_size = nk * v->vbv_ms / 1000;
    int r = x264_encoder_reconfig(v->x, &p); if (r == 0) { v->par = p; v->kbps = nk; } dlog("[venc] bitrate -> %d kbps (%s)", nk, r == 0 ? "ok" : "rejected"); }
  if (v->want_idr) { v->want_idr = 0; force = true; v->st.forced_idr++; }
  x264_picture_t pic, po; x264_picture_init(&pic);
  pic.img.i_csp = X264_CSP_NV12; pic.img.i_plane = 2;
  pic.img.plane[0] = (uint8_t *)y; pic.img.i_stride[0] = yp; pic.img.plane[1] = (uint8_t *)uv; pic.img.i_stride[1] = uvp;
  pic.i_pts = v->pts++; pic.i_type = force ? X264_TYPE_IDR : X264_TYPE_AUTO;
  x264_nal_t *nal = NULL; int nn = 0;
  OSTime t = OSGetSystemTime();
  int sz = x264_encoder_encode(v->x, &nal, &nn, &pic, &po);
  float ms = (float)OSTicksToMicroseconds(OSGetSystemTime() - t) / 1000.0f;
  if (sz < 0) { dlog("[venc] encode error %d", sz); return -1; }
  if (sz == 0 || nn == 0) return 0;
  int maxnal = 0; int n = assemble(v, nal, nn, &maxnal); if (n <= 0) return n;
  int cnt = 0; for (int i = 0; i < n - 4;) { if (!v->au[i] && !v->au[i + 1] && !v->au[i + 2] && v->au[i + 3] == 1) { cnt++; i += 4; } else i++; }
  memset(out, 0, sizeof *out);
  out->data = v->au; out->size = (size_t)n; out->ts90k = ts; out->keyframe = po.b_keyframe || po.i_type == X264_TYPE_IDR; out->nals = cnt; out->max_nal = maxnal;
  out->frame_no = v->frame_no++; out->enc_ms = ms;
  v->st.encoded++; v->st.bytes += (uint64_t)n; if (out->keyframe) v->st.idr++;
  v->st.last_ms = ms; v->ms_sum += ms; v->st.avg_ms = (float)(v->ms_sum / v->st.encoded); if (ms > v->st.max_ms) v->st.max_ms = ms;
  note_rate(v, ts, (uint32_t)n);
  return n;
}

int venc_encode(Venc *v, const uint8_t *y, const uint8_t *uv, int y_pitch, int uv_pitch, bool force_idr, VencOut *out) {
  if (!v || v->th || !y || !uv || !out) return -1;
  v->st.submitted++;
  return enc_frame(v, y, uv, y_pitch, uv_pitch, force_idr, venc_now90k(v), out);
}

static inline uint8_t clampu8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
void venc_rgba_to_nv12(const uint8_t *rgba, int pitch, int w, int h, uint8_t *y, int yp, uint8_t *uv, int uvp) {
  for (int j = 0; j < h; j += 2) {
    const uint8_t *r0 = rgba + (size_t)j * pitch, *r1 = r0 + pitch; uint8_t *y0 = y + (size_t)j * yp, *y1 = y0 + yp, *c = uv + (size_t)(j / 2) * uvp;
    for (int i = 0; i < w; i += 2) {
      int R = 0, G = 0, B = 0;
      #define PX(p, o) { int r_ = (p)[0], g_ = (p)[1], b_ = (p)[2]; o = (uint8_t)((66 * r_ + 129 * g_ + 25 * b_ + 128 + 4096) >> 8); R += r_; G += g_; B += b_; }
      PX(r0 + i * 4, y0[i]); PX(r0 + i * 4 + 4, y0[i + 1]); PX(r1 + i * 4, y1[i]); PX(r1 + i * 4 + 4, y1[i + 1]);
      #undef PX
      R >>= 2; G >>= 2; B >>= 2;
      c[i] = clampu8((-38 * R - 74 * G + 112 * B + 128 + 32768) >> 8); c[i + 1] = clampu8((112 * R - 94 * G - 18 * B + 128 + 32768) >> 8);
    }
  }
}

int venc_encode_rgba(Venc *v, const uint8_t *rgba, int pitch, bool force_idr, VencOut *out) {
  if (!v || !rgba) return -1;
  if (!v->nv12) { v->nv12 = memalign(64, (size_t)v->w * v->h * 3 / 2); if (!v->nv12) return -1; }
  venc_rgba_to_nv12(rgba, pitch, v->w, v->h, v->nv12, v->w, v->nv12 + (size_t)v->w * v->h, v->w);
  return venc_encode(v, v->nv12, v->nv12 + (size_t)v->w * v->h, v->w, v->w, force_idr, out);
}

/* ---------------- async ---------------- */
static void push_out(Venc *v, const VencOut *o) {
  if (v->cb) { v->cb(v->cbu, o); return; }
  SDL_LockMutex(v->mx);
  int lim = v->oq_held >= 0 ? OUTQ - 1 : OUTQ;
  if (v->oq_n >= lim) { v->oq_head = (v->oq_head + 1) % OUTQ; v->oq_n--; }            /* consumer too slow: lose the oldest */
  int s = (v->oq_head + v->oq_n) % OUTQ; OutSlot *sl = &v->oq[s];
  if (sl->cap < o->size) { free(sl->buf); sl->cap = o->size + 8192; sl->buf = malloc(sl->cap); if (!sl->buf) { sl->cap = 0; SDL_UnlockMutex(v->mx); return; } }
  memcpy(sl->buf, o->data, o->size); sl->o = *o; sl->o.data = sl->buf; v->oq_n++;
  SDL_UnlockMutex(v->mx);
}

static int enc_thread(void *arg) {
  Venc *v = arg;
  OSSetThreadAffinity(OSGetCurrentThread(), (uint32_t)(1u << v->core)); OSSetThreadPriority(OSGetCurrentThread(), v->prio);
  dlog("[venc] encoder thread on core %d prio %d", v->core, v->prio);
  size_t fsz = (size_t)v->w * v->h;
  for (;;) {
    SDL_LockMutex(v->mx);
    while (v->pending < 0 && !v->quit) SDL_CondWait(v->cv, v->mx);
    if (v->quit) { SDL_UnlockMutex(v->mx); break; }
    int s = v->pending; v->pending = -1; v->enc_slot = s; uint32_t ts = v->in_ts[s];
    SDL_UnlockMutex(v->mx);
    VencOut o; int n = enc_frame(v, v->in[s], v->in[s] + fsz, v->w, v->w, false, ts, &o);
    SDL_LockMutex(v->mx); v->enc_slot = -1; SDL_UnlockMutex(v->mx);
    if (n > 0) push_out(v, &o);
  }
  return 0;
}

void venc_set_callback(Venc *v, VencCb cb, void *user) { if (v) { v->cb = cb; v->cbu = user; } }

bool venc_start(Venc *v, int core, int prio) {
  if (!v || v->th) return false;
  size_t fsz = (size_t)v->w * v->h * 3 / 2;
  for (int i = 0; i < 2; i++) if (!v->in[i] && !(v->in[i] = memalign(64, fsz))) return false;
  v->mx = SDL_CreateMutex(); v->cv = SDL_CreateCond(); if (!v->mx || !v->cv) return false;
  v->core = core < 0 || core > 2 ? 0 : core; v->prio = prio < 0 || prio > 31 ? 20 : prio; v->quit = false;
  v->th = SDL_CreateThreadWithStackSize(enc_thread, "venc", 512 * 1024, v);   /* x264 analysis uses sizeable stack arrays */
  return v->th != NULL;
}

static void copy_plane(uint8_t *d, int dp, const uint8_t *s, int sp, int w, int h) {
  if (dp == sp && dp == w) { memcpy(d, s, (size_t)w * h); return; }
  for (int j = 0; j < h; j++) memcpy(d + (size_t)j * dp, s + (size_t)j * sp, w);
}

bool venc_submit(Venc *v, const uint8_t *y, const uint8_t *uv, int y_pitch, int uv_pitch, uint32_t ts90k) {
  if (!v || !v->th || !y || !uv) return false;
  if (ts90k == VENC_TS_NOW) ts90k = venc_now90k(v);
  bool replaced = false;
  SDL_LockMutex(v->mx);
  int t = v->enc_slot >= 0 ? 1 - v->enc_slot : (v->pending >= 0 ? v->pending : 0);
  if (v->pending == t) { v->pending = -1; replaced = true; v->st.dropped++; }          /* encoder still busy: replace the waiting frame */
  v->st.submitted++;
  SDL_UnlockMutex(v->mx);
  size_t fsz = (size_t)v->w * v->h;
  copy_plane(v->in[t], v->w, y, y_pitch, v->w, v->h); copy_plane(v->in[t] + fsz, v->w, uv, uv_pitch, v->w, v->h / 2);
  SDL_LockMutex(v->mx); v->in_ts[t] = ts90k; v->pending = t; SDL_CondSignal(v->cv); SDL_UnlockMutex(v->mx);
  return !replaced;
}

bool venc_poll(Venc *v, VencOut *out) {
  if (!v || !v->mx || !out) return false;
  SDL_LockMutex(v->mx);
  v->oq_held = -1;
  bool got = v->oq_n > 0;
  if (got) { int s = v->oq_head; *out = v->oq[s].o; v->oq_held = s; v->oq_head = (s + 1) % OUTQ; v->oq_n--; }
  SDL_UnlockMutex(v->mx);
  return got;
}

bool venc_busy(Venc *v) {
  if (!v || !v->mx) return false;
  SDL_LockMutex(v->mx); bool b = v->pending >= 0 || v->enc_slot >= 0; SDL_UnlockMutex(v->mx); return b;
}

void venc_request_idr(Venc *v) { if (v) v->want_idr = 1; }
void venc_set_bitrate(Venc *v, int kbps) { if (v && kbps >= 20) v->new_kbps = kbps; }
void venc_stats(Venc *v, VencStats *st) {
  if (!v || !st) return;
  if (v->mx) SDL_LockMutex(v->mx);
  *st = v->st;
  if (v->mx) SDL_UnlockMutex(v->mx);
}

void venc_close(Venc *v) {
  if (!v) return;
  if (v->th) { SDL_LockMutex(v->mx); v->quit = true; SDL_CondSignal(v->cv); SDL_UnlockMutex(v->mx); SDL_WaitThread(v->th, NULL); v->th = NULL; }
  if (v->x) x264_encoder_close(v->x);
  dlog("[venc] close: %u frames, %u IDR (%u forced), %u dropped, avg %.1f ms max %.1f ms", (unsigned)v->st.encoded, (unsigned)v->st.idr, (unsigned)v->st.forced_idr, (unsigned)v->st.dropped, v->st.avg_ms, v->st.max_ms);
  for (int i = 0; i < OUTQ; i++) free(v->oq[i].buf);
  free(v->in[0]); free(v->in[1]); free(v->au); free(v->nv12);
  if (v->cv) SDL_DestroyCond(v->cv);
  if (v->mx) SDL_DestroyMutex(v->mx);
  free(v);
}
