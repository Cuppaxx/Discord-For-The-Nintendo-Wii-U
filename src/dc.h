#ifndef DC_H
#define DC_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include "nxnet.h"
#include "nx.h"

#define MAX_GUILDS 200
#define MAX_CHANS 600
#define MAX_MSGS 120
#define MAX_AT 3

typedef struct { char id[24]; char name[72]; char icon[40]; unsigned long long perms; bool owner; } Guild;   /* perms: my permissions in it (from /users/@me/guilds) */
#define P_KICK (1ULL << 1)
#define P_BAN (1ULL << 2)
#define P_ADMIN (1ULL << 3)
#define P_MANAGE_MSG (1ULL << 13)
#define P_MUTE (1ULL << 22)
#define P_DEAFEN (1ULL << 23)
#define P_MOVE (1ULL << 24)
#define P_TIMEOUT (1ULL << 40)
typedef struct { char id[24]; char name[72]; int type; int pos; char parent[24]; bool header; char av[110]; unsigned long long last; NxNameStyle ns; char uid[24]; char topic[120]; } Chan;   /* av = CDN path of the DM/group avatar, last = last_message_id */
typedef struct {
  char name[56]; char url[600]; bool image; int w, h; bool video, gif, audio; char src[600];   /* src: original media (video/audio file, gifv mp4, gif) */
  char anim_url[600]; char ctype[64];          /* anim_url: animated image to play inline (gif / gifv webp) */
  void *anim; uint32_t anim_seen;             /* runtime: open Anim (anim.h) while on screen */
  bool voice; float dur; unsigned char wave[256]; int nwave;   /* voice message: duration + Discord waveform */
  SDL_Texture *tex; int tw, th; int state;   /* 0 none, 1 loading, 2 ready, 3 failed */
} Attach;
#define MAX_REACT 8
typedef struct { char key[100]; char code[110]; int count; bool me; } React;   /* key: drawable text (emoji or \x01c:id\x02), code: API form (emoji or name:id) */
typedef struct {
  char id[24]; char author[56]; char authorid[24]; char avatar[48]; char time[8]; unsigned color; long ts;
  char *text; int nat; Attach at[MAX_AT];
  NxNameStyle ns;                              /* author display-name style */
  bool has_reply; char reply_author[56], reply_authorid[24]; NxNameStyle reply_ns; char *reply;   /* "replying to" header (reply NULL = original deleted) */
  bool forwarded; char *card;
  React re[MAX_REACT]; int nre;                 /* reactions */
  char *raw;                                   /* original content (for editing) */                  /* forwarded message label; link-embed title/description card */
  int hcache, wcache;                          /* cached pixel height for a layout width */
} Msg;

void dc_init(void);
const char *dc_token(void); const char *dc_api(void); bool dc_has_token(void);
void dc_set_token(const char *t);
void dc_logout(void);
const char *dc_config_note(void);
const char *dc_ra_url(void);
const char *dc_autojoin(void);
bool dc_fakemic(void);   /* test aid: config.txt fakemic=1 -> recorders use a test tone instead of the GamePad mic */
const char *dc_dir(void);           /* SD app folder */
void dc_avatar_url(const char *userid, const char *hash, char *out, size_t cap);
void dc_cdn_url(const char *path, char *out, size_t cap);      /* "avatars/ID/HASH" -> full png url */
void dc_set_blocked(const char *relationships_json);           /* remember type-2 (blocked) users; hidden from the DM list */
int dc_parse_guild_order(const char *settings_json, char ids[][24], int max);   /* guild ids in the user's own order (guild_folders, flattened) */

NxJob *dc_get(const char *path);
NxJob *dc_post_message(const char *chan, const char *text);
NxJob *dc_send(const char *chan, const char *text, const char *reply_to);   /* reply_to: message id or NULL */
NxJob *dc_request(const char *method, const char *path, const char *body);   /* any REST call; body "" for empty PUT/DELETE */
void dc_urlenc(const char *in, char *out, size_t cap);
NxJob *dc_upload(const char *chan, const char *payload_json, const char *filename, const char *content_type, const unsigned char *data, size_t len);   /* multipart message with one file */

int dc_parse_guilds(const char *json, Guild *out, int max);
int dc_parse_channels(const char *json, Chan *out, int max);          /* text channels grouped under category headers */
int dc_parse_dms(const char *json, Chan *out, int max);
int dc_parse_messages(const char *json, Msg *out, int max);           /* returns oldest-first */
void dc_free_msg(Msg *m);
#include <jansson.h>
void dc_parse_style(json_t *user, NxNameStyle *out);
char *dc_clean_text(const char *in);
void dc_set_roles(const char *guild_id, const char *roles_json);   /* GET /guilds/{id}/roles */
const char *dc_roles_guild(void);   /* mentions/custom emoji/timestamps -> display text (malloc'd) */
const char *dc_error_text(const NxResp *r);
#endif
