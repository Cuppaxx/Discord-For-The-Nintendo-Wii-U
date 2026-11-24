/* voice-message recorder: GamePad mic -> Opus -> Ogg Opus (RFC 7845) in memory, plus Discord's waveform */
#include "rec.h"
#include "dc.h"
#include "voice.h"
#include "dlog.h"
#include <SDL2/SDL.h>
#include <opus/opus.h>
#include <mic/mic.h>
#include <coreinit/thread.h>
#include <coreinit/cache.h>
#include <mbedtls/base64.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RFRAME 960                       /* 20 ms @ 48 kHz */
static volatile int st = REC_IDLE; static volatile bool stop_req, cancel_req; static SDL_Thread *th; static char err[160];
static volatile int lvl; static volatile uint32_t frames; static int max_frames;
static uint8_t *ogg; static size_t ogg_n, ogg_cap; static char wave_b64[400]; static float secs_done;
static uint8_t *amp; static int namp, cap_amp;            /* one peak value per 20 ms frame (for the waveform) */

/* ---- Ogg ---- */
static uint32_t crc_tab[256];
static void crc_init(void) { for (uint32_t i = 0; i < 256; i++) { uint32_t r = i << 24; for (int k = 0; k < 8; k++) r = r & 0x80000000u ? (r << 1) ^ 0x04C11DB7u : r << 1; crc_tab[i] = r; } }
static uint32_t ogg_crc(const uint8_t *d, size_t n) { uint32_t c = 0; for (size_t i = 0; i < n; i++) c = (c << 8) ^ crc_tab[((c >> 24) ^ d[i]) & 255]; return c; }
static bool out_put(const void *d, size_t n) {
  if (ogg_n + n > ogg_cap) { size_t nc = ogg_cap ? ogg_cap * 2 : 65536; while (nc < ogg_n + n) nc *= 2; uint8_t *g = realloc(ogg, nc); if (!g) return false; ogg = g; ogg_cap = nc; }
  memcpy(ogg + ogg_n, d, n); ogg_n += n; return true;
}
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t serial, pageno;
/* one page from up to 255 lacing values worth of packets */
static bool write_page(const uint8_t *const *pk, const int *pl, int np, uint64_t granule, int flags) {
  uint8_t hdr[27 + 255]; int nseg = 0; size_t body = 0;
  for (int i = 0; i < np; i++) { int l = pl[i]; while (l >= 255) { hdr[27 + nseg++] = 255; l -= 255; } hdr[27 + nseg++] = (uint8_t)l; body += pl[i]; }
  memcpy(hdr, "OggS", 4); hdr[4] = 0; hdr[5] = (uint8_t)flags; le64(hdr + 6, granule); le32(hdr + 14, serial); le32(hdr + 18, pageno++); le32(hdr + 22, 0); hdr[26] = (uint8_t)nseg;
  size_t start = ogg_n; if (!out_put(hdr, 27 + nseg)) return false;
  for (int i = 0; i < np; i++) if (!out_put(pk[i], pl[i])) return false;
  le32(ogg + start + 22, ogg_crc(ogg + start, 27 + nseg + body)); return true;
}

/* ---- capture + encode thread ---- */
static inline int16_t soft_clip(int v) { int a = v < 0 ? -v : v; if (a > 16384) a = 16384 + (a - 16384) / 3; if (a > 32767) a = 32767; return (int16_t)(v < 0 ? -a : a); }
static int worker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2);
  bool fake = dc_fakemic(); MICHandle h = -1; MICWorkMemory wm = {0}; int16_t *mb = NULL;
  if (!fake) { wm.sampleMaxCount = 0x2800; wm.sampleBuffer = memalign(0x40, wm.sampleMaxCount * 2); MICError e = MIC_ERROR_OK; h = MICInit(MIC_INSTANCE_0, 0, &wm, &e);
    if (h < 0 || e != MIC_ERROR_OK || MICOpen(h) != MIC_ERROR_OK) { snprintf(err, sizeof err, "Couldn't open the GamePad mic%s", h >= 0 ? " (is a voice call using it?)" : ""); if (h >= 0) MICUninit(h); free(wm.sampleBuffer); st = REC_ERROR; return 0; }
    mb = wm.sampleBuffer; }
  int oe; OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &oe); if (!enc) { snprintf(err, sizeof err, "Opus encoder failed (%d)", oe); st = REC_ERROR; goto out; }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000)); opus_int32 pre = 312; opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&pre));
  { /* headers: OpusHead + OpusTags, each on its own page */
    uint8_t head[19]; memcpy(head, "OpusHead", 8); head[8] = 1; head[9] = 1; head[10] = pre & 255; head[11] = pre >> 8; le32(head + 12, 48000); head[16] = head[17] = 0; head[18] = 0;
    const uint8_t *p1[1] = {head}; int l1[1] = {19}; write_page(p1, l1, 1, 0, 0x02);
    const char *vendor = "DiscordWiiU"; uint8_t tags[64]; memcpy(tags, "OpusTags", 8); le32(tags + 8, (uint32_t)strlen(vendor)); memcpy(tags + 12, vendor, strlen(vendor)); le32(tags + 12 + strlen(vendor), 0);
    const uint8_t *p2[1] = {tags}; int l2[1] = {(int)(16 + strlen(vendor))}; write_page(p2, l2, 1, 0, 0); }
  static int16_t acc[8192]; int accn = 0; static int16_t pcm[RFRAME]; static uint8_t pk[50][400]; int pkl[50]; const uint8_t *pkp[50]; int npk = 0, lace = 0; uint64_t gran = 0;
  float ph = 0; uint32_t t0 = SDL_GetTicks();
  while (!cancel_req) {
    bool last = stop_req || (int)frames >= max_frames;
    if (!last) {
      if (fake) { uint32_t want = (SDL_GetTicks() - t0) * 32; while (accn < 4096 && (uint32_t)(frames * 640 + accn) < want) { float env = 0.5f + 0.5f * sinf((frames * 640 + accn) / 6000.0f); acc[accn++] = (int16_t)(env * 6000 * sinf(ph)); ph += 2 * 3.14159265f * 330 / 32000; } }
      else { MICStatus ms; if (MICGetStatus(h, &ms) != MIC_ERROR_OK) break; int n = ms.availableData; if (n > 0) { DCInvalidateRange(mb, wm.sampleMaxCount * 2); if (n > 4096 - accn) n = 4096 - accn; for (int i = 0; i < n; i++) acc[accn++] = mb[(ms.bufferPos + i) % wm.sampleMaxCount]; MICSetDataConsumed(h, n); } }
    }
    while (accn >= 640 || (last && accn > 0)) {          /* 640 @ 32 kHz = 20 ms */
      int have = accn >= 640 ? 640 : accn; float g = voice_get_mic_gain() * 2.0f; int gi = (int)(g * 256), peak = 0;
      for (int i = 0; i < RFRAME; i++) { float s = i * 640.0f / RFRAME; int i0 = (int)s; float fr = s - i0; int i1 = i0 + 1 < have ? i0 + 1 : i0; int v = i0 < have ? (int)((1 - fr) * acc[i0] + fr * acc[i1]) : 0; pcm[i] = soft_clip((v * gi) >> 8); int a = abs(pcm[i]); if (a > peak) peak = a; }
      memmove(acc, acc + have, (accn - have) * 2); accn -= have; lvl = peak;
      if (namp >= cap_amp) { cap_amp = cap_amp ? cap_amp * 2 : 1024; amp = realloc(amp, cap_amp); } if (amp) amp[namp++] = (uint8_t)(peak >> 7);
      int n = opus_encode(enc, pcm, RFRAME, pk[npk], sizeof pk[0]); if (n < 0) n = 0;
      pkl[npk] = n; pkp[npk] = pk[npk]; lace += n / 255 + 1; npk++; frames++; gran += RFRAME;
      if (npk == 50 || lace > 200) { write_page(pkp, pkl, npk, gran + pre, 0); npk = 0; lace = 0; }
      if ((int)frames >= max_frames) break;
    }
    if (last) break;
    SDL_Delay(5);
  }
  if (!cancel_req) {
    if (!npk) { static uint8_t sil[3] = {0xF8, 0xFF, 0xFE}; pkp[0] = sil; pkl[0] = 3; npk = 1; gran += RFRAME; }
    write_page(pkp, pkl, npk, gran + pre, 0x04);         /* end of stream */
    secs_done = frames * 0.02f;
    /* waveform: up to 256 samples spread over the recording */
    uint8_t wv[256]; int nw = namp < 256 ? namp : 256; for (int i = 0; i < nw; i++) { int a = i * namp / nw, b = (i + 1) * namp / nw, mx = 0; for (int k = a; k < b && k < namp; k++) if (amp[k] > mx) mx = amp[k]; wv[i] = (uint8_t)mx; }
    size_t olen = 0; if (nw) mbedtls_base64_encode((unsigned char *)wave_b64, sizeof wave_b64, &olen, wv, nw); wave_b64[olen] = 0;
    dlog("[rec] done: %.1f s, %u frames, ogg %u bytes, waveform %d", secs_done, (unsigned)frames, (unsigned)ogg_n, nw);
    { char p[300]; snprintf(p, sizeof p, "%s/last_voice_message.ogg", dc_dir()); FILE *f = fopen(p, "wb"); if (f) { fwrite(ogg, 1, ogg_n, f); fclose(f); } }   /* kept for checking */
    st = REC_DONE;
  }
  opus_encoder_destroy(enc);
out:
  if (!fake && h >= 0) { MICClose(h); MICUninit(h); } free(wm.sampleBuffer);
  return 0;
}
bool rec_start(int max_secs) {
  if (st == REC_RECORDING) return false; rec_cancel(); if (!crc_tab[1]) crc_init();
  ogg_n = 0; namp = 0; frames = 0; lvl = 0; err[0] = 0; wave_b64[0] = 0; secs_done = 0; stop_req = cancel_req = false; pageno = 0; serial = (uint32_t)SDL_GetTicks() * 2654435761u;
  max_frames = (max_secs > 0 ? max_secs : 120) * 50; st = REC_RECORDING;
  th = SDL_CreateThreadWithStackSize(worker, "rec", 256 * 1024, NULL); if (!th) { st = REC_ERROR; snprintf(err, sizeof err, "Couldn't start the recorder"); return false; }
  return true;
}
void rec_stop(void) { if (st == REC_RECORDING) { stop_req = true; if (th) { SDL_WaitThread(th, NULL); th = NULL; } } }
void rec_cancel(void) { if (th) { cancel_req = true; SDL_WaitThread(th, NULL); th = NULL; } if (st != REC_ERROR) st = REC_IDLE; }
int rec_state(void) { if (st == REC_RECORDING && th && (int)frames >= max_frames) { rec_stop(); } return st; }
const char *rec_error(void) { return err; }
float rec_seconds(void) { return st == REC_DONE ? secs_done : frames * 0.02f; }
int rec_level(void) { return lvl; }
bool rec_take(const uint8_t **o, size_t *len, float *s, const char **wv) { if (st != REC_DONE || !ogg_n) return false; *o = ogg; *len = ogg_n; *s = secs_done; *wv = wave_b64; return true; }
