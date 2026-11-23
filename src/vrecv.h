#ifndef VRECV_H
#define VRECV_H
/* vrecv - receiving Discord video (voice-call cameras and Go Live streams).
 * RTP H.264 (RFC 6184, packetization-mode 1: single NAL / STAP-A / FU-A) -> frame reassembly with a small
 * reorder window -> optional DAVE (E2EE) frame decrypt -> Wii U hardware H.264 decoder -> NV12 -> RGBA -> SDL texture.
 * Loss: the stream waits for the next keyframe and asks the sender for one (RTCP PLI via the connection's callback).
 * Threads: vr_rtp() from the connection's network thread; one internal decode thread; vr_tick()/vr_texture() main thread. */
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <jansson.h>

enum { VR_CONN_VOICE = 0, VR_CONN_STREAM = 1, VR_CONN_TEST = 2, VR_CONN_SELF = 3, VR_NCONN = 4 };   /* SELF: our own camera preview (local, no network) */
#define VR_PT_H264 103          /* payload types we advertise in SELECT_PROTOCOL codecs */
#define VR_PT_H264_RTX 104
#define VR_MAX_STREAMS 8         /* remote video sources tracked at once */
#define VR_MAX_DECODERS 4        /* hardware decoder instances this module opens at once */

/* DAVE frame decrypt for a connection: 0 = out/w hold the plaintext frame, else drop the frame.
 * Called on the decode thread (may block on the DAVE lock). */
typedef int (*VrDecryptFn)(int conn, const char *user_id, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w);
typedef void (*VrPliFn)(int conn, uint32_t media_ssrc);    /* send an RTCP PLI for this ssrc (network thread) */

void vr_init(void);                                         /* idempotent; starts the decode thread */
void vr_set_conn(int conn, VrDecryptFn dec, VrPliFn pli);   /* dec NULL = no E2EE on this connection */
void vr_set_dave(int conn, bool on);                        /* frames of this connection must be DAVE-decrypted */
void vr_conn_reset(int conn);                               /* forget every stream of the connection (disconnect) */
/* op 12 VIDEO: user's video ssrcs (primary + rtx per simulcast layer); n == 0 removes the user's video */
void vr_map(int conn, const char *user_id, const uint32_t *ssrc, const uint32_t *rtx, int n);
/* incoming voice-gateway op 12 VIDEO payload ("d"): maps/unmaps the user's ssrcs. Returns the user id (or NULL). */
const char *vr_on_video_op(int conn, json_t *d);
/* one decrypted (transport layer) RTP packet: payload after the header extension, padding removed.
 * Returns true if it was video (consumed), false if the caller should treat it as audio. */
bool vr_rtp(int conn, uint32_t ssrc, int pt, uint16_t seq, uint32_t ts, bool marker, const uint8_t *pl, int n);

/* local picture source (own camera preview): open, feed NV12 pictures from any one thread, close */
void vr_local_open(int conn, const char *user_id);
bool vr_local_frame(int conn, const char *user_id, const uint8_t *y, const uint8_t *uv, int pitch, int w, int h, uint32_t ts90k);
void vr_local_close(int conn, const char *user_id);

/* ---- UI (main thread) ---- */
void vr_tick(void);                                          /* upload newly decoded frames; call once per frame */
bool vr_active(int conn, const char *user_id);               /* user has video mapped on this connection */
SDL_Texture *vr_texture(int conn, const char *user_id, int *w, int *h);   /* latest frame, NULL until the first one */
const char *vr_status(int conn, const char *user_id);        /* short state text ("waiting for keyframe", errors...) */
void vr_set_focus(int conn, const char *user_id);            /* this one is shown full size (converted at full res); NULL = none */
int vr_sink_quality(int conn);                               /* old: op 15 "any" value */
int vr_sink_wants(int conn, char *buf, int cap);             /* op 15 MEDIA_SINK_WANTS "d" JSON: any 50 for tiles, 100 (+pixelCounts) for the focused video */

typedef struct { uint32_t pkts, rtx_pkts, frames, drops, decrypt_ok, decrypt_fail, decoded, errors, plis; bool need_key; int w, h;
                 uint64_t exec_us, conv_us; uint32_t n_exec, n_conv, presented, late_drops; } VrStats;   /* exec = decode time without conversion */
bool vr_stats(int conn, const char *user_id, VrStats *out);

/* RFC 6184 packetiser (packetization-mode 1) for an Annex-B access unit: small NALs are aggregated into STAP-A,
 * NALs larger than mtu are split into FU-A, other NALs go out as single-NAL packets. cb gets each RTP payload;
 * marker is set on the last one. Works on DAVE-encrypted frames too (they keep their start codes). Returns packets. */
int vr_h264_packetize(const uint8_t *annexb, size_t n, int mtu, void (*cb)(void *user, const uint8_t *pl, int len, bool marker), void *user);

/* ---- self test (SD:/wiiu/apps/MingusDiscord/vtest.h264 present) ----
 * Annex-B file -> DAVE video encrypt (2-member MLS group) -> RFC 6184 packetise -> reorder + one lost packet ->
 * vr_rtp() on VR_CONN_TEST (decrypt, depacketise, hardware decode); shown as user_id's video. Results in log.txt. */
void vr_request_ui_bench(void);                              /* self test: log renderer formats / upload timings once (next vr_tick) */
void vr_selftest_start(const char *dir, const char *show_as_user_id);
#endif
