#ifndef VSEND_H
#define VSEND_H
/* vsend - our camera into a voice call: GamePad camera (cam.c; labelled test pattern without a camera, e.g. Cemu)
 * -> 2x2 downscale -> x264 (venc.c, own thread on core 0) -> voice_send_video_au() (voice.c: DAVE encrypt,
 * RFC 6184 packetise, RTP + transport encryption, UDP). Optional self preview through vrecv (VR_CONN_SELF).
 * Started/stopped by voice_set_camera() in voice.c; all vs_* calls from one thread except vs_request_idr / vs_set_paused. */
#include <stdbool.h>
#include <stdint.h>

#define VS_W 320
#define VS_H 240
#define VS_FPS 20          /* sent frame rate: camera runs at 30 fps, 2 of every 3 frames are kept on a 50 ms grid */
#define VS_CAM_FPS 30
#define VS_KBPS 250

bool vs_start(const char *self_user_id, int idr_secs, bool preview);   /* false: could not open camera/encoder */
void vs_stop(void);                       /* blocks until the capture + encoder threads ended */
bool vs_active(void);
void vs_request_idr(void);                /* any thread: RTCP PLI/FIR, new viewer */
void vs_set_paused(bool paused);          /* any thread: server's media sink wants said 0 for our stream */
bool vs_fake(void);                       /* sending the test pattern (no camera) */
const char *vs_status(void);
#endif
