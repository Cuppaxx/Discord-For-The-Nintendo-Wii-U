/* dave_c.h - minimal plain-C wrapper over libdave's own C API (dave/dave.h)
 * for the Wii U. Discord voice opcodes are handled by the caller:
 *   21 prepare_transition / 22 execute_transition / 23 transition_ready
 *   24 prepare_epoch / 25 mls_external_sender / 26 mls_key_package
 *   27 mls_proposals / 28 mls_commit_welcome / 29 announce_commit_transition
 *   30 mls_welcome / 31 mls_invalid_commit_welcome
 * This wrapper only deals with the raw MLS bytes (strip the binary-frame
 * header yourself: 2-byte seq + 1-byte opcode on server->client binary frames,
 * 1-byte opcode on client->server).
 * Buffers returned via out-params are malloc'd by libdave: free with dave_free(). */
#ifndef DAVE_C_H
#define DAVE_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct dave_session dave_session_t;
typedef struct dave_encryptor dave_encryptor_t;
typedef struct dave_decryptor dave_decryptor_t;

void     dave_free(void *p);
uint16_t dave_max_protocol_version(void);

/* optional log sink: severity 0=verbose 1=info 2=warning 3=error */
/* REQUIRED for production: register a real entropy source (fill buf, return 0 on success).
 * Without it a weak timing-based fallback is used. Call before creating a session. */
void dave_set_entropy_source(int (*cb)(void *buf, size_t len));

void dave_set_log_callback(void (*cb)(int severity, const char *file, int line, const char *msg));

/* failure callback (source, reason) for MLS protocol errors; may be NULL */
typedef void (*dave_fail_cb)(const char *source, const char *reason, void *user);

/* Session. channel_id = voice channel id (MLS group id), user_id = decimal string of our user id.
 * Call dave_session_init() on opcode 24 / when joining (and again for a new epoch==1 group). */
dave_session_t *dave_session_new(dave_fail_cb cb, void *user);
void            dave_session_free(dave_session_t *s);
void            dave_session_init(dave_session_t *s, uint16_t protocol_version,
                                  uint64_t channel_id, const char *user_id);
void            dave_session_reset(dave_session_t *s);
void            dave_session_set_protocol_version(dave_session_t *s, uint16_t v);
uint16_t        dave_session_get_protocol_version(dave_session_t *s);

/* opcode 25 payload -> set external sender */
void dave_session_set_external_sender(dave_session_t *s, const uint8_t *data, size_t len);

/* Our key package: send as opcode 26 (prefix byte 26 on the wire). malloc'd; 0 on success. */
int  dave_session_get_key_package(dave_session_t *s, uint8_t **out, size_t *out_len);

/* opcode 27 payload -> optional commit+welcome bytes to send as opcode 28.
 * *out may come back NULL/0 if there is nothing to send. recognized_ids = decimal
 * user-id strings of everyone currently in the call. */
int  dave_session_process_proposals(dave_session_t *s, const uint8_t *data, size_t len,
                                    const char **recognized_ids, size_t n_ids,
                                    uint8_t **out, size_t *out_len);

/* opcode 29 / 30 payloads. Return: 0 ok, 1 failed (send opcode 31 invalid_commit_welcome
 * and re-send key package), 2 ignored. roster_ids optional (uint64 array, free with dave_free). */
int  dave_session_process_commit(dave_session_t *s, const uint8_t *data, size_t len,
                                 uint64_t **roster_ids, size_t *n_roster);
int  dave_session_process_welcome(dave_session_t *s, const uint8_t *data, size_t len,
                                  const char **recognized_ids, size_t n_ids,
                                  uint64_t **roster_ids, size_t *n_roster);

/* Encryptor (one, for our own outgoing audio). Call set_ratchet after every epoch
 * transition executes (opcode 22 / right after welcome), with our own user id. */
dave_encryptor_t *dave_encryptor_new(void);
void              dave_encryptor_free(dave_encryptor_t *e);
int               dave_encryptor_set_ratchet(dave_encryptor_t *e, dave_session_t *s, const char *self_user_id);
void              dave_encryptor_set_passthrough(dave_encryptor_t *e, int on);
int               dave_encryptor_has_ratchet(dave_encryptor_t *e);
size_t            dave_encryptor_max_size(dave_encryptor_t *e, size_t opus_len);
/* returns 0 on success (bytes in out/out_len), else a DAVEEncryptorResultCode */
int dave_encrypt_opus(dave_encryptor_t *e, uint32_t ssrc, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, size_t *out_len);

/* Decryptor (one per remote user). */
dave_decryptor_t *dave_decryptor_new(void);
void              dave_decryptor_free(dave_decryptor_t *d);
int               dave_decryptor_set_ratchet(dave_decryptor_t *d, dave_session_t *s, const char *peer_user_id);
void              dave_decryptor_set_passthrough(dave_decryptor_t *d, int on);
size_t            dave_decryptor_max_size(dave_decryptor_t *d, size_t enc_len);
/* returns 0 on success, else a DAVEDecryptorResultCode */
int dave_decrypt_opus(dave_decryptor_t *d, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, size_t *out_len);

/* ---- video (camera / Go Live) ----
 * DAVE encrypts a whole encoded frame (before RTP packetisation). For H.264 the input must be Annex-B
 * (start codes); the output keeps every start code as 4 bytes, NAL headers and the slice-header bytes up to
 * the PPS id in the clear (libdave codec_utils), so the RTP packetiser can still split it into NAL units.
 * Decrypting is codec-unaware: pass the reassembled Annex-B frame (4-byte start codes, exactly as libwebrtc's
 * depacketiser emits it) and get the plaintext frame back.
 * Use a decryptor that is separate from the user's audio decryptor (same ratchet): senders may run separate
 * nonce counters per media type, and each decryptor keeps its own replay window. */
#define DAVE_C_CODEC_VP8  2
#define DAVE_C_CODEC_VP9  3
#define DAVE_C_CODEC_H264 4
#define DAVE_C_CODEC_H265 5
#define DAVE_C_CODEC_AV1  6
size_t dave_encryptor_max_size_video(dave_encryptor_t *e, size_t frame_len);
/* codec: DAVE_C_CODEC_*; ssrc = the video (primary) ssrc the frame is sent on. 0 on success, else a
 * DAVEEncryptorResultCode (or -1 bad codec, -2 out_cap < dave_encryptor_max_size_video()). */
int dave_encrypt_video(dave_encryptor_t *e, uint32_t ssrc, int codec, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap, size_t *out_len);
size_t dave_decryptor_max_size_video(dave_decryptor_t *d, size_t enc_len);
/* 0 on success, else a DAVEDecryptorResultCode (or -2 out_cap too small) */
int dave_decrypt_video(dave_decryptor_t *d, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap, size_t *out_len);
/* 1 if the buffer ends with the DAVE trailer magic (0xFA 0xFA), i.e. looks E2EE-wrapped */
int dave_frame_looks_encrypted(const uint8_t *in, size_t in_len);

/* ---- test helpers (not for production paths) ----
 * dave_test_make_pair: builds a real 2-member MLS group like the voice server would (fake external sender,
 * external add proposal, commit + welcome): *a is member ua (committer), *b member ub. 0 on success.
 * dave_test_get_key: raw AES-128 key of generation gen of user uid's ratchet in session s (so a frame can be
 * re-checked by an independent AES-GCM implementation, e.g. on a little-endian PC). 0 on success. */
int dave_test_make_pair(uint64_t group_id, const char *ua, const char *ub, dave_session_t **a, dave_session_t **b);
int dave_test_get_key(dave_session_t *s, const char *uid, uint32_t gen, uint8_t key[16]);

#ifdef __cplusplus
}
#endif
#endif
