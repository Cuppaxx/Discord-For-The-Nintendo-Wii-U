#ifndef GW_H
#define GW_H
#include <stdbool.h>
/* Discord main gateway (websocket) on its own thread: identify, heartbeat, and the voice-state handshake.
 * Everything else (READY etc.) is received and dropped: the text client keeps using REST polling. */
enum { GW_OFF, GW_CONNECTING, GW_READY, GW_ERROR };
typedef struct { char user_id[24]; char username[64]; char channel_id[24]; bool mute, deaf; bool self; char avatar[48]; bool video, stream; } GwVoiceMember;
#define GW_MAX_MEMBERS 32
typedef struct {
  int state; char err[200];
  /* result of a join request */
  char session_id[80]; char vtoken[120]; char endpoint[160]; char guild_id[24]; char channel_id[24];
  bool have_state, have_server;
  GwVoiceMember members[GW_MAX_MEMBERS]; int nmembers;   /* people in the channel we joined (from VOICE_STATE_UPDATE events) */
  unsigned gen;                                            /* bumps on every change */
  /* Go Live: the one stream we watch (gw_watch_stream) */
  char stream_key[96]; int stream_state; char stream_reason[48];
} GwInfo;
enum { GWS_NONE, GWS_REQUESTED, GWS_CREATED, GWS_CONNECTING, GWS_ENDED };
/* watch a Go Live stream ("guild:<guild>:<channel>:<owner>" or "call:<channel>:<owner>"); "" or NULL stops watching.
 * Sends op 20 WATCH_STREAM; on STREAM_CREATE + STREAM_SERVER_UPDATE the stream connection (golive.c) is started. */
void gw_watch_stream(const char *stream_key);
void gw_set_self_video(bool on);         /* VOICE_STATE self_video (for the camera sender); resends the voice state */
void gw_start(const char *token, const char *user_id);
void gw_stop(void);
void gw_join_voice(const char *guild_id, const char *channel_id, bool mute, bool deaf);   /* guild_id may be "" (DM call) */
void gw_leave_voice(void);
void gw_snapshot(GwInfo *out);          /* thread-safe copy */
/* live chat events for one watched channel (the one on screen) */
enum { GWE_CREATE = 1, GWE_UPDATE, GWE_DELETE, GWE_REACT, GWE_TYPING, GWE_DM, GWE_CALL_CREATE, GWE_CALL_UPDATE, GWE_CALL_DELETE };   /* GWE_CALL_*: DM/group calls (any channel) */   /* GWE_DM: a new DM in a channel that is not on screen */
void gw_watch_channel(const char *channel_id);
bool gw_pop_event(int *kind, char **json);   /* json: the event's "d" object (caller frees) */
bool gw_ready(void);
void gw_set_guild_ids(const char *const *ids, int n);   /* my servers (from REST): sent as cached guild versions so READY skips their full data */
/* who is in which voice channel, in every server (READY + VOICE_STATE_UPDATE) */
typedef struct { char user[24], chan[24], guild[24], name[64], avatar[48]; bool mute, deaf, video, stream; } GwVS;
int gw_voice_table(GwVS *out, int max);   /* copy of the table */
unsigned gw_voice_gen(void);              /* bumps when the table changes */
void gw_fill_member(const char *uid, const char *name, const char *avatar);   /* fill a missing name/avatar (DM calls send none) */
#endif
