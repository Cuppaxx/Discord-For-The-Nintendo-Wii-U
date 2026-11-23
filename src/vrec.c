/* vrec - video message recorder: camera -> x264, mic -> AAC-LC, muxed into an MP4 in memory. API: vrec.h. */
#include "vrec.h"
#include "venc.h"
#include "cam.h"
#include "dc.h"
#include "dlog.h"
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <mic/mic.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/cache.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AR 32000                 /* GamePad mic rate; AAC encodes it directly */
#define ABITRATE 64000
#define CHUNK_SECS 0.5           /* mdat interleave granularity */

typedef struct { uint32_t off, size, t; uint8_t key; } Samp;      /* t: video = 90 kHz capture time */
typedef struct { uint8_t *d; size_t n, cap; Samp *s; int ns, scap; } Track;

static volatile int st = VREC_IDLE; static volatile bool stop_req, cancel_req, audio_err;
static SDL_Thread *vth, *ath; static SDL_mutex *pmx;
static char err[160];
static int cfg_w = 480, cfg_h = 360, cfg_fps = 15; static int rw, rh, rfps, max_secs, vkbps;
static Track V, A; static uint8_t sps[128], pps[128]; static int spsn, ppsn;
static volatile uint32_t vbytes, abytes, rec_ms; static volatile int lvl; static volatile bool camfake;
static uint8_t asc[16]; static int ascn, apad; static int64_t asamples;
static uint8_t *mp4; static size_t mp4n; static float secs_done;
static uint8_t *pv; static uint32_t pvseq; static OSTime t_start;

static uint32_t elapsed_ms(void) { return (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - t_start); }
static uint32_t elapsed_90k(void) { return (uint32_t)(OSTicksToMicroseconds(OSGetSystemTime() - t_start) * 9 / 100); }

static bool trk_add(Track *t, const uint8_t *d, size_t n, uint32_t ts, bool key) {
  if (t->n + n > t->cap) { size_t nc = t->cap ? t->cap * 2 : 1 << 20; while (nc < t->n + n) nc *= 2; uint8_t *g = realloc(t->d, nc); if (!g) return false; t->d = g; t->cap = nc; }
  if (t->ns >= t->scap) { int nc = t->scap ? t->scap * 2 : 1024; Samp *g = realloc(t->s, nc * sizeof *g); if (!g) return false; t->s = g; t->scap = nc; }
  if (d) memcpy(t->d + t->n, d, n);
  t->s[t->ns++] = (Samp){ (uint32_t)t->n, (uint32_t)n, ts, key }; t->n += n; return true;
}
static void trk_free(Track *t) { free(t->d); free(t->s); memset(t, 0, sizeof *t); }

/* Annex-B AU -> AVCC sample (4-byte lengths); SPS/PPS go to avcC, AUD/SEI dropped */
static bool add_video(const uint8_t *d, size_t n, uint32_t t90, bool key) {
  if (!V.ns && !key) return true;                                   /* the file must start with an IDR */
  size_t start = V.n; if (!trk_add(&V, NULL, n + 16, t90, key)) return false; V.n = start;   /* reserve, then fill */
  size_t i = 0;
  while (i + 3 < n) {
    if (!(d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1)))) { i++; continue; }
    size_t s = i + (d[i + 2] == 1 ? 3 : 4), e = s;
    while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 3 < n && d[e + 2] == 0 && d[e + 3] == 1)))) e++;
    if (e + 3 > n) e = n;
    size_t len = e - s; int t = len ? d[s] & 31 : 0;
    if (t == 7 && len <= sizeof sps) { if (!spsn) { memcpy(sps, d + s, len); spsn = (int)len; } }
    else if (t == 8 && len <= sizeof pps) { if (!ppsn) { memcpy(pps, d + s, len); ppsn = (int)len; } }
    else if (t != 9 && t != 6 && len) { uint8_t *o = V.d + V.n; o[0] = (uint8_t)(len >> 24); o[1] = (uint8_t)(len >> 16); o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len; memcpy(o + 4, d + s, len); V.n += len + 4; }
    i = e;
  }
  V.s[V.ns - 1].size = (uint32_t)(V.n - start); vbytes = (uint32_t)V.n;
  return true;
}

/* ---------------- audio thread: mic / test tone -> AAC ---------------- */
static inline float soft(float x) { float a = fabsf(x); if (a > 0.5f) a = 0.5f + (a - 0.5f) / 3; if (a > 1) a = 1; return x < 0 ? -a : a; }
static int aworker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(OSGetCurrentThread(), 18);
  bool fake = dc_fakemic(); MICHandle h = -1; MICWorkMemory wm = {0}; int16_t *mb = NULL;
  AVCodecContext *c = NULL; AVFrame *fr = NULL; AVPacket *pk = NULL;
  if (!fake) { wm.sampleMaxCount = 0x2800; wm.sampleBuffer = memalign(0x40, wm.sampleMaxCount * 2); MICError e = MIC_ERROR_OK; h = MICInit(MIC_INSTANCE_0, 0, &wm, &e);
    if (h < 0 || e != MIC_ERROR_OK || MICOpen(h) != MIC_ERROR_OK) { snprintf(err, sizeof err, "Couldn't open the GamePad mic%s", h >= 0 ? " (is a voice call using it?)" : ""); if (h >= 0) MICUninit(h); h = -1; audio_err = true; goto out; }
    mb = wm.sampleBuffer; }
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  c = codec ? avcodec_alloc_context3(codec) : NULL;
  if (!c) { snprintf(err, sizeof err, "AAC encoder missing"); audio_err = true; goto out; }
  c->sample_rate = AR; av_channel_layout_default(&c->ch_layout, 1); c->sample_fmt = AV_SAMPLE_FMT_FLTP; c->bit_rate = ABITRATE;
  c->time_base = (AVRational){1, AR}; c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; c->profile = AV_PROFILE_AAC_LOW;
  av_opt_set(c->priv_data, "aac_coder", "fast", 0);                 /* default twoloop is far too slow for Espresso */
  { int r = avcodec_open2(c, codec, NULL); if (r < 0) { snprintf(err, sizeof err, "AAC encoder open failed (%d)", r); audio_err = true; goto out; } }
  ascn = c->extradata_size > 0 && c->extradata_size <= (int)sizeof asc ? c->extradata_size : 0; if (ascn) memcpy(asc, c->extradata, ascn);
  apad = c->initial_padding; int fs = c->frame_size > 0 ? c->frame_size : 1024;
  dlog("[vrec] audio: %s, AAC-LC %d Hz mono %d kbps, frame %d, priming %d, ASC %d bytes", fake ? "TEST TONE (fakemic)" : "GamePad mic", AR, ABITRATE / 1000, fs, apad, ascn);
  fr = av_frame_alloc(); pk = av_packet_alloc();
  if (!fr || !pk) { audio_err = true; goto out; }
  fr->nb_samples = fs; fr->format = AV_SAMPLE_FMT_FLTP; av_channel_layout_copy(&fr->ch_layout, &c->ch_layout); fr->sample_rate = AR;
  if (av_frame_get_buffer(fr, 0) < 0) { audio_err = true; goto out; }
  static int16_t acc[8192]; int accn = 0; int64_t gen = 0; double ph = 0; double enc_ms = 0; int nenc = 0;
  for (;;) {
    bool last = stop_req || cancel_req;
    if (!last) {
      if (fake) { int64_t want = (int64_t)elapsed_ms() * (AR / 1000);
        while (accn < 8192 && gen < want) { int64_t k = gen % AR; bool beep = k < AR / 10;   /* 100 ms 880 Hz beep each second, else a quiet 330 Hz tone */
          ph += 2 * M_PI * (beep ? 880 : 330) / AR; if (ph > 2 * M_PI) ph -= 2 * M_PI; acc[accn++] = (int16_t)((beep ? 9000 : 2500) * sin(ph)); gen++; } }
      else { MICStatus ms; if (MICGetStatus(h, &ms) != MIC_ERROR_OK) { snprintf(err, sizeof err, "GamePad mic stopped"); audio_err = true; break; }
        int n = ms.availableData; if (n > 8192 - accn) n = 8192 - accn; if (n > 0) { DCInvalidateRange(mb, (uint32_t)wm.sampleMaxCount * 2);   /* ring is DMA-written: drop stale cache lines (else near-silence on hardware) */
          for (int i = 0; i < n; i++) acc[accn++] = mb[(ms.bufferPos + i) % wm.sampleMaxCount]; MICSetDataConsumed(h, n); } }
    }
    while (accn >= fs || (last && accn > 0)) {
      int have = accn >= fs ? fs : accn; if (av_frame_make_writable(fr) < 0) break;
      float *o = (float *)fr->data[0]; int peak = 0;
      for (int i = 0; i < fs; i++) { int v = i < have ? acc[i] : 0; int a = v < 0 ? -v : v; if (a > peak) peak = a; o[i] = fake ? v / 32768.0f : soft(v * 2 / 32768.0f); }
      lvl = peak; memmove(acc, acc + have, (size_t)(accn - have) * 2); accn -= have;
      fr->pts = asamples; asamples += have;
      OSTime t0 = OSGetSystemTime();
      if (avcodec_send_frame(c, fr) == 0) while (avcodec_receive_packet(c, pk) == 0) { trk_add(&A, pk->data, (size_t)pk->size, 0, true); abytes = (uint32_t)A.n; av_packet_unref(pk); }
      enc_ms += OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1000.0; nenc++;
    }
    if (last) break;
    SDL_Delay(5);
  }
  if (!cancel_req) { avcodec_send_frame(c, NULL); while (avcodec_receive_packet(c, pk) == 0) { trk_add(&A, pk->data, (size_t)pk->size, 0, true); av_packet_unref(pk); } abytes = (uint32_t)A.n; }
  dlog("[vrec] audio done: %lld samples, %d AAC packets, %u bytes, AAC encode %.2f ms per 1024-sample frame", (long long)asamples, A.ns, (unsigned)A.n, nenc ? enc_ms / nenc : 0.0);
out:
  av_packet_free(&pk); av_frame_free(&fr); avcodec_free_context(&c);
  if (h >= 0) { MICClose(h); MICUninit(h); }
  free(wm.sampleBuffer);
  return 0;
}

/* ---------------- MP4 writer ---------------- */
typedef struct { uint8_t *d; size_t n, cap; bool bad; } Buf;
static void bput(Buf *b, const void *p, size_t n) {
  if (b->bad) return;
  if (b->n + n > b->cap) { size_t nc = b->cap ? b->cap * 2 : 65536; while (nc < b->n + n) nc *= 2; uint8_t *g = realloc(b->d, nc); if (!g) { b->bad = true; return; } b->d = g; b->cap = nc; }
  if (p) memcpy(b->d + b->n, p, n); else memset(b->d + b->n, 0, n);
  b->n += n;
}
static void u8(Buf *b, uint32_t v) { uint8_t x = (uint8_t)v; bput(b, &x, 1); }
static void u16(Buf *b, uint32_t v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; bput(b, x, 2); }
static void u32(Buf *b, uint32_t v) { uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, x, 4); }
static void zeros(Buf *b, size_t n) { bput(b, NULL, n); }
static size_t box(Buf *b, const char *t) { size_t at = b->n; u32(b, 0); bput(b, t, 4); return at; }
static size_t fbox(Buf *b, const char *t, int ver, uint32_t flags) { size_t at = box(b, t); u32(b, ((uint32_t)ver << 24) | flags); return at; }
static void endbox(Buf *b, size_t at) { if (b->bad) return; uint32_t s = (uint32_t)(b->n - at); b->d[at] = (uint8_t)(s >> 24); b->d[at + 1] = (uint8_t)(s >> 16); b->d[at + 2] = (uint8_t)(s >> 8); b->d[at + 3] = (uint8_t)s; }
static void matrix(Buf *b) { u32(b, 0x10000); u32(b, 0); u32(b, 0); u32(b, 0); u32(b, 0x10000); u32(b, 0); u32(b, 0); u32(b, 0); u32(b, 0x40000000); }

typedef struct { int trk, first, n; uint32_t off; } Chunk;
static Chunk *chunks; static int nchunks;

static void w_hdlr(Buf *b, const char *type, const char *name) { size_t h = fbox(b, "hdlr", 0, 0); u32(b, 0); bput(b, type, 4); zeros(b, 12); bput(b, name, strlen(name) + 1); endbox(b, h); }
static void w_dinf(Buf *b) { size_t d = box(b, "dinf"); size_t r = fbox(b, "dref", 0, 0); u32(b, 1); size_t u = fbox(b, "url ", 0, 1); endbox(b, u); endbox(b, r); endbox(b, d); }
static void w_stsc_stco(Buf *b, int trk, uint32_t base) {
  size_t s = fbox(b, "stsc", 0, 0); size_t cntat = b->n; u32(b, 0); uint32_t ent = 0; int prev = -1, ci = 0;
  for (int i = 0; i < nchunks; i++) if (chunks[i].trk == trk) { ci++; if (chunks[i].n != prev) { u32(b, (uint32_t)ci); u32(b, (uint32_t)chunks[i].n); u32(b, 1); ent++; prev = chunks[i].n; } }
  if (!b->bad) { b->d[cntat] = (uint8_t)(ent >> 24); b->d[cntat + 1] = (uint8_t)(ent >> 16); b->d[cntat + 2] = (uint8_t)(ent >> 8); b->d[cntat + 3] = (uint8_t)ent; }
  endbox(b, s);
  s = fbox(b, "stco", 0, 0); u32(b, (uint32_t)ci); for (int i = 0; i < nchunks; i++) if (chunks[i].trk == trk) u32(b, base + chunks[i].off); endbox(b, s);
}
static void w_stsz(Buf *b, const Track *t) { size_t s = fbox(b, "stsz", 0, 0); u32(b, 0); u32(b, (uint32_t)t->ns); for (int i = 0; i < t->ns; i++) u32(b, t->s[i].size); endbox(b, s); }
static void w_tkhd(Buf *b, int id, uint32_t dur_ms, bool audio) {
  size_t t = fbox(b, "tkhd", 0, 3); u32(b, 0); u32(b, 0); u32(b, (uint32_t)id); u32(b, 0); u32(b, dur_ms); zeros(b, 8); u16(b, 0); u16(b, audio ? 1 : 0); u16(b, audio ? 0x0100 : 0); u16(b, 0); matrix(b);
  u32(b, audio ? 0 : (uint32_t)rw << 16); u32(b, audio ? 0 : (uint32_t)rh << 16); endbox(b, t);
}
static void w_mdhd(Buf *b, uint32_t ts, uint32_t dur) { size_t m = fbox(b, "mdhd", 0, 0); u32(b, 0); u32(b, 0); u32(b, ts); u32(b, dur); u16(b, 0x55C4); u16(b, 0); endbox(b, m); }

static void build_moov(Buf *b, uint32_t base, uint32_t vdur90, uint32_t adur_media, uint32_t adur_play) {
  uint32_t vms = (uint32_t)((uint64_t)vdur90 / 90), ams = (uint32_t)((uint64_t)adur_play * 1000 / AR), mms = vms > ams ? vms : ams;
  size_t moov = box(b, "moov");
  { size_t m = fbox(b, "mvhd", 0, 0); u32(b, 0); u32(b, 0); u32(b, 1000); u32(b, mms); u32(b, 0x10000); u16(b, 0x0100); zeros(b, 10); matrix(b); zeros(b, 24); u32(b, A.ns ? 3 : 2); endbox(b, m); }
  /* video */
  { size_t tr = box(b, "trak"); w_tkhd(b, 1, vms, false);
    size_t md = box(b, "mdia"); w_mdhd(b, 90000, vdur90); w_hdlr(b, "vide", "VideoHandler");
    size_t mi = box(b, "minf"); { size_t v = fbox(b, "vmhd", 0, 1); zeros(b, 8); endbox(b, v); } w_dinf(b);
    size_t sb = box(b, "stbl");
    { size_t sd = fbox(b, "stsd", 0, 0); u32(b, 1);
      size_t a = box(b, "avc1"); zeros(b, 6); u16(b, 1); zeros(b, 16); u16(b, (uint32_t)rw); u16(b, (uint32_t)rh); u32(b, 0x00480000); u32(b, 0x00480000); u32(b, 0); u16(b, 1);
      { uint8_t cn[32] = {0}; const char *nm = "x264 (Wii U)"; cn[0] = (uint8_t)strlen(nm); memcpy(cn + 1, nm, strlen(nm)); bput(b, cn, 32); }
      u16(b, 0x18); u16(b, 0xFFFF);
      size_t c = box(b, "avcC"); u8(b, 1); u8(b, sps[1]); u8(b, sps[2]); u8(b, sps[3]); u8(b, 0xFF); u8(b, 0xE1); u16(b, (uint32_t)spsn); bput(b, sps, spsn); u8(b, 1); u16(b, (uint32_t)ppsn); bput(b, pps, ppsn); endbox(b, c);
      endbox(b, a); endbox(b, sd); }
    { size_t s = fbox(b, "stts", 0, 0); size_t cntat = b->n; u32(b, 0); uint32_t ent = 0, run = 0, cur = 0;
      for (int i = 0; i < V.ns; i++) { uint32_t d = i + 1 < V.ns ? V.s[i + 1].t - V.s[i].t : 90000 / rfps; if (!d) d = 1;
        if (run && d == cur) run++; else { if (run) { u32(b, run); u32(b, cur); ent++; } cur = d; run = 1; } }
      if (run) { u32(b, run); u32(b, cur); ent++; }
      if (!b->bad) { b->d[cntat] = (uint8_t)(ent >> 24); b->d[cntat + 1] = (uint8_t)(ent >> 16); b->d[cntat + 2] = (uint8_t)(ent >> 8); b->d[cntat + 3] = (uint8_t)ent; }
      endbox(b, s); }
    { int nk = 0; for (int i = 0; i < V.ns; i++) nk += V.s[i].key; size_t s = fbox(b, "stss", 0, 0); u32(b, (uint32_t)nk); for (int i = 0; i < V.ns; i++) if (V.s[i].key) u32(b, (uint32_t)i + 1); endbox(b, s); }
    w_stsz(b, &V); w_stsc_stco(b, 0, base);
    endbox(b, sb); endbox(b, mi); endbox(b, md); endbox(b, tr); }
  /* audio */
  if (A.ns) { size_t tr = box(b, "trak"); w_tkhd(b, 2, ams, true);
    { size_t e = box(b, "edts"); size_t l = fbox(b, "elst", 0, 0); u32(b, 1); u32(b, ams); u32(b, (uint32_t)apad); u16(b, 1); u16(b, 0); endbox(b, l); endbox(b, e); }   /* skip the encoder priming */
    size_t md = box(b, "mdia"); w_mdhd(b, AR, adur_media); w_hdlr(b, "soun", "SoundHandler");
    size_t mi = box(b, "minf"); { size_t s = fbox(b, "smhd", 0, 0); u16(b, 0); u16(b, 0); endbox(b, s); } w_dinf(b);
    size_t sb = box(b, "stbl");
    { size_t sd = fbox(b, "stsd", 0, 0); u32(b, 1);
      size_t a = box(b, "mp4a"); zeros(b, 6); u16(b, 1); zeros(b, 8); u16(b, 1); u16(b, 16); u16(b, 0); u16(b, 0); u32(b, (uint32_t)AR << 16);
      size_t e = fbox(b, "esds", 0, 0);
      int dsi = 2 + ascn, dcd = 13 + dsi, es = 3 + (2 + dcd) + 3;
      u8(b, 3); u8(b, (uint32_t)es); u16(b, 2); u8(b, 0);
      u8(b, 4); u8(b, (uint32_t)dcd); u8(b, 0x40); u8(b, 0x15); u8(b, 0); u16(b, 0x0600); u32(b, ABITRATE); u32(b, ABITRATE);
      u8(b, 5); u8(b, (uint32_t)ascn); bput(b, asc, ascn);
      u8(b, 6); u8(b, 1); u8(b, 2);
      endbox(b, e); endbox(b, a); endbox(b, sd); }
    { size_t s = fbox(b, "stts", 0, 0); u32(b, 1); u32(b, (uint32_t)A.ns); u32(b, 1024); endbox(b, s); }
    w_stsz(b, &A); w_stsc_stco(b, 1, base);
    endbox(b, sb); endbox(b, mi); endbox(b, md); endbox(b, tr); }
  endbox(b, moov);
}

static bool mux(void) {
  if (!V.ns || !spsn || !ppsn) { snprintf(err, sizeof err, "no video was recorded"); return false; }
  /* plan interleaved chunks of ~CHUNK_SECS */
  free(chunks); chunks = NULL; nchunks = 0; int cap = 0;
  V.s[0].t = 0;            /* first picture is shown from t=0 (held until the 2nd frame): keeps A/V sync with the audio, which starts at 0 */
  int vi = 0, ai = 0; uint32_t off = 0;
  for (int k = 1; vi < V.ns || ai < A.ns; k++) {
    double tend = k * CHUNK_SECS;
    for (int trk = 0; trk < 2; trk++) {
      int first = trk ? ai : vi, j = first;
      if (!trk) while (j < V.ns && V.s[j].t < tend * 90000) j++;
      else while (j < A.ns && (double)j * 1024 < tend * AR) j++;
      if (j == first) continue;
      if (nchunks >= cap) { cap = cap ? cap * 2 : 256; Chunk *g = realloc(chunks, cap * sizeof *g); if (!g) return false; chunks = g; }
      Track *t = trk ? &A : &V; uint32_t bytes = 0; for (int i = first; i < j; i++) bytes += t->s[i].size;
      chunks[nchunks++] = (Chunk){ trk, first, j - first, off }; off += bytes;
      if (trk) ai = j; else vi = j;
    }
  }
  uint32_t vdur = V.s[V.ns - 1].t + 90000 / rfps;
  uint32_t adur_media = (uint32_t)A.ns * 1024, adur_play = asamples > 0 ? (uint32_t)asamples : 0;
  if (adur_play + apad > adur_media) adur_play = adur_media > (uint32_t)apad ? adur_media - apad : 0;
  Buf ftyp = {0}; { size_t f = box(&ftyp, "ftyp"); bput(&ftyp, "isom", 4); u32(&ftyp, 0x200); bput(&ftyp, "isomiso2avc1mp41", 16); endbox(&ftyp, f); }
  Buf mv = {0}; build_moov(&mv, 0, vdur, adur_media, adur_play);   /* size pass */
  uint32_t base = (uint32_t)(ftyp.n + mv.n + 8); mv.n = 0; build_moov(&mv, base, vdur, adur_media, adur_play);
  Buf out = {0}; bput(&out, ftyp.d, ftyp.n); bput(&out, mv.d, mv.n);
  u32(&out, 8 + off); bput(&out, "mdat", 4);
  for (int i = 0; i < nchunks; i++) { Track *t = chunks[i].trk ? &A : &V; Samp *s0 = &t->s[chunks[i].first], *s1 = &t->s[chunks[i].first + chunks[i].n - 1]; bput(&out, t->d + s0->off, s1->off + s1->size - s0->off); }
  bool ok = !ftyp.bad && !mv.bad && !out.bad;
  free(ftyp.d); free(mv.d);
  if (!ok) { free(out.d); snprintf(err, sizeof err, "out of memory while writing the MP4"); return false; }
  mp4 = out.d; mp4n = out.n; secs_done = vdur / 90000.0f;
  return true;
}

/* ---------------- video thread ---------------- */
static int vworker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0); OSSetThreadPriority(OSGetCurrentThread(), 20);
  Cam *cam = cam_open(rfps); camfake = cam_is_fake(cam);
  VencOpts o = { rw, rh, rfps, vkbps, 0, 2, 2000 }; Venc *v = cam ? venc_open_ex(&o) : NULL;
  uint8_t *buf = memalign(64, (size_t)rw * rh * 3 / 2);
  dlog("[vrec] start %dx%d @%d fps, video %d kbps, max %d s, camera: %s", rw, rh, rfps, vkbps, max_secs, cam ? cam_status(cam) : "-");
  if (!cam || !v || !buf) { snprintf(err, sizeof err, "couldn't start the camera/encoder"); audio_err = true; }
  uint8_t *ny = buf, *nuv = buf + (size_t)rw * rh; double enc_sum = 0; int enc_n = 0, frames_in = 0;
  /* source crop with the target's aspect ratio */
  int cw = CAM_W, ch = CAM_H; if (rw * CAM_H > rh * CAM_W) ch = (CAM_W * rh / rw) & ~1; else cw = (CAM_H * rw / rh) & ~1;
  int cx = ((CAM_W - cw) / 2) & ~1, cy = ((CAM_H - ch) / 2) & ~1;
  while (!stop_req && !cancel_req && !audio_err) {
    uint32_t ms = elapsed_ms(); rec_ms = ms;
    if (ms >= (uint32_t)max_secs * 1000 || vbytes + abytes + 65536 >= VREC_MAX_BYTES) { dlog("[vrec] limit reached (%u ms, %u bytes)", (unsigned)ms, (unsigned)(vbytes + abytes)); break; }
    const uint8_t *y, *uv; int pitch; uint32_t seq;
    if (!cam_frame(cam, &y, &uv, &pitch, &seq)) { SDL_Delay(2); continue; }
    uint32_t t90 = elapsed_90k(); frames_in++; camfake = cam_is_fake(cam);
    if (rw * 2 == CAM_W && rh * 2 == CAM_H) cam_downscale_half(y, uv, pitch, CAM_W, CAM_H, ny, nuv, rw);
    else cam_crop_scale(y, uv, pitch, cx, cy, cw, ch, ny, nuv, rw, rw, rh);
    cam_adjust(ny, nuv, rw, rw, rh);
    SDL_LockMutex(pmx); if (pv) memcpy(pv, buf, (size_t)rw * rh * 3 / 2); pvseq++; SDL_UnlockMutex(pmx);
    VencOut out; int n = venc_encode(v, ny, nuv, rw, rw, false, &out);
    if (n > 0) { enc_sum += out.enc_ms; enc_n++; if (!add_video(out.data, out.size, t90, out.keyframe)) { snprintf(err, sizeof err, "out of memory"); audio_err = true; } }
  }
  if (st == VREC_RECORDING) st = VREC_FINISHING;
  stop_req = true;
  if (ath) { SDL_WaitThread(ath, NULL); ath = NULL; }
  dlog("[vrec] video: %d camera frames, %d encoded (avg %.1f ms), %u bytes", frames_in, enc_n, enc_n ? enc_sum / enc_n : 0.0, (unsigned)V.n);
  venc_close(v); cam_close(cam); free(buf);
  if (cancel_req) return 0;
  if (audio_err) { if (!err[0]) snprintf(err, sizeof err, "recording failed"); st = VREC_ERROR; return 0; }
  OSTime t0 = OSGetSystemTime();
  if (mux()) { dlog("[vrec] MP4 %u bytes, %.2f s, %d video + %d audio samples, %d chunks, mux %.1f ms", (unsigned)mp4n, secs_done, V.ns, A.ns, nchunks, OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1000.0); st = VREC_DONE; }
  else st = VREC_ERROR;
  trk_free(&V); trk_free(&A); free(chunks); chunks = NULL; nchunks = 0;
  return 0;
}

/* ---------------- API ---------------- */
void vrec_config(int w, int h, int fps) {
  if (w >= 64 && h >= 64 && w <= CAM_W && h <= CAM_H) { cfg_w = w & ~1; cfg_h = h & ~1; }
  cfg_fps = fps >= 30 ? 30 : 15;
}
static void join_all(void) {
  if (vth) { SDL_WaitThread(vth, NULL); vth = NULL; }
  if (ath) { SDL_WaitThread(ath, NULL); ath = NULL; }
}
bool vrec_start(int secs) {
  if (st == VREC_RECORDING || st == VREC_FINISHING) return false;
  join_all(); free(mp4); mp4 = NULL; mp4n = 0; trk_free(&V); trk_free(&A);
  if (!pmx) pmx = SDL_CreateMutex();
  rw = cfg_w; rh = cfg_h; rfps = cfg_fps; max_secs = secs > 0 ? (secs > 120 ? 120 : secs) : VREC_DEFAULT_SECS;
  /* video bitrate: quality target by pixel rate, capped so max_secs fit in VREC_MAX_BYTES (8% container/VBV margin) */
  int want = rw * rh * rfps / 2200; if (want < 300) want = 300; if (want > 2500) want = 2500;
  int budget = (int)((double)VREC_MAX_BYTES * 8 / 1000 * 0.92 / max_secs) - ABITRATE / 1000 - 8;
  vkbps = want < budget ? want : budget; if (vkbps < 100) vkbps = 100;
  free(pv); pv = malloc((size_t)rw * rh * 3 / 2); pvseq = 0;
  spsn = ppsn = 0; ascn = 0; apad = 0; asamples = 0; vbytes = abytes = rec_ms = 0; lvl = 0; err[0] = 0; secs_done = 0; camfake = false;
  stop_req = cancel_req = audio_err = false; t_start = OSGetSystemTime(); st = VREC_RECORDING;
  ath = SDL_CreateThreadWithStackSize(aworker, "vrec-a", 256 * 1024, NULL);
  vth = SDL_CreateThreadWithStackSize(vworker, "vrec", 512 * 1024, NULL);
  if (!ath || !vth) { cancel_req = true; join_all(); snprintf(err, sizeof err, "Couldn't start the recorder"); st = VREC_ERROR; return false; }
  return true;
}
void vrec_stop(void) { if (st == VREC_RECORDING) stop_req = true; }
void vrec_cancel(void) {
  cancel_req = true; join_all(); trk_free(&V); trk_free(&A); free(chunks); chunks = NULL; nchunks = 0;
  free(mp4); mp4 = NULL; mp4n = 0; if (st != VREC_ERROR) st = VREC_IDLE;
}
int vrec_state(void) { return st; }
const char *vrec_error(void) { return err; }
float vrec_seconds(void) { return st == VREC_DONE ? secs_done : rec_ms / 1000.0f; }
int vrec_level(void) { return lvl; }
bool vrec_cam_fake(void) { return camfake; }
bool vrec_preview(uint8_t *rgba, int w, int h, uint32_t *seq) {
  if (!pmx || !pv || !rgba || w <= 0 || h <= 0) return false;
  SDL_LockMutex(pmx);
  if (pvseq == 0 || (seq && *seq == pvseq)) { SDL_UnlockMutex(pmx); return false; }
  const uint8_t *py = pv, *puv = pv + (size_t)rw * rh;
  for (int j = 0; j < h; j++) { int sy = j * rh / h; const uint8_t *yr = py + (size_t)sy * rw, *ur = puv + (size_t)(sy / 2) * rw; uint8_t *o = rgba + (size_t)j * w * 4;
    for (int i = 0; i < w; i++) { int sx = i * rw / w, cx = sx & ~1, u = ur[cx] - 128, v = ur[cx + 1] - 128, yy = 1192 * (yr[sx] - 16);
      int r = (yy + 1634 * v) >> 10, g = (yy - 833 * u - 400 * v) >> 10, b = (yy + 2066 * u) >> 10;
      o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r); o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g); o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b); o[3] = 255; o += 4; } }
  if (seq) *seq = pvseq;
  SDL_UnlockMutex(pmx); return true;
}
bool vrec_take(uint8_t **o, size_t *len, int *w, int *h, float *secs) {
  if (st != VREC_DONE || !mp4) return false;
  join_all();
  *o = mp4; *len = mp4n; if (w) *w = rw; if (h) *h = rh; if (secs) *secs = secs_done;
  mp4 = NULL; mp4n = 0; st = VREC_IDLE; return true;
}
