/* video.c - media playback for Discord: For the Nintendo Wii U (copied from youtube/src/video.c, DASH removed, js.h logging replaced).
 * Progressive download, MP4 demux (H.264 via the Wii U hardware decoder), WebM/Opus and MP3 audio, AAC via libavcodec,
 * audio-clock synchronised presentation. Decoding runs on its own thread; vid_tick() on the UI thread only uploads frames.
 * Additions over the original: several players at once (per-instance decoder, callback routing, decoder calls
 * serialised by one mutex), VID_F_LOOP (seamless restart from bytes kept in memory), VID_F_MUTE, per-player download cap,
 * the download job is freed once its bytes are copied (no second copy kept), exact-size buffer when Content-Length is known. */
#include <coreinit/cache.h>
#include "video.h"
#include "yuv.h"
#include "nxnet.h"
#include "nx.h"
#include <h264/decode.h>
#include <coreinit/thread.h>
#include <opus/opus.h>
#include <mpg123.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <math.h>
#include <strings.h>

#define MAX_DL (96u << 20)
#define FQ 3

typedef struct { uint64_t off; uint32_t size; double pts; bool key; } Sample;
typedef struct { uint8_t *rgba; int w, h; double pts; bool full; } Frame;

struct Vid {
  char vurl[2200], aurl[2200]; int flags; size_t maxdl;
  NxJob *vjob, *ajob; uint8_t *vbuf; size_t vlen, vcap, vtotal; bool vdone; uint8_t *abuf; size_t alen, acap; bool adone;
  volatile int state; char err[200];
  /* video track */
  bool has_video, moov_ok, aac_in_mp4; int vw, vh, profile, level, nal_size; uint8_t *hdr; size_t hdrlen;   /* hdr = annex-b SPS+PPS */
  Sample *vs; int nvs; volatile int vpos; double vdur, vperiod;
  /* decoder */
  void *dmem; uint8_t *fb; size_t fbsz; bool dec_open; SDL_Thread *th; SDL_mutex *mx; volatile bool stop; volatile bool flushreq; volatile int seekpos;
  Frame fq[FQ]; int fqr, fqw; double last_q_pts;
  SDL_Texture *tex; int tw, th2; double last_pts; bool frame_shown;
  double loop_base; volatile int loops;
  /* audio */
  int akind; Sample *as; int nas; int apos; double adur; int arate, ach; OpusDecoder *opus; mpg123_handle *mh; size_t mp3fed; bool mp3fmt;
  SDL_AudioDeviceID adev; uint64_t aqueued; bool audio_started, audio_eof; bool webm_parsed; uint8_t *opushead; int preskip;
  AVCodecContext *aac; AVFrame *aframe; AVPacket *apkt; uint8_t *asc; size_t asclen; int aac_rate, aac_ch;
  /* clock */
  double wall0, paused_at, pause_total; bool paused; double seek_base; uint32_t load_t0;
};

static char g_ua[400] = "User-Agent: DiscordWiiU/1.0 (Nintendo Wii U homebrew)";
void vid_set_ua(const char *ua) { snprintf(g_ua, sizeof g_ua, "%s", ua && *ua ? ua : "User-Agent: Mozilla/5.0"); }
static void (*g_log)(const char *line);
void vid_set_log(void (*fn)(const char *line)) { g_log = fn; }
static void vlog(const char *fmt, ...) { if (!g_log) return; char b[1800]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_log(b); }
static void seterr(Vid *v, const char *m) { if (v->state != VID_ERROR) { snprintf(v->err, sizeof v->err, "%s", m); v->state = VID_ERROR; vlog("[vid %p] error: %s", (void *)v, m); } }

/* ---------- byte helpers ---------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---------- MP4 demux ---------- */
typedef struct { const uint8_t *stts, *ctts, *stsc, *stsz, *stco, *stss; size_t nstts, nctts, nstsc, nstsz, nstco, nstss; bool co64; int ctts_ver; uint32_t timescale; char handler[5]; const uint8_t *avcc; size_t avcclen; int w, h; bool mp4a; uint64_t dur; const uint8_t *asc; size_t asclen; int arate, ach; } Trak;

static void parse_stbl(const uint8_t *p, size_t len, Trak *t) {
  size_t i = 0;
  while (i + 8 <= len) { uint64_t sz = rd32(p + i); const uint8_t *ty = p + i + 4; size_t hd = 8; if (sz == 1 && i + 16 <= len) { sz = rd64(p + i + 8); hd = 16; } if (sz == 0) sz = len - i; if (sz < hd || i + sz > len) break;
    const uint8_t *b = p + i + hd; size_t bl = (size_t)sz - hd;
    if (!memcmp(ty, "stsd", 4) && bl > 16) { size_t e = 8; uint32_t n = rd32(b + 4); for (uint32_t k = 0; k < n && e + 16 <= bl; k++) { uint32_t es = rd32(b + e); const uint8_t *et = b + e + 4; if (es < 16 || e + es > bl) break;
        if (!memcmp(et, "avc1", 4) || !memcmp(et, "avc3", 4)) { if (es >= 86) { t->w = rd16(b + e + 32); t->h = rd16(b + e + 34); size_t c = e + 86; while (c + 8 <= e + es) { uint32_t cs = rd32(b + c); if (cs < 8) break; if (!memcmp(b + c + 4, "avcC", 4)) { t->avcc = b + c + 8; t->avcclen = cs - 8; } c += cs; } } }
        else if (!memcmp(et, "mp4a", 4)) { t->mp4a = true; if (es >= 36) { t->ach = rd16(b + e + 24); t->arate = rd16(b + e + 32); size_t c = e + 36; while (c + 8 <= e + es) { uint32_t cs = rd32(b + c); if (cs < 8) break;
            if (!memcmp(b + c + 4, "esds", 4)) { const uint8_t *q = b + c + 12, *qe = b + c + cs; while (q + 2 < qe) { if (*q == 0x05) { q++; size_t l = 0; int k = 0; while (q < qe && k < 4) { l = (l << 7) | (*q & 0x7F); if (!(*q & 0x80)) { q++; break; } q++; k++; } if (q + l <= qe && l >= 2) { t->asc = q; t->asclen = l; } break; } q++; } }
            c += cs; } } }
        e += es; } }
    else if (!memcmp(ty, "stts", 4)) { t->stts = b + 8; t->nstts = rd32(b + 4); }
    else if (!memcmp(ty, "ctts", 4)) { t->ctts = b + 8; t->nctts = rd32(b + 4); t->ctts_ver = b[0]; }
    else if (!memcmp(ty, "stsc", 4)) { t->stsc = b + 8; t->nstsc = rd32(b + 4); }
    else if (!memcmp(ty, "stsz", 4)) { t->stsz = b + 4; t->nstsz = rd32(b + 8); }
    else if (!memcmp(ty, "stco", 4)) { t->stco = b + 8; t->nstco = rd32(b + 4); t->co64 = false; }
    else if (!memcmp(ty, "co64", 4)) { t->stco = b + 8; t->nstco = rd32(b + 4); t->co64 = true; }
    else if (!memcmp(ty, "stss", 4)) { t->stss = b + 8; t->nstss = rd32(b + 4); }
    i += (size_t)sz; }
}
static void parse_trak(const uint8_t *p, size_t len, Trak *t, int depth) {
  size_t i = 0; if (depth > 6) return;
  while (i + 8 <= len) { uint64_t sz = rd32(p + i); const uint8_t *ty = p + i + 4; size_t hd = 8; if (sz == 1 && i + 16 <= len) { sz = rd64(p + i + 8); hd = 16; } if (sz == 0) sz = len - i; if (sz < hd || i + sz > len) break;
    const uint8_t *b = p + i + hd; size_t bl = (size_t)sz - hd;
    if (!memcmp(ty, "mdia", 4) || !memcmp(ty, "minf", 4)) parse_trak(b, bl, t, depth + 1);
    else if (!memcmp(ty, "stbl", 4)) parse_stbl(b, bl, t);
    else if (!memcmp(ty, "mdhd", 4) && bl >= 20) { if (b[0] == 1 && bl >= 32) { t->timescale = rd32(b + 20); t->dur = rd64(b + 24); } else { t->timescale = rd32(b + 12); t->dur = rd32(b + 16); } }
    else if (!memcmp(ty, "hdlr", 4) && bl >= 12) { memcpy(t->handler, b + 8, 4); t->handler[4] = 0; }
    i += (size_t)sz; }
}
/* build the sample table for a track; *span_out = sum of all sample durations (the loop period) */
static Sample *build_samples(const Trak *t, int *count, double *dur_out, double *span_out) {
  if (!t->stts || !t->stsc || !t->stsz || !t->stco || !t->timescale) return NULL;
  uint32_t n = t->nstsz; if (!n || n > 2000000) return NULL; uint32_t fixed = rd32(t->stsz); /* stsz: sample_size then count */
  Sample *s = calloc(n, sizeof(Sample)); if (!s) return NULL;
  for (uint32_t i = 0; i < n; i++) s[i].size = fixed ? fixed : rd32(t->stsz + 8 + 4 * i);
  uint32_t si = 0;
  for (uint32_t e = 0; e < t->nstsc && si < n; e++) { uint32_t first = rd32(t->stsc + e * 12), per = rd32(t->stsc + e * 12 + 4); uint32_t last = e + 1 < t->nstsc ? rd32(t->stsc + (e + 1) * 12) : t->nstco + 1; if (first < 1) first = 1;
    for (uint32_t c = first; c < last && si < n; c++) { if (c - 1 >= t->nstco) break; uint64_t off = t->co64 ? rd64(t->stco + 8 * (c - 1)) : rd32(t->stco + 4 * (c - 1)); for (uint32_t k = 0; k < per && si < n; k++) { s[si].off = off; off += s[si].size; si++; } } }
  double ts = (double)t->timescale; uint64_t dts = 0; uint32_t k = 0; uint32_t lastdelta = 0;
  for (uint32_t e = 0; e < t->nstts && k < n; e++) { uint32_t cnt = rd32(t->stts + e * 8), delta = rd32(t->stts + e * 8 + 4); lastdelta = delta; for (uint32_t j = 0; j < cnt && k < n; j++) { s[k].pts = dts / ts; dts += delta; k++; } }
  for (; k < n; k++) { s[k].pts = dts / ts; dts += lastdelta; }
  if (t->ctts) { uint32_t idx = 0; for (uint32_t e = 0; e < t->nctts && idx < n; e++) { uint32_t cnt = rd32(t->ctts + e * 8); int32_t off = (int32_t)rd32(t->ctts + e * 8 + 4); for (uint32_t j = 0; j < cnt && idx < n; j++) { s[idx].pts += off / ts; idx++; } } }
  if (t->stss) { for (uint32_t e = 0; e < t->nstss; e++) { uint32_t sn = rd32(t->stss + 4 * e); if (sn >= 1 && sn <= n) s[sn - 1].key = true; } } else for (uint32_t i = 0; i < n; i++) s[i].key = true;
  *count = (int)n; *dur_out = t->dur / ts; if (span_out) *span_out = dts / ts; return s;
}
/* find moov in the bytes we have; returns 1 parsed, 0 need more data, -1 not an mp4 */
static int mp4_try_parse(Vid *v) {
  const uint8_t *p = v->vbuf; size_t len = v->vlen, i = 0; bool any = false; bool mute = v->flags & VID_F_MUTE;
  while (i + 8 <= len) { uint64_t sz = rd32(p + i); const uint8_t *ty = p + i + 4; size_t hd = 8; if (sz == 1) { if (i + 16 > len) return 0; sz = rd64(p + i + 8); hd = 16; } if (sz == 0) sz = v->vdone ? len - i : 0; if (sz < hd) return any ? 0 : -1;
    if (!any && memcmp(ty, "ftyp", 4) && memcmp(ty, "moov", 4) && memcmp(ty, "free", 4) && memcmp(ty, "mdat", 4) && memcmp(ty, "wide", 4) && memcmp(ty, "skip", 4)) return -1;
    any = true;
    if (!memcmp(ty, "moov", 4)) { if (i + sz > len) return 0;
      const uint8_t *b = p + i + hd; size_t bl = (size_t)sz - hd; size_t j = 0; double mdur = 0;
      while (j + 8 <= bl) { uint32_t s2 = rd32(b + j); if (s2 < 8 || j + s2 > bl) break; const uint8_t *t2 = b + j + 4;
        if (!memcmp(t2, "mvhd", 4)) { const uint8_t *m = b + j + 8; if (m[0] == 1) { uint32_t tsc = rd32(m + 20); uint64_t d = rd64(m + 24); if (tsc) mdur = d / (double)tsc; } else { uint32_t tsc = rd32(m + 12); if (tsc) mdur = rd32(m + 16) / (double)tsc; } }
        if (!memcmp(t2, "trak", 4)) { Trak t; memset(&t, 0, sizeof t); parse_trak(b + j + 8, s2 - 8, &t, 0);
          if (!strcmp(t.handler, "vide") && t.avcc && !v->vs) { v->vs = build_samples(&t, &v->nvs, &v->vdur, &v->vperiod); if (v->vs) { v->has_video = true; v->vw = t.w; v->vh = t.h; const uint8_t *a = t.avcc; if (t.avcclen >= 7) { v->profile = a[1]; v->level = a[3]; v->nal_size = (a[4] & 3) + 1; int nsps = a[5] & 31; size_t o = 6; uint8_t *h = malloc(t.avcclen + 64); size_t hl = 0;
              for (int q = 0; q < nsps && o + 2 <= t.avcclen; q++) { uint16_t l = rd16(a + o); o += 2; if (o + l > t.avcclen) break; memcpy(h + hl, "\0\0\0\1", 4); hl += 4; memcpy(h + hl, a + o, l); hl += l; o += l; }
              if (o < t.avcclen) { int npps = a[o++]; for (int q = 0; q < npps && o + 2 <= t.avcclen; q++) { uint16_t l = rd16(a + o); o += 2; if (o + l > t.avcclen) break; memcpy(h + hl, "\0\0\0\1", 4); hl += 4; memcpy(h + hl, a + o, l); hl += l; o += l; } }
              v->hdr = h; v->hdrlen = hl; } } }
          else if (!strcmp(t.handler, "soun") && t.mp4a && !v->as && !mute) { v->aac_in_mp4 = true; v->as = build_samples(&t, &v->nas, &v->adur, NULL); if (t.asc) { v->asc = malloc(t.asclen); memcpy(v->asc, t.asc, t.asclen); v->asclen = t.asclen; } v->aac_rate = t.arate; v->aac_ch = t.ach; } }
        j += s2; }
      if (mdur > 0 && v->vdur <= 0) v->vdur = mdur; v->moov_ok = true; return 1; }
    if (i + sz > len) { if (!memcmp(ty, "mdat", 4)) { i += (size_t)sz; continue; } return 0; }   /* skip mdat even if not downloaded yet */
    i += (size_t)sz; }
  return v->vdone ? -1 : 0;
}

/* ---------- WebM (Opus) demux ---------- */
static uint64_t ebml_vint(const uint8_t *p, size_t len, size_t *used, bool keep_marker) {
  if (!len) { *used = 0; return 0; } uint8_t b = p[0]; int n = 1; uint8_t mask = 0x80; while (n <= 8 && !(b & mask)) { mask >>= 1; n++; } if (n > 8 || (size_t)n > len) { *used = 0; return 0; }
  uint64_t v = keep_marker ? b : (b & (mask - 1)); for (int i = 1; i < n; i++) v = v << 8 | p[i]; *used = (size_t)n; return v;
}
static void webm_parse(Vid *v) {
  const uint8_t *p = v->abuf; size_t len = v->alen, i = 0; double tscale = 1e6; int opus_track = -1; size_t cap = 0; uint64_t cluster_tc = 0;
  v->nas = 0;
  while (i < len) { size_t u1, u2; uint64_t id = ebml_vint(p + i, len - i, &u1, true); if (!u1) break; uint64_t sz = ebml_vint(p + i + u1, len - i - u1, &u2, false); if (!u2) break; size_t hd = u1 + u2; bool unknown = (sz == (uint64_t)-1) || (u2 == 1 && sz == 127) || (u2 == 8 && sz == 0xFFFFFFFFFFFFFFull);
    const uint8_t *b = p + i + hd; size_t bl = unknown ? len - i - hd : (size_t)sz; if (i + hd > len) break; if (!unknown && i + hd + bl > len) bl = len - i - hd;
    bool descend = id == 0x18538067 || id == 0x1549A966 || id == 0x1654AE6B || id == 0xAE || id == 0x1F43B675 || id == 0xA0 || id == 0xE1;
    if (id == 0x2AD7B1) { uint64_t t = 0; for (size_t k = 0; k < bl; k++) t = t << 8 | b[k]; if (t) tscale = (double)t; }
    else if (id == 0x4489) { if (bl == 4) { uint32_t x = rd32(b); float f; memcpy(&f, &x, 4); v->adur = f * tscale / 1e9; } else if (bl == 8) { uint64_t x = rd64(b); double d; memcpy(&d, &x, 8); v->adur = d * tscale / 1e9; } }
    else if (id == 0xD7) { uint64_t t = 0; for (size_t k = 0; k < bl; k++) t = t << 8 | b[k]; if (opus_track < 0) opus_track = (int)t; }
    else if (id == 0x86) { if (bl >= 6 && !strncmp((const char *)b, "A_OPUS", 6)) { } else if (bl >= 2 && opus_track >= 0 && strncmp((const char *)b, "A_OPUS", bl < 6 ? bl : 6)) opus_track = -2; }
    else if (id == 0x63A2 && !v->opushead && bl >= 19 && !memcmp(b, "OpusHead", 8)) { v->opushead = malloc(bl); memcpy(v->opushead, b, bl); v->ach = b[9]; v->preskip = b[10] | b[11] << 8; }
    else if (id == 0xE7) { uint64_t t = 0; for (size_t k = 0; k < bl; k++) t = t << 8 | b[k]; cluster_tc = t; }
    else if (id == 0xA3 || id == 0xA1) { size_t u3; uint64_t tn = ebml_vint(b, bl, &u3, false); if (u3 && bl >= u3 + 3 && (opus_track < 0 || (int)tn == opus_track || opus_track == -2)) { int16_t rel = (int16_t)rd16(b + u3); uint8_t flags = b[u3 + 2]; size_t doff = u3 + 3; if ((flags & 6) == 0) { if (v->nas == (int)cap) { cap = cap ? cap * 2 : 1024; v->as = realloc(v->as, cap * sizeof(Sample)); }
          Sample *s = &v->as[v->nas++]; s->off = (uint64_t)(b + doff - p); s->size = (uint32_t)(bl - doff); s->pts = ((double)cluster_tc + rel) * tscale / 1e9; s->key = true; } } }
    if (descend) i += hd; else i += hd + bl; }
  v->webm_parsed = true;
}

/* ---------- H.264 decoder: instance registry + callback routing ---------- */
static SDL_SpinLock g_reglock; static Vid *g_reg[VID_MAX_DECODERS]; static OSThread *g_regth[VID_MAX_DECODERS];
static SDL_mutex *g_h264mx;                                    /* decoder calls of all instances are serialised (hardware is shared) */
static bool reg_add(Vid *v) { bool ok = false; SDL_AtomicLock(&g_reglock); for (int i = 0; i < VID_MAX_DECODERS; i++) if (!g_reg[i]) { g_reg[i] = v; g_regth[i] = OSGetCurrentThread(); ok = true; break; } SDL_AtomicUnlock(&g_reglock); return ok; }
static void reg_del(Vid *v) { SDL_AtomicLock(&g_reglock); for (int i = 0; i < VID_MAX_DECODERS; i++) if (g_reg[i] == v) { g_reg[i] = NULL; g_regth[i] = NULL; } SDL_AtomicUnlock(&g_reglock); }
int vid_decoders_open(void) { int n = 0; SDL_AtomicLock(&g_reglock); for (int i = 0; i < VID_MAX_DECODERS; i++) if (g_reg[i]) n++; SDL_AtomicUnlock(&g_reglock); return n; }
static void dec_lock(void) { if (g_h264mx) SDL_LockMutex(g_h264mx); }
static void dec_unlock(void) { if (g_h264mx) SDL_UnlockMutex(g_h264mx); }
static Vid *resolve_cb(H264DecodeOutput *out) {
  static bool said[5]; Vid *v = NULL; int how = 0; SDL_AtomicLock(&g_reglock);
  for (int i = 0; i < VID_MAX_DECODERS && !v; i++) if (g_reg[i] && out->userMemory == (void *)g_reg[i]) { v = g_reg[i]; how = 1; }
  if (!v) { OSThread *t = OSGetCurrentThread(); for (int i = 0; i < VID_MAX_DECODERS && !v; i++) if (g_reg[i] && g_regth[i] == t) { v = g_reg[i]; how = 2; } }
  if (!v && out->frameCount > 0 && out->decodeResults && out->decodeResults[0]) { const uint8_t *fbp = out->decodeResults[0]->framebuffer; for (int i = 0; i < VID_MAX_DECODERS && !v; i++) if (g_reg[i] && g_reg[i]->fb && fbp >= g_reg[i]->fb && fbp < g_reg[i]->fb + g_reg[i]->fbsz) { v = g_reg[i]; how = 3; } }
  if (!v) { int n = 0; Vid *only = NULL; for (int i = 0; i < VID_MAX_DECODERS; i++) if (g_reg[i]) { n++; only = g_reg[i]; } if (n == 1) { v = only; how = 4; } }
  SDL_AtomicUnlock(&g_reglock);
  if (!said[how]) { said[how] = true; vlog("[vid] frame callback routed by %s (userMemory=%p)", how == 1 ? "user pointer" : how == 2 ? "calling thread" : how == 3 ? "frame buffer" : how == 4 ? "only instance" : "NOTHING (frame dropped)", out->userMemory); }
  return v;
}
static inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
/* NV12 -> RGBA: table-driven converter in yuv.c (bit-identical to the old per-pixel loop, checked by the vrecv self
 * test), frames of 300k+ pixels split across two cores */
static void nv12_to_rgba(const uint8_t *y, const uint8_t *uv, int pitch, int w, int h, uint8_t *out) {
  yuv_nv12_to_rgba(y, uv, pitch, w, h, out, w * 4, w, h, (size_t)w * h >= 300000 ? 2 : 1);
}
static void frame_cb(H264DecodeOutput *out) {
  if (!out) return; Vid *v = resolve_cb(out); if (!v) return;
  for (int i = 0; i < out->frameCount; i++) { H264DecodeResult *r = out->decodeResults[i];  if (!r || !r->framebuffer || (r->status != 0 && r->status != 100)) continue;
    int w = r->width, h = r->height; if (r->cropEnableFlag) { w = r->width - r->cropLeft - r->cropRight; h = r->height - r->cropTop - r->cropBottom; } else { if (v->vw > 0 && v->vw <= w) w = v->vw; if (v->vh > 0 && v->vh <= h) h = v->vh; } if (w <= 0 || h <= 0 || w > 1920 || h > 1088) continue;
    /* wait for a free slot (the UI thread presents frames by pts); bounded, since we may hold the shared decoder lock */
    uint32_t t0 = SDL_GetTicks(); bool drop = false;
    for (;;) { SDL_LockMutex(v->mx); bool full = v->fq[v->fqw].full; SDL_UnlockMutex(v->mx); if (!full || v->stop || v->flushreq) break; if (SDL_GetTicks() - t0 > 250) { drop = true; break; } SDL_Delay(4); }
    if (v->stop || v->flushreq) return; if (drop) continue;
    Frame *f = &v->fq[v->fqw]; size_t need = (size_t)w * h * 4; if (f->w != w || f->h != h || !f->rgba) { free(f->rgba); f->rgba = malloc(need); f->w = w; f->h = h; } if (!f->rgba) continue;
    DCInvalidateRange(r->framebuffer, (uint32_t)r->nextLine * r->height * 3 / 2);   /* the decoder DMA-writes this: drop stale cache lines */
    const uint8_t *yp = (const uint8_t *)r->framebuffer, *uvp = yp + (size_t)r->nextLine * r->height; int cl = r->cropEnableFlag ? r->cropLeft : 0, ct = r->cropEnableFlag ? r->cropTop : 0;
    nv12_to_rgba(yp + ct * r->nextLine + cl, uvp + (ct / 2) * r->nextLine + (cl & ~1), r->nextLine, w, h, f->rgba);
    f->pts = r->timestamp; v->last_q_pts = r->timestamp; SDL_LockMutex(v->mx); f->full = true; v->fqw = (v->fqw + 1) % FQ; SDL_UnlockMutex(v->mx); }
}
static bool dec_open(Vid *v) {
  uint32_t need = 0; int prof = v->profile == 66 || v->profile == 77 || v->profile == 100 ? v->profile : 100; int lvl = v->level > 0 ? v->level : 41;
  if (!reg_add(v)) { seterr(v, "too many videos playing at once"); return false; }
  dec_lock();
  H264Error e0 = H264DECMemoryRequirement(prof, lvl, v->vw, v->vh, &need); vlog("[vid %p] memreq prof=%d lvl=%d %dx%d -> err=%d need=%u (decoders open: %d)", (void *)v, prof, lvl, v->vw, v->vh, (int)e0, need, vid_decoders_open());
  if (e0 != H264_ERROR_OK || !need) { dec_unlock(); seterr(v, "H.264 decoder unavailable for this stream"); return false; }
  v->dmem = memalign(256, need + 256); if (!v->dmem) { dec_unlock(); seterr(v, "out of memory (decoder)"); return false; }
  { H264Error e1 = H264DECInitParam((int32_t)need, v->dmem); if (e1 != H264_ERROR_OK) { dec_unlock(); vlog("[vid] initparam err=%d", (int)e1); seterr(v, "H.264 init failed"); return false; } }
  H264DECSetParam_FPTR_OUTPUT(v->dmem, frame_cb); uint32_t one = 1; H264DECSetParam(v->dmem, H264_PARAMETER_OUTPUT_PER_FRAME, &one); void *um = v; H264DECSetParam_USER_MEMORY(v->dmem, &um);
  v->fbsz = (size_t)((v->vw + 255) & ~255) * ((v->vh + 63) & ~63) * 2 + 65536;     /* decoder pitch rounds up to 256, planes are padded: be generous */ v->fb = memalign(256, v->fbsz); if (!v->fb) { dec_unlock(); seterr(v, "out of memory (frames)"); return false; }
  { H264Error e2 = H264DECOpen(v->dmem); H264Error e3 = e2 == H264_ERROR_OK ? H264DECBegin(v->dmem) : e2; vlog("[vid %p] open err=%d begin err=%d fbsz=%u", (void *)v, (int)e2, (int)e3, (unsigned)v->fbsz);
    if (e2 != H264_ERROR_OK || e3 != H264_ERROR_OK) { if (e2 == H264_ERROR_OK) H264DECClose(v->dmem); dec_unlock(); char m[120]; snprintf(m, sizeof m, "H.264 open failed (err %d/%d, %d other decoders open)", (int)e2, (int)e3, vid_decoders_open() - 1); seterr(v, m); return false; } }
  v->dec_open = true; dec_unlock(); return true;
}
static void dec_close(Vid *v) { if (v->dec_open) { dec_lock(); H264DECEnd(v->dmem); H264DECClose(v->dmem); dec_unlock(); v->dec_open = false; } reg_del(v); free(v->dmem); v->dmem = NULL; free(v->fb); v->fb = NULL; }

/* pull newly downloaded bytes into our buffers (exact-size buffer when the length is known) */
static void pump_download(Vid *v, NxJob *j, uint8_t **buf, size_t *len, size_t *cap, bool *done, size_t *total) {
  if (!j) return; size_t tot = 0; size_t have = nxnet_job_avail(j, &tot); if (tot) *total = tot; size_t lim = v->maxdl;
  if (have > *len) { if (have > *cap) { size_t nc; if (tot >= have && tot <= lim) nc = tot; else { nc = *cap ? *cap : (1u << 20); while (nc < have) nc *= 2; if (nc > lim) nc = lim; } if (have > nc) have = nc;
      if (nc > *cap) { SDL_LockMutex(v->mx); uint8_t *nb = realloc(*buf, nc); if (nb) { *buf = nb; *cap = nc; } SDL_UnlockMutex(v->mx); if (!nb) return; } }
    if (have > *cap) have = *cap; if (have > *len) *len += nxnet_job_copy(j, *len, *buf + *len, have - *len); }
  if (nxnet_job_done(j) && (*len >= nxnet_job_avail(j, NULL) || *len >= lim)) *done = true;   /* done only once every byte is copied */
}
static void au_build(Vid *v, const Sample *s, bool with_hdr, uint8_t **au, size_t *aucap, size_t *n_out) {
  size_t need = s->size + v->hdrlen + 64 + (size_t)s->size / 2; if (need > *aucap) { *aucap = need * 2; *au = realloc(*au, *aucap); } size_t n = 0;
  if (with_hdr) { memcpy(*au, v->hdr, v->hdrlen); n = v->hdrlen; }
  const uint8_t *src = v->vbuf + s->off; size_t rem = s->size;
  while (rem > (size_t)v->nal_size) { uint32_t nl = 0; for (int k = 0; k < v->nal_size; k++) nl = nl << 8 | src[k]; src += v->nal_size; rem -= v->nal_size; if (nl > rem) nl = (uint32_t)rem; if (n + 4 + nl > *aucap) break; memcpy(*au + n, "\0\0\0\1", 4); n += 4; memcpy(*au + n, src, nl); n += nl; src += nl; rem -= nl; }
  *n_out = n;
}
static int decode_thread(void *arg) {
  Vid *v = arg; OSThread *self = OSGetCurrentThread();
  OSSetThreadAffinity(self, (v->flags & VID_F_LOOP) ? (OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2) : OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(self, 24);
  uint8_t *au = NULL; size_t aucap = 0; int parsed = 0; bool opened = false;
  while (!v->stop) {
    pump_download(v, v->vjob, &v->vbuf, &v->vlen, &v->vcap, &v->vdone, &v->vtotal);
    if (v->vdone && v->vjob && v->vlen >= nxnet_job_avail(v->vjob, NULL)) {            /* everything copied: drop the job (and its copy of the body) */
      int st = nxnet_job_status(v->vjob); vlog("[vid %p] download done: HTTP %d, %u bytes", (void *)v, st, (unsigned)v->vlen);
      nxnet_job_cancel(v->vjob); v->vjob = NULL;
      if (!parsed && (st >= 300 || (st == 0 && v->vlen == 0))) { char m[80]; snprintf(m, sizeof m, st ? "download failed (HTTP %d)" : "download failed (network)", st); seterr(v, m); break; } }
    if (!parsed) { int r = mp4_try_parse(v); if (r < 0) { if (v->vlen >= 12 && v->vbuf[0] == 0x1A && v->vbuf[1] == 0x45 && !(v->flags & (VID_F_MUTE | VID_F_MP4_ONLY))) { /* a webm given as the main url: audio only */ SDL_LockMutex(v->mx); v->abuf = v->vbuf; v->alen = v->vlen; v->acap = v->vcap; v->vbuf = NULL; v->vlen = v->vcap = 0; v->ajob = v->vjob; v->vjob = NULL; v->adone = v->vdone; SDL_UnlockMutex(v->mx); parsed = 1; v->akind = 1; }
        else if (v->vlen >= 3 && !(v->flags & (VID_F_MUTE | VID_F_MP4_ONLY)) && (!memcmp(v->vbuf, "ID3", 3) || (v->vbuf[0] == 0xFF && (v->vbuf[1] & 0xE0) == 0xE0))) { SDL_LockMutex(v->mx); v->abuf = v->vbuf; v->alen = v->vlen; v->acap = v->vcap; v->vbuf = NULL; v->vlen = v->vcap = 0; v->ajob = v->vjob; v->vjob = NULL; v->adone = v->vdone; SDL_UnlockMutex(v->mx); parsed = 1; v->akind = 2; }
        else { char hex[200]; size_t k = 0; for (size_t i = 0; i < v->vlen && i < 48 && k + 6 < sizeof hex; i++) { unsigned char c = v->vbuf[i]; k += (size_t)sprintf(hex + k, c >= 32 && c < 127 ? "%c" : "<%02x>", c); } vlog("[vid %p] unsupported format: len=%u done=%d head=%s", (void *)v, (unsigned)v->vlen, v->vdone, hex); seterr(v, "Unsupported media format (need MP4/H.264, WebM/Opus or MP3)"); break; } }
      else if (r == 0) { if (v->vdone && !v->vjob) { seterr(v, "Truncated or damaged MP4 (no index)"); break; } SDL_Delay(30); continue; }
      else { parsed = 1; if (v->vperiod <= 0) v->vperiod = v->vdur; if (v->vperiod <= 0 && v->nvs > 0) v->vperiod = v->vs[v->nvs - 1].pts + 0.04;
        vlog("[vid %p] moov: video=%d %dx%d samples=%d prof=%d lvl=%d nal=%d hdr=%u dur=%.2f period=%.3f aac=%d flags=%d", (void *)v, v->has_video, v->vw, v->vh, v->nvs, v->profile, v->level, v->nal_size, (unsigned)v->hdrlen, v->vdur, v->vperiod, v->aac_in_mp4, v->flags);
        if (!v->has_video) { if (v->aac_in_mp4 && v->as) { v->akind = 4; } else { seterr(v, "No H.264 video track found"); break; } }
        if (v->flags & VID_F_MUTE) v->akind = 0;
        else if (v->aac_in_mp4 && v->as && v->nas > 0) { v->akind = 4; if (v->ajob) { nxnet_job_cancel(v->ajob); v->ajob = NULL; } } else if (v->aurl[0]) v->akind = 1; else if (v->aac_in_mp4) v->akind = 3; } }
    if (!v->has_video) { SDL_Delay(50); continue; }
    if (!opened) { if (!dec_open(v)) break; opened = true; }
    if (v->flushreq) { SDL_LockMutex(v->mx); for (int i = 0; i < FQ; i++) v->fq[i].full = false; v->fqr = v->fqw = 0; v->vpos = v->seekpos; v->last_q_pts = -1e9; v->flushreq = false; SDL_UnlockMutex(v->mx); dec_lock(); H264DECEnd(v->dmem); H264DECBegin(v->dmem); dec_unlock(); }
    if (v->vpos >= v->nvs) {
      if ((v->flags & VID_F_LOOP) && v->vdone && v->nvs > 0 && v->vperiod > 0) {          /* seamless loop: drain, restart the decoder, continue the timeline */
        dec_lock(); H264DECFlush(v->dmem); H264DECEnd(v->dmem); H264DECBegin(v->dmem); dec_unlock();
        v->loop_base += v->vperiod; v->vpos = 0; v->loops++; vlog("[vid %p] loop %d (restart from the bytes in memory)", (void *)v, v->loops); continue; }
      SDL_Delay(20); continue; }
    Sample *s = &v->vs[v->vpos]; if (s->off + s->size > v->vlen) { if (v->vdone) { v->vpos = v->nvs; continue; } SDL_Delay(30); continue; }
    /* don't run ahead of presentation: wait for a free slot before decoding (so the callback rarely has to wait) */
    { bool full; SDL_LockMutex(v->mx); full = v->fq[v->fqw].full; SDL_UnlockMutex(v->mx); if (full) { SDL_Delay(4); continue; } }
    size_t n = 0; au_build(v, s, s->key || v->vpos == 0, &au, &aucap, &n);
    dec_lock(); H264DECSetBitstream(v->dmem, au, (uint32_t)n, s->pts + v->loop_base); H264DECExecute(v->dmem, v->fb); dec_unlock();
    v->vpos++;
  }
  free(au); return 0;
}

/* ---------- audio ---------- */
static bool audio_open(Vid *v, int rate, int ch) {
  if (!(SDL_WasInit(SDL_INIT_AUDIO))) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioSpec want, have; memset(&want, 0, sizeof want); want.freq = rate; want.format = AUDIO_S16SYS; want.channels = (Uint8)ch; want.samples = 2048;
  v->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); if (!v->adev) return false; v->arate = have.freq; v->ach = have.channels; return true;
}
static double audio_clock(const Vid *v) { if (!v->adev || !v->audio_started) return -1; uint32_t q = SDL_GetQueuedAudioSize(v->adev); double played = (double)(v->aqueued > q ? v->aqueued - q : 0) / (v->arate * v->ach * 2); return v->seek_base + played; }
static void audio_feed(Vid *v) {
  if (v->akind == 1) {
    if (!v->webm_parsed) { if (v->alen > 4096 || v->adone) webm_parse(v); else return; }
    if (!v->opus) { if (!v->opushead) return; int err = 0; v->opus = opus_decoder_create(48000, v->ach == 1 ? 1 : 2, &err); if (!v->opus) { v->akind = 3; return; } if (!audio_open(v, 48000, v->ach == 1 ? 1 : 2)) { v->akind = 3; return; } }
    int fed = 0; static int16_t pcm[5760 * 2];
    while (v->apos < v->nas && SDL_GetQueuedAudioSize(v->adev) < (uint32_t)(v->arate * v->ach * 2 / 2) && fed < 12) { Sample *s = &v->as[v->apos]; if (s->off + s->size > v->alen) break;
      int n = opus_decode(v->opus, v->abuf + s->off, (int32_t)s->size, pcm, 5760, 0); if (n > 0) { SDL_QueueAudio(v->adev, pcm, (uint32_t)n * v->ach * 2); v->aqueued += (uint64_t)n * v->ach * 2; } v->apos++; fed++; }
    if (v->apos >= v->nas && v->adone) v->audio_eof = true;
    if (!v->audio_started && (SDL_GetQueuedAudioSize(v->adev) > 8192 || v->audio_eof)) { v->audio_started = true; if (!v->paused) SDL_PauseAudioDevice(v->adev, 0); }
    if (v->adone && v->alen > 0 && v->webm_parsed && v->nas == 0) v->akind = 3;
  } else if (v->akind == 4) {                                     /* AAC inside the MP4 (ffmpeg libavcodec, software) */
    if (!v->aac) { const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC); if (!codec) { v->akind = 3; return; } v->aac = avcodec_alloc_context3(codec); if (!v->aac) { v->akind = 3; return; }
      if (v->asc && v->asclen) { v->aac->extradata = av_mallocz(v->asclen + AV_INPUT_BUFFER_PADDING_SIZE); memcpy(v->aac->extradata, v->asc, v->asclen); v->aac->extradata_size = (int)v->asclen; }
      if (avcodec_open2(v->aac, codec, NULL) < 0) { vlog("[vid] aac open failed"); v->akind = 3; return; } v->aframe = av_frame_alloc(); v->apkt = av_packet_alloc(); vlog("[vid %p] aac decoder open, %d samples, rate %d ch %d, asc %u bytes", (void *)v, v->nas, v->aac_rate, v->aac_ch, (unsigned)v->asclen); }
    int fed = 0; static int16_t pcm[8192 * 2];
    while (v->apos < v->nas && (!v->adev || SDL_GetQueuedAudioSize(v->adev) < (uint32_t)(v->arate * v->ach * 2 / 2)) && fed < 10) { Sample *s = &v->as[v->apos]; if (s->off + s->size > v->vlen) break;
      uint8_t *buf = av_malloc(s->size + AV_INPUT_BUFFER_PADDING_SIZE); if (!buf) break; SDL_LockMutex(v->mx); memcpy(buf, v->vbuf + s->off, s->size); SDL_UnlockMutex(v->mx); memset(buf + s->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
      av_packet_unref(v->apkt); if (av_packet_from_data(v->apkt, buf, (int)s->size) < 0) { av_free(buf); break; } v->apkt->pts = (int64_t)(s->pts * 1000);
      if (avcodec_send_packet(v->aac, v->apkt) >= 0) { while (avcodec_receive_frame(v->aac, v->aframe) >= 0) { AVFrame *f = v->aframe; int ch = f->ch_layout.nb_channels, n = f->nb_samples; if (ch < 1) continue; if (ch > 2) ch = 2;
          if (!v->adev) { if (!audio_open(v, f->sample_rate, ch)) { v->akind = 3; return; } }
          int oc = v->ach; int cnt = n < 8192 ? n : 8192;
          if (f->format == AV_SAMPLE_FMT_FLTP) { for (int i = 0; i < cnt; i++) for (int c = 0; c < oc; c++) { const float *pl = (const float *)f->extended_data[c < ch ? c : 0]; float x = pl[i] * 32767.0f; pcm[i * oc + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); } }
          else if (f->format == AV_SAMPLE_FMT_FLT) { const float *pl = (const float *)f->extended_data[0]; for (int i = 0; i < cnt; i++) for (int c = 0; c < oc; c++) { float x = pl[i * ch + (c < ch ? c : 0)] * 32767.0f; pcm[i * oc + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); } }
          else if (f->format == AV_SAMPLE_FMT_S16P) { for (int i = 0; i < cnt; i++) for (int c = 0; c < oc; c++) pcm[i * oc + c] = ((const int16_t *)f->extended_data[c < ch ? c : 0])[i]; }
          else if (f->format == AV_SAMPLE_FMT_S16) { const int16_t *pl = (const int16_t *)f->extended_data[0]; for (int i = 0; i < cnt; i++) for (int c = 0; c < oc; c++) pcm[i * oc + c] = pl[i * ch + (c < ch ? c : 0)]; }
          else continue;
          SDL_QueueAudio(v->adev, pcm, (uint32_t)cnt * oc * 2); v->aqueued += (uint64_t)cnt * oc * 2; } }
      v->apos++; fed++; }
    if (v->apos >= v->nas && v->vdone) v->audio_eof = true;
    if (v->adev && !v->audio_started && (SDL_GetQueuedAudioSize(v->adev) > 8192 || v->audio_eof)) { v->audio_started = true; if (!v->paused) SDL_PauseAudioDevice(v->adev, 0); }
  } else if (v->akind == 2) {
    if (!v->mh) { static bool inited; if (!inited) { mpg123_init(); inited = true; } int err = 0; v->mh = mpg123_new(NULL, &err); if (!v->mh || mpg123_open_feed(v->mh) != MPG123_OK) { v->akind = 3; return; } mpg123_format_none(v->mh); mpg123_format(v->mh, 44100, MPG123_STEREO | MPG123_MONO, MPG123_ENC_SIGNED_16); mpg123_format(v->mh, 48000, MPG123_STEREO | MPG123_MONO, MPG123_ENC_SIGNED_16); mpg123_format(v->mh, 32000, MPG123_STEREO | MPG123_MONO, MPG123_ENC_SIGNED_16); mpg123_format(v->mh, 22050, MPG123_STEREO | MPG123_MONO, MPG123_ENC_SIGNED_16); }
    if (v->mp3fed < v->alen) { size_t chunk = v->alen - v->mp3fed; if (chunk > 65536) chunk = 65536; mpg123_feed(v->mh, v->abuf + v->mp3fed, chunk); v->mp3fed += chunk; }
    static unsigned char out[32768]; int guard = 0;
    while ((!v->adev || SDL_GetQueuedAudioSize(v->adev) < (uint32_t)(v->arate * v->ach * 2 / 2)) && guard++ < 8) { size_t done = 0; int r = mpg123_read(v->mh, out, sizeof out, &done);
      if (r == MPG123_NEW_FORMAT || (!v->mp3fmt && done)) { long rate; int ch, enc; mpg123_getformat(v->mh, &rate, &ch, &enc); if (!v->adev) { if (!audio_open(v, (int)rate, ch)) { v->akind = 3; return; } } v->mp3fmt = true; }
      if (done && v->adev) { SDL_QueueAudio(v->adev, out, (uint32_t)done); v->aqueued += done; }
      if (r == MPG123_NEED_MORE) { if (v->adone && v->mp3fed >= v->alen) v->audio_eof = true; break; } if (r == MPG123_DONE || r == MPG123_ERR) { v->audio_eof = true; break; } }
    if (v->adev && !v->audio_started && (SDL_GetQueuedAudioSize(v->adev) > 8192 || v->audio_eof)) { v->audio_started = true; if (!v->paused) SDL_PauseAudioDevice(v->adev, 0); }
  }
}

/* ---------- public ---------- */
Vid *vid_open_ex(const char *url, int flags, size_t max_bytes) {
  if (!g_h264mx) g_h264mx = SDL_CreateMutex();
  Vid *v = calloc(1, sizeof *v); if (!v) return NULL; snprintf(v->vurl, sizeof v->vurl, "%s", url ? url : ""); v->flags = flags; v->maxdl = max_bytes ? max_bytes : MAX_DL;
  v->mx = SDL_CreateMutex(); v->state = VID_LOADING; v->seekpos = 0; v->last_q_pts = -1e9;
  const char *h[] = {g_ua, "Accept: */*", "Accept-Encoding: identity", "Range: bytes=0-", NULL};
  vlog("[vid %p] open flags=%d max=%u %.150s", (void *)v, flags, (unsigned)v->maxdl, v->vurl);
  v->vjob = nxnet_start("GET", v->vurl, h, NULL, 0, v->maxdl);
  v->th = SDL_CreateThreadWithStackSize(decode_thread, "vdec", 256 * 1024, v); v->wall0 = SDL_GetTicks() / 1000.0;
  if (!v->th) seterr(v, "could not start the decode thread");
  return v;
}
Vid *vid_open(const char *video_url, const char *audio_url) {
  if (!audio_url || !*audio_url) return vid_open_ex(video_url, 0, 0);
  if (!g_h264mx) g_h264mx = SDL_CreateMutex();
  Vid *v = calloc(1, sizeof *v); snprintf(v->vurl, sizeof v->vurl, "%s", video_url); snprintf(v->aurl, sizeof v->aurl, "%s", audio_url); v->maxdl = MAX_DL;
  v->mx = SDL_CreateMutex(); v->state = VID_LOADING; v->seekpos = 0; v->last_q_pts = -1e9;
  const char *h[] = {g_ua, "Accept: */*", "Accept-Encoding: identity", "Range: bytes=0-", NULL};
  vlog("[vid %p] open %.120s + audio", (void *)v, v->vurl);
  v->vjob = nxnet_start("GET", v->vurl, h, NULL, 0, MAX_DL); v->ajob = nxnet_start("GET", v->aurl, h, NULL, 0, MAX_DL);
  v->th = SDL_CreateThreadWithStackSize(decode_thread, "vdec", 256 * 1024, v); v->wall0 = SDL_GetTicks() / 1000.0; return v;
}
void vid_close(Vid *v) {
  if (!v) return; v->stop = true; if (v->th) SDL_WaitThread(v->th, NULL); dec_close(v);
  if (v->vjob) nxnet_job_cancel(v->vjob); if (v->ajob) nxnet_job_cancel(v->ajob);
  if (v->adev) SDL_CloseAudioDevice(v->adev); if (v->opus) opus_decoder_destroy(v->opus); if (v->mh) { mpg123_close(v->mh); mpg123_delete(v->mh); } if (v->aac) avcodec_free_context(&v->aac); if (v->aframe) av_frame_free(&v->aframe); if (v->apkt) av_packet_free(&v->apkt); free(v->asc);
  for (int i = 0; i < FQ; i++) free(v->fq[i].rgba); if (v->tex) SDL_DestroyTexture(v->tex); free(v->vbuf); if (v->abuf != v->vbuf) free(v->abuf); free(v->vs); free(v->as); free(v->hdr); free(v->opushead); SDL_DestroyMutex(v->mx); free(v);
}
static double play_clock(Vid *v) {
  double a = audio_clock(v); if (a >= 0 && v->akind != 3 && !v->audio_eof) return a;
  if (v->paused) return v->seek_base + (v->paused_at - v->wall0 - v->pause_total);
  return v->seek_base + (SDL_GetTicks() / 1000.0 - v->wall0 - v->pause_total);
}
void vid_tick(Vid *v) {
  if (!v || v->state == VID_ERROR) return;
  if (v->ajob && v->akind == 1) { size_t dummy = 0; pump_download(v, v->ajob, &v->abuf, &v->alen, &v->acap, &v->adone, &dummy);           /* separate audio stream: pumped here, never blocked by the decoder */
    if (v->adone && v->alen == 0) { vlog("[vid] audio stream failed (HTTP %d), using the mp4 audio", nxnet_job_status(v->ajob)); v->akind = v->aac_in_mp4 && v->as && v->nas > 0 ? 4 : 3; } }
  if (v->akind == 1 || v->akind == 2 || v->akind == 4) audio_feed(v);
  if (v->state == VID_LOADING) { bool vready = !v->has_video || v->fq[v->fqr].full; bool aready = !(v->akind == 1 || v->akind == 2 || v->akind == 4) || v->audio_started; if (!v->load_t0) v->load_t0 = SDL_GetTicks(); if (vready && !aready && SDL_GetTicks() - v->load_t0 > 5000 && v->has_video) aready = true;   /* don't hold the picture hostage to slow audio */
    if (vready && aready && (v->moov_ok || v->akind)) { v->state = v->paused ? VID_PAUSED : VID_PLAYING; v->wall0 = SDL_GetTicks() / 1000.0; v->pause_total = 0;
      if (v->has_video && !v->adev && v->fq[v->fqr].full && v->fq[v->fqr].pts > 0) v->seek_base = v->fq[v->fqr].pts;   /* wall clock: show the first frame now */ if (v->paused) v->paused_at = v->wall0; if (v->adev && !v->paused) SDL_PauseAudioDevice(v->adev, 0); } else return; }
  if (v->paused) return;
  double t = play_clock(v);
  if (v->has_video) { for (int guard = 0; guard < 3; guard++) { SDL_LockMutex(v->mx); Frame *f = &v->fq[v->fqr]; bool ready = f->full && f->pts <= t + 0.005; SDL_UnlockMutex(v->mx); if (!ready) break;
      if (!v->tex || v->tw != f->w || v->th2 != f->h) { if (v->tex) SDL_DestroyTexture(v->tex); v->tex = SDL_CreateTexture(nx_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, f->w, f->h); v->tw = f->w; v->th2 = f->h; }
      if (v->tex) SDL_UpdateTexture(v->tex, NULL, f->rgba, f->w * 4); v->last_pts = f->pts; v->frame_shown = true;
      SDL_LockMutex(v->mx); f->full = false; v->fqr = (v->fqr + 1) % FQ; SDL_UnlockMutex(v->mx); }
    if (!(v->flags & VID_F_LOOP) && v->vpos >= v->nvs && v->vdone && !v->fq[v->fqr].full && t > v->last_pts + 0.2) { if (!v->adev || v->audio_eof || SDL_GetQueuedAudioSize(v->adev) == 0) v->state = VID_ENDED; } }
  else if (v->audio_eof && v->adev && SDL_GetQueuedAudioSize(v->adev) == 0) v->state = VID_ENDED;
}
int vid_state(const Vid *v) { return v ? v->state : VID_ERROR; }
const char *vid_error(const Vid *v) { return v ? v->err : "no player"; }
void vid_toggle(Vid *v) {
  if (!v || v->state == VID_ERROR) return;
  if (v->state == VID_LOADING) { v->paused = !v->paused; return; }                 /* remembered: starts paused / playing */
  if (v->state == VID_ENDED) { v->paused = false; vid_seek(v, -1e9); v->state = VID_PLAYING; return; }
  if (v->paused) { v->pause_total += SDL_GetTicks() / 1000.0 - v->paused_at; v->paused = false; if (v->adev) SDL_PauseAudioDevice(v->adev, 0); v->state = VID_PLAYING; }
  else { v->paused = true; v->paused_at = SDL_GetTicks() / 1000.0; if (v->adev) SDL_PauseAudioDevice(v->adev, 1); v->state = VID_PAUSED; }
}
void vid_seek(Vid *v, double rel) {
  if (!v || v->state == VID_ERROR || v->state == VID_LOADING || (v->flags & VID_F_LOOP)) return;   /* looping clips: no seeking */
  double target = play_clock(v) + rel; double dur = vid_duration(v); if (target < 0) target = 0; if (dur > 0 && target > dur - 0.5) target = dur - 0.5; if (target < 0) target = 0;
  if (v->has_video) { int best = 0; for (int i = 0; i < v->nvs; i++) if (v->vs[i].key && v->vs[i].pts <= target) best = i; target = v->vs[best].pts; v->seekpos = best; v->flushreq = true; while (v->flushreq && !v->stop && v->state != VID_ERROR) SDL_Delay(2); }
  if (v->adev) { SDL_ClearQueuedAudio(v->adev); v->aqueued = 0; }
  if (v->akind == 1 || v->akind == 4) { int i = 0; while (i < v->nas && v->as[i].pts < target) i++; v->apos = i; v->audio_eof = false; if (v->aac) avcodec_flush_buffers(v->aac); }
  else if (v->akind == 2 && v->mh) { mpg123_open_feed(v->mh); v->mp3fed = 0; v->mp3fmt = false; v->audio_eof = false; /* mp3 restarts from the top: approximate */ target = 0; }
  v->seek_base = target; v->wall0 = SDL_GetTicks() / 1000.0; v->pause_total = 0; if (v->paused) v->paused_at = v->wall0; v->last_pts = target - 1; if (v->state == VID_ENDED) v->state = v->paused ? VID_PAUSED : VID_PLAYING;
}
double vid_time(const Vid *v) {
  if (!v) return 0; double t = play_clock((Vid *)v), d = vid_duration(v);
  if ((v->flags & VID_F_LOOP) && v->vperiod > 0) { t = fmod(t < 0 ? 0 : t, v->vperiod); return t; }
  if (v->state == VID_ENDED && d > 0) return d; if (d > 0 && t > d) t = d; return t < 0 ? 0 : t;
}
double vid_duration(const Vid *v) { if (!v) return 0; if (v->vdur > 0) return v->vdur; return v->adur; }
int vid_loaded_pct(const Vid *v) { if (!v) return 0; if (v->vdone || (!v->vjob && v->adone)) return 100; if (v->vtotal) return (int)(100.0 * v->vlen / v->vtotal); return v->vlen ? 50 : 0; }
SDL_Texture *vid_frame(Vid *v, int *w, int *h) { if (!v || !v->tex) return NULL; if (w) *w = v->tw; if (h) *h = v->th2; return v->tex; }
bool vid_has_video(const Vid *v) { return v && v->has_video; }
bool vid_audio_unsupported(const Vid *v) { return v && v->akind == 3; }
int vid_loops(const Vid *v) { return v ? v->loops : 0; }
