#ifndef VOICE_H
#define VOICE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Discord voice connection: voice gateway (websocket) + UDP/RTP with Opus, GamePad mic in, mixed playback out. */
enum { VC_OFF, VC_CONNECTING, VC_UDP, VC_CONNECTED, VC_ERROR };
#define VC_MAX_PEERS 32
typedef struct { uint32_t ssrc; char user_id[24]; bool speaking; uint32_t last_pkt; } VcPeer;
typedef struct {
  int state; char err[200]; char mode[48]; int dave_version;        /* negotiated DAVE protocol version (0 = no E2EE) */
  bool dave_group;         /* joined the MLS group (have key ratchets) */
  bool dave_active;        /* our outgoing audio is actually E2E encrypted */
 
  bool muted; bool mic_ok; bool speaking;          /* our mic state */
  int mic_level;                                   /* recent mic peak after gain, 0..32767 (level meter) */
  VcPeer peers[VC_MAX_PEERS]; int npeers;
  uint32_t rx_packets, tx_packets; int ping_ms;
  char users[VC_MAX_PEERS][24]; int nusers;          /* everyone the voice server says is in the call (op 11/5), incl. people there before we joined */
} VcInfo;
void voice_start(const char *server_id, const char *channel_id, const char *user_id, const char *session_id, const char *token, const char *endpoint);
void voice_stop(void);
void voice_set_mute(bool m);
void voice_snapshot(VcInfo *out);
void voice_set_user_gain(const char *user_id, float gain);   /* 0 = muted for me, 1 = normal, up to 3 = amplified (300%) */
float voice_get_user_gain(const char *user_id);
void voice_set_deaf(bool deaf);
void voice_set_mic_gain(float g);     /* 1.0 = 100% (the old fixed level), 0.5 .. 4.0 */
float voice_get_mic_gain(void);
void voice_set_mic_agc(bool on);      /* auto volume: raises quiet speech, lowers loud */
bool voice_get_mic_agc(void);
int voice_mic_level(void);            /* live level for the meter (works whenever the mic thread runs) */
typedef struct {
  bool ns;          /* noise suppression: low-cut filter + adaptive noise gate */
  bool echo;        /* echo reduction: duck the mic while call audio plays (GamePad speaker next to its mic) */
  bool ptt;         /* push-to-talk (voice_ptt) instead of voice activity */
  bool music;       /* Opus "music" mode: no speech processing, full band */
  bool fec;         /* Opus in-band FEC (loss protection) */
  int sens;         /* input sensitivity: 0 = automatic, 1..100 manual threshold */
  int kbps;         /* Opus bitrate 16..128 */
  float out_gain;   /* output volume 0..2 */
} VoiceSettings;
void voice_get_settings(VoiceSettings *s);
void voice_set_settings(const VoiceSettings *s);
void voice_ptt(bool down);
int voice_vad_threshold(void);
void voice_mic_test(bool on);         /* play your processed mic back to you (works outside calls too) */
bool voice_mic_testing(void);        /* current speaking threshold (for the meter marker) */                               /* stop playing everyone (self-deafen) */
/* ---- video support (vrecv.c / golive.c / the video sender) ---- */
#include <SDL2/SDL_mutex.h>
#include <jansson.h>
SDL_mutex *voice_dave_mutex(void);       /* serialises ALL libdave/OpenSSL calls in the app (OpenSSL is built no-threads) */
json_t *voice_codecs_json(void);         /* new "codecs" array for SELECT_PROTOCOL: opus PT 120, H264 PT 103 / RTX 104 */
void voice_video_ssrcs(uint32_t *audio, uint32_t *video, uint32_t *rtx);   /* our ssrcs from READY (video/rtx for a sender) */
/* our camera (vsend.c): on = capture + encode + send, op 12 + self_video announce, self preview in our tile.
 * Needs a connected call (no-op otherwise); voice_stop turns it off. UI thread. */
void voice_set_camera(bool on);
bool voice_camera_on(void);
const char *voice_camera_status(void);   /* e.g. "camera: ... - test pattern" while on, "" when off */
/* encoder thread (vsend.c): DAVE encrypt + packetise + send one Annex-B access unit. 0 ok, <0 not sent (-2 = E2EE failed) */
int voice_send_video_au(const uint8_t *au, size_t n, uint32_t ts90k, bool key);
void voice_camera_selftest_start(const char *self_user_id);   /* only with SD:.../vsend_selftest.txt and no call: dry-run loopback test, nothing sent */
#endif
