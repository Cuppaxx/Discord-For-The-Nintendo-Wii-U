#ifndef REC_H
#define REC_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* voice-message recorder: GamePad mic -> Opus (48 kHz mono, 32 kbps) -> Ogg Opus file in memory + Discord waveform.
 * Runs its own mic thread (not while a voice call holds the mic). With dc_fakemic() a test tone replaces the mic. */
enum { REC_IDLE, REC_RECORDING, REC_DONE, REC_ERROR };
bool rec_start(int max_secs);
void rec_stop(void);                 /* finish: state becomes REC_DONE */
void rec_cancel(void);
int rec_state(void);
const char *rec_error(void);
float rec_seconds(void);
int rec_level(void);                 /* recent peak 0..32767 for the meter */
/* after REC_DONE: the .ogg bytes (owned by the recorder until the next start/cancel), its duration and the
 * base64 waveform Discord wants (up to 256 amplitude bytes) */
bool rec_take(const uint8_t **ogg, size_t *len, float *secs, const char **waveform_b64);
#endif
