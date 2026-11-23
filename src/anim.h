#ifndef ANIM_H
#define ANIM_H
/* anim - animated media for the chat: animated GIF / WebP images, silent looping MP4 clips ("gifv"), playable
 * video attachments and audio attachments. All calls on the main (render) thread. Downloads go through nxnet;
 * image decoding runs on the download worker thread; MP4s are decoded by the Wii U hardware H.264 decoder on a
 * per-clip thread (video.c); audio-only files use libavformat/libavcodec (aplay.c).
 *
 * Limits (memory is shared with the chat images):
 *  - GIF / WebP: file <= ANIM_GIF_MAX_FILE (bigger files are refused), canvas <= ANIM_GIF_MAX_PIXELS, at most
 *    ANIM_GIF_MAX_FRAMES frames and ANIM_GIF_MEM bytes of scaled RGBA frames per image, and ANIM_TOTAL_MEM bytes for
 *    all open images together (a new image gets what is left, at least one frame). Frames are scaled to fit
 *    max_w x max_h (never enlarged; <= 0 = no limit). When not all frames fit, every k-th frame is kept with the
 *    delays summed (speed unchanged, fewer steps). Decoding needs one extra canvas (W*H*4, GIF "restore previous"
 *    twice; WebPAnimDecoder ~2x) transiently. Frames become GPU textures lazily when first shown (RAM copy freed).
 *    Outputs <= ANIM_SMALL_PIXELS (e.g. GIF-picker previews at 200x150) get ANIM_SMALL_MEM (4 MB) instead, so a grid
 *    of 12 previews holds <= 48 MB of frames (a 169-frame 498x282 Klipy WebP at 200x113 keeps every 4th frame).
 *  - Clip (anim_open_clip): file <= ANIM_CLIP_MAX_FILE, kept in RAM for looping (no re-download); 3 decoded RGBA
 *    frames queued + 1 texture + the H.264 decoder's working memory for the clip size.
 *  - Video / audio (anim_open_video): file <= ANIM_VIDEO_MAX_FILE, whole file kept in RAM (plays while downloading).
 *  - At most VID_MAX_DECODERS (8) H.264 clips/videos decoding at once. */
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#define ANIM_GIF_MAX_FILE   (8u << 20)
#define ANIM_GIF_MAX_PIXELS (2048 * 2048)
#define ANIM_GIF_MAX_FRAMES 120
#define ANIM_GIF_MEM        (24u << 20)
#define ANIM_TOTAL_MEM      (128u << 20)
#define ANIM_SMALL_PIXELS   (320 * 240)   /* outputs this small (picker previews) get the smaller budget below */
#define ANIM_SMALL_MEM      (4u << 20)
#define ANIM_CLIP_MAX_FILE  (24u << 20)
#define ANIM_VIDEO_MAX_FILE (96u << 20)

typedef struct Anim Anim;
enum { ANIM_LOADING, ANIM_READY, ANIM_ERROR };

Anim *anim_open_gif(const char *url, int max_w, int max_h);   /* animated GIF (a WebP served instead also works), loops forever */
Anim *anim_open_webp(const char *url, int max_w, int max_h);  /* animated (or still) WebP, e.g. Klipy/Tenor gifv thumbnails: preferred for gifv */
Anim *anim_open_clip(const char *mp4_url);                     /* silent looping MP4 (gifv fallback), hardware decoded */
Anim *anim_open_video(const char *url);                         /* video or audio attachment: plays once with audio; pause/seek.
                                                                   MP4/MOV H.264 -> hardware video; mp3/m4a/aac/ogg/opus/wav/flac/webm...
                                                                   (by extension, or when the MP4 player rejects the file) -> audio only */
Anim *anim_open_media(const char *url, const char *content_type);   /* same, routed by the attachment's content_type when known
                                                                   (video/mp4|quicktime -> H.264 player, anything else -> audio) */
void anim_tick(Anim *a);                                       /* main thread, once per frame (only for animations on screen) */
SDL_Texture *anim_frame(Anim *a, int *w, int *h);              /* current frame or NULL (audio: always NULL); owned by the Anim */
int anim_state(const Anim *a);                                  /* ANIM_LOADING / ANIM_READY / ANIM_ERROR */
const char *anim_error(const Anim *a);                          /* message when ANIM_ERROR */
bool anim_has_video(const Anim *a);                             /* false for audio-only media */
void anim_toggle(Anim *a);                                      /* pause / resume (video/audio: replays when ended) */
void anim_seek(Anim *a, double rel_secs);                       /* video/audio only */
double anim_time(const Anim *a);                                /* seconds (GIF/WebP/clip: position in the loop) */
double anim_duration(const Anim *a);                            /* seconds (GIF/WebP: one loop) */
bool anim_paused(const Anim *a);
bool anim_ended(const Anim *a);                                 /* video/audio finished (toggle to replay) */
int anim_loaded_pct(const Anim *a);                             /* download progress 0..100 */
void anim_close(Anim *a);                                       /* any time, also while loading */
void anim_set_log(void (*fn)(const char *line));                /* optional diagnostics sink (called from any thread) */
size_t anim_frame_mem(void);                                    /* bytes of decoded GIF/WebP frames currently held */
#endif
