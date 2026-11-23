#ifndef VREC_H
#define VREC_H
/* vrec - record a short video message: GamePad camera (cam.c; labelled test pattern when there is no camera, e.g. Cemu)
 * -> x264 H.264 Constrained Baseline (venc.c) + GamePad mic -> AAC-LC mono (ffmpeg native "aac" encoder)
 * -> MP4 (ISO BMFF: ftyp + moov first ("faststart") + interleaved mdat, avc1/avcC + mp4a/esds), built in memory.
 * Plays in browsers / Discord desktop / mobile (standard H.264 + AAC-LC MP4).
 * Size: duration is capped (max_secs, default VREC_DEFAULT_SECS) and the video bitrate is chosen so the file stays
 * under VREC_MAX_BYTES; recording also stops by itself when that many bytes are reached.
 * With dc_fakemic() (config.txt fakemic=1) a test tone replaces the mic (Cemu's mic emulation crashes Cemu here).
 * Threads: video capture+encode thread on core 0, audio capture+AAC thread on core 2; all vrec_* calls from one thread
 * (the UI). Not while a voice call holds the mic (MICInit fails -> VREC_ERROR) unless fakemic is on.
 * Real camera and real mic capture are UNVERIFIED (Cemu: test pattern + test tone only). */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define VREC_DEFAULT_SECS 30
#define VREC_MAX_BYTES (9500 * 1024)        /* stay below Discord's 10 MB free upload limit */

enum { VREC_IDLE, VREC_RECORDING, VREC_FINISHING, VREC_DONE, VREC_ERROR };

/* optional, before vrec_start: picture size (even, from the 640x480 camera; 4:3 recommended) and frame rate
 * (15 or 30; the camera only does these). Defaults: 480x360 @ 15 fps. */
void vrec_config(int w, int h, int fps);
bool vrec_start(int max_secs);              /* max_secs <= 0 -> VREC_DEFAULT_SECS (also capped at 120) */
void vrec_stop(void);                       /* request the end: state goes VREC_FINISHING, then VREC_DONE (non-blocking) */
void vrec_cancel(void);                     /* stop and discard (blocks until the threads ended, < ~0.2 s) */
int vrec_state(void);
const char *vrec_error(void);
float vrec_seconds(void);                   /* recorded so far (or final duration once done) */
int vrec_level(void);                       /* recent audio peak 0..32767 (meter) */
bool vrec_cam_fake(void);                   /* the video is the test pattern (no camera) */
/* live preview of what is being recorded: latest frame scaled (nearest) into rgba (w*h*4, R,G,B,A bytes).
 * Returns false if there is no new frame since *seq (pass 0 the first time; *seq is updated). */
bool vrec_preview(uint8_t *rgba, int w, int h, uint32_t *seq);
/* after VREC_DONE: takes ownership of the MP4 bytes (free() them); state returns to VREC_IDLE. */
bool vrec_take(uint8_t **mp4, size_t *len, int *w, int *h, float *secs);
#endif
