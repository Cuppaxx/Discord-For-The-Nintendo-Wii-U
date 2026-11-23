#ifndef APLAY_H
#define APLAY_H
/* aplay - audio-only attachment player: progressive HTTP download kept in RAM -> libavformat demux (mp3, ogg, wav,
 * flac, mov/m4a, matroska/webm, aac) -> libavcodec decode (mp3, aac, vorbis, opus, flac, alac, pcm) -> SDL audio.
 * Decoding on its own thread; ap_tick on the main thread feeds the device. States are the VID_* values (video.h). */
#include <stdbool.h>
#include <stddef.h>

typedef struct APlay APlay;
void ap_set_log(void (*fn)(const char *line));
APlay *ap_open(const char *url, size_t max_bytes);
void ap_tick(APlay *p);
int ap_state(const APlay *p);
const char *ap_error(const APlay *p);
void ap_toggle(APlay *p);                 /* pause / resume; replays when ended */
void ap_seek(APlay *p, double rel_secs);
double ap_time(const APlay *p);
double ap_duration(const APlay *p);
int ap_loaded_pct(const APlay *p);
const char *ap_codec(const APlay *p);     /* e.g. "flac 44100 Hz 2 ch" once known */
void ap_close(APlay *p);
#endif
