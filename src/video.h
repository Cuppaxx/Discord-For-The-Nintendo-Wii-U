#ifndef VIDEO_H
#define VIDEO_H
/* media player (copied from youtube/src/video.c and trimmed for MingusDiscord: no DASH, no js.h).
 * Progressive HTTP fetch -> MP4 (H.264, hardware decoded) / WebM (Opus) / MP3 -> texture + audio.
 * Several players may be open at once: each has its own H.264 decoder instance and decode thread; frame-output
 * callbacks are routed to the right player (decoder user pointer, else the calling thread, else the frame buffer). */
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct Vid Vid;
enum { VID_LOADING, VID_PLAYING, VID_PAUSED, VID_ENDED, VID_ERROR };
enum { VID_F_LOOP = 1,    /* restart from the first frame at the end (bytes stay in memory, decoder restarted), never ENDED */
       VID_F_MUTE = 2,    /* ignore any audio track (no audio device opened, wall-clock timing) */
       VID_F_MP4_ONLY = 4 }; /* fail with "Unsupported media format" instead of playing WebM/MP3 audio itself */
#define VID_MAX_DECODERS 8 /* simultaneous H.264 decoder instances allowed by this module */

void vid_set_ua(const char *ua_header);     /* "User-Agent: ..." used for stream downloads */
void vid_set_log(void (*fn)(const char *line));   /* diagnostics sink (any thread); NULL = silent */
Vid *vid_open(const char *video_url, const char *audio_url);   /* audio_url: separate audio stream (optional, NULL = use the container's) */
Vid *vid_open_ex(const char *url, int flags, size_t max_bytes); /* flags VID_F_*; max_bytes 0 = 96 MB */
void vid_close(Vid *v);
void vid_tick(Vid *v);                       /* main thread, every frame: presents decoded frames, feeds audio */
int vid_state(const Vid *v);
const char *vid_error(const Vid *v);         /* message when state == VID_ERROR */
void vid_toggle(Vid *v);                     /* play / pause */
void vid_seek(Vid *v, double secs);          /* relative seek (best effort: to the nearest keyframe) */
double vid_time(const Vid *v);
double vid_duration(const Vid *v);
int vid_loaded_pct(const Vid *v);            /* download progress 0..100 */
SDL_Texture *vid_frame(Vid *v, int *w, int *h);   /* current frame (may be NULL) */
bool vid_has_video(const Vid *v);
bool vid_audio_unsupported(const Vid *v);    /* container had audio we cannot decode */
int vid_loops(const Vid *v);                 /* VID_F_LOOP: completed loops so far */
int vid_decoders_open(void);                 /* H.264 decoder instances currently open (all players) */
#endif
