#ifndef VENC_H
#define VENC_H
/* venc - software H.264 encoder for sending the GamePad camera (x264, GPL v2+; see third_party/x264/BUILD-NOTES.md).
 *
 * Output: H.264 Constrained Baseline (profile_idc 66 + constraint_set0/1, level 3.1 => SPS bytes 42 C0 1F; Discord
 * signals 42e01f), no B-frames, CAVLC, 1 reference, zerolatency (no lookahead, no frame delay: one frame in = one
 * access unit out), Annex-B with 4-byte start codes, SPS+PPS repeated in front of every IDR, slices capped at
 * VENC_SLICE_MAX bytes (so a slice fits one RTP packet after DAVE + RTP overhead; bigger slices only if a single
 * macroblock does not fit), periodic IDR every VENC_IDR_SECONDS plus on demand (PLI/FIR), ABR + VBV (CBR-ish).
 *
 * Two ways to use it:
 *  1) synchronous: venc_encode()/venc_encode_rgba() encode on the calling thread (the call blocks ~ms/frame).
 *  2) asynchronous (recommended for the UI): venc_start() spawns one encoder thread pinned to a core.
 *     venc_submit() copies a frame into a 2-slot queue and returns at once (never blocks on encoding: if the
 *     encoder is still busy and a frame is already waiting, the waiting one is replaced by the new one = dropped).
 *     Results come back either through a callback on the encoder thread (venc_set_callback, e.g. DAVE encrypt +
 *     packetise + send right there) or by polling from any one thread (venc_poll, 4-slot output ring).
 *     Do not call venc_encode* while the thread is running.
 * Frame size: w and h must be even (NV12). Odd multiples of 16 are fine (x264 crops).  */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VENC_SLICE_MAX 1100      /* bytes per slice NAL (x264 slice-max-size) */
#define VENC_IDR_SECONDS 2       /* periodic keyframe interval */

typedef struct Venc Venc;

typedef struct {
  const uint8_t *data;   /* Annex-B access unit (00 00 00 01 + NAL ...); valid until the next encode/poll on this Venc */
  size_t size;           /* bytes */
  uint32_t ts90k;        /* 90 kHz timestamp of the frame's capture/submit time (monotonic, starts at 0 on open):
                            add a random per-stream offset for RTP */
  bool keyframe;         /* IDR (SPS+PPS included) */
  int nals;              /* NAL units in data */
  int max_nal;           /* largest NAL in bytes (without start code) */
  uint32_t frame_no;     /* 0,1,2... encoded frames */
  float enc_ms;          /* time spent in x264_encoder_encode for this frame */
} VencOut;

typedef struct {
  uint32_t submitted, encoded, dropped, idr, forced_idr;   /* dropped = replaced in the queue before being encoded */
  uint64_t bytes;
  float last_ms, avg_ms, max_ms;                            /* encode time per frame */
  float kbps;                                               /* output bitrate over the last ~2 s */
} VencStats;

typedef void (*VencCb)(void *user, const VencOut *out);    /* runs on the encoder thread; out->data valid during the call */

/* fps: nominal frame rate (rate control, IDR interval, timestamps when none given). kbps: target bitrate.
 * Returns NULL on bad args / out of memory / x264 open failure (reason logged with dlog). */
Venc *venc_open(int w, int h, int fps, int kbps);
/* same with knobs (recording to a file wants no slice limit and a longer GOP):
 * slice_max 0 = one slice per frame; idr_secs <= 0 = VENC_IDR_SECONDS; vbv_ms <= 0 = 1000 */
typedef struct { int w, h, fps, kbps, slice_max, idr_secs, vbv_ms; } VencOpts;
Venc *venc_open_ex(const VencOpts *o);
void venc_close(Venc *v);                                   /* stops the thread too */
void venc_size(const Venc *v, int *w, int *h);

/* ---- synchronous ----
 * NV12 in (y plane, interleaved uv plane at half height). Returns AU bytes (>0, *out filled), 0 = no output, <0 error. */
int venc_encode(Venc *v, const uint8_t *y, const uint8_t *uv, int y_pitch, int uv_pitch, bool force_idr, VencOut *out);
/* RGBA (R,G,B,A bytes) in, converted to NV12 (BT.601 limited range) internally; image must be w x h. */
int venc_encode_rgba(Venc *v, const uint8_t *rgba, int pitch, bool force_idr, VencOut *out);

/* ---- asynchronous ----
 * core: 0, 1 or 2 (Espresso core the encoder thread is pinned to; the Discord client uses 0 - CPU2 runs voice,
 * CPU1 the UI). prio: OS thread priority (0 highest .. 31; voice threads use 14-16, use ~20 so audio wins). */
bool venc_start(Venc *v, int core, int prio);
void venc_set_callback(Venc *v, VencCb cb, void *user);     /* set before venc_start (or NULL to use venc_poll) */
/* copy one NV12 frame into the queue (non-blocking). ts90k: capture time in 90 kHz units, or VENC_TS_NOW.
 * Returns false if a previously queued, not yet encoded frame was replaced (= dropped). */
#define VENC_TS_NOW 0xFFFFFFFFu
bool venc_submit(Venc *v, const uint8_t *y, const uint8_t *uv, int y_pitch, int uv_pitch, uint32_t ts90k);
bool venc_poll(Venc *v, VencOut *out);                       /* next encoded AU (oldest first); data valid until the next venc_poll */
bool venc_busy(Venc *v);                                     /* a frame is queued or being encoded */

/* ---- any thread ---- */
void venc_request_idr(Venc *v);                              /* next encoded frame is an IDR (RTCP PLI / FIR / new viewer) */
void venc_set_bitrate(Venc *v, int kbps);                    /* applied before the next frame (x264_encoder_reconfig) */
void venc_stats(Venc *v, VencStats *st);
uint32_t venc_now90k(const Venc *v);                         /* the clock used for VENC_TS_NOW */

/* ---- helpers ---- */
/* RGBA -> NV12 (BT.601 limited range, 2x2 chroma average). w, h even. */
void venc_rgba_to_nv12(const uint8_t *rgba, int pitch, int w, int h, uint8_t *y, int y_pitch, uint8_t *uv, int uv_pitch);
#endif
