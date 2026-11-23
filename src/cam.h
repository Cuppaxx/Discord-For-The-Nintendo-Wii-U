#ifndef CAM_MOD_H
#define CAM_MOD_H
/* cam - Wii U GamePad camera capture (wut camera/camera.h: CAMInit/CAMOpen/CAMSubmitTargetSurface), 640x480 NV12.
 *
 * The camera library decodes on its own thread and reports finished surfaces through an event callback; this module
 * keeps a ring of CAM_NBUF surfaces, hands the newest finished one to the caller and re-submits the others.
 * FALLBACK: if CAMInit/CAMOpen fails, no frame arrives within 3 s, or the camera delivers only flat/blank frames for
 * 3 s (Cemu 2.6: CAMInit/CAMOpen succeed and frames arrive, but they are all zero = green), cam_frame()
 * switches to a generated, clearly labelled moving test pattern ("TEST PATTERN - NO CAMERA") at the requested fps,
 * so the encode/send pipeline can be exercised anywhere. cam_is_fake() tells which one you get.
 * Real capture is UNVERIFIED (written from the wut header; never run on hardware).
 * Threads: all calls from one thread (e.g. the UI or the encoder feeder); only one Cam may be open at a time. */
#include <stdint.h>
#include <stdbool.h>

#define CAM_W 640
#define CAM_H 480
#define CAM_PITCH 768        /* pitch of both planes of returned frames (camera and test pattern alike) */
#define CAM_NBUF 6

typedef struct Cam Cam;

Cam *cam_open(int fps);                  /* fps 15 or 30 (camera supports only these). NULL only when out of memory. */
/* newest frame not returned before: NV12, Y = *y, UV interleaved = *uv (CAM_H/2 rows), both with *pitch.
 * Pointers stay valid until the next cam_frame()/cam_close(). Returns false if nothing new since the last call. */
bool cam_frame(Cam *c, const uint8_t **y, const uint8_t **uv, int *pitch, uint32_t *seq);
void cam_close(Cam *c);
/* Ownership: the surface returned by cam_frame() is not handed back to the camera before the next cam_frame()/cam_close(),
 * so read or copy it before calling again; it is cache-invalidated before being returned (the camera writes it by DMA). */
uint64_t cam_frame_time_us(const Cam *c);  /* capture time (DECODE_DONE event, OSGetSystemTime-based us) of the last returned frame */
void cam_counts(const Cam *c, uint32_t *captured, uint32_t *failed);   /* frames the camera finished (or patterns made) / failed decodes */
bool cam_is_fake(const Cam *c);
const char *cam_status(const Cam *c);    /* human readable state, e.g. "camera: CAMInit failed (-9) - test pattern" */
int cam_error(const Cam *c);             /* last CAMError (0 = none) */

/* ---- NV12 helpers (pitches in bytes; widths/heights even) ---- */
/* 2x2 box filter, e.g. 640x480 -> 320x240 */
/* picture adjustments applied to outgoing frames (the Wii U camera library has no exposure controls) */
void cam_set_adjust(float exposure_ev, float saturation);   /* ev -1.5..+1.5 (0 = as captured), saturation 0..2 (1 = as captured) */
void cam_get_adjust(float *exposure_ev, float *saturation);
void cam_adjust(uint8_t *y, uint8_t *uv, int pitch, int w, int h);   /* NV12, in place */
void cam_downscale_half(const uint8_t *sy, const uint8_t *suv, int spitch, int sw, int sh, uint8_t *dy, uint8_t *duv, int dpitch);
/* crop (cx,cy,cw,ch) out of the source and scale it to dw x dh (nearest neighbour; even coordinates) */
void cam_crop_scale(const uint8_t *sy, const uint8_t *suv, int spitch, int cx, int cy, int cw, int ch,
                    uint8_t *dy, uint8_t *duv, int dpitch, int dw, int dh);
/* the fallback pattern itself (frame n), any even size; label drawn on top (NULL = none) */
void cam_pattern(uint8_t *y, uint8_t *uv, int pitch, int w, int h, uint32_t n, const char *label);
/* 5x7 bitmap text (A-Z 0-9 space - : . ( ) / # %), white on a black box; uv may be NULL (Y only); scale >= 1 */
void cam_text(uint8_t *y, uint8_t *uv, int pitch, int w, int h, int x0, int y0, int scale, const char *s);
#endif
