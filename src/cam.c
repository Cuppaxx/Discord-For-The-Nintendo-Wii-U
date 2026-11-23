/* cam - GamePad camera capture with a test-pattern fallback. API: cam.h. */
#include <math.h>
#include "cam.h"
#include "dlog.h"
#include <camera/camera.h>
#include <coreinit/time.h>
#include <coreinit/mutex.h>
#include <coreinit/cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define FIRST_FRAME_TIMEOUT_MS 3000

enum { B_QUEUED, B_DONE, B_HELD, B_IDLE };

struct Cam {
  int fps; bool fake; int err; char status[160];
  /* real camera */
  CAMHandle h; bool opened; void *work; uint8_t *buf[CAM_NBUF]; CAMSurface surf[CAM_NBUF];
  volatile int state[CAM_NBUF]; volatile uint32_t bseq[CAM_NBUF]; volatile OSTime btime[CAM_NBUF]; volatile uint32_t done_seq; volatile bool detached; volatile uint32_t failed;
  OSMutex mx; OSTime t_open;
  /* fake */
  uint8_t *fbuf; uint32_t fseq; OSTime t_next;
  uint32_t out_seq; int blank, nonblank;
  OSTime last_time;               /* capture time of the frame last returned by cam_frame */
};

static Cam *volatile g_cam;      /* the camera callback carries no user pointer */

static uint32_t ms_since(OSTime t) { return (uint32_t)OSTicksToMilliseconds(OSGetSystemTime() - t); }

static void cam_event(CAMEventData *e) {
  Cam *c = g_cam; if (!c || !e) return;
  if (e->eventType == CAMERA_DECODE_DONE) {
    OSLockMutex(&c->mx);
    for (int i = 0; i < CAM_NBUF; i++) if (c->buf[i] == (uint8_t *)e->decode.surfaceBuffer) {
      if (e->decode.failed) { c->state[i] = B_IDLE; c->failed++; } else { c->state[i] = B_DONE; c->bseq[i] = ++c->done_seq; c->btime[i] = OSGetSystemTime(); }
      break; }
    OSUnlockMutex(&c->mx);
  } else if (e->eventType == CAMERA_DRC_DETACH) c->detached = true;
}

static void resubmit_idle(Cam *c) {
  for (int i = 0; i < CAM_NBUF; i++) {
    OSLockMutex(&c->mx); bool idle = c->state[i] == B_IDLE; if (idle) c->state[i] = B_QUEUED; OSUnlockMutex(&c->mx);
    if (idle) { CAMError e = CAMSubmitTargetSurface(c->h, &c->surf[i]); if (e != CAMERA_ERROR_OK) { OSLockMutex(&c->mx); c->state[i] = B_IDLE; OSUnlockMutex(&c->mx); c->err = e; } }
  }
}

static void real_close(Cam *c) {
  if (c->opened) { CAMClose(c->h); c->opened = false; }
  if (c->h >= 0) { CAMExit(c->h); c->h = -1; }
  if (g_cam == c) g_cam = NULL;
  for (int i = 0; i < CAM_NBUF; i++) { free(c->buf[i]); c->buf[i] = NULL; }
  free(c->work); c->work = NULL;
}

static bool go_fake(Cam *c, const char *why) {
  real_close(c);
  c->fake = true;
  snprintf(c->status, sizeof c->status, "%s - TEST PATTERN (no camera)", why);
  dlog("[cam] %s", c->status);
  if (!c->fbuf) c->fbuf = memalign(256, CAMERA_YUV_BUFFER_SIZE);
  c->t_next = OSGetSystemTime();
  return c->fbuf != NULL;
}

static bool real_open(Cam *c) {
  CAMStreamInfo si = { .type = CAMERA_STREAM_TYPE_1, .height = CAM_H, .width = CAM_W };
  int32_t need = CAMGetMemReq(&si);
  dlog("[cam] CAMGetMemReq -> %d", (int)need);
  if (need <= 0) { c->err = need; snprintf(c->status, sizeof c->status, "camera: CAMGetMemReq failed (%d)", (int)need); return false; }
  c->work = memalign(256, (size_t)need);
  if (!c->work) { snprintf(c->status, sizeof c->status, "camera: no memory"); return false; }
  CAMError e = CAMCheckMemSegmentation(c->work, (uint32_t)need);
  if (e != CAMERA_ERROR_OK) { c->err = e; snprintf(c->status, sizeof c->status, "camera: work memory rejected (%d)", (int)e); return false; }
  for (int i = 0; i < CAM_NBUF; i++) {
    c->buf[i] = memalign(CAMERA_YUV_BUFFER_ALIGNMENT, CAMERA_YUV_BUFFER_SIZE);
    if (!c->buf[i]) { snprintf(c->status, sizeof c->status, "camera: no memory for surfaces"); return false; }
    memset(c->buf[i], 0, CAMERA_YUV_BUFFER_SIZE);
    DCFlushRange(c->buf[i], CAMERA_YUV_BUFFER_SIZE);   /* no dirty CPU cache lines may later be written back over camera data */
    if ((e = CAMCheckMemSegmentation(c->buf[i], CAMERA_YUV_BUFFER_SIZE)) != CAMERA_ERROR_OK) { c->err = e; snprintf(c->status, sizeof c->status, "camera: surface memory rejected (%d)", (int)e); return false; }
    CAMSurface *s = &c->surf[i]; memset(s, 0, sizeof *s);
    s->surfaceSize = CAMERA_YUV_BUFFER_SIZE; s->surfaceBuffer = c->buf[i]; s->height = CAM_H; s->width = CAM_W; s->pitch = CAM_PITCH;
    s->alignment = CAMERA_YUV_BUFFER_ALIGNMENT; s->tileMode = 0; s->pixelFormat = 0;
    c->state[i] = B_IDLE;
  }
  CAMSetupInfo su; memset(&su, 0, sizeof su);
  su.streamInfo = si; su.workMem.size = (uint32_t)need; su.workMem.pMem = c->work; su.eventHandler = cam_event;
  su.mode.forceDrc = FALSE; su.mode.fps = c->fps >= 30 ? CAMERA_FPS_30 : CAMERA_FPS_15;
  su.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_CPU0;
  g_cam = c;
  CAMError ie = CAMERA_ERROR_OK;
  c->h = CAMInit(0, &su, &ie);
  dlog("[cam] CAMInit -> handle %d err %d", (int)c->h, (int)ie);
  if (c->h < 0 || ie != CAMERA_ERROR_OK) { c->err = ie; snprintf(c->status, sizeof c->status, "camera: CAMInit failed (%d)", (int)ie); if (c->h >= 0 && ie != CAMERA_ERROR_OK) { CAMExit(c->h); } c->h = -1; return false; }
  e = CAMOpen(c->h);
  dlog("[cam] CAMOpen -> %d", (int)e);
  if (e != CAMERA_ERROR_OK) { c->err = e; snprintf(c->status, sizeof c->status, "camera: CAMOpen failed (%d)", (int)e); return false; }
  c->opened = true;
  resubmit_idle(c);
  c->t_open = OSGetSystemTime();
  snprintf(c->status, sizeof c->status, "camera: waiting for first frame");
  return true;
}

Cam *cam_open(int fps) {
  Cam *c = calloc(1, sizeof *c); if (!c) return NULL;
  c->fps = fps >= 30 ? 30 : 15; c->h = -1;
  OSInitMutex(&c->mx);
  if (g_cam) { go_fake(c, "camera: already in use by another Cam"); return c; }
  if (!real_open(c)) { char why[160]; snprintf(why, sizeof why, "%s", c->status); if (!go_fake(c, why)) { cam_close(c); return NULL; } }
  return c;
}

bool cam_is_fake(const Cam *c) { return !c || c->fake; }
const char *cam_status(const Cam *c) { return c ? c->status : "no camera"; }
int cam_error(const Cam *c) { return c ? c->err : 0; }

bool cam_frame(Cam *c, const uint8_t **y, const uint8_t **uv, int *pitch, uint32_t *seq) {
  if (!c) return false;
  if (!c->fake) {
    if (c->detached) { go_fake(c, "camera: GamePad detached"); }
    else {
      int best = -1; uint32_t bs = 0;
      OSLockMutex(&c->mx);
      for (int i = 0; i < CAM_NBUF; i++) if (c->state[i] == B_DONE && (best < 0 || (int32_t)(c->bseq[i] - bs) > 0)) { best = i; bs = c->bseq[i]; }
      if (best >= 0) for (int i = 0; i < CAM_NBUF; i++) if ((c->state[i] == B_DONE && i != best) || c->state[i] == B_HELD) c->state[i] = B_IDLE;   /* older + previously returned go back */
      if (best >= 0) c->state[best] = B_HELD;
      uint32_t ok = c->done_seq; OSUnlockMutex(&c->mx);
      resubmit_idle(c);
      /* the camera (UVD) writes the surface by DMA: drop any stale CPU cache lines of it before reading, otherwise
       * lines of an older frame show through (horizontal stripes seen on real hardware) */
      if (best >= 0) { DCInvalidateRange(c->buf[best], CAMERA_YUV_BUFFER_SIZE); c->last_time = c->btime[best]; }
      if (best >= 0 && !c->nonblank) {                 /* Cemu 2.6 "emulates" the camera with all-zero frames: detect a flat image */
        const uint8_t *b = c->buf[best]; uint8_t v0 = b[0]; bool flat = true;
        for (int k = 0; k < 256 && flat; k++) { size_t o = (size_t)(k * 7919) % CAMERA_YUV_BUFFER_SIZE; if ((o % CAM_PITCH) < CAM_W && b[o] != v0 && !(o >= CAMERA_Y_BUFFER_SIZE && b[o] == b[CAMERA_Y_BUFFER_SIZE])) flat = false; }
        if (!flat) c->nonblank = 1;
        else {                                          /* hold flat frames back; a real sensor may start flat for a moment */
          if (++c->blank >= 10 && ms_since(c->t_open) > FIRST_FRAME_TIMEOUT_MS) { go_fake(c, "camera: only blank frames (no image - emulator?)"); goto pattern; }
          return false; }
      }
      if (best >= 0) {
        if (c->out_seq == 0) { snprintf(c->status, sizeof c->status, "camera: live (GamePad)"); dlog("[cam] first camera frame after %u ms", (unsigned)ms_since(c->t_open)); }
        *y = c->buf[best]; *uv = c->buf[best] + CAMERA_Y_BUFFER_SIZE; *pitch = CAM_PITCH; *seq = ++c->out_seq; return true; }
      if (!ok && ms_since(c->t_open) > FIRST_FRAME_TIMEOUT_MS) go_fake(c, "camera: opened but no frames in 3 s");
      else return false;
    }
  }
pattern:
  /* test pattern, paced to fps */
  OSTime now = OSGetSystemTime(), per = OSMillisecondsToTicks(1000) / c->fps;
  if ((int64_t)(now - c->t_next) < 0) return false;
  c->t_next += per; if ((int64_t)(now - c->t_next) > per * 2) c->t_next = now + per;   /* fell far behind: resync */
  char lab[64]; snprintf(lab, sizeof lab, "TEST PATTERN - NO CAMERA");
  cam_pattern(c->fbuf, c->fbuf + CAMERA_Y_BUFFER_SIZE, CAM_PITCH, CAM_W, CAM_H, c->fseq++, lab);
  *y = c->fbuf; *uv = c->fbuf + CAMERA_Y_BUFFER_SIZE; *pitch = CAM_PITCH; *seq = ++c->out_seq; c->last_time = now;
  return true;
}

uint64_t cam_frame_time_us(const Cam *c) { return c ? (uint64_t)OSTicksToMicroseconds(c->last_time) : 0; }
void cam_counts(const Cam *c, uint32_t *captured, uint32_t *failed) {
  if (captured) *captured = c ? (c->fake ? c->fseq : c->done_seq) : 0;
  if (failed) *failed = c ? c->failed : 0;
}

void cam_close(Cam *c) {
  if (!c) return;
  if (!c->fake) real_close(c);
  free(c->fbuf); free(c);
}

/* ---------------- helpers ---------------- */
void cam_downscale_half(const uint8_t *sy, const uint8_t *suv, int sp, int sw, int sh, uint8_t *dy, uint8_t *duv, int dp) {
  int dw = sw / 2, dh = sh / 2;
  for (int j = 0; j < dh; j++) {
    const uint8_t *a = sy + (size_t)(2 * j) * sp, *b = a + sp; uint8_t *o = dy + (size_t)j * dp;
    for (int i = 0; i < dw; i++) o[i] = (uint8_t)((a[2 * i] + a[2 * i + 1] + b[2 * i] + b[2 * i + 1] + 2) >> 2);
  }
  for (int j = 0; j < dh / 2; j++) {           /* chroma: source has sw/2 UV pairs x sh/2 rows -> dw/2 pairs x dh/2 rows */
    const uint8_t *a = suv + (size_t)(2 * j) * sp, *b = a + sp; uint8_t *o = duv + (size_t)j * dp;
    for (int i = 0; i < dw / 2; i++) { int k = 4 * i;
      o[2 * i] = (uint8_t)((a[k] + a[k + 2] + b[k] + b[k + 2] + 2) >> 2);
      o[2 * i + 1] = (uint8_t)((a[k + 1] + a[k + 3] + b[k + 1] + b[k + 3] + 2) >> 2); }
  }
}

void cam_crop_scale(const uint8_t *sy, const uint8_t *suv, int sp, int cx, int cy, int cw, int ch, uint8_t *dy, uint8_t *duv, int dp, int dw, int dh) {
  cx &= ~1; cy &= ~1;
  uint32_t fx = (uint32_t)(((uint64_t)cw << 16) / dw), fy = (uint32_t)(((uint64_t)ch << 16) / dh);
  for (int j = 0; j < dh; j++) {
    const uint8_t *r = sy + (size_t)(cy + ((j * fy) >> 16)) * sp + cx; uint8_t *o = dy + (size_t)j * dp; uint32_t x = fx / 2;
    for (int i = 0; i < dw; i++, x += fx) o[i] = r[x >> 16];
  }
  for (int j = 0; j < dh / 2; j++) {
    const uint8_t *r = suv + (size_t)(cy / 2 + ((j * fy) >> 16)) * sp + cx; uint8_t *o = duv + (size_t)j * dp; uint32_t x = fx / 2;
    for (int i = 0; i < dw / 2; i++, x += fx) { int s = (int)(x >> 16) * 2; o[2 * i] = r[s]; o[2 * i + 1] = r[s + 1]; }
  }
}

static const struct { char c; uint8_t r[7]; } FONT[] = {
  {'A',{14,17,17,31,17,17,17}},{'B',{30,17,17,30,17,17,30}},{'C',{14,17,16,16,16,17,14}},{'D',{30,17,17,17,17,17,30}},
  {'E',{31,16,16,30,16,16,31}},{'F',{31,16,16,30,16,16,16}},{'G',{14,17,16,23,17,17,15}},{'H',{17,17,17,31,17,17,17}},
  {'I',{14,4,4,4,4,4,14}},{'J',{7,2,2,2,2,18,12}},{'K',{17,18,20,24,20,18,17}},{'L',{16,16,16,16,16,16,31}},
  {'M',{17,27,21,21,17,17,17}},{'N',{17,17,25,21,19,17,17}},{'O',{14,17,17,17,17,17,14}},{'P',{30,17,17,30,16,16,16}},
  {'Q',{14,17,17,17,21,18,13}},{'R',{30,17,17,30,20,18,17}},{'S',{15,16,16,14,1,1,30}},{'T',{31,4,4,4,4,4,4}},
  {'U',{17,17,17,17,17,17,14}},{'V',{17,17,17,17,17,10,4}},{'W',{17,17,17,21,21,21,10}},{'X',{17,17,10,4,10,17,17}},
  {'Y',{17,17,17,10,4,4,4}},{'Z',{31,1,2,4,8,16,31}},
  {'0',{14,17,19,21,25,17,14}},{'1',{4,12,4,4,4,4,14}},{'2',{14,17,1,2,4,8,31}},{'3',{31,2,4,2,1,17,14}},
  {'4',{2,6,10,18,31,2,2}},{'5',{31,16,30,1,1,17,14}},{'6',{6,8,16,30,17,17,14}},{'7',{31,1,2,4,8,8,8}},
  {'8',{14,17,17,14,17,17,14}},{'9',{14,17,17,15,1,2,12}},
  {'-',{0,0,0,31,0,0,0}},{':',{0,12,12,0,12,12,0}},{'.',{0,0,0,0,0,12,12}},{'(',{2,4,8,8,8,4,2}},{')',{8,4,2,2,2,4,8}},
  {'/',{0,1,2,4,8,16,0}},{'#',{10,10,31,10,31,10,10}},{'%',{24,25,2,4,8,19,3}},{'x',{0,0,17,10,4,10,17}},
};

void cam_text(uint8_t *y, uint8_t *uv, int pitch, int w, int h, int x0, int y0, int sc, const char *s) {
  if (sc < 1) sc = 1;
  int n = (int)strlen(s), bw = (n * 6 + 1) * sc, bh = 9 * sc;
  for (int j = y0; j < y0 + bh && j < h; j++) if (j >= 0) for (int i = x0; i < x0 + bw && i < w; i++) if (i >= 0) y[(size_t)j * pitch + i] = 16;
  if (uv) for (int j = y0 / 2; j < (y0 + bh) / 2 && j < h / 2; j++) if (j >= 0) for (int i = x0 & ~1; i < x0 + bw && i < w; i++) if (i >= 0) uv[(size_t)j * pitch + i] = 128;
  for (int k = 0; k < n; k++) {
    char ch = s[k]; if (ch >= 'a' && ch <= 'z' && ch != 'x') ch = (char)(ch - 32);
    const uint8_t *g = NULL; for (size_t f = 0; f < sizeof FONT / sizeof FONT[0]; f++) if (FONT[f].c == ch) { g = FONT[f].r; break; }
    if (!g) continue;
    for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++) if (g[r] & (16 >> b))
      for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
        int px = x0 + (1 + k * 6 + b) * sc + dx, py = y0 + (1 + r) * sc + dy;
        if (px >= 0 && px < w && py >= 0 && py < h) y[(size_t)py * pitch + px] = 235; }
  }
}

/* moving test pattern: scrolling 75% colour bars, luma ramp, a bouncing box, frame counter, label */
void cam_pattern(uint8_t *y, uint8_t *uv, int pitch, int w, int h, uint32_t n, const char *label) {
  static const uint8_t bars[8][3] = { {180,128,128}, {168,44,136}, {145,147,44}, {133,63,52}, {63,193,204}, {51,109,212}, {28,212,120}, {16,128,128} };   /* Y,U,V: white yellow cyan green magenta red blue black */
  int barh = h * 2 / 3, off = (int)(n * 4) % w;
  for (int j = 0; j < h; j++) { uint8_t *r = y + (size_t)j * pitch;
    if (j < barh) for (int i = 0; i < w; i++) r[i] = bars[(((i + off) % w) * 8) / w][0];
    else for (int i = 0; i < w; i++) r[i] = (uint8_t)(16 + (i * 219) / w); }
  for (int j = 0; j < h / 2; j++) { uint8_t *r = uv + (size_t)j * pitch;
    if (j * 2 < barh) for (int i = 0; i < w / 2; i++) { const uint8_t *b = bars[((((2 * i) + off) % w) * 8) / w]; r[2 * i] = b[1]; r[2 * i + 1] = b[2]; }
    else for (int i = 0; i < w; i++) r[i] = 128; }
  /* bouncing box */
  int bs = h / 6, rx = w - bs, ry = h - bs, t = (int)n * 5;
  int bx = (t % (2 * rx)); if (bx > rx) bx = 2 * rx - bx; int by = ((t * 3 / 4) % (2 * ry)); if (by > ry) by = 2 * ry - by;
  bx &= ~1; by &= ~1;
  for (int j = by; j < by + bs; j++) memset(y + (size_t)j * pitch + bx, 235 - ((j - by) * 100 / bs), bs);
  for (int j = by / 2; j < (by + bs) / 2; j++) for (int i = bx; i < bx + bs; i += 2) { uv[(size_t)j * pitch + i] = 90; uv[(size_t)j * pitch + i + 1] = 240; }
  int sc = w >= 600 ? 3 : w >= 400 ? 2 : 1;
  /* text kept inside the central 16:9 band so a 16:9 crop still shows it */
  if (label) cam_text(y, uv, pitch, w, h, 8, h * 3 / 20, sc, label);
  char fb[32]; snprintf(fb, sizeof fb, "FRAME %u", (unsigned)n); cam_text(y, uv, pitch, w, h, 8, h * 17 / 20 - 10 * sc, sc, fb);
}

/* ---- exposure / saturation (lookup tables, rebuilt when the settings change) ---- */
static float adj_ev = 0.0f, adj_sat = 1.0f; static volatile int adj_gen = 1; static int lut_gen; static uint8_t ylut[256], uvlut[256];
void cam_set_adjust(float ev, float sat) { if (ev < -1.5f) ev = -1.5f; if (ev > 1.5f) ev = 1.5f; if (sat < 0) sat = 0; if (sat > 2) sat = 2; adj_ev = ev; adj_sat = sat; adj_gen++; }
void cam_get_adjust(float *ev, float *sat) { if (ev) *ev = adj_ev; if (sat) *sat = adj_sat; }
void cam_adjust(uint8_t *y, uint8_t *uv, int pitch, int w, int h) {
  float ev = adj_ev, sat = adj_sat; if (fabsf(ev) < 0.01f && fabsf(sat - 1.0f) < 0.01f) return;
  if (lut_gen != adj_gen) { lut_gen = adj_gen; float g = powf(2.0f, ev);
    for (int i = 0; i < 256; i++) { float v = 16.0f + (i - 16) * g; if (ev > 0) { float t = (v - 16.0f) / 219.0f; if (t > 0.85f) v = 16.0f + 219.0f * (0.85f + (1.0f - expf(-(t - 0.85f) * 6.0f)) * 0.15f); }   /* soft knee: brighter without blowing out */
      ylut[i] = (uint8_t)(v < 16 ? (i < 16 ? i : 16) : v > 235 ? 235 : v + 0.5f);
      float u = 128.0f + (i - 128) * sat; uvlut[i] = (uint8_t)(u < 16 ? 16 : u > 240 ? 240 : u + 0.5f); } }
  if (fabsf(ev) >= 0.01f) for (int j = 0; j < h; j++) { uint8_t *r = y + (size_t)j * pitch; for (int i = 0; i < w; i++) r[i] = ylut[r[i]]; }
  if (fabsf(sat - 1.0f) >= 0.01f) for (int j = 0; j < h / 2; j++) { uint8_t *r = uv + (size_t)j * pitch; for (int i = 0; i < w; i++) r[i] = uvlut[r[i]]; }
}
