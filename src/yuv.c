/* yuv.c - fast NV12 -> RGBA, see yuv.h */
#include "yuv.h"
#include <SDL2/SDL.h>
#include <coreinit/thread.h>
#include <string.h>

static int32_t TY[256], TRV[256], TGU[256], TGV[256], TBU[256];
static uint8_t CLIP[1024];          /* index (v >> 10) + 384 */
static bool inited;
static void init(void) {
  for (int i = 0; i < 256; i++) { TY[i] = 1192 * (i - 16); TRV[i] = 1634 * (i - 128); TGU[i] = -833 * (i - 128); TGV[i] = -400 * (i - 128); TBU[i] = 2066 * (i - 128); }
  for (int i = 0; i < 1024; i++) { int v = i - 384; CLIP[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
  inited = true;
}
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PIX(r, g, b) ((uint32_t)(r) << 24 | (uint32_t)(g) << 16 | (uint32_t)(b) << 8 | 0xFFu)
#else
#define PIX(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xFF000000u)
#endif
#define CL(v) CLIP[((v) >> 10) + 384]

/* rows [r0, r1) of the output */
static void conv_rows(const uint8_t *y, const uint8_t *uv, int pitch, int sw, int sh, uint8_t *dst, int dpitch, int ow, int oh, int r0, int r1) {
  if (ow == sw && oh == sh) {                 /* 1:1 - two rows at a time, one UV pair per 2x2 block */
    for (int j = r0; j < r1; j += 2) {
      const uint8_t *y0 = y + (size_t)j * pitch, *y1 = y0 + pitch, *u = uv + (size_t)(j >> 1) * pitch;
      uint32_t *o0 = (uint32_t *)(dst + (size_t)j * dpitch), *o1 = (uint32_t *)(dst + (size_t)(j + 1) * dpitch); bool two = j + 1 < r1;
      int i = 0;
      for (; i + 1 < ow; i += 2) {
        int U = u[i], V = u[i + 1]; int rv = TRV[V], gc = TGU[U] + TGV[V], bu = TBU[U]; int a;
        a = TY[y0[i]];     o0[i]     = PIX(CL(a + rv), CL(a + gc), CL(a + bu));
        a = TY[y0[i + 1]]; o0[i + 1] = PIX(CL(a + rv), CL(a + gc), CL(a + bu));
        if (two) { a = TY[y1[i]]; o1[i] = PIX(CL(a + rv), CL(a + gc), CL(a + bu)); a = TY[y1[i + 1]]; o1[i + 1] = PIX(CL(a + rv), CL(a + gc), CL(a + bu)); }
      }
      if (i < ow) { int U = u[i & ~1], V = u[(i & ~1) + 1]; int rv = TRV[V], gc = TGU[U] + TGV[V], bu = TBU[U]; int a = TY[y0[i]]; o0[i] = PIX(CL(a + rv), CL(a + gc), CL(a + bu));
        if (two) { a = TY[y1[i]]; o1[i] = PIX(CL(a + rv), CL(a + gc), CL(a + bu)); } }
    }
    return;
  }
  /* downscale: nearest neighbour, x map in 16.16 fixed point */
  uint32_t xs = (uint32_t)(((uint64_t)sw << 16) / ow), ys = (uint32_t)(((uint64_t)sh << 16) / oh);
  for (int j = r0; j < r1; j++) {
    int sy = (int)(((uint32_t)j * ys) >> 16); const uint8_t *yr = y + (size_t)sy * pitch, *ur = uv + (size_t)(sy >> 1) * pitch; uint32_t *o = (uint32_t *)(dst + (size_t)j * dpitch);
    uint32_t fx = xs >> 1;                      /* sample pixel centres */
    for (int i = 0; i < ow; i++, fx += xs) { int sx = (int)(fx >> 16); int cx = sx & ~1; int rv = TRV[ur[cx + 1]], gc = TGU[ur[cx]] + TGV[ur[cx + 1]], bu = TBU[ur[cx]]; int a = TY[yr[sx]];
      o[i] = PIX(CL(a + rv), CL(a + gc), CL(a + bu)); }
  }
}

/* helper thread for the second half of big frames */
typedef struct { const uint8_t *y, *uv; int pitch, sw, sh; uint8_t *dst; int dpitch, ow, oh, r0, r1; } Job;
static SDL_sem *go, *done; static SDL_Thread *helper; static Job job; static SDL_SpinLock hl;
static int helper_main(void *a) {
  (void)a; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU1); OSSetThreadPriority(OSGetCurrentThread(), 20);
  for (;;) { SDL_SemWait(go); conv_rows(job.y, job.uv, job.pitch, job.sw, job.sh, job.dst, job.dpitch, job.ow, job.oh, job.r0, job.r1); SDL_SemPost(done); }
  return 0;
}
void yuv_nv12_to_rgba(const uint8_t *y, const uint8_t *uv, int pitch, int sw, int sh, uint8_t *dst, int dst_pitch, int ow, int oh, int threads) {
  if (!inited) init(); if (ow > sw) ow = sw; if (oh > sh) oh = sh; if (ow <= 0 || oh <= 0) return;
  if (threads >= 2 && oh >= 64) {
    SDL_AtomicLock(&hl);                         /* one split job at a time (several decoders may call this) */
    if (!helper) { go = SDL_CreateSemaphore(0); done = SDL_CreateSemaphore(0); helper = SDL_CreateThreadWithStackSize(helper_main, "yuv", 64 * 1024, NULL); }
    if (helper) { int mid = (oh / 2) & ~1; job = (Job){y, uv, pitch, sw, sh, dst, dst_pitch, ow, oh, mid, oh}; SDL_SemPost(go);
      conv_rows(y, uv, pitch, sw, sh, dst, dst_pitch, ow, oh, 0, mid); SDL_SemWait(done); SDL_AtomicUnlock(&hl); return; }
    SDL_AtomicUnlock(&hl);
  }
  conv_rows(y, uv, pitch, sw, sh, dst, dst_pitch, ow, oh, 0, oh);
}

static inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
void yuv_nv12_to_rgba_ref(const uint8_t *y, const uint8_t *uv, int pitch, int w, int h, uint8_t *out) {   /* = video.c's original */
  for (int j = 0; j < h; j++) { const uint8_t *yr = y + j * pitch, *ur = uv + (j / 2) * pitch; uint8_t *o = out + (size_t)j * w * 4;
    for (int i = 0; i + 1 < w; i += 2) { int u = ur[i] - 128, vv = ur[i + 1] - 128; int rv = 1634 * vv, gv = -833 * u - 400 * vv, bv = 2066 * u;
      for (int k = 0; k < 2; k++) { int yy = 1192 * (yr[i + k] - 16); o[0] = clamp8((yy + rv) >> 10); o[1] = clamp8((yy + gv) >> 10); o[2] = clamp8((yy + bv) >> 10); o[3] = 255; o += 4; } }
    if (w & 1) { int i = w - 1; int u = ur[i & ~1] - 128, vv = ur[(i & ~1) + 1] - 128; int yy = 1192 * (yr[i] - 16); o[0] = clamp8((yy + 1634 * vv) >> 10); o[1] = clamp8((yy - 833 * u - 400 * vv) >> 10); o[2] = clamp8((yy + 2066 * u) >> 10); o[3] = 255; } }
}
