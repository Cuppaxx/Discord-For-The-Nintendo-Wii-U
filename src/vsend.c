/* vsend.c - camera -> encoder -> voice.c video sender. See vsend.h. */
#include "vsend.h"
#include "venc.h"
#include "cam.h"
#include "vrecv.h"
#include "voice.h"
#include "dlog.h"
#include <SDL2/SDL.h>
#include <coreinit/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Venc *enc; static SDL_Thread *th; static volatile bool stop_flag, paused; static volatile bool running;
static char uid[24]; static bool preview, fake; static char status[96];
static uint8_t *dy, *duv;
static uint32_t n_cap, n_sent_au, n_fail;

static void on_au(void *user, const VencOut *o) {          /* encoder thread */
  (void)user; if (stop_flag || paused) return;
  int rc = voice_send_video_au(o->data, o->size, o->ts90k, o->keyframe);
  if (rc == 0) n_sent_au++; else { n_fail++; if (n_fail <= 5 || n_fail % 100 == 0) dlog("[vsend] frame %u not sent (rc %d)", (unsigned)o->frame_no, rc); if (rc == -2) venc_request_idr(enc); }
  if (o->frame_no < 3 || (o->keyframe && o->frame_no < 200)) dlog("[vsend] AU #%u %u bytes %s nals %d max %d enc %.1f ms", (unsigned)o->frame_no, (unsigned)o->size, o->keyframe ? "IDR" : "", o->nals, o->max_nal, o->enc_ms);
}
static int capture(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0); OSSetThreadPriority(OSGetCurrentThread(), 19);
  Cam *c = cam_open(VS_CAM_FPS); if (!c) { snprintf(status, sizeof status, "camera: out of memory"); running = false; return 0; }
  bool said = false;
  /* exact VS_FPS cadence: keep a camera frame when its capture time reaches the next slot of a 1/VS_FPS grid
   * (30 -> 20 fps keeps 2 of 3); the RTP timestamp is the slot time, so receivers see a steady 20 fps clock. */
  const uint64_t slot_us = 1000000 / VS_FPS; uint64_t t0 = 0, next_slot = 0; uint32_t last_ts = 0; bool have_ts = false;
  uint64_t win_t = 0; uint32_t win_cap0 = 0, win_got = 0, win_kept = 0, win_enc0 = 0, win_drop0 = 0; double win_ms0 = 0;
  while (!stop_flag) {
    const uint8_t *y, *uv; int pitch; uint32_t seq;
    if (!cam_frame(c, &y, &uv, &pitch, &seq)) { SDL_Delay(3); continue; }
    fake = cam_is_fake(c); if (!said && fake) { said = true; dlog("[vsend] %s", cam_status(c)); }
    snprintf(status, sizeof status, "%s", cam_status(c));
    uint64_t t = cam_frame_time_us(c); win_got++;
    if (!t0) { t0 = t; next_slot = 0; win_t = t; }
    uint64_t rel = t - t0;
    if (rel < next_slot) continue;                                   /* between grid slots: skip this camera frame */
    uint64_t slot = rel / slot_us; next_slot = (slot + 1) * slot_us;
    uint32_t ts = (uint32_t)(slot * (90000 / VS_FPS));
    if (have_ts && ts == last_ts) continue;
    last_ts = ts; have_ts = true;
    cam_downscale_half(y, uv, pitch, CAM_W, CAM_H, dy, duv, VS_W); cam_adjust(dy, duv, VS_W, VS_W, VS_H); n_cap++; win_kept++;
    if (!paused) venc_submit(enc, dy, duv, VS_W, VS_W, ts);         /* never queues: a waiting frame is replaced (dropped) */
    if (preview) vr_local_frame(VR_CONN_SELF, uid, dy, duv, VS_W, VS_W, VS_H, ts);
    if (t - win_t >= 5000000) {                                       /* cheap stats line every 5 s */
      VencStats s; venc_stats(enc, &s); uint32_t cap = 0, fail = 0; cam_counts(c, &cap, &fail); double secs = (t - win_t) / 1e6;
      uint32_t enc_n = s.encoded - win_enc0; double ms = enc_n ? (s.avg_ms * s.encoded - win_ms0) / enc_n : 0;
      dlog("[vsend] 5s: camera %.1f fps (seen %.1f), sent %.1f fps, encoded %.1f fps, dropped %u, encode avg %.1f ms (max %.1f overall), %.0f kbps, sent %u failed %u%s%s",
           (cap - win_cap0) / secs, win_got / secs, win_kept / secs, enc_n / secs, (unsigned)(s.dropped - win_drop0), ms, s.max_ms, s.kbps, (unsigned)n_sent_au, (unsigned)n_fail, fake ? " [test pattern]" : "", paused ? " [paused by server]" : "");
      win_t = t; win_cap0 = cap; win_got = win_kept = 0; win_enc0 = s.encoded; win_drop0 = s.dropped; win_ms0 = s.avg_ms * s.encoded; }
  }
  cam_close(c); return 0;
}
bool vs_start(const char *self_uid, int idr_secs, bool pv) {
  if (running) return true;
  snprintf(uid, sizeof uid, "%s", self_uid ? self_uid : ""); preview = pv; stop_flag = paused = fake = false; n_cap = n_sent_au = n_fail = 0; snprintf(status, sizeof status, "starting camera...");
  dy = malloc(VS_W * VS_H); duv = malloc(VS_W * VS_H / 2);
  VencOpts o = {VS_W, VS_H, VS_FPS, VS_KBPS, VENC_SLICE_MAX, idr_secs, 0};
  enc = dy && duv ? venc_open_ex(&o) : NULL;
  if (!enc) { dlog("[vsend] encoder open failed"); free(dy); free(duv); dy = duv = NULL; snprintf(status, sizeof status, "encoder failed"); return false; }
  venc_set_callback(enc, on_au, NULL);
  if (!venc_start(enc, 0, 20)) { dlog("[vsend] encoder thread failed"); venc_close(enc); enc = NULL; free(dy); free(duv); dy = duv = NULL; return false; }
  if (preview) vr_local_open(VR_CONN_SELF, uid);
  running = true; th = SDL_CreateThreadWithStackSize(capture, "vcam", 128 * 1024, NULL);
  dlog("[vsend] camera on: %dx%d @ %d fps (camera %d fps), %d kbps, IDR every %d s, preview %d", VS_W, VS_H, VS_FPS, VS_CAM_FPS, VS_KBPS, idr_secs > 0 ? idr_secs : VENC_IDR_SECONDS, preview);
  return true;
}
void vs_stop(void) {
  if (!running) return; stop_flag = true; if (th) { SDL_WaitThread(th, NULL); th = NULL; }
  venc_close(enc); enc = NULL; free(dy); free(duv); dy = duv = NULL;
  if (preview) vr_local_close(VR_CONN_SELF, uid);
  running = false; dlog("[vsend] camera off: captured %u, sent %u frames, failed %u", (unsigned)n_cap, (unsigned)n_sent_au, (unsigned)n_fail);
}
bool vs_active(void) { return running; }
void vs_request_idr(void) { Venc *e = enc; if (running && e) venc_request_idr(e); }
void vs_set_paused(bool p) { if (paused != p) { paused = p; dlog("[vsend] %s by the server", p ? "paused" : "resumed"); if (!p) vs_request_idr(); } }
bool vs_fake(void) { return fake; }
const char *vs_status(void) { return status; }
