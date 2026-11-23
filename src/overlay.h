#ifndef OVERLAY_H
#define OVERLAY_H
#include "dc.h"
#include "nx.h"
#include <stdbool.h>
/* modal overlays drawn on top of the chat: message actions, emoji picker, user profile */
enum { EMO_INSERT, EMO_REACT };
enum { OVR_NONE, OVR_INSERT, OVR_REACT, OVR_REPLY, OVR_MENTION, OVR_OPEN_DM, OVR_PROFILE, OVR_EMOJI_REACT, OVR_SEND_TEXT, OVR_EDIT, OVR_DELETE, OVR_OPEN_MEDIA, OVR_PIN, OVR_MEMBER_MENU, OVR_LOGOUT };
typedef struct {
  int kind;                 /* OVR_* */
  char text[160];           /* OVR_INSERT: text for the composer; OVR_OPEN_DM: channel name */
  char code[160];           /* OVR_REACT: reaction in API form (emoji or name:id) */
  char msg_id[24], user_id[24], chan_id[24]; char name[80];
} OvResult;
typedef SDL_Texture *(*OvImgFn)(const char *cdn_path, int size);   /* circular image from the CDN (main.c's cache) */
void ov_init(OvImgFn img);
bool ov_active(void);
void ov_open_actions(const char *msg_id, const char *user_id, const char *name);
void ov_open_actions_ex(const char *msg_id, const char *user_id, const char *name, bool own);   /* own: adds Edit / Delete */
void ov_actions_media(bool has_media);
void ov_actions_mod(bool can_manage_messages);   /* call before ov_open_actions*: moderators get Delete / Pin on any message */
void ov_open_member(const char *uid, const char *name, const char *guild_id, unsigned long long perms, bool in_voice, bool srv_mute, bool srv_deaf);   /* volume, local mute, moderation */
void ov_open_people(int n, const char ids[][24], const char names[][80], const bool *mute, const bool *deaf, const char *guild_id, unsigned long long perms);
void ov_load_volumes(void);
void ov_open_mic(void);                         /* mic volume (amplify) + auto volume + level meter */                      /* per-user volumes saved on the SD card */
void ov_profile_perms(unsigned long long perms); /* moderation available from the profile card's Options button */   /* call before ov_open_actions*: adds "Open attachment" */
void ov_open_emoji(int purpose, const char *guild_id, const char *msg_id);
void ov_emoji_guilds(const Guild *g, int n);   /* all my servers: their custom emoji show under "All servers" (fetched in the background) */
void ov_open_gifs(void);                 /* GIF picker (Klipy via Discord); picking returns OVR_SEND_TEXT with the GIF link */
void ov_open_profile(const char *user_id, const char *guild_id);
bool ov_input(const NxInput *in, OvResult *res);   /* while active it consumes all input; fills res (kind != OVR_NONE) when the user picks something */
void ov_tick(void);
void ov_draw(void);
#endif
