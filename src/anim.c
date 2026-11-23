/* anim.c - animated images (GIF: own streaming decoder, one canvas in memory; WebP: libwebpdemux WebPAnimDecoder),
 * frames scaled as they are decoded; looping silent MP4 clips and video attachments (video.c, hardware H.264);
 * audio-only attachments (aplay.c, libavformat/libavcodec). See anim.h for the limits. */
#include "anim.h"
#include "video.h"
#include "aplay.h"
#include "nxnet.h"
#include "nx.h"
#include <coreinit/thread.h>
#include <webp/demux.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <strings.h>

static void (*g_alog)(const char *line);
void anim_set_log(void (*fn)(const char *line)) { g_alog = fn; vid_set_log(fn); ap_set_log(fn); }
static void alog(const char *fmt, ...) { if (!g_alog) return; char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_alog(b); }

static const char *k_ua = "User-Agent: MingusDiscord/1.0 (Nintendo Wii U homebrew)";
static SDL_atomic_t g_frame_kb;                                 /* decoded image-animation frames of all open Anims (KB) */
size_t anim_frame_mem(void) { return (size_t)SDL_AtomicGet(&g_frame_kb) << 10; }

typedef struct { uint8_t *rgba; SDL_Texture *tex; int delay; } AFrame;

/* shared between the Anim (main thread) and the download worker that decodes the image; freed by the last owner */
typedef struct {
  SDL_atomic_t ref, cancel; volatile int posted;
  int max_w, max_h;
  AFrame *fr; int nfr, keep, w, h, src_w, src_h, src_frames, step; uint32_t cycle_ms; int mem_kb; bool ok; char err[160]; const char *fmt;
  int *sx, *snx; size_t fbytes;                                 /* scaler tables */
} ImgJob;

enum { K_IMG, K_CLIP, K_VIDEO, K_AUDIO };
struct Anim {
  int kind; Vid *v; APlay *ap; char url[2200];
  ImgJob *gj; NxJob *job; int state; char err[160]; int mem_kb;
  AFrame *fr; int nfr, w, h, cur; uint32_t next_t, cycle_ms, pause_rem; bool started, paused;
};

/* ======================= frame sink: scale + keep every k-th frame within the budget ======================= */
static void scale_box(const uint8_t *src, int W, int H, uint8_t *dst, int w, int h, const int *sx, const int *snx) {   /* premultiplied box filter, <= 4x4 taps */
  for (int dy = 0; dy < h; dy++) { int y0 = (int)((int64_t)dy * H / h), y1 = (int)((int64_t)(dy + 1) * H / h); if (y1 <= y0) y1 = y0 + 1; int ny = y1 - y0 < 4 ? y1 - y0 : 4; int sy[4]; for (int k = 0; k < ny; k++) sy[k] = y0 + k * (y1 - y0) / ny;
    uint8_t *o = dst + (size_t)dy * w * 4;
    for (int dx = 0; dx < w; dx++, o += 4) { uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
      for (int ky = 0; ky < ny; ky++) { const uint8_t *row = src + (size_t)sy[ky] * W * 4; for (int kx = 0; kx < snx[dx]; kx++) { const uint8_t *p = row + (size_t)sx[dx * 4 + kx] * 4; uint32_t al = p[3]; r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al; cnt++; } }
      if (a) { o[0] = (uint8_t)(r / a); o[1] = (uint8_t)(g / a); o[2] = (uint8_t)(b / a); o[3] = (uint8_t)(a / cnt); } else { o[0] = o[1] = o[2] = o[3] = 0; } } }
}
static bool sink_setup(ImgJob *g, int W, int H, int count) {
  double s = 1.0; if (g->max_w > 0 && W > g->max_w) s = (double)g->max_w / W; if (g->max_h > 0 && H * s > g->max_h) s = (double)g->max_h / H;
  int sw = (int)(W * s + 0.5), sh = (int)(H * s + 0.5); if (sw < 1) sw = 1; if (sh < 1) sh = 1; if (sw > W) sw = W; if (sh > H) sh = H;
  size_t fbytes = (size_t)sw * sh * 4; long avail = (long)(ANIM_TOTAL_MEM >> 10) - SDL_AtomicGet(&g_frame_kb); size_t budget = (size_t)sw * sh <= ANIM_SMALL_PIXELS ? ANIM_SMALL_MEM : ANIM_GIF_MEM; if (avail < (long)(budget >> 10)) budget = avail > 0 ? (size_t)avail << 10 : 0;
  int keep = (int)(budget / fbytes); if (keep > ANIM_GIF_MAX_FRAMES) keep = ANIM_GIF_MAX_FRAMES; if (keep < 1) keep = 1; if (keep > count) keep = count;
  g->src_w = W; g->src_h = H; g->src_frames = count; g->w = sw; g->h = sh; g->keep = keep; g->step = (count + keep - 1) / keep; g->fbytes = fbytes;
  g->mem_kb = (int)((fbytes * keep + 1023) >> 10); SDL_AtomicAdd(&g_frame_kb, g->mem_kb);   /* reserve; trimmed to the real use at the end */
  g->fr = calloc((size_t)keep, sizeof(AFrame)); g->sx = malloc(sizeof(int) * 4 * sw); g->snx = malloc(sizeof(int) * sw);
  if (!g->fr || !g->sx || !g->snx) { snprintf(g->err, sizeof g->err, "out of memory (%dx%d)", W, H); return false; }
  for (int dx = 0; dx < sw; dx++) { int x0 = (int)((int64_t)dx * W / sw), x1 = (int)((int64_t)(dx + 1) * W / sw); if (x1 <= x0) x1 = x0 + 1; int nx = x1 - x0 < 4 ? x1 - x0 : 4; g->snx[dx] = nx; for (int k = 0; k < nx; k++) g->sx[dx * 4 + k] = x0 + k * (x1 - x0) / nx; }
  return true;
}
static bool sink_emit(ImgJob *g, int fi, const uint8_t *canvas, int dms) {   /* canvas: src_w x src_h straight RGBA */
  if (fi % g->step == 0 && g->nfr < g->keep) { AFrame *af = &g->fr[g->nfr]; af->rgba = malloc(g->fbytes); if (!af->rgba) return false;
    if (g->w == g->src_w && g->h == g->src_h) memcpy(af->rgba, canvas, g->fbytes); else scale_box(canvas, g->src_w, g->src_h, af->rgba, g->w, g->h, g->sx, g->snx);
    af->delay = dms; g->nfr++; }
  else if (g->nfr) g->fr[g->nfr - 1].delay += dms;
  g->cycle_ms += (uint32_t)dms; OSYieldThread(); return true;
}
static void sink_end(ImgJob *g) {
  free(g->sx); g->sx = NULL; free(g->snx); g->snx = NULL;
  int used = (int)((g->fbytes * g->nfr + 1023) >> 10); SDL_AtomicAdd(&g_frame_kb, used - g->mem_kb); g->mem_kb = used;
  if (g->nfr > 0 && !SDL_AtomicGet(&g->cancel)) g->ok = true; else if (!g->err[0]) snprintf(g->err, sizeof g->err, "%s decode failed", g->fmt);
}

/* ======================= GIF (worker thread) ======================= */
static bool skip_sub(const uint8_t *d, size_t n, size_t *i) { while (*i < n) { uint8_t l = d[(*i)++]; if (!l) return true; *i += l; } return false; }
/* LZW-decode the image data sub-blocks at *pi into out[0..npix); returns pixels produced, *pi ends after the block terminator */
static size_t lzw_decode(const uint8_t *d, size_t n, size_t *pi, int minsize, uint8_t *out, size_t npix, uint16_t *prefix, uint8_t *suffix, uint8_t *stack) {
  size_t i = *pi, op = 0; int blk = 0; bool term = false;
  if (minsize < 1 || minsize > 11) { skip_sub(d, n, &i); *pi = i; return 0; }
  int clear = 1 << minsize, eoi = clear + 1, avail = clear + 2, csize = minsize + 1, cmask = (1 << csize) - 1, old = -1, first = 0;
  for (int c = 0; c < clear; c++) { prefix[c] = 0; suffix[c] = (uint8_t)c; }
  uint32_t bits = 0; int nbits = 0;
  for (;;) {
    while (nbits < csize) { if (blk == 0) { if (i >= n) { term = true; break; } blk = d[i++]; if (!blk) { term = true; break; } } if (i >= n) { term = true; break; } bits |= (uint32_t)d[i++] << nbits; nbits += 8; blk--; }
    if (nbits < csize) break;
    int code = (int)(bits & (uint32_t)cmask); bits >>= csize; nbits -= csize;
    if (code == clear) { avail = clear + 2; csize = minsize + 1; cmask = (1 << csize) - 1; old = -1; continue; }
    if (code == eoi) break;
    if (old < 0) { if (code >= clear) break; if (op < npix) out[op++] = (uint8_t)code; old = first = code; continue; }
    int in = code, sp = 0;
    if (code >= avail) { if (code > avail) break; stack[sp++] = (uint8_t)first; code = old; }
    while (code >= clear && sp < 4095) { stack[sp++] = suffix[code]; code = prefix[code]; }
    if (code >= clear) break;
    first = code; stack[sp++] = (uint8_t)code;
    if (avail < 4096) { prefix[avail] = (uint16_t)old; suffix[avail] = (uint8_t)first; avail++; if (!(avail & cmask) && avail < 4096) { csize++; cmask = (1 << csize) - 1; } }
    old = in;
    while (sp > 0 && op < npix) out[op++] = stack[--sp];
  }
  if (!term) { i += (size_t)blk; skip_sub(d, n, &i); }       /* stopped early (end code / damage): skip the rest of the data */
  *pi = i > n ? n : i; return op;
}
static void gif_decode(ImgJob *g, const uint8_t *d, size_t n) {
  g->fmt = "GIF";
  int W = d[6] | d[7] << 8, H = d[8] | d[9] << 8; uint8_t lsd = d[10]; size_t i = 13;
  uint8_t gct[768]; int gctn = 0; if (lsd & 0x80) { gctn = 2 << (lsd & 7); if (i + (size_t)gctn * 3 > n) { snprintf(g->err, sizeof g->err, "truncated GIF"); return; } memcpy(gct, d + i, (size_t)gctn * 3); i += (size_t)gctn * 3; }
  size_t body = i;
  int count = 0; { size_t k = body; while (k < n) { uint8_t b = d[k++]; if (b == 0x3B) break; if (b == 0x21) { if (k >= n) break; k++; if (!skip_sub(d, n, &k)) break; }   /* pre-scan: count frames */
      else if (b == 0x2C) { if (k + 9 > n) break; int fx = d[k] | d[k + 1] << 8, fy = d[k + 2] | d[k + 3] << 8, fw = d[k + 4] | d[k + 5] << 8, fh = d[k + 6] | d[k + 7] << 8; uint8_t f = d[k + 8]; k += 9;
        if (!W || !H) { if (fx + fw > W) W = fx + fw; if (fy + fh > H) H = fy + fh; } if (f & 0x80) k += (size_t)(2 << (f & 7)) * 3; k++; count++; if (!skip_sub(d, n, &k)) break; }
      else break; } }
  if (!count || W <= 0 || H <= 0) { snprintf(g->err, sizeof g->err, "GIF has no frames"); return; }
  if ((int64_t)W * H > ANIM_GIF_MAX_PIXELS) { snprintf(g->err, sizeof g->err, "GIF too large (%dx%d)", W, H); return; }
  size_t cpx = (size_t)W * H;
  uint8_t *canvas = calloc(cpx, 4), *saved = NULL, *idx = NULL; size_t idxcap = 0; uint16_t *prefix = malloc(4096 * 2); uint8_t *suffix = malloc(4096), *stack = malloc(4096); int *rows = NULL; size_t rowscap = 0;
  if (!sink_setup(g, W, H, count) || !canvas || !prefix || !suffix || !stack) { if (!g->err[0]) snprintf(g->err, sizeof g->err, "out of memory (GIF %dx%d)", W, H); goto done; }
  int disp = 0, trans = -1, delay = 0, prev_disp = 0, px = 0, py = 0, pw = 0, ph = 0, fi = 0;
  i = body;
  while (i < n && !SDL_AtomicGet(&g->cancel)) { uint8_t b = d[i++];
    if (b == 0x3B) break;
    if (b == 0x21) { if (i >= n) break; uint8_t label = d[i++];
      if (label == 0xF9 && i < n && d[i] >= 4 && i + 5 <= n) { const uint8_t *e = d + i + 1; disp = (e[0] >> 2) & 7; delay = e[1] | e[2] << 8; trans = (e[0] & 1) ? e[3] : -1; }
      if (!skip_sub(d, n, &i)) break; continue; }
    if (b != 0x2C) break;
    if (i + 9 > n) break;
    int fx = d[i] | d[i + 1] << 8, fy = d[i + 2] | d[i + 3] << 8, fw = d[i + 4] | d[i + 5] << 8, fh = d[i + 6] | d[i + 7] << 8; uint8_t f = d[i + 8]; i += 9;
    const uint8_t *pal = gct; int paln = gctn; if (f & 0x80) { paln = 2 << (f & 7); if (i + (size_t)paln * 3 > n) break; pal = d + i; i += (size_t)paln * 3; }
    if (i >= n) break; int minsize = d[i++];
    size_t npix = (size_t)fw * fh; if (npix > (size_t)ANIM_GIF_MAX_PIXELS) break;
    if (npix > idxcap) { free(idx); idx = malloc(npix ? npix : 1); idxcap = npix; if (!idx) { idxcap = 0; break; } }
    size_t got = 0; if (npix) got = lzw_decode(d, n, &i, minsize, idx, npix, prefix, suffix, stack); else skip_sub(d, n, &i);
    /* dispose of the previous frame, then draw this one */
    if (prev_disp == 2) { for (int y = py; y < py + ph && y < H; y++) { if (px >= W) break; int x1 = px + pw > W ? W : px + pw; memset(canvas + ((size_t)y * W + px) * 4, 0, (size_t)(x1 - px) * 4); } }
    else if (prev_disp == 3 && saved) memcpy(canvas, saved, cpx * 4);
    if (disp == 3) { if (!saved) saved = malloc(cpx * 4); if (saved) memcpy(saved, canvas, cpx * 4); }
    if ((size_t)fh > rowscap) { free(rows); rows = malloc(sizeof(int) * (fh ? fh : 1)); rowscap = rows ? (size_t)fh : 0; }
    if (rows) { if (f & 0x40) { int k = 0; static const int st[4] = {0, 4, 2, 1}, sp[4] = {8, 8, 4, 2}; for (int p = 0; p < 4; p++) for (int y = st[p]; y < fh; y += sp[p]) rows[k++] = y; } else for (int y = 0; y < fh; y++) rows[y] = y;
      for (int r = 0; r < fh; r++) { size_t base = (size_t)r * fw; if (base >= got) break; int y = fy + rows[r]; if (y >= H) continue; uint8_t *o = canvas + ((size_t)y * W + fx) * 4;
        for (int x = 0; x < fw && fx + x < W && base + x < got; x++, o += 4) { int c = idx[base + x]; if (c == trans || c >= paln) continue; o[0] = pal[c * 3]; o[1] = pal[c * 3 + 1]; o[2] = pal[c * 3 + 2]; o[3] = 255; } } }
    int dms = delay * 10; if (dms < 20) dms = 100;                           /* browsers: tiny/zero delays play at 10 fps */
    if (!sink_emit(g, fi, canvas, dms)) break;
    prev_disp = disp; px = fx; py = fy; pw = fw; ph = fh; disp = 0; trans = -1; delay = 0; fi++;
  }
done:
  sink_end(g);
  free(canvas); free(saved); free(idx); free(prefix); free(suffix); free(stack); free(rows);
}

/* ======================= WebP (worker thread) ======================= */
static void webp_decode(ImgJob *g, const uint8_t *d, size_t n) {
  g->fmt = "WebP";
  WebPAnimDecoderOptions o; if (!WebPAnimDecoderOptionsInit(&o)) { snprintf(g->err, sizeof g->err, "webp: bad library version"); return; }
  o.color_mode = MODE_RGBA; o.use_threads = 0;
  WebPData wd = {d, n}; WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &o); WebPAnimInfo info;
  if (!dec || !WebPAnimDecoderGetInfo(dec, &info)) { snprintf(g->err, sizeof g->err, "not a valid WebP"); if (dec) WebPAnimDecoderDelete(dec); return; }
  int W = (int)info.canvas_width, H = (int)info.canvas_height, count = info.frame_count ? (int)info.frame_count : 1;
  if ((int64_t)W * H > ANIM_GIF_MAX_PIXELS || W <= 0 || H <= 0) { snprintf(g->err, sizeof g->err, "WebP too large (%dx%d)", W, H); WebPAnimDecoderDelete(dec); return; }
  if (sink_setup(g, W, H, count)) {
    int prev = 0, fi = 0;
    while (WebPAnimDecoderHasMoreFrames(dec) && !SDL_AtomicGet(&g->cancel)) { uint8_t *buf; int ts; if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
      int dms = ts - prev; prev = ts; if (dms <= 10) dms = 100; if (count == 1 && dms < 100) dms = 1000;
      if (!sink_emit(g, fi++, buf, dms)) break; }
  }
  WebPAnimDecoderDelete(dec); sink_end(g);
}

static void gj_free(ImgJob *g) { if (!g) return; for (int i = 0; g->fr && i < g->nfr; i++) free(g->fr[i].rgba); free(g->fr); free(g->sx); free(g->snx); if (g->mem_kb) SDL_AtomicAdd(&g_frame_kb, -g->mem_kb); free(g); }
static void gj_unref(ImgJob *g) { if (g && SDL_AtomicDecRef(&g->ref)) gj_free(g); }
static long content_length(const char *h) { for (const char *p = h; p && *p; ) { while (*p == ' ') p++; if (!strncasecmp(p, "content-length:", 15)) return atol(p + 15); p = strchr(p, '\n'); if (p) p++; } return -1; }

static void img_post(NxResp *r, void *ud) {                    /* download worker thread */
  ImgJob *g = ud; OSSetThreadPriority(OSGetCurrentThread(), 26);   /* below the UI and the video decoders */
  uint32_t t0 = SDL_GetTicks(); g->fmt = "image";
  if (!SDL_AtomicGet(&g->cancel)) {
    long clen = content_length(r->headers); const uint8_t *d = (const uint8_t *)r->body; size_t n = r->len;
    if (r->status != 200 && r->status != 206) { if (r->status) snprintf(g->err, sizeof g->err, "HTTP %d", r->status); else snprintf(g->err, sizeof g->err, "network error: %.120s", r->err); }
    else if (n > ANIM_GIF_MAX_FILE || clen > (long)ANIM_GIF_MAX_FILE) snprintf(g->err, sizeof g->err, "image too large (%ld KB, limit %u KB)", (clen > 0 ? clen : (long)n) / 1024, ANIM_GIF_MAX_FILE / 1024);
    else if (n >= 14 && !memcmp(d, "GIF8", 4)) gif_decode(g, d, n);
    else if (n >= 16 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) webp_decode(g, d, n);
    else snprintf(g->err, sizeof g->err, "not a GIF or WebP (%.40s)", r->ctype);
    alog("[anim] %s %u bytes: %dx%d %d frames -> %dx%d %d frames (step %d), cycle %u ms, %d KB, decode %u ms, all frames %u KB: %s", g->fmt, (unsigned)n, g->src_w, g->src_h, g->src_frames, g->w, g->h, g->nfr, g->step, g->cycle_ms, g->mem_kb, (unsigned)(SDL_GetTicks() - t0), (unsigned)SDL_AtomicGet(&g_frame_kb), g->ok ? "ok" : g->err);
  }
  free(r->body); r->body = NULL; r->len = 0;                   /* release the compressed bytes right away */
  g->posted = 1; gj_unref(g);
}

/* ======================= public ======================= */
static Anim *img_open(const char *url, int max_w, int max_h, const char *accept) {
  Anim *a = calloc(1, sizeof *a); if (!a) return NULL; a->kind = K_IMG; a->state = ANIM_LOADING;
  ImgJob *g = calloc(1, sizeof *g); if (!g) { a->state = ANIM_ERROR; snprintf(a->err, sizeof a->err, "out of memory"); return a; }
  g->max_w = max_w; g->max_h = max_h; SDL_AtomicSet(&g->ref, 2); a->gj = g;
  const char *h[] = {k_ua, accept, NULL};
  alog("[anim %p] image %.150s (fit %dx%d)", (void *)a, url, max_w, max_h);
  a->job = nxnet_start_ex("GET", url, h, NULL, 0, ANIM_GIF_MAX_FILE + (64u << 10), img_post, g);
  return a;
}
Anim *anim_open_gif(const char *url, int max_w, int max_h) { return img_open(url, max_w, max_h, "Accept: image/gif,image/webp;q=0.9,image/*;q=0.8"); }
Anim *anim_open_webp(const char *url, int max_w, int max_h) { return img_open(url, max_w, max_h, "Accept: image/webp,image/gif;q=0.9,image/*;q=0.8"); }
Anim *anim_open_clip(const char *mp4_url) {
  Anim *a = calloc(1, sizeof *a); if (!a) return NULL; a->kind = K_CLIP; a->state = ANIM_LOADING;
  alog("[anim %p] clip %.150s", (void *)a, mp4_url); a->v = vid_open_ex(mp4_url, VID_F_LOOP | VID_F_MUTE, ANIM_CLIP_MAX_FILE); return a;
}
/* audio-only by file extension (the query string is ignored) */
static bool audio_ext(const char *url) {
  static const char *ex[] = {"mp3", "m4a", "aac", "ogg", "oga", "opus", "wav", "flac", "weba", "mka", "webm", "alac", "caf", NULL};
  const char *q = strpbrk(url, "?#"); size_t len = q ? (size_t)(q - url) : strlen(url); const char *dot = NULL;
  for (size_t i = 0; i < len; i++) { if (url[i] == '.') dot = url + i; else if (url[i] == '/') dot = NULL; }
  if (!dot) return false; size_t el = len - (size_t)(dot + 1 - url);
  for (int i = 0; ex[i]; i++) if (strlen(ex[i]) == el && !strncasecmp(dot + 1, ex[i], el)) return true;
  return false;
}
Anim *anim_open_video(const char *url) { return anim_open_media(url, NULL); }
Anim *anim_open_media(const char *url, const char *content_type) {
  Anim *a = calloc(1, sizeof *a); if (!a) return NULL; a->state = ANIM_LOADING; snprintf(a->url, sizeof a->url, "%s", url);
  const char *ct = content_type ? content_type : ""; bool h264ish = !strncasecmp(ct, "video/mp4", 9) || !strncasecmp(ct, "video/quicktime", 15) || !strncasecmp(ct, "video/x-m4v", 11);
  bool audio = *ct ? !h264ish : audio_ext(url);                 /* audio/*, video/webm, video/x-matroska... -> libavformat */
  if (audio) { a->kind = K_AUDIO; alog("[anim %p] media (libavformat) %.150s", (void *)a, url); a->ap = ap_open(url, ANIM_VIDEO_MAX_FILE); }
  else { a->kind = K_VIDEO; alog("[anim %p] video %.150s", (void *)a, url); a->v = vid_open_ex(url, VID_F_MP4_ONLY, ANIM_VIDEO_MAX_FILE); }
  return a;
}

static void img_collect(Anim *a) {                              /* the worker finished: adopt its frames */
  NxResp r; nxnet_job_take(a->job, &r); a->job = NULL; ImgJob *g = a->gj; a->gj = NULL;
  if (!g->posted) { snprintf(a->err, sizeof a->err, "download failed: %.100s", r.err[0] ? r.err : "worker"); nxnet_free(&r); a->state = ANIM_ERROR; gj_unref(g); gj_unref(g); return; }
  nxnet_free(&r);
  if (g->ok) { a->fr = g->fr; a->nfr = g->nfr; a->w = g->w; a->h = g->h; a->cycle_ms = g->cycle_ms; a->mem_kb = g->mem_kb; g->fr = NULL; g->nfr = 0; g->mem_kb = 0; a->state = ANIM_READY; }
  else { snprintf(a->err, sizeof a->err, "%s", g->err[0] ? g->err : "image failed"); a->state = ANIM_ERROR; }
  gj_unref(g);
}
static SDL_Texture *img_tex(Anim *a, int i) {
  AFrame *f = &a->fr[i];
  if (!f->tex && f->rgba) { f->tex = SDL_CreateTexture(nx_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a->w, a->h);
    if (f->tex) { SDL_UpdateTexture(f->tex, NULL, f->rgba, a->w * 4); SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND); free(f->rgba); f->rgba = NULL; } }
  return f->tex;
}
void anim_tick(Anim *a) {
  if (!a) return;
  if (a->kind == K_AUDIO) { ap_tick(a->ap); return; }
  if (a->kind != K_IMG) { vid_tick(a->v);
    if (a->kind == K_VIDEO && vid_state(a->v) == VID_ERROR) { const char *e = vid_error(a->v);       /* not H.264 MP4 (webm/vp9, audio-only mp4...): try the generic player */
      if (!strncmp(e, "Unsupported media", 17) || !strncmp(e, "No H.264", 8)) { alog("[anim %p] video player: %s -> trying libavformat (audio only)", (void *)a, e); vid_close(a->v); a->v = NULL; a->kind = K_AUDIO; a->ap = ap_open(a->url, ANIM_VIDEO_MAX_FILE); } }
    return; }
  if (a->state == ANIM_LOADING && a->job && nxnet_job_done(a->job)) img_collect(a);
  if (a->state != ANIM_READY || !a->nfr) return;
  uint32_t now = SDL_GetTicks();
  if (!a->started) { a->started = true; a->cur = 0; a->next_t = now + (uint32_t)a->fr[0].delay; if (a->paused) a->pause_rem = (uint32_t)a->fr[0].delay; }
  else if (!a->paused && a->nfr > 1 && (int32_t)(now - a->next_t) >= 0) {
    for (int guard = 0; (int32_t)(now - a->next_t) >= 0 && guard < a->nfr; guard++) { a->cur = (a->cur + 1) % a->nfr; a->next_t += (uint32_t)a->fr[a->cur].delay; }
    if ((int32_t)(now - a->next_t) >= 0) a->next_t = now + (uint32_t)a->fr[a->cur].delay;   /* not ticked for a while: resync */
  }
  img_tex(a, a->cur); if (a->nfr > 1) img_tex(a, (a->cur + 1) % a->nfr);   /* upload ahead, one frame per tick */
}
SDL_Texture *anim_frame(Anim *a, int *w, int *h) {
  if (!a) return NULL;
  if (a->kind == K_AUDIO) return NULL;
  if (a->kind != K_IMG) return vid_frame(a->v, w, h);
  if (a->state != ANIM_READY || !a->nfr) return NULL;
  SDL_Texture *t = img_tex(a, a->cur);
  if (!t) for (int k = 1; k < a->nfr && !t; k++) t = a->fr[(a->cur + a->nfr - k) % a->nfr].tex;   /* upload failed: show the last good frame */
  if (t) { if (w) *w = a->w; if (h) *h = a->h; }
  return t;
}
static int map_vstate(int s) { switch (s) { case VID_LOADING: return ANIM_LOADING; case VID_ERROR: return ANIM_ERROR; default: return ANIM_READY; } }
int anim_state(const Anim *a) {
  if (!a) return ANIM_ERROR;
  if (a->kind == K_IMG) return a->state;
  if (a->kind == K_AUDIO) return map_vstate(ap_state(a->ap));
  return map_vstate(vid_state(a->v));
}
const char *anim_error(const Anim *a) { if (!a) return "no anim"; if (a->kind == K_AUDIO) return ap_error(a->ap); if (a->kind != K_IMG) return vid_error(a->v); return a->err; }
void anim_toggle(Anim *a) {
  if (!a) return; if (a->kind == K_AUDIO) { ap_toggle(a->ap); return; } if (a->kind != K_IMG) { vid_toggle(a->v); return; }
  uint32_t now = SDL_GetTicks();
  if (!a->paused) { a->paused = true; a->pause_rem = a->started && (int32_t)(a->next_t - now) > 0 ? a->next_t - now : 0; }
  else { a->paused = false; a->next_t = now + a->pause_rem; }
}
void anim_seek(Anim *a, double rel) { if (!a) return; if (a->kind == K_VIDEO) vid_seek(a->v, rel); else if (a->kind == K_AUDIO) ap_seek(a->ap, rel); }
double anim_time(const Anim *a) {
  if (!a) return 0; if (a->kind == K_AUDIO) return ap_time(a->ap); if (a->kind != K_IMG) return vid_time(a->v);
  if (a->state != ANIM_READY || !a->started) return 0;
  uint32_t ms = 0, now = SDL_GetTicks(); for (int i = 0; i < a->cur; i++) ms += (uint32_t)a->fr[i].delay;
  uint32_t rem = a->paused ? a->pause_rem : ((int32_t)(a->next_t - now) > 0 ? a->next_t - now : 0); int d = a->fr[a->cur].delay; ms += (uint32_t)(d > (int)rem ? d - (int)rem : 0);
  return ms / 1000.0;
}
double anim_duration(const Anim *a) { if (!a) return 0; if (a->kind == K_AUDIO) return ap_duration(a->ap); if (a->kind != K_IMG) return vid_duration(a->v); return a->cycle_ms / 1000.0; }
bool anim_paused(const Anim *a) { if (!a) return false; if (a->kind == K_AUDIO) return ap_state(a->ap) == VID_PAUSED; if (a->kind != K_IMG) return vid_state(a->v) == VID_PAUSED; return a->paused; }
bool anim_ended(const Anim *a) { if (!a) return false; if (a->kind == K_AUDIO) return ap_state(a->ap) == VID_ENDED; return a->kind == K_VIDEO && vid_state(a->v) == VID_ENDED; }
bool anim_has_video(const Anim *a) { if (!a) return false; if (a->kind == K_IMG || a->kind == K_CLIP) return true; if (a->kind == K_AUDIO) return false; return vid_has_video(a->v); }
int anim_loaded_pct(const Anim *a) {
  if (!a) return 0; if (a->kind == K_AUDIO) return ap_loaded_pct(a->ap); if (a->kind != K_IMG) return vid_loaded_pct(a->v);
  if (!a->job) return 100; size_t tot = 0, have = nxnet_job_avail(a->job, &tot); return tot ? (int)(100.0 * have / tot) : 0;
}
void anim_close(Anim *a) {
  if (!a) return;
  if (a->v) vid_close(a->v); if (a->ap) ap_close(a->ap);
  if (a->job) { if (nxnet_job_done(a->job)) { img_collect(a); } else { SDL_AtomicSet(&a->gj->cancel, 1); nxnet_job_cancel(a->job); a->job = NULL; gj_unref(a->gj); a->gj = NULL; } }
  for (int i = 0; a->fr && i < a->nfr; i++) { if (a->fr[i].tex) SDL_DestroyTexture(a->fr[i].tex); free(a->fr[i].rgba); }
  if (a->mem_kb) SDL_AtomicAdd(&g_frame_kb, -a->mem_kb);
  free(a->fr); free(a);
}
