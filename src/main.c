/* Discord: For the Nintendo Wii U - native Discord client for Wii U (REST polling, QR login, no PC needed) */
#include <SDL2/SDL_image.h>
#include "nx.h"
#include "nxnet.h"
#include "dc.h"
#include "overlay.h"
#include "md.h"
#include "anim.h"
#include "rec.h"
#include "vrec.h"
#include "ra.h"
#include "qrcodegen.h"
#include "gw.h"
#include "netmem.h"
#include "voice.h"
#include "vcview.h"
#include "dlog.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define C_BG RGB(49, 51, 56)
#define C_SIDE RGB(43, 45, 49)
#define C_DARK RGB(30, 31, 34)
#define C_SEL RGB(64, 66, 73)
#define C_BLURPLE RGB(88, 101, 242)
#define C_TEXT RGB(219, 222, 225)
#define C_MUTED RGB(148, 155, 164)
#define C_WHITE RGB(255, 255, 255)
#define C_RED RGB(237, 66, 69)
#define C_GREEN RGB(35, 165, 90)
#define C_INPUT RGB(56, 58, 64)
#define C_PANEL RGB(35, 36, 40)        /* user panel at the bottom of the sidebar */
#define C_ROWSEL RGB(64, 66, 73)
#define C_ICON RGB(181, 186, 193)
#define USER_H 60
#define VCP_H 62

#define RAIL_W 76
#define CH_X 76
#define CH_W 264
#define CHAT_X 340
#define CHAT_W 940
#define HDR_H 56
#define LIST_Y 64
#define MSG_BOT 636
#define TEXT_X (CHAT_X + 78)
#define TEXT_W 832
#define GUILD_PITCH 64

typedef enum { S_LOGIN, S_MAIN, S_QR } Screen;
enum { J_NONE, J_GUILDS, J_CHANS, J_DMS, J_MSGS, J_POLL, J_SEND, J_ME, J_REL, J_SETT, J_REFRESH, J_ROLES };

static Guild guilds[MAX_GUILDS + 1]; static int ng;
static char gorder[MAX_GUILDS * 2][24]; static int ngorder;
/* server folders (settings.guild_folders with an id): the rail shows them as collapsible groups */
typedef struct { char name[64]; unsigned color; bool has_color, open; } Folder;
static Folder folders[96]; static int nfolders; static short gorder_f[MAX_GUILDS * 2];   /* folder of each gorder entry (-1 none) */
static signed char gfold[MAX_GUILDS + 1];                                              /* folder of each guilds[] entry */
typedef struct { int kind, gi, fi; } RItem;                                             /* kind 0 DMs, 1 server, 2 folder */
static RItem rail[MAX_GUILDS + 100]; static int nrail, rsel, rscroll; static bool sett_ready, rel_ready;   /* user guild order (settings) + blocked list (relationships), fetched once per refresh */
static Chan chans[MAX_CHANS]; static int nc; static Chan dms[MAX_CHANS]; static int ndm;
static Msg msgs[MAX_MSGS]; static int nm;
static Screen screen = S_LOGIN;
static int pane, gsel, gscroll, csel = -1, cscroll;
static bool in_dms;
static char cur_chan[24], cur_name[80], view_guild[24];
static float chat_scroll; static int chat_total;
static char compose[1500]; static char status[220]; static uint32_t status_until;
static NxJob *job; static int jk; static char jctx[24];
static struct { char id[24], name[80]; uint32_t until; } typing[6]; static int ntyping;   /* "X is typing..." from the gateway */
static char edit_id[24];                                /* composer is editing this message */
/* local read state: last read message id per channel (SD:/.../readstate.txt), so unread DMs/channels can be marked */
static struct { unsigned long long ch, last; } rs[3000]; static int nrs; static bool rs_dirty; static uint32_t rs_saved;
static unsigned long long *rs_get(unsigned long long ch) { for (int i = 0; i < nrs; i++) if (rs[i].ch == ch) return &rs[i].last; return NULL; }
static void rs_set(unsigned long long ch, unsigned long long last) { unsigned long long *p = rs_get(ch); if (p) { if (last > *p) { *p = last; rs_dirty = true; } } else if (nrs < 3000) { rs[nrs].ch = ch; rs[nrs].last = last; nrs++; rs_dirty = true; } }
static void rs_load(void) { char p[300]; snprintf(p, sizeof p, "%s/readstate.txt", dc_dir()); FILE *f = fopen(p, "rb"); if (!f) return; unsigned long long a, b; while (nrs < 3000 && fscanf(f, "%llu %llu", &a, &b) == 2) { rs[nrs].ch = a; rs[nrs].last = b; nrs++; } fclose(f); }
static void rs_save(void) { if (!rs_dirty || nx_ticks() - rs_saved < 5000) return; rs_saved = nx_ticks(); rs_dirty = false; char p[300]; snprintf(p, sizeof p, "%s/readstate.txt", dc_dir()); FILE *f = fopen(p, "wb"); if (!f) return; for (int i = 0; i < nrs; i++) fprintf(f, "%llu %llu\n", rs[i].ch, rs[i].last); fclose(f); }
static bool chan_unread(const Chan *c) {           /* first sight of a channel counts as read */
  if (!c->last || c->header) return false; unsigned long long id = strtoull(c->id, NULL, 10); unsigned long long *p = rs_get(id);
  if (!p) { rs_set(id, c->last); return false; } return c->last > *p;
}
static NxJob *ack_job;
static int text_center(int cx, int y, const char *s, int size, NxColor c);
static int anims_open;                                  /* inline animations currently open (closed when off screen) */
static Anim *mv; static bool mv_on; static char mv_name[80]; static SDL_Texture *mv_still; static int mv_sw, mv_sh; static bool mv_owns_still;   /* media viewer */
static void mv_close(void) { if (mv) anim_close(mv); mv = NULL; mv_on = false; }
static void mv_open(const Attach *a) {
  mv_close(); snprintf(mv_name, sizeof mv_name, "%s", a->name); mv_still = a->state == 2 ? a->tex : NULL; mv_sw = a->tw; mv_sh = a->th; mv_on = true;
  if (a->audio || a->video) mv = anim_open_media(a->src[0] ? a->src : a->url, a->ctype[0] ? a->ctype : NULL);
  else if (a->anim_url[0]) mv = strstr(a->anim_url, ".webp") ? anim_open_webp(a->anim_url, 1180, 560) : anim_open_gif(a->anim_url, 1180, 560);
  else if (a->src[0] && strstr(a->src, ".mp4")) mv = anim_open_clip(a->src);
}
static void mv_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 225));
  SDL_Texture *t = NULL; int w = 0, h = 0; if (mv) { anim_tick(mv); t = anim_frame(mv, &w, &h); }
  if (!t && mv_still && !(mv && !anim_has_video(mv) && anim_state(mv) != ANIM_LOADING && (mv_sw == 0))) { t = mv_still; w = mv_sw; h = mv_sh; }
  bool audio = mv && anim_state(mv) != ANIM_LOADING && !anim_has_video(mv) && !t;
  if (t && w > 0 && h > 0) { float s = fminf(1180.0f / w, 560.0f / h); int dw = (int)(w * s), dh = (int)(h * s); nx_draw_tex(t, (NX_W - dw) / 2, 20 + (580 - dh) / 2, dw, dh); }
  else if (audio || (mv && anim_state(mv) == ANIM_LOADING)) { nx_circle(NX_W / 2, 280, 90, C_BLURPLE); text_center(NX_W / 2, 238, audio ? "\xE2\x99\xAA" : "...", 3 | NX_BOLD, C_WHITE); }
  nx_text(40, 612, mv_name, 1 | NX_BOLD, C_WHITE, 700);
  if (mv && anim_state(mv) == ANIM_ERROR) nx_text(40, 650, anim_error(mv), 0, C_RED, 1200);
  else if (mv) { double d = anim_duration(mv), tt = anim_time(mv); char b[80]; snprintf(b, sizeof b, "%d:%02d / %d:%02d   %s%s", (int)tt / 60, (int)tt % 60, (int)d / 60, (int)d % 60, anim_paused(mv) || anim_ended(mv) ? "paused" : "playing", anim_loaded_pct(mv) < 100 ? "  (loading)" : "");
    nx_text(40, 650, b, 0, C_MUTED, 0); nx_rrect(40, 684, NX_W - 80, 8, 4, C_SEL); if (d > 0) nx_rrect(40, 684, (int)((NX_W - 80) * fmin(1.0, tt / d)), 8, 4, C_BLURPLE); }
  nx_text(NX_W - 470, 612, "A play/pause   <-/-> 10s   ZL/ZR 60s   B close", 0, C_MUTED, 0);
}
static void mv_input(NxInput *in) {
  if (in->pressed[B_B] || (in->touch_pressed && in->ty < 600 && !mv)) { mv_close(); return; }
  if (!mv) return;
  if (in->pressed[B_A] || (in->touch_pressed && in->ty < 600)) anim_toggle(mv);
  if (in->pressed[B_LEFT]) anim_seek(mv, -10); if (in->pressed[B_RIGHT]) anim_seek(mv, 10);
  if (in->pressed[B_ZL]) anim_seek(mv, -60); if (in->pressed[B_ZR]) anim_seek(mv, 60);
  if (in->touch_pressed && in->ty >= 670 && anim_duration(mv) > 0) { double frac = (in->tx - 40) / (double)(NX_W - 80); anim_seek(mv, frac * anim_duration(mv) - anim_time(mv)); }
}
static void anims_sweep(void) {                          /* close inline animations that went off screen */
  uint32_t now = nx_ticks(); int open = 0;
  for (int i = 0; i < nm; i++) for (int k = 0; k < msgs[i].nat; k++) { Attach *a = &msgs[i].at[k]; if (!a->anim) continue;
    if (now - a->anim_seen > 2500) { anim_close((Anim *)a->anim); a->anim = NULL; } else open++; }
  anims_open = open;
}
static char voice_guild[24];   /* tentative definition; declared again with the other voice state below */
typedef struct { char id[24], name[80], avatar[48], g[24]; int state; NxJob *job; } VUser;   /* 0 want, 1 loading, 2 ok, 3 failed; g: server to ask for the nickname ("" = the voice server) */
#define NVU 128
static VUser vusers[NVU]; static int nvusers;
static VUser *vuser_g(const char *g, const char *id) {
  for (int i = 0; i < nvusers; i++) if (!strcmp(vusers[i].id, id)) return &vusers[i];
  if (nvusers >= NVU) return NULL; VUser *u = &vusers[nvusers++]; memset(u, 0, sizeof *u); snprintf(u->id, 24, "%s", id); snprintf(u->g, 24, "%s", g ? g : ""); return u;
}
static VUser *vuser(const char *id) { return vuser_g(NULL, id); }
static void vusers_tick(void) {
  int loading = 0; for (int i = 0; i < nvusers; i++) if (vusers[i].job) loading++;
  for (int i = 0; i < nvusers; i++) { VUser *u = &vusers[i];
    if (u->job && nxnet_job_done(u->job)) { NxResp r; nxnet_job_take(u->job, &r); u->job = NULL; loading--;
      if (r.status == 200) { json_t *j = json_loads(r.body, 0, NULL); json_t *us = json_object_get(j, "user") ? json_object_get(j, "user") : j;
        const char *nk = json_string_value(json_object_get(j, "nick")), *gn = json_string_value(json_object_get(us, "global_name")), *un = json_string_value(json_object_get(us, "username")), *av = json_string_value(json_object_get(us, "avatar"));
        snprintf(u->name, sizeof u->name, "%s", nk ? nk : gn ? gn : un ? un : u->id); snprintf(u->avatar, sizeof u->avatar, "%s", av ? av : ""); u->state = 2; json_decref(j); }
      else if (u->state == 1 && (u->g[0] || voice_guild[0]) && r.status != 200 && !strstr(u->name, "/")) { snprintf(u->name, sizeof u->name, "/"); u->state = 0; }   /* member lookup refused: retry as a plain user */
      else u->state = 3;
      nxnet_free(&r); }
    else if (u->state == 0 && loading < 2) { char p[120]; const char *gg = u->g[0] ? u->g : voice_guild; if (gg[0] && strcmp(u->name, "/")) snprintf(p, sizeof p, "/guilds/%s/members/%s", gg, u->id); else snprintf(p, sizeof p, "/users/%s", u->id); u->job = dc_get(p); u->state = 1; loading++; } }
}
static unsigned long long my_perms(void) { if (in_dms || gsel <= 0 || gsel >= ng) return 0; return guilds[gsel].perms; }
static bool deaf_on;
static char voice_chan[24];   /* tentative definition (declared again with the voice state below) */
static char my_name[80], my_avatar[48]; static NxNameStyle my_ns; static int tz_min = 0;   /* tz_min: minutes behind UTC (Discord settings.timezone_offset) */
static char cur_topic[120];
/* "Today at 4:39 PM" / "Yesterday at ..." / "11/07/2026 4:39 PM" in the user's own timezone */
static long local_day(long t) { return (t >= 0 ? t : t - 86399) / 86400; }
static void fmt_time(long ts, char *out, size_t n, bool date_too) {
  time_t lt = (time_t)(ts - tz_min * 60); struct tm a; if (!gmtime_r(&lt, &a)) { snprintf(out, n, "?"); return; }
  int h12 = a.tm_hour % 12 ? a.tm_hour % 12 : 12; const char *ap = a.tm_hour < 12 ? "AM" : "PM";
  long today = local_day((long)time(NULL)), d = local_day((long)lt);   /* the console clock runs on local time */
  if (!date_too) snprintf(out, n, "%d:%02d %s", h12, a.tm_min, ap);
  else if (d == today) snprintf(out, n, "Today at %d:%02d %s", h12, a.tm_min, ap);
  else if (d == today - 1) snprintf(out, n, "Yesterday at %d:%02d %s", h12, a.tm_min, ap);
  else snprintf(out, n, "%02d/%02d/%04d %d:%02d %s", a.tm_mon + 1, a.tm_mday, a.tm_year + 1900, h12, a.tm_min, ap);
}
static void fmt_date(long ts, char *out, size_t n) { static const char *mn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}; time_t lt = (time_t)(ts - tz_min * 60); struct tm a; if (gmtime_r(&lt, &a)) snprintf(out, n, "%s %d, %d", mn[a.tm_mon], a.tm_mday, a.tm_year + 1900); else out[0] = 0; }
/* ---- small vector icons (Discord-like) ---- */
static void ico_line(int x1, int y1, int x2, int y2, int th, NxColor c) { SDL_Renderer *r = nx_renderer(); SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); for (int t = -th / 2; t <= th / 2; t++) { SDL_RenderDrawLine(r, x1 + t, y1, x2 + t, y2); } }
static void ico_mic(int cx, int cy, NxColor c, bool off) { nx_rrect(cx - 5, cy - 12, 10, 16, 5, c); nx_rect(cx - 9, cy - 2, 2, 7, c); nx_rect(cx + 7, cy - 2, 2, 7, c); nx_rect(cx - 9, cy + 5, 18, 2, c); nx_rect(cx - 1, cy + 7, 2, 4, c); nx_rect(cx - 5, cy + 11, 10, 2, c); if (off) ico_line(cx - 11, cy - 12, cx + 11, cy + 12, 3, C_RED); }
static void ico_phones(int cx, int cy, NxColor c, NxColor bg, bool off) { nx_circle(cx, cy, 12, c); nx_circle(cx, cy, 9, bg); nx_rect(cx - 13, cy, 26, 14, bg); nx_rrect(cx - 12, cy - 1, 6, 12, 2, c); nx_rrect(cx + 6, cy - 1, 6, 12, 2, c); if (off) ico_line(cx - 12, cy - 12, cx + 12, cy + 12, 3, C_RED); }
static void ico_gear(int cx, int cy, NxColor c, NxColor bg) { for (int k = 0; k < 8; k++) { float a = k * 3.14159265f / 4; int tx = cx + (int)(9 * cosf(a)), ty = cy + (int)(9 * sinf(a)); nx_rect(tx - 2, ty - 2, 5, 5, c); } nx_circle(cx, cy, 8, c); nx_circle(cx, cy, 3, bg); }
static void ico_speaker(int x, int y, NxColor c) { nx_rect(x, y + 5, 4, 6, c); for (int i = 0; i < 6; i++) nx_rect(x + 4 + i, y + 5 - i, 1, 6 + 2 * i, c); nx_text(x + 12, y - 6, ")", 0 | NX_BOLD, c, 0); }
static void ico_hash(int x, int y, NxColor c) { ico_line(x + 6, y, x + 3, y + 18, 2, c); ico_line(x + 13, y, x + 10, y + 18, 2, c); nx_rect(x + 1, y + 5, 15, 2, c); nx_rect(x, y + 11, 15, 2, c); }
static int list_bot(void) { return NX_H - USER_H - (voice_chan[0] ? VCP_H : 0); }
static int msel = -1;                                   /* selected message (chat pane), -1 none */
static char reply_to[24], reply_name[80]; static bool send_keep_compose;   /* a GIF send must not wipe the draft */               /* composer is replying to this message */
static NxJob *rjob; static bool want_refresh;           /* reaction PUT/DELETE; refresh messages afterwards (keeps scroll) */
typedef struct { int i, x, y, w, h, kind, ri; } Hit;    /* hit boxes recorded while drawing the chat: 0 message, 1 avatar/name, 2 reaction chip */
static Hit hits[200]; static int nhits;
static int tap_x, tap_y, tap_moved; static bool tap_down;
static char ment_id[8][24], ment_name[8][80]; static int nment;   /* mentions typed into the composer: <@id> shown as @name */
static bool want_guilds, want_dms, want_msgs, send_pending; static char want_chans[24]; static uint32_t next_poll;
static char send_buf[1500];
static struct { NxJob *job; char mid[24]; int idx; } img[2];
static uint32_t stick_t;
static void say(const char *s, int ms);
/* voice */
static char my_user_id[24]; static bool want_me, gw_started;
static bool vc_chat;   /* the chat on screen is a voice channel's text chat */
static char voice_chan[24], voice_name[80], voice_guild[24]; static bool voice_started, voice_muted; static bool show_voice;   /* show_voice: chat pane shows the voice panel */
static void voice_leave(void) { if (!voice_chan[0]) return; gw_leave_voice(); voice_stop(); voice_chan[0] = 0; voice_started = false; show_voice = false; say("Left voice", 1500); }
static void open_channel(const Chan *c);
static void open_vc_chat(const char *id, const char *name) {      /* a voice channel's built-in text chat */
  Chan t; memset(&t, 0, sizeof t); snprintf(t.id, 24, "%s", id); snprintf(t.name, sizeof t.name, "%s", name); t.type = 0; open_channel(&t); vc_chat = true;
}
static void open_vc_chat_cur(void) { if (voice_chan[0]) open_vc_chat(voice_chan, voice_name); }
static void voice_join(const Chan *c) {
  if (voice_chan[0] && !strcmp(voice_chan, c->id)) { show_voice = true; pane = 2; return; }   /* already in it: just show the call */
  if (!my_user_id[0]) { say("Still connecting to Discord...", 2000); return; }
  if (voice_chan[0] && strcmp(voice_chan, c->id)) { gw_leave_voice(); voice_stop(); voice_started = false; }
  snprintf(voice_chan, sizeof voice_chan, "%s", c->id); snprintf(voice_name, sizeof voice_name, "%s", c->name); snprintf(voice_guild, sizeof voice_guild, "%s", in_dms ? "" : view_guild);
  gw_join_voice(voice_guild, c->id, voice_muted, false); show_voice = true; pane = 2; say(in_dms ? "Calling..." : "Joining voice...", 3000);
}
static void voice_tick(void) {
  static bool aj_done; const char *aj = dc_autojoin();      /* test aid only: config.txt autojoin=<guild>:<channel> */
  if (!aj_done && aj[0] && my_user_id[0] && gw_started) { aj_done = true; char g[24] = "", c[24] = ""; if (sscanf(aj, "%23[0-9]:%23[0-9]", g, c) == 2) { snprintf(voice_chan, sizeof voice_chan, "%s", c); snprintf(voice_name, sizeof voice_name, "autojoin"); snprintf(voice_guild, sizeof voice_guild, "%s", g); gw_join_voice(g, c, voice_muted, false); show_voice = true; pane = 2; } }
  if (!voice_chan[0] || voice_started) return;
  GwInfo g; gw_snapshot(&g);
  if (g.have_state && g.have_server && !strcmp(g.channel_id, voice_chan)) { voice_start(voice_guild[0] ? voice_guild : voice_chan, voice_chan, my_user_id, g.session_id, g.vtoken, g.endpoint); voice_started = true; }   /* DM calls: server id = channel id */
  if (g.state == GW_ERROR && g.err[0]) say(g.err, 4000);
}

/* image cache: avatars, server icons, group icons - keyed by CDN path ("avatars/ID/HASH", "icons/ID/HASH") */
typedef struct { char path[110]; int sz; SDL_Texture *tex; int state; NxJob *job; uint32_t used; } Av;   /* 0 want, 1 loading, 2 ok, 3 failed */
static Av avs[96];

static void anim_log(const char *line) { dlog("[anim] %s", line); }
static void say(const char *s, int ms) { snprintf(status, sizeof status, "%s", s); status_until = nx_ticks() + ms; }
static Chan *view(void) { return in_dms ? dms : chans; }
static int view_n(void) { return in_dms ? ndm : nc; }
static GwVS vst[3000]; static int nvst; static unsigned vst_gen = ~0u;
static void vst_refresh(void) { unsigned gg = gw_voice_gen(); if (gg == vst_gen) return; vst_gen = gg; nvst = gw_voice_table(vst, 3000); }
static int vc_count(const char *chan) { int n = 0; for (int i = 0; i < nvst; i++) if (!strcmp(vst[i].chan, chan)) n++; return n > 12 ? 12 : n; }
#define VCU_H 30
static int row_h(const Chan *c) { if (!c->header && !in_dms && c->type == 2) return 40 + VCU_H * vc_count(c->id); return c->header ? 34 : (in_dms ? 52 : 40); }

static unsigned name_color(const char *s) {
  static const unsigned pal[] = {0x5865f2, 0xeb459e, 0x3ba55d, 0xf0b232, 0xed4245, 0x00b0f4, 0x9b59b6, 0xe67e22};
  unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return pal[h % 8];
}
#define UC(c) RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)
static int text_center(int cx, int y, const char *s, int size, NxColor c) { int w = nx_text_w(s, size); nx_text(cx - w / 2, y, s, size, c, 0); return w; }

static void clear_msgs(void) { for (int i = 0; i < nm; i++) dc_free_msg(&msgs[i]); nm = 0; chat_scroll = 0; for (int i = 0; i < 2; i++) if (img[i].job) { nxnet_job_cancel(img[i].job); img[i].job = NULL; } }

static void open_channel(const Chan *c) {
  if (c->type == 2) { voice_join(c); return; }
  mv_close(); show_voice = false; vc_chat = false; clear_msgs(); msel = -1; reply_to[0] = 0; nment = 0; ntyping = 0; gw_watch_channel(c->id); snprintf(cur_chan, sizeof cur_chan, "%s", c->id); snprintf(cur_name, sizeof cur_name, "%s", c->name); snprintf(cur_topic, sizeof cur_topic, "%s", c->topic);
  want_msgs = true; next_poll = nx_ticks() + 3000; pane = 2; edit_id[0] = 0;
  if (c->last) rs_set(strtoull(c->id, NULL, 10), c->last);
}
static void select_guild(int i) {
  gsel = i; chans[0].id[0] = 0; nc = 0; ndm = 0; csel = -1; cscroll = 0; clear_msgs(); cur_chan[0] = 0;
  if (i == 0) { in_dms = true; want_dms = true; want_chans[0] = 0; }
  else { in_dms = false; snprintf(want_chans, sizeof want_chans, "%s", guilds[i].id); snprintf(view_guild, sizeof view_guild, "%s", guilds[i].id); }
  pane = 1;
}
static void first_channel(void) { Chan *v = view(); for (int i = 0; i < view_n(); i++) if (!v[i].header) { csel = i; return; } csel = -1; }
static void go_login(const char *why) { screen = S_LOGIN; if (why) say(why, 8000); clear_msgs(); ng = nc = ndm = 0; cur_chan[0] = 0; }

/* ---------------- avatars ---------------- */
static SDL_Texture *av_get(const char *path, int sz) {
  if (!path || !path[0]) return NULL;
  Av *free_slot = NULL, *oldest = NULL;
  for (int i = 0; i < 96; i++) {
    Av *a = &avs[i];
    if (a->path[0] && !strcmp(a->path, path)) { a->used = nx_ticks(); return a->state == 2 ? a->tex : NULL; }
    if (!a->path[0]) { if (!free_slot) free_slot = a; }
    else if (a->state != 1 && (!oldest || a->used < oldest->used)) oldest = a;
  }
  Av *s = free_slot ? free_slot : oldest; if (!s) return NULL;
  if (s->tex) SDL_DestroyTexture(s->tex); memset(s, 0, sizeof *s);
  snprintf(s->path, sizeof s->path, "%s", path); s->sz = sz; s->used = nx_ticks(); return NULL;
}
static SDL_Texture *av_tex(const Msg *m) { if (!m->avatar[0]) return NULL; char p[110]; snprintf(p, sizeof p, "avatars/%s/%s", m->authorid, m->avatar); return av_get(p, 44); }
static void av_tick(void) {
  int loading = 0; for (int i = 0; i < 96; i++) if (avs[i].job) loading++;
  for (int i = 0; i < 96; i++) {
    Av *a = &avs[i];
    if (a->job && nxnet_job_done(a->job)) { NxResp r; nxnet_job_take(a->job, &r); a->job = NULL; loading--; if (r.status == 200 && r.len) { a->tex = strstr(a->path, "#rr") ? nx_rrect_tex_from_mem(r.body, (int)r.len, a->sz, a->sz * 3 / 10) : nx_circle_tex_from_mem(r.body, (int)r.len, a->sz); a->state = a->tex ? 2 : 3; } else a->state = 3; nxnet_free(&r); }
    else if (a->path[0] && a->state == 0 && !a->job && loading < 3) { char u[300], pth[120]; snprintf(pth, sizeof pth, "%s", a->path); char *rr = strstr(pth, "#rr"); if (rr) *rr = 0; dc_cdn_url(pth, u, sizeof u); const char *h[] = {"User-Agent: Mozilla/5.0", NULL}; a->state = 1; a->job = nxnet_start("GET", u, h, NULL, 0, 1u << 20); loading++; }
  }
}

/* put the guild list in the user's own order (guild_folders from settings, folders flattened); unknown guilds go last */
static void apply_order(void) {
  if (!ngorder || ng < 2) return;
  static Guild tmp[MAX_GUILDS + 1]; static bool used[MAX_GUILDS + 1]; char sel[24]; snprintf(sel, sizeof sel, "%s", gsel > 0 && gsel < ng ? guilds[gsel].id : "");
  memset(used, 0, sizeof used); int n = 0;
  for (int o = 0; o < ngorder; o++) for (int i = 1; i < ng; i++) if (!used[i] && !strcmp(guilds[i].id, gorder[o])) { used[i] = true; tmp[n++] = guilds[i]; break; }
  for (int i = 1; i < ng; i++) if (!used[i]) tmp[n++] = guilds[i];
  for (int i = 0; i < n; i++) guilds[i + 1] = tmp[i];
  if (sel[0]) for (int i = 1; i < ng; i++) if (!strcmp(guilds[i].id, sel)) { gsel = i; break; }
  for (int i = 0; i < ng; i++) { gfold[i] = -1; for (int o = 0; o < ngorder && i > 0; o++) if (!strcmp(gorder[o], guilds[i].id)) { gfold[i] = (signed char)gorder_f[o]; break; } }
}

/* ---------------- inline images inside text: Twemoji ("t:<seq>") and Discord custom emoji ("c:<id>") ---------------- */
typedef struct { char key[96]; SDL_Surface *sf; int state; NxJob *job; uint32_t used; bool retried; } Inl;   /* 0 want, 1 loading, 2 ok, 3 failed */
static Inl inl[256];
static SDL_Surface *inline_provider(const char *key, bool *pending) {
  Inl *fr = NULL, *old = NULL;
  for (int i = 0; i < 256; i++) { Inl *e = &inl[i];
    if (e->key[0] && !strcmp(e->key, key)) { e->used = nx_ticks(); if (e->state == 2) return e->sf; if (e->state < 2) *pending = true; return NULL; }
    if (!e->key[0]) { if (!fr) fr = e; } else if (e->state != 1 && (!old || e->used < old->used)) old = e; }
  Inl *e = fr ? fr : old; if (!e) { *pending = true; return NULL; }
  if (e->sf) SDL_FreeSurface(e->sf); memset(e, 0, sizeof *e); snprintf(e->key, sizeof e->key, "%s", key); e->used = nx_ticks(); *pending = true; return NULL;
}
static void inline_url(const Inl *e, char *u, size_t cap) {
  if (!strncmp(e->key, "c:", 2)) snprintf(u, cap, "https://cdn.discordapp.com/emojis/%s.png?size=48&quality=lossless", e->key + 2);
  else { char seq[96]; snprintf(seq, sizeof seq, "%s", e->key + 2);
    if (e->retried) { char *f; while ((f = strstr(seq, "-fe0f"))) memmove(f, f + 5, strlen(f + 5) + 1); }   /* twemoji names some files without fe0f */
    snprintf(u, cap, "https://cdn.jsdelivr.net/gh/jdecked/twemoji@latest/assets/72x72/%s.png", seq); }
}
static void inline_tick(void) {
  int loading = 0; for (int i = 0; i < 256; i++) if (inl[i].job) loading++;
  for (int i = 0; i < 256; i++) { Inl *e = &inl[i];
    if (e->job && nxnet_job_done(e->job)) { NxResp r; nxnet_job_take(e->job, &r); e->job = NULL; loading--;
      SDL_Surface *s = NULL; if (r.status == 200 && r.len) { SDL_Surface *raw = IMG_Load_RW(SDL_RWFromConstMem(r.body, (int)r.len), 1); if (raw) { s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0); SDL_FreeSurface(raw); } }
      if (s) { e->sf = s; e->state = 2; }
      else if (!e->retried && !strncmp(e->key, "t:", 2) && strstr(e->key, "-fe0f")) { e->retried = true; e->state = 0; }
      else e->state = 3;
      nxnet_free(&r); if (e->state >= 2) nx_inline_changed(); }
    else if (e->key[0] && e->state == 0 && !e->job && loading < 4) { char u[300]; inline_url(e, u, sizeof u); const char *h[] = {"User-Agent: Mozilla/5.0", NULL}; e->state = 1; e->job = nxnet_start("GET", u, h, NULL, 0, 1u << 19); loading++; }
  }
}

/* ---------------- network state machine ---------------- */
static void start_next(void) {
  if (job) return; char path[160];
  if (want_me) { want_me = false; job = dc_get("/users/@me"); jk = J_ME; return; }
  if (send_pending) { if (edit_id[0] && !send_keep_compose) { json_t *eo = json_object(); json_object_set_new(eo, "content", json_string(send_buf)); char *eb = json_dumps(eo, JSON_COMPACT); json_decref(eo); snprintf(path, sizeof path, "/channels/%s/messages/%s", cur_chan, edit_id); job = dc_request("PATCH", path, eb); free(eb); jk = J_SEND; want_refresh = true; return; }
    job = dc_send(cur_chan, send_buf, reply_to); jk = J_SEND; return; }
  if (want_refresh && cur_chan[0]) { want_refresh = false; char p2[100]; snprintf(p2, sizeof p2, "/channels/%s/messages?limit=40", cur_chan); snprintf(jctx, sizeof jctx, "%s", cur_chan); job = dc_get(p2); jk = J_REFRESH; return; }
  if (want_guilds) { want_guilds = false; job = dc_get("/users/@me/guilds"); jk = J_GUILDS; return; }
  if (!sett_ready && ng > 1) { sett_ready = true; job = dc_get("/users/@me/settings"); jk = J_SETT; return; }
  if (!in_dms && view_guild[0] && strcmp(dc_roles_guild(), view_guild)) { dc_set_roles(view_guild, "[]"); snprintf(path, sizeof path, "/guilds/%s/roles", view_guild); snprintf(jctx, sizeof jctx, "%s", view_guild); job = dc_get(path); jk = J_ROLES; return; }
  if (want_chans[0]) { snprintf(path, sizeof path, "/guilds/%s/channels", want_chans); snprintf(jctx, sizeof jctx, "%s", want_chans); want_chans[0] = 0; job = dc_get(path); jk = J_CHANS; return; }
  if (want_dms) { if (!rel_ready) { rel_ready = true; job = dc_get("/users/@me/relationships"); jk = J_REL; return; } want_dms = false; job = dc_get("/users/@me/channels"); jk = J_DMS; return; }
  if (cur_chan[0] && want_msgs) { want_msgs = false; snprintf(path, sizeof path, "/channels/%s/messages?limit=40", cur_chan); snprintf(jctx, sizeof jctx, "%s", cur_chan); job = dc_get(path); jk = J_MSGS; return; }
  if (cur_chan[0] && nx_ticks() >= next_poll) {
    snprintf(jctx, sizeof jctx, "%s", cur_chan);
    if (nm > 0) snprintf(path, sizeof path, "/channels/%s/messages?after=%s&limit=50", cur_chan, msgs[nm - 1].id); else snprintf(path, sizeof path, "/channels/%s/messages?limit=40", cur_chan);
    job = dc_get(path); jk = J_POLL; next_poll = nx_ticks() + (gw_ready() ? 15000 : 3500);   /* the gateway pushes messages live; polling is the fallback */
  }
}
static void img_start(void) {
  for (int s = 0; s < 2; s++) {
    if (img[s].job) continue;
    for (int i = nm - 1; i >= 0; i--) for (int a = 0; a < msgs[i].nat; a++) {
      Attach *at = &msgs[i].at[a]; if (!at->image || at->state != 0) continue;
      at->state = 1; snprintf(img[s].mid, 24, "%s", msgs[i].id); img[s].idx = a;
      const char *h[] = {"User-Agent: Mozilla/5.0", NULL}; img[s].job = nxnet_start("GET", at->url, h, NULL, 0, 3u << 20); goto next;
    }
    next:;
  }
}
static void img_poll(void) {
  for (int s = 0; s < 2; s++) if (img[s].job && nxnet_job_done(img[s].job)) {
    NxResp r; nxnet_job_take(img[s].job, &r); img[s].job = NULL;
    for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, img[s].mid)) {
      Attach *at = &msgs[i].at[img[s].idx];
      if (r.status == 200 && r.len) { at->tex = nx_tex_from_mem(r.body, (int)r.len, &at->tw, &at->th); at->state = at->tex ? 2 : 3; } else at->state = 3;
      msgs[i].wcache = 0; break;
    }
    nxnet_free(&r);
  }
}
/* ---- attach menu (+), voice-message recording + upload ---- */
static bool att_on, rec_ui; static int att_sel; static NxJob *up_job;
static const char *att_items[] = {"Voice message", "Video clip", "GIF", "Emoji"};
static void vm_send(void) {
  const uint8_t *d; size_t n; float s; const char *wv; if (!rec_take(&d, &n, &s, &wv) || !cur_chan[0] || up_job) return;
  char pj[1200]; int k = snprintf(pj, sizeof pj, "{\"flags\":8192,\"attachments\":[{\"id\":\"0\",\"filename\":\"voice-message.ogg\",\"duration_secs\":%.2f,\"waveform\":\"%s\"}]", s, wv);
  if (reply_to[0]) k += snprintf(pj + k, sizeof pj - k, ",\"message_reference\":{\"message_id\":\"%s\",\"fail_if_not_exists\":false}", reply_to);
  snprintf(pj + k, sizeof pj - k, "}");
  up_job = dc_upload(cur_chan, pj, "voice-message.ogg", "audio/ogg", d, n); say("Sending voice message...", 2000); reply_to[0] = 0;
}
static void up_tick(void) { if (up_job && nxnet_job_done(up_job)) { NxResp r; nxnet_job_take(up_job, &r); up_job = NULL; if (r.status >= 200 && r.status < 300) { say("Sent", 1500); next_poll = 0; } else { char b[220]; snprintf(b, sizeof b, "Upload failed: %s %.100s", dc_error_text(&r), r.body ? r.body : ""); say(b, 6000); dlog("[upload] HTTP %d %.300s", r.status, r.body ? r.body : ""); } nxnet_free(&r); } }
static void draw_att(void) {
  int x = CHAT_X + 16, y = 648 - 4 * 52 - 20; nx_rrect(x, y, 300, 4 * 52 + 12, 10, C_DARK);
  for (int i = 0; i < 4; i++) { int yy = y + 6 + i * 52; if (i == att_sel) nx_rrect(x + 6, yy, 288, 46, 8, C_BLURPLE); nx_text(x + 22, yy + 10, att_items[i], 1, C_WHITE, 0); }
}
/* video clip recording: camera + mic -> MP4 (vrec.c), live preview, upload */
static bool vrec_ui, vrec_send_pending; static SDL_Texture *vprev; static uint8_t *vprev_px; static uint32_t vprev_seq;
#define VPW 480
#define VPH 360
static void vclip_start(void) {
  if (voice_chan[0] && voice_started && !dc_fakemic()) { say("Leave the call to record a video (the call is using the mic)", 4000); return; }
  vrec_config(VPW, VPH, 15); if (vrec_start(30)) { vrec_ui = true; vrec_send_pending = false; vprev_seq = 0; } else say(vrec_error(), 4000);
}
static void vclip_send(void) {
  uint8_t *mp4 = NULL; size_t len = 0; int w = 0, h = 0; float s = 0; if (!vrec_take(&mp4, &len, &w, &h, &s)) return;
  if (!cur_chan[0] || up_job) { free(mp4); say("Couldn't send the video right now", 3000); return; }
  char pj[400]; int k = snprintf(pj, sizeof pj, "{\"attachments\":[{\"id\":\"0\",\"filename\":\"video.mp4\"}]");
  if (reply_to[0]) k += snprintf(pj + k, sizeof pj - k, ",\"message_reference\":{\"message_id\":\"%s\",\"fail_if_not_exists\":false}", reply_to);
  snprintf(pj + k, sizeof pj - k, "}");
  up_job = dc_upload(cur_chan, pj, "video.mp4", "video/mp4", mp4, len); free(mp4); reply_to[0] = 0;
  char b[80]; snprintf(b, sizeof b, "Uploading video (%.1f MB)...", len / 1048576.0); say(b, 4000);
}
static void draw_vrec(void) {
  if (vrec_send_pending && vrec_state() == VREC_DONE) { vclip_send(); vrec_send_pending = false; vrec_ui = false; return; }
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 200));
  if (!vprev) { vprev = SDL_CreateTexture(nx_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, VPW, VPH); vprev_px = malloc(VPW * VPH * 4); }
  if (vprev && vprev_px && vrec_preview(vprev_px, VPW, VPH, &vprev_seq)) SDL_UpdateTexture(vprev, NULL, vprev_px, VPW * 4);
  int px = (NX_W - 640) / 2, py = 40; nx_rrect(px - 8, py - 8, 656, 496, 12, C_DARK); if (vprev && vprev_seq) nx_draw_tex(vprev, px, py, 640, 480); else text_center(NX_W / 2, py + 220, "Starting camera...", 1, C_MUTED);
  int s = vrec_state(); float secs = vrec_seconds(); char t[48]; snprintf(t, sizeof t, "%d:%02d / 0:30", (int)secs / 60, (int)secs % 60);
  if (s == VREC_RECORDING) { if ((nx_ticks() / 500) & 1) nx_circle(px + 24, py + 24, 9, C_RED); nx_text(px + 40, py + 10, "REC", 1 | NX_BOLD, C_WHITE, 0); }
  if (vrec_cam_fake()) nx_text(px + 10, py + 448, "Test pattern (no camera)", 0, RGB(240, 178, 50), 0);
  int y = py + 500; text_center(NX_W / 2, y, s == VREC_ERROR ? vrec_error() : s == VREC_FINISHING ? "Finishing..." : s == VREC_DONE ? "Ready to send" : t, 1 | NX_BOLD, s == VREC_ERROR ? C_RED : C_WHITE);
  float lf = vrec_level() / 32767.0f; if (lf > 1) lf = 1; nx_rrect(NX_W / 2 - 170, y + 40, 340, 10, 5, C_SEL); if (s == VREC_RECORDING) nx_rrect(NX_W / 2 - 170, y + 40, (int)(340 * lf), 10, 5, C_GREEN);
  text_center(NX_W / 2, y + 62, s == VREC_ERROR ? "B  close" : s == VREC_DONE ? "A  send        B  discard" : "A  send     X  stop     B  cancel", 1, C_TEXT);
}
static void vrec_input(NxInput *in) {
  int s = vrec_state();
  if (in->pressed[B_B] || (s == VREC_ERROR && (in->pressed[B_A] || in->touch_pressed))) { vrec_cancel(); vrec_ui = false; vrec_send_pending = false; return; }
  if (in->pressed[B_X] && s == VREC_RECORDING) { vrec_stop(); return; }
  if (in->pressed[B_A] || in->touch_pressed) { if (s == VREC_RECORDING || s == VREC_FINISHING) { vrec_stop(); vrec_send_pending = true; } else if (s == VREC_DONE) { vclip_send(); vrec_ui = false; } }
}
static void att_pick(int i) {
  att_on = false;
  if (i == 0) { if (voice_chan[0] && voice_started) { say("Leave the call to record a voice message (the call is using the mic)", 4000); return; } if (rec_start(120)) rec_ui = true; else say(rec_error(), 4000); }
  else if (i == 1) vclip_start();
  else if (i == 2) ov_open_gifs(); else if (i == 3) ov_emoji_guilds(guilds + 1, ng - 1), ov_open_emoji(EMO_INSERT, in_dms ? "" : view_guild, NULL);
}
static void att_input(NxInput *in) {
  if (in->pressed[B_B] || in->pressed[B_PLUS]) { att_on = false; return; }
  if (in->pressed[B_DOWN] && att_sel < 3) att_sel++; if (in->pressed[B_UP] && att_sel > 0) att_sel--;
  if (in->pressed[B_A]) att_pick(att_sel);
  if (in->touch_pressed) { int x = CHAT_X + 16, y = 648 - 4 * 52 - 20; for (int i = 0; i < 4; i++) if (in->tx >= x && in->tx < x + 300 && in->ty >= y + 6 + i * 52 && in->ty < y + 52 + i * 52) { att_pick(i); return; } att_on = false; }
}
static void draw_rec(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 170)); int x = NX_W / 2 - 230, y = 170; nx_rrect(x, y, 460, 330, 16, C_DARK);
  int s = rec_state(); float secs = rec_seconds(); char t[32]; snprintf(t, sizeof t, "%d:%02d", (int)secs / 60, (int)secs % 60);
  int pulse = s == REC_RECORDING ? (int)(8 * (1 + sinf(nx_ticks() / 200.0f))) : 0; nx_circle(NX_W / 2, y + 90, 46 + pulse, RGBA(237, 66, 69, 90)); nx_circle(NX_W / 2, y + 90, 44, C_RED); ico_mic(NX_W / 2, y + 90, C_WHITE, false);
  text_center(NX_W / 2, y + 150, s == REC_ERROR ? "Recording failed" : s == REC_DONE ? "Ready to send" : "Recording voice message", 1 | NX_BOLD, C_WHITE);
  text_center(NX_W / 2, y + 186, s == REC_ERROR ? rec_error() : t, 1, s == REC_ERROR ? C_RED : C_MUTED);
  float lf = rec_level() / 32767.0f; if (lf > 1) lf = 1; nx_rrect(x + 60, y + 228, 340, 10, 5, C_SEL); if (s == REC_RECORDING) nx_rrect(x + 60, y + 228, (int)(340 * lf), 10, 5, C_GREEN);
  text_center(NX_W / 2, y + 268, s == REC_ERROR ? "B  close" : s == REC_DONE ? "A  send        B  discard" : "A  send     X  stop     B  cancel", 1, C_TEXT);
  if (dc_fakemic()) text_center(NX_W / 2, y + 300, "(test tone - fakemic=1)", 0, RGB(240, 178, 50));
}
static void rec_input(NxInput *in) {
  int s = rec_state();
  if (in->pressed[B_B] || (s == REC_ERROR && (in->pressed[B_A] || in->touch_pressed))) { rec_cancel(); rec_ui = false; return; }
  if (in->pressed[B_X] && s == REC_RECORDING) { rec_stop(); return; }             /* stop and review before sending */
  if (in->pressed[B_A] || in->touch_pressed) { if (s == REC_RECORDING) rec_stop(); if (rec_state() == REC_DONE) vm_send(); rec_ui = false; }
}
/* ---- DM / group calls: incoming-call screen + ringtone ---- */
static bool inc_on; static char inc_chan[24], inc_name[80], inc_av[110]; static NxJob *ring_job;
static char oring_chan[24]; static uint32_t oring_t0; static int oring_n = -1;
static SDL_AudioDeviceID ring_dev; static uint32_t ring_next;
static void ring_tick(void) {                         /* classic dual tone: 1.2 s on, 2.8 s off, while a call is ringing us */
  if (!inc_on) { if (ring_dev) SDL_ClearQueuedAudio(ring_dev); return; }
  if (!ring_dev) { if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO); SDL_AudioSpec w = {0}, hv; w.freq = 48000; w.format = AUDIO_S16SYS; w.channels = 2; w.samples = 1024; ring_dev = SDL_OpenAudioDevice(NULL, 0, &w, &hv, 0); if (ring_dev) SDL_PauseAudioDevice(ring_dev, 0); }
  if (!ring_dev || nx_ticks() < ring_next) return; ring_next = nx_ticks() + 4000;
  static int16_t tone[48000 * 12 / 10 * 2]; int n = 48000 * 12 / 10;
  for (int i = 0; i < n; i++) { float t = i / 48000.0f, env = i < 960 ? i / 960.0f : (n - i < 960 ? (n - i) / 960.0f : 1.0f);
    int16_t s = (int16_t)(env * 4000.0f * (sinf(2 * 3.14159265f * 440 * t) + sinf(2 * 3.14159265f * 480 * t))); tone[2 * i] = tone[2 * i + 1] = s; }
  SDL_QueueAudio(ring_dev, tone, n * 4);
}
static void voice_join(const Chan *c);
static void call_start(void) {                        /* call the open DM / group DM: join its voice and ring the others */
  if (!in_dms || !cur_chan[0]) return; Chan c; memset(&c, 0, sizeof c); snprintf(c.id, 24, "%s", cur_chan); snprintf(c.name, sizeof c.name, "%s", cur_name); c.type = 2;
  voice_join(&c); snprintf(oring_chan, 24, "%s", cur_chan); oring_t0 = nx_ticks(); oring_n = -1; char p[100]; snprintf(p, sizeof p, "/channels/%s/call/ring", cur_chan); if (!ring_job) ring_job = dc_request("POST", p, "{\"recipients\":null}");
}
static void call_accept(void) {
  inc_on = false; select_guild(0); Chan t; memset(&t, 0, sizeof t); snprintf(t.id, 24, "%s", inc_chan); snprintf(t.name, sizeof t.name, "%s", inc_name); t.type = 1; open_channel(&t);
  t.type = 2; voice_join(&t);
}
static void call_decline(void) {
  inc_on = false; char p[100], b[80]; snprintf(p, sizeof p, "/channels/%s/call/stop-ringing", inc_chan); snprintf(b, sizeof b, "{\"recipients\":[\"%s\"]}", my_user_id); if (!ring_job) ring_job = dc_request("POST", p, b);
}
static void draw_incoming(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 170)); int x = NX_W / 2 - 220, y = 150; nx_rrect(x, y, 440, 380, 16, RGB(30, 31, 34));
  SDL_Texture *t = inc_av[0] ? av_get(inc_av, 34) : NULL; int pulse = (int)(6 * (1 + sinf(nx_ticks() / 250.0f)));
  nx_circle(NX_W / 2, y + 110, 64 + pulse, RGBA(35, 165, 90, 90)); if (t) nx_draw_tex(t, NX_W / 2 - 60, y + 50, 120, 120); else nx_circle(NX_W / 2, y + 110, 60, C_BLURPLE);
  int w = nx_text_w(inc_name, 2 | NX_BOLD); if (w > 400) w = 400; nx_text(NX_W / 2 - w / 2, y + 190, inc_name, 2 | NX_BOLD, C_WHITE, 400);
  text_center(NX_W / 2, y + 236, "Incoming call...", 1, C_MUTED);
  nx_circle(NX_W / 2 - 80, y + 316, 34, C_GREEN); text_center(NX_W / 2 - 80, y + 302, "A", 1 | NX_BOLD, C_WHITE);
  nx_circle(NX_W / 2 + 80, y + 316, 34, C_RED); text_center(NX_W / 2 + 80, y + 302, "B", 1 | NX_BOLD, C_WHITE);
}
static void incoming_input(NxInput *in) {
  int y = 150; if (in->pressed[B_A] || (in->touch_pressed && in->tx < NX_W / 2 && in->ty > y + 270 && in->ty < y + 360)) call_accept();
  else if (in->pressed[B_B] || (in->touch_pressed && in->tx >= NX_W / 2 && in->ty > y + 270 && in->ty < y + 360)) call_decline();
}
static bool refresh_keep;
static void apply_msgs(const char *body, bool poll);
static const char *name_for(const char *uid) {
  for (int i = nm - 1; i >= 0; i--) if (!strcmp(msgs[i].authorid, uid)) return msgs[i].author;
  for (int i = 0; i < ndm; i++) if (!strcmp(dms[i].uid, uid)) return dms[i].name;
  return "Someone";
}
static void live_events(void) {                    /* gateway events for the open channel */
  int kind; char *js_; int budget = 20;
  while (budget-- > 0 && gw_pop_event(&kind, &js_)) {
    json_t *d = json_loads(js_, 0, NULL);
    const char *ch = d ? json_string_value(json_object_get(d, "channel_id")) : NULL, *id = d ? json_string_value(json_object_get(d, "id")) : NULL;
    if (d && ch && kind >= GWE_CALL_CREATE) {
      json_t *rg = json_object_get(d, "ringing"); bool me = false; int others = 0; size_t ri; json_t *rv; json_array_foreach(rg, ri, rv) if (json_is_string(rv)) { if (!strcmp(json_string_value(rv), my_user_id)) me = true; else others++; }
      if (!strcmp(ch, oring_chan)) oring_n = kind == GWE_CALL_DELETE ? 0 : others;   /* our outgoing call: who is still being rung */
      if (kind != GWE_CALL_DELETE && me && strcmp(ch, voice_chan)) {
        if (!inc_on || strcmp(inc_chan, ch)) { inc_on = true; ring_next = 0; snprintf(inc_chan, 24, "%s", ch); snprintf(inc_name, sizeof inc_name, "Incoming call"); inc_av[0] = 0;
          for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].id, ch)) { snprintf(inc_name, sizeof inc_name, "%s", dms[k].name); snprintf(inc_av, sizeof inc_av, "%s", dms[k].av); } } }
      else if (inc_on && !strcmp(inc_chan, ch)) inc_on = false;
    }
    else if (d && ch && kind == GWE_DM) {                 /* new DM elsewhere: bump it to the top, mark unread, toast */
      unsigned long long nid = id ? strtoull(id, NULL, 10) : 0; int at = -1; for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].id, ch)) at = k;
      if (at > 0) { Chan t = dms[at]; memmove(&dms[1], &dms[0], at * sizeof dms[0]); dms[0] = t; if (in_dms && csel >= 0 && csel < at) csel++; else if (in_dms && csel == at) csel = 0; at = 0; }
      if (at == 0) dms[0].last = nid; else want_dms = in_dms;
      json_t *au = json_object_get(d, "author"); const char *gn = au ? json_string_value(json_object_get(au, "global_name")) : NULL, *un = au ? json_string_value(json_object_get(au, "username")) : NULL;
      const char *aid = au ? json_string_value(json_object_get(au, "id")) : NULL;
      if (!aid || strcmp(aid, my_user_id)) { char t[200]; char *body = dc_clean_text(json_string_value(json_object_get(d, "content")) ? json_string_value(json_object_get(d, "content")) : "");
        snprintf(t, sizeof t, "%s: %.120s", gn ? gn : un ? un : "New message", body[0] ? body : "[attachment]"); free(body); say(t, 5000); }
    }
    else if (d && ch && !strcmp(ch, cur_chan)) {
      if (kind == GWE_CREATE) { size_t n = strlen(js_); char *arr = malloc(n + 3); arr[0] = '['; memcpy(arr + 1, js_, n); arr[n + 1] = ']'; arr[n + 2] = 0; apply_msgs(arr, true); free(arr);
        json_t *au = json_object_get(d, "author"); const char *aid = au ? json_string_value(json_object_get(au, "id")) : NULL; if (aid) for (int k = 0; k < ntyping; k++) if (!strcmp(typing[k].id, aid)) typing[k].until = 0; }
      else if (kind == GWE_UPDATE && id) { for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, id)) { if (json_object_get(d, "content") || json_object_get(d, "embeds")) want_refresh = true; break; } }
      else if (kind == GWE_DELETE && id) { for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, id)) { dc_free_msg(&msgs[i]); memmove(&msgs[i], &msgs[i + 1], (nm - i - 1) * sizeof msgs[0]); nm--; if (msel >= nm) msel = nm - 1; break; } }
      else if (kind == GWE_REACT) want_refresh = true;
      else if (kind == GWE_TYPING) { const char *uid = json_string_value(json_object_get(d, "user_id")); if (uid && strcmp(uid, my_user_id)) {
          json_t *mem = json_object_get(d, "member"), *u = mem ? json_object_get(mem, "user") : NULL; const char *nk = mem ? json_string_value(json_object_get(mem, "nick")) : NULL, *gn = u ? json_string_value(json_object_get(u, "global_name")) : NULL, *un = u ? json_string_value(json_object_get(u, "username")) : NULL;
          const char *nmn = nk ? nk : gn ? gn : un ? un : name_for(uid); int at = -1; for (int k = 0; k < ntyping; k++) if (!strcmp(typing[k].id, uid)) at = k;
          if (at < 0 && ntyping < 6) at = ntyping++; if (at >= 0) { snprintf(typing[at].id, 24, "%s", uid); snprintf(typing[at].name, 80, "%s", nmn); typing[at].until = nx_ticks() + 9000; } } }
    }
    json_decref(d); free(js_);
  }
}
static void apply_msgs(const char *body, bool poll) {
  static Msg tmp[MAX_MSGS]; int n = dc_parse_messages(body, tmp, MAX_MSGS);
  if (n < 0) { say("Could not read messages", 3000); return; }
  if (n > 0 && cur_chan[0]) { unsigned long long nid = strtoull(tmp[n - 1].id, NULL, 10), ch = strtoull(cur_chan, NULL, 10); unsigned long long *p = rs_get(ch);
    if (!p || nid > *p) { rs_set(ch, nid); if (!ack_job) { char ap[120]; snprintf(ap, sizeof ap, "/channels/%s/messages/%s/ack", cur_chan, tmp[n - 1].id); ack_job = dc_request("POST", ap, "{\"token\":null}"); } }
    for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].id, cur_chan) && nid > dms[k].last) dms[k].last = nid; }
  if (!poll) { float keep = chat_scroll; char selid[24] = ""; if (msel >= 0 && msel < nm) snprintf(selid, 24, "%s", msgs[msel].id); clear_msgs(); for (int i = 0; i < n; i++) msgs[i] = tmp[i]; nm = n; chat_scroll = refresh_keep ? keep : 0; msel = -1; if (refresh_keep && selid[0]) for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, selid)) msel = i; return; }
  for (int i = 0; i < n; i++) {
    bool dup = false; for (int k = 0; k < nm; k++) if (!strcmp(msgs[k].id, tmp[i].id)) { dup = true; break; }
    if (dup || nm >= MAX_MSGS) { dc_free_msg(&tmp[i]); continue; }
    msgs[nm++] = tmp[i];
  }
}
static void net_tick(void) {
  if (job && nxnet_job_done(job)) {
    NxResp r; nxnet_job_take(job, &r); int k = jk; job = NULL; jk = J_NONE;
    bool ok = r.status >= 200 && r.status < 300;
    if (!ok && r.status == 401) { go_login("Discord rejected the token (401). Log in again."); dc_logout(); nxnet_free(&r); return; }
    if (!ok && k != J_POLL) say(dc_error_text(&r), 5000);
    if (ok) switch (k) {
      case J_GUILDS: { int n = dc_parse_guilds(r.body, guilds + 1, MAX_GUILDS); if (n >= 0) { ng = n + 1; { static const char *gp[MAX_GUILDS]; for (int q = 0; q < n; q++) gp[q] = guilds[q + 1].id; gw_set_guild_ids(gp, n); } snprintf(guilds[0].name, 72, "Direct Messages"); guilds[0].id[0] = 0; apply_order(); if (cur_chan[0] == 0 && gsel == 0 && !in_dms) select_guild(0), pane = 0; } break; }
      case J_SETT: { { json_t *sj0 = json_loads(r.body, 0, NULL); json_t *tz = json_object_get(sj0, "timezone_offset"); if (json_is_integer(tz)) tz_min = (int)json_integer_value(tz); json_decref(sj0); }
        ngorder = dc_parse_guild_order(r.body, gorder, MAX_GUILDS * 2); nfolders = 0; for (int k = 0; k < ngorder; k++) gorder_f[k] = -1;
        json_t *sj = json_loads(r.body, 0, NULL); json_t *gf = json_object_get(sj, "guild_folders"); size_t fi; json_t *f; int pos = 0;
        json_array_foreach(gf, fi, f) { json_t *ids = json_object_get(f, "guild_ids"); int n = (int)json_array_size(ids); bool real = !json_is_null(json_object_get(f, "id")) && json_object_get(f, "id") && n > 0;
          int fx = -1; if (real && nfolders < 96) { fx = nfolders++; Folder *F = &folders[fx]; memset(F, 0, sizeof *F); const char *fn = json_string_value(json_object_get(f, "name")); snprintf(F->name, sizeof F->name, "%s", fn ? fn : "");
            json_t *col = json_object_get(f, "color"); if (json_is_integer(col)) { F->color = (unsigned)json_integer_value(col); F->has_color = true; } }
          for (int k = 0; k < n && pos < ngorder; k++) gorder_f[pos++] = (short)fx; }
        json_decref(sj); apply_order(); break; }
      case J_REL: dc_set_blocked(r.body); break;
      case J_CHANS: { if (strcmp(jctx, view_guild) == 0 && !in_dms) { nc = dc_parse_channels(r.body, chans, MAX_CHANS); if (nc < 0) nc = 0; first_channel(); } break; }
      case J_DMS: { if (in_dms) { ndm = dc_parse_dms(r.body, dms, MAX_CHANS); if (ndm < 0) ndm = 0; first_channel(); } break; }
      case J_MSGS: if (!strcmp(jctx, cur_chan)) apply_msgs(r.body, false); break;
      case J_ROLES: if (!strcmp(jctx, view_guild)) { dc_set_roles(view_guild, r.body); if (nm) want_refresh = true; } break;
      case J_REFRESH: if (!strcmp(jctx, cur_chan)) { refresh_keep = true; apply_msgs(r.body, false); refresh_keep = false; } break;
      case J_POLL: if (!strcmp(jctx, cur_chan)) apply_msgs(r.body, true); break;
      case J_SEND: if (!send_keep_compose) edit_id[0] = 0; send_pending = false; if (!send_keep_compose) { compose[0] = 0; nment = 0; } send_keep_compose = false; reply_to[0] = 0; next_poll = 0; break;
      case J_ME: { json_t *j = json_loads(r.body, 0, NULL); const char *id = j ? json_string_value(json_object_get(j, "id")) : NULL;
        if (j) { const char *gn = json_string_value(json_object_get(j, "global_name")), *un = json_string_value(json_object_get(j, "username")), *av = json_string_value(json_object_get(j, "avatar")); snprintf(my_name, sizeof my_name, "%s", gn ? gn : un ? un : ""); snprintf(my_avatar, sizeof my_avatar, "%s", av ? av : ""); dc_parse_style(j, &my_ns); } if (id) { snprintf(my_user_id, sizeof my_user_id, "%s", id); if (!gw_started) { gw_start(dc_token(), my_user_id); gw_started = true; } } if (j) json_decref(j); break; }
    }
    if (!ok && k == J_SEND) { send_pending = false; send_keep_compose = false; }
    nxnet_free(&r);
  }
  if (rjob && nxnet_job_done(rjob)) { NxResp rr; nxnet_job_take(rjob, &rr); rjob = NULL; if (rr.status >= 200 && rr.status < 300) want_refresh = true; else say(dc_error_text(&rr), 3000); nxnet_free(&rr); }
  live_events(); rs_save(); ring_tick(); up_tick();
  if (ring_job && nxnet_job_done(ring_job)) { NxResp rr; nxnet_job_take(ring_job, &rr); ring_job = NULL; if (rr.status >= 300) dlog("[call] ring/stop -> HTTP %d", rr.status); nxnet_free(&rr); } vusers_tick();
  if (ack_job && nxnet_job_done(ack_job)) { NxResp ar; nxnet_job_take(ack_job, &ar); ack_job = NULL; nxnet_free(&ar); }
  start_next(); img_poll(); img_start(); av_tick(); inline_tick(); voice_tick(); ov_tick();
}

/* ---------------- messages ---------------- */
static bool new_day(int i) { return i == 0 || local_day(msgs[i].ts - tz_min * 60) != local_day(msgs[i - 1].ts - tz_min * 60); }
static bool grouped(int i) { return i > 0 && !new_day(i) && !msgs[i].has_reply && !msgs[i].forwarded && !strcmp(msgs[i].authorid, msgs[i - 1].authorid) && msgs[i].ts >= msgs[i - 1].ts && msgs[i].ts - msgs[i - 1].ts < 420; }
static void draw_avatar(int i, int x, int y) {
  Msg *m = &msgs[i]; SDL_Texture *t = av_tex(m);
  if (t) { nx_draw_tex(t, x, y, 44, 44); return; }
  nx_circle(x + 22, y + 22, 22, UC(name_color(m->authorid)));
  char b[2] = {m->author[0] ? m->author[0] : '?', 0}; if (b[0] >= 'a' && b[0] <= 'z') b[0] -= 32; text_center(x + 22, y + 9, b, 1 | NX_BOLD, C_WHITE);
}
static void img_box(const Attach *a, int *w, int *h) { int tw = a->tw, th = a->th; float s = 1.0f; if (tw > 460) s = 460.0f / tw; if (th * s > 300) s = 300.0f / th; *w = (int)(tw * s); *h = (int)(th * s); }
#define CARD_W 560
/* one function both measures (draw=false) and draws a message, so the layout can never disagree with the drawing */
static int layout_msg(int i, int y, bool draw) {
  Msg *m = &msgs[i]; bool g = grouped(i); int top = y, cy;
  if (new_day(i) && m->ts > 0) {                                          /* date divider */
    if (draw) { char dt[48]; fmt_date(m->ts, dt, sizeof dt); int tw = nx_text_w(dt, 0 | NX_BOLD); int mid = CHAT_X + CHAT_W / 2; nx_rect(CHAT_X + 16, y + 18, CHAT_W - 32, 1, RGB(63, 65, 71)); nx_rect(mid - tw / 2 - 8, y + 6, tw + 16, 24, C_BG); nx_text(mid - tw / 2, y + 6, dt, 0 | NX_BOLD, C_MUTED, 0); }
    y += 36; }
  if (m->has_reply) {                                                     /* "replying to" header */
    if (draw) {
      nx_rect(CHAT_X + 38, y + 14, 2, 14, C_SEL); nx_rect(CHAT_X + 38, y + 12, 30, 2, C_SEL);
      if (m->reply) { int nw = nx_name(TEXT_X, y + 2, m->reply_author, 0 | NX_BOLD, &m->reply_ns, UC(name_color(m->reply_authorid)), 260);
        nx_text(TEXT_X + nw + 8, y + 2, m->reply, 0, C_MUTED, TEXT_W - nw - 20); }
      else nx_text(TEXT_X, y + 2, "Original message was deleted", 0, C_MUTED, 0);
    }
    y += 26;
  }
  if (!g) {
    if (draw) { draw_avatar(i, CHAT_X + 16, y + 8);
      int nw = nx_name(TEXT_X, y + 6, m->author, 1 | NX_BOLD, &m->ns, UC(m->color), 440);
      char tb[48]; fmt_time(m->ts, tb, sizeof tb, true); nx_text(TEXT_X + nw + 12, y + 10, tb, 0, C_MUTED, 0); }
    cy = y + 38;
  } else cy = y + 4;
  int fstart = cy;
  if (m->forwarded) { if (draw) nx_text(TEXT_X + 10, cy, "->  Forwarded", 0, C_MUTED, 0); cy += 26; }
  int xo = m->forwarded ? 12 : 0;                                         /* forwarded content is indented next to a bar */
  if (draw && m->nat && m->at[0].voice && m->text[0]) { static uint32_t lt; if (nx_ticks() - lt > 1000) { lt = nx_ticks(); const unsigned char *tb = (const unsigned char *)m->text; dlog("[uaf] draw voice msg %s text %p bytes %02x %02x %02x %02x", m->id, (void *)m->text, tb[0], tb[1], tb[2], tb[3]); } }
  if (m->text[0]) cy += md_layout(TEXT_X + xo, cy, m->text, TEXT_W - xo, draw);
  for (int k = 0; k < m->nat; k++) {
    Attach *a = &m->at[k];
    if (a->image && a->state == 2) { int w, hh; img_box(a, &w, &hh);
      if (draw) { nx_round(TEXT_X + xo - 2, cy - 2, w + 4, hh + 4, C_DARK);
        SDL_Texture *ft = NULL; int fw, fh;
        if (a->anim_url[0] || (a->gif && a->src[0])) { if (!a->anim && anims_open < 6) { a->anim = a->anim_url[0] ? (strstr(a->anim_url, ".webp") ? anim_open_webp(a->anim_url, w, hh) : anim_open_gif(a->anim_url, w, hh)) : NULL; if (a->anim) anims_open++; }
          if (!a->anim && !a->anim_url[0] && a->src[0] && strstr(a->src, ".mp4") && anims_open < 4) { a->anim = anim_open_clip(a->src); if (a->anim) anims_open++; }
          if (a->anim) { a->anim_seen = nx_ticks(); anim_tick((Anim *)a->anim); if (anim_state((Anim *)a->anim) == ANIM_ERROR && a->src[0] && strstr(a->src, ".mp4") && a->anim_url[0]) { anim_close((Anim *)a->anim); a->anim = anim_open_clip(a->src); a->anim_url[0] = 0; } else ft = anim_frame((Anim *)a->anim, &fw, &fh); } }
        nx_draw_tex(ft ? ft : a->tex, TEXT_X + xo, cy, w, hh);
        if (nhits < 200) hits[nhits++] = (Hit){i, TEXT_X + xo, cy, w, hh, 4, k};
        if (a->gif || !strcmp(a->name, "GIF")) { nx_round(TEXT_X + xo + 8, cy + 8, 44, 24, RGB(0, 0, 0)); nx_text(TEXT_X + xo + 16, cy + 9, "GIF", 0 | NX_BOLD, C_WHITE, 0); }
        if (a->video) { int cx = TEXT_X + xo + w / 2, cc = cy + hh / 2; nx_circle(cx, cc, 26, RGBA(0, 0, 0, 170)); for (int t = 0; t < 22; t++) nx_rect(cx - 8 + t, cc - 11 + t / 2, 1, 22 - t, C_WHITE); } }
      cy += hh + 10; }
    else if (a->voice) { int bw = 380;
      if (draw) { nx_rrect(TEXT_X + xo, cy, bw, 56, 28, C_SIDE); nx_circle(TEXT_X + xo + 28, cy + 28, 18, C_BLURPLE); for (int t2 = 0; t2 < 14; t2++) nx_rect(TEXT_X + xo + 23 + t2, cy + 21 + t2 / 2, 1, 14 - t2, C_WHITE);
        int bars = 40, wx = TEXT_X + xo + 56; for (int b = 0; b < bars; b++) { int v = a->nwave ? a->wave[b * a->nwave / bars] : 40; int bh = 4 + v * 26 / 255; nx_rrect(wx + b * 6, cy + 28 - bh / 2, 3, bh, 1, RGB(181, 186, 193)); }
        char du[16]; snprintf(du, sizeof du, "%d:%02d", (int)a->dur / 60, (int)a->dur % 60); nx_text(TEXT_X + xo + bw - 60, cy + 14, du, 0, C_MUTED, 0);
        if (nhits < 200) hits[nhits++] = (Hit){i, TEXT_X + xo, cy, bw, 56, 4, k}; }
      cy += 64; }
    else if (a->audio) { char t[120]; snprintf(t, sizeof t, "%s", a->name); int tw = nx_text_w(t, 1); if (tw > 420) tw = 420;
      if (draw) { nx_rrect(TEXT_X + xo, cy, tw + 90, 52, 10, C_SIDE); nx_circle(TEXT_X + xo + 28, cy + 26, 18, C_BLURPLE); for (int t2 = 0; t2 < 16; t2++) nx_rect(TEXT_X + xo + 22 + t2, cy + 18 + t2 / 2, 1, 16 - t2, C_WHITE);
        nx_text(TEXT_X + xo + 56, cy + 4, t, 1, C_TEXT, 430); nx_text(TEXT_X + xo + 56, cy + 30, "audio - tap or A to play", 0, C_MUTED, 0);
        if (nhits < 200) hits[nhits++] = (Hit){i, TEXT_X + xo, cy, tw + 90, 52, 4, k}; }
      cy += 60; }
    else { char t[120]; snprintf(t, sizeof t, "%s  %s", a->video ? (a->state == 3 ? "[video]" : "[loading video...]") : a->image ? (a->state == 3 ? "[image unavailable]" : "[loading image...]") : "[file]", a->name);
      if (draw) { int cw2 = nx_text_w(t, 0) + 24; nx_round(TEXT_X + xo, cy, cw2, 28, C_SIDE); nx_text(TEXT_X + xo + 12, cy + 2, t, 0, C_BLURPLE, TEXT_W - 30); if (a->video && nhits < 200) hits[nhits++] = (Hit){i, TEXT_X + xo, cy, cw2, 28, 4, k}; }
      cy += 32; }
  }
  if (m->card) {                                                          /* link preview text */
    int th = nx_wrap(0, 0, m->card, 0, C_TEXT, CARD_W - 32, 8, false);
    if (draw) { nx_round(TEXT_X + xo, cy, CARD_W, th + 20, C_SIDE); nx_rect(TEXT_X + xo, cy, 4, th + 20, C_BLURPLE); nx_wrap(TEXT_X + xo + 16, cy + 10, m->card, 0, C_TEXT, CARD_W - 32, 8, true); }
    cy += th + 28;
  }
  if (m->nre) {                                                           /* reaction chips */
    int rx = TEXT_X + xo;
    for (int k = 0; k < m->nre; k++) { React *re = &m->re[k]; char cnt[12]; snprintf(cnt, sizeof cnt, "%d", re->count);
      int w = nx_text_w(re->key, 1) + nx_text_w(cnt, 0) + 30; if (rx + w > TEXT_X + TEXT_W) { rx = TEXT_X + xo; cy += 38; }
      if (draw) { nx_rrect(rx, cy, w, 32, 8, re->me ? RGB(56, 60, 110) : C_SIDE); if (re->me) { nx_rect(rx + 4, cy, w - 8, 1, C_BLURPLE); nx_rect(rx + 4, cy + 31, w - 8, 1, C_BLURPLE); }
        int ew = nx_text(rx + 10, cy + 3, re->key, 1, C_WHITE, 0); nx_text(rx + 16 + ew, cy + 6, cnt, 0, re->me ? C_WHITE : C_MUTED, 0);
        if (nhits < 200) hits[nhits++] = (Hit){i, rx, cy, w, 32, 2, k}; }
      rx += w + 6; }
    cy += 40;
  }
  if (m->forwarded && draw) nx_rect(TEXT_X, fstart, 3, cy - fstart - 4, C_SEL);
  int h = cy - top; int minh = 58 + (m->has_reply ? 26 : 0) + (new_day(i) && m->ts > 0 ? 36 : 0); if (!g && h < minh) h = minh;
  return h + (g ? 4 : 10);
}
static int msg_height(int i) {
  Msg *m = &msgs[i]; int key = TEXT_W + (grouped(i) ? 100000 : 0);
  if (m->wcache == key) return m->hcache;
  m->hcache = layout_msg(i, 0, false); m->wcache = key; return m->hcache;
}
static void draw_msg(int i, int y) {
  int h = msg_height(i);
  if (i == msel) { nx_rect(CHAT_X, y - 4, CHAT_W, h, RGB(56, 58, 66)); nx_rect(CHAT_X, y - 4, 3, h, C_BLURPLE); }
  if (nhits < 200) hits[nhits++] = (Hit){i, CHAT_X, y - 4, CHAT_W, h, 0, 0};
  if (!grouped(i) && nhits < 200) { int ry = y - 4 + (msgs[i].has_reply ? 26 : 0) + (new_day(i) && msgs[i].ts > 0 ? 36 : 0); hits[nhits++] = (Hit){i, CHAT_X + 10, ry, TEXT_X - CHAT_X + nx_name_w(msgs[i].author, 1 | NX_BOLD, &msgs[i].ns), 54, 1, 0}; }
  layout_msg(i, y - 4, true);
}

/* ---------------- drawing: lists ---------------- */
static void build_rail(void) {
  nrail = 0; rail[nrail++] = (RItem){0, 0, -1}; int lastf = -1;
  for (int i = 1; i < ng && nrail < (int)(sizeof rail / sizeof rail[0]); i++) { int f = gfold[i];
    if (f >= 0 && f < nfolders) { if (f != lastf) { rail[nrail++] = (RItem){2, i, f}; lastf = f; } if (folders[f].open) rail[nrail++] = (RItem){1, i, f}; }
    else { lastf = -1; rail[nrail++] = (RItem){1, i, -1}; } }
  if (rsel >= nrail) rsel = nrail - 1; if (rsel < 0) rsel = 0;
}
static int rail_h(int k) { return k == 0 ? GUILD_PITCH + 10 : GUILD_PITCH; }
static SDL_Texture *guild_icon(int i, int sz) { if (i <= 0 || !guilds[i].icon[0]) return NULL; char gp[110]; snprintf(gp, sizeof gp, "icons/%s/%s", guilds[i].id, guilds[i].icon); return av_get(gp, sz); }
static SDL_Texture *guild_icon_rr(int i) { if (i <= 0 || !guilds[i].icon[0]) return NULL; char gp[120]; snprintf(gp, sizeof gp, "icons/%s/%s#rr", guilds[i].id, guilds[i].icon); return av_get(gp, 52); }
static void draw_guild_icon(int i, int x, int y, int d, bool sel) {
  SDL_Texture *gt = sel ? guild_icon_rr(i) : guild_icon(i, 52); if (sel && !gt) gt = guild_icon(i, 52); int cx = x + d / 2;
  if (sel && !gt) nx_rrect(x, y, d, d, 16, C_BLURPLE);
  else if (!gt) nx_circle(cx, y + d / 2, d / 2, i == 0 ? C_SEL : UC(name_color(guilds[i].name)));
  if (gt) nx_draw_tex(gt, x, y, d, d);
  else if (i == 0) { if (!sel) { nx_circle(cx, y + d / 2, d / 2, RGB(49, 51, 56)); } nx_rrect(cx - 13, y + d / 2 - 9, 26, 17, 6, C_WHITE); nx_rect(cx - 7, y + d / 2 + 6, 5, 5, C_WHITE); nx_circle(cx - 5, y + d / 2 - 1, 3, sel ? C_BLURPLE : RGB(49, 51, 56)); nx_circle(cx + 5, y + d / 2 - 1, 3, sel ? C_BLURPLE : RGB(49, 51, 56)); }   /* Discord-ish chat bubble */
  else { char b[2] = {guilds[i].name[0] ? guilds[i].name[0] : '?', 0}; if (b[0] >= 'a' && b[0] <= 'z') b[0] -= 32; text_center(cx, y + d / 2 - 14, b, 1 | NX_BOLD, C_WHITE); }
}
/* the bird: lives at the bottom of the server rail (its own strip, so it never covers anything) */
#define BIRD_H 64
static SDL_Texture *bird_tex; static int bird_w, bird_h; static bool bird_tried; static uint32_t bird_hop, bird_next = 6000;
static void bird_poke(void) { bird_hop = nx_ticks(); }
static void draw_bird(void) {
  if (!bird_tried) { bird_tried = true; SDL_Surface *s = IMG_Load("/vol/content/bird.png"); if (s) { bird_w = s->w; bird_h = s->h; bird_tex = SDL_CreateTextureFromSurface(nx_renderer(), s); SDL_FreeSurface(s); } }
  if (!bird_tex) return; uint32_t now = nx_ticks(); if (now > bird_next) { bird_hop = now; bird_next = now + 5000 + (now * 2654435761u >> 20) % 9000; }
  int dy = 0; uint32_t t = now - bird_hop; if (t < 360) { float f = t / 360.0f; dy = (int)(14 * 4 * f * (1 - f)); }
  int h = 52, w = bird_w * h / (bird_h ? bird_h : 1); nx_rect(20, NX_H - BIRD_H, 36, 2, C_SEL);
  nx_rect(RAIL_W / 2 - 18, NX_H - 8, 36, 3, RGBA(0, 0, 0, t < 360 ? 40 : 70));   /* little shadow */
  nx_draw_tex(bird_tex, RAIL_W / 2 - w / 2, NX_H - 8 - h - dy, w, h);
}
static void draw_guilds(void) {
  build_rail();
  nx_rect(0, 0, RAIL_W, NX_H, C_DARK);
  nx_clip(0, 0, RAIL_W, NX_H - BIRD_H);
  int y = 14, cx = RAIL_W / 2;
  for (int k = rscroll; k < nrail && y < NX_H - BIRD_H; k++) {
    RItem *it = &rail[k];
    if (it->kind == 1 && it->fi >= 0) { unsigned fc = folders[it->fi].has_color ? folders[it->fi].color : 0x5865F2; nx_rect(cx - 32, y - 6, 64, GUILD_PITCH, RGBA((fc >> 16) & 255, (fc >> 8) & 255, fc & 255, 50)); }
    if (it->kind == 2) {                                                  /* folder */
      Folder *F = &folders[it->fi]; unsigned fc = F->has_color ? F->color : 0x5865F2; bool has_sel = gfold[gsel] == it->fi && gsel > 0;
      if (F->open) { nx_rect(cx - 32, y - 6, 64, GUILD_PITCH, RGBA((fc >> 16) & 255, (fc >> 8) & 255, fc & 255, 50)); nx_rrect(cx - 26, y, 52, 52, 16, RGBA((fc >> 16) & 255, (fc >> 8) & 255, fc & 255, 120));
        nx_rrect(cx - 14, y + 18, 28, 20, 3, UC(fc)); nx_rect(cx - 14, y + 14, 12, 6, UC(fc)); }                           /* open-folder glyph */
      else { nx_rrect(cx - 26, y, 52, 52, 16, RGBA((fc >> 16) & 255, (fc >> 8) & 255, fc & 255, 70)); int m = 0;
        for (int i = it->gi; i < ng && gfold[i] == it->fi && m < 4; i++, m++) { int mx = cx - 22 + (m % 2) * 23, my = y + 4 + (m / 2) * 23; SDL_Texture *t = guild_icon(i, 52); if (t) nx_draw_tex(t, mx, my, 21, 21); else nx_circle(mx + 10, my + 10, 10, UC(name_color(guilds[i].name))); } }
      if (has_sel && !F->open) nx_round(0, y + 8, 5, 36, C_WHITE);
    } else { bool sel = it->gi == gsel; if (sel) nx_round(0, y + 8, 5, 36, C_WHITE); draw_guild_icon(it->gi, cx - 26, y, 52, sel); }
    if (pane == 0 && k == rsel) { NxColor w = C_WHITE; nx_rect(cx - 30, y - 4, 60, 2, w); nx_rect(cx - 30, y + 54, 60, 2, w); nx_rect(cx - 30, y - 4, 2, 60, w); nx_rect(cx + 28, y - 4, 2, 60, w); }
    y += rail_h(k); if (k == 0) nx_rect(20, y - 10, 36, 2, C_SEL);
  }
  nx_unclip(); draw_bird();
}
static void rail_visible(void) { if (rsel < rscroll) rscroll = rsel; for (;;) { int y = 14; for (int k = rscroll; k <= rsel; k++) y += rail_h(k); if (y <= NX_H - BIRD_H - 10 || rscroll >= rsel) break; rscroll++; } }
static void rail_activate(int k) {
  if (k < 0 || k >= nrail) return; RItem *it = &rail[k];
  if (it->kind == 2) { folders[it->fi].open = !folders[it->fi].open; build_rail(); return; }
  select_guild(it->gi);
}
static void draw_channels(void) {
  int lb = list_bot();
  nx_rect(CH_X, 0, CH_W, NX_H, C_SIDE);
  /* header: server name with a chevron, or "Direct Messages" */
  nx_rect(CH_X, 0, CH_W, HDR_H, C_SIDE); nx_rect(CH_X, HDR_H - 1, CH_W, 1, RGB(31, 32, 35)); nx_rect(CH_X, HDR_H, CH_W, 2, RGBA(0, 0, 0, 40));
  if (in_dms) nx_text(CH_X + 16, 15, "Direct Messages", 1 | NX_BOLD, C_WHITE, CH_W - 28);
  else { nx_text(CH_X + 16, 15, guilds[gsel].name, 1 | NX_BOLD, C_WHITE, CH_W - 52); ico_line(CH_X + CH_W - 30, 24, CH_X + CH_W - 24, 30, 2, C_TEXT); ico_line(CH_X + CH_W - 24, 30, CH_X + CH_W - 18, 24, 2, C_TEXT); }
  vst_refresh(); nx_clip(CH_X, LIST_Y - 4, CH_W, lb - LIST_Y + 4); Chan *v = view(); int n = view_n(), y = LIST_Y;
  for (int i = cscroll; i < n && y < lb; i++) {
    int h = row_h(&v[i]);
    if (v[i].header) {                                                        /* category: UPPERCASE with a chevron */
      char up[80]; int k = 0; for (; v[i].name[k] && k < 78; k++) { char ch = v[i].name[k]; up[k] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; } up[k] = 0;
      ico_line(CH_X + 8, y + 16, CH_X + 12, y + 20, 2, C_MUTED); ico_line(CH_X + 12, y + 20, CH_X + 16, y + 16, 2, C_MUTED);
      nx_text(CH_X + 22, y + 9, up, 0 | NX_BOLD, C_MUTED, CH_W - 34); y += h; continue; }
    bool sel = (i == csel);
    int rh = (!in_dms && v[i].type == 2) ? 40 : h;   /* voice channels: people are listed under the row */
    if (sel) { nx_rrect(CH_X + 8, y, CH_W - 16, rh - 4, 6, pane == 1 ? RGB(78, 80, 88) : C_ROWSEL); }
    bool unr = !sel && chan_unread(&v[i]); if (unr) nx_round(CH_X, y + rh / 2 - 6, 4, 12, C_WHITE);
    if (in_dms) { SDL_Texture *dt = av_get(v[i].av, 34); int dcy = y + (h - 4) / 2; if (dt) nx_draw_tex(dt, CH_X + 19, dcy - 17, 34, 34); else nx_circle(CH_X + 36, dcy, 17, UC(name_color(v[i].name))); char b[2] = {v[i].name[0] ? v[i].name[0] : '?', 0}; if (b[0] >= 'a' && b[0] <= 'z') b[0] -= 32; if (!dt) text_center(CH_X + 36, y + 6, b, 0 | NX_BOLD, C_WHITE); nx_name(CH_X + 62, y + 10, v[i].name, unr ? 1 | NX_BOLD : 1, &v[i].ns, sel || unr ? C_WHITE : C_MUTED, CH_W - 80); }
    else { bool vc = v[i].type == 2, live = vc && !strcmp(v[i].id, voice_chan); NxColor ic = live ? C_GREEN : (sel ? C_WHITE : RGB(128, 132, 142));
      if (vc) ico_speaker(CH_X + 18, y + 9, ic); else ico_hash(CH_X + 18, y + 9, ic);
      nx_text(CH_X + 46, y + 7, v[i].name, unr ? 1 | NX_BOLD : 1, live ? C_GREEN : (sel || unr ? C_WHITE : C_MUTED), CH_W - 64);
      if (vc) { int uy = y + 38, k = 0; VcInfo vi; bool mine = live; if (mine) voice_snapshot(&vi);
        for (int q = 0; q < nvst && k < 12; q++) { GwVS *p = &vst[q]; if (strcmp(p->chan, v[i].id)) continue; k++;
          bool me = !strcmp(p->user, my_user_id); const char *nm = p->name, *av = p->avatar; VUser *u = NULL;
          if (me) { nm = my_name[0] ? my_name : "You"; av = my_avatar; } else if (!nm[0] || !av[0]) { u = vuser_g(guilds[gsel].id, p->user); if (u && u->state == 2) { if (!nm[0]) nm = u->name; if (!av[0]) av = u->avatar; } }
          bool talk = false; if (mine) { if (me) talk = vi.speaking; else for (int z = 0; z < vi.npeers; z++) if (!strcmp(vi.peers[z].user_id, p->user)) talk = vi.peers[z].speaking; }
          SDL_Texture *t = NULL; if (av && av[0]) { char ap[120]; snprintf(ap, sizeof ap, "avatars/%s/%s.png?size=64", p->user, av); t = av_get(ap, 24); }
          if (talk) nx_circle(CH_X + 58, uy + 12, 13, C_GREEN);
          if (t) nx_draw_tex(t, CH_X + 47, uy + 1, 22, 22); else nx_circle(CH_X + 58, uy + 12, 11, UC(name_color(p->user)));
          int rx = CH_X + CH_W - 14;
          if (p->deaf) { rx -= 22; ico_phones(rx + 9, uy + 12, C_MUTED, C_SIDE, true); } else if (p->mute) { rx -= 20; ico_mic(rx + 9, uy + 12, C_MUTED, true); }
          if (p->stream) { rx -= 40; nx_rrect(rx, uy + 3, 36, 18, 4, C_RED); nx_text(rx + 4, uy + 2, "LIVE", 0 | NX_BOLD, C_WHITE, 0); } else if (p->video) { rx -= 36; nx_text(rx, uy + 2, "cam", 0 | NX_BOLD, C_MUTED, 0); }
          nx_text(CH_X + 76, uy + 1, nm[0] ? nm : "...", 0, talk ? C_WHITE : C_MUTED, rx - (CH_X + 80)); uy += VCU_H; } } }
    y += h;
  }
  if (!n) nx_text(CH_X + 18, LIST_Y + 12, (job && (jk == J_CHANS || jk == J_DMS || jk == J_ROLES)) || want_chans[0] || want_dms ? "Loading..." : "Nothing here yet", 1, C_MUTED, 0);
  nx_unclip();
  /* voice connected panel */
  if (voice_chan[0]) { int py = lb; nx_rect(CH_X, py, CH_W, VCP_H, C_PANEL); nx_rect(CH_X + 8, py, CH_W - 16, 1, RGB(50, 52, 57));
    VcInfo vi; voice_snapshot(&vi); bool ok = vi.state == VC_CONNECTED; nx_text(CH_X + 14, py + 8, ok ? "Voice Connected" : "Connecting...", 0 | NX_BOLD, ok ? C_GREEN : RGB(240, 178, 50), 0);
    char where[160]; const char *gname = ""; for (int k = 1; k < ng; k++) if (!strcmp(guilds[k].id, voice_guild)) gname = guilds[k].name; snprintf(where, sizeof where, "%s%s%s", voice_name, gname[0] ? " / " : "", gname); nx_text(CH_X + 14, py + 32, where, 0, C_MUTED, CH_W - 80);
    nx_rrect(CH_X + CH_W - 52, py + 12, 40, 36, 8, RGB(60, 62, 68)); nx_text(CH_X + CH_W - 44, py + 15, "\xE2\x98\x8E", 1, C_RED, 0); }   /* disconnect */
  /* user panel */
  { int py = NX_H - USER_H; nx_rect(CH_X, py, CH_W, USER_H, C_PANEL);
    char ap[120]; SDL_Texture *t = NULL; if (my_avatar[0] && my_user_id[0]) { snprintf(ap, sizeof ap, "avatars/%s/%s", my_user_id, my_avatar); t = av_get(ap, 34); }
    if (t) nx_draw_tex(t, CH_X + 10, py + 13, 34, 34); else nx_circle(CH_X + 27, py + 30, 17, C_BLURPLE);
    nx_circle(CH_X + 40, py + 42, 7, C_PANEL); nx_circle(CH_X + 40, py + 42, 5, C_GREEN);          /* online */
    nx_name(CH_X + 52, py + 8, my_name[0] ? my_name : "You", 0 | NX_BOLD, &my_ns, C_WHITE, 104); nx_text(CH_X + 52, py + 32, voice_muted ? "Muted" : deaf_on ? "Deafened" : "Online", 0, C_MUTED, 104);
    ico_mic(CH_X + CH_W - 92, py + 30, voice_muted ? C_RED : C_ICON, voice_muted);
    ico_phones(CH_X + CH_W - 58, py + 30, deaf_on ? C_RED : C_ICON, C_PANEL, deaf_on);
    ico_gear(CH_X + CH_W - 24, py + 30, C_ICON, C_PANEL); }
}

/* ---------------- drawing: chat ---------------- */
static void member_fill(GwVoiceMember *m) {           /* DM calls (and some servers) send voice states without names: look them up */
  if (m->username[0] && m->avatar[0]) return;
  if (m->self || !strcmp(m->user_id, my_user_id)) { if (!m->username[0]) snprintf(m->username, sizeof m->username, "%s", my_name[0] ? my_name : "You"); if (!m->avatar[0]) snprintf(m->avatar, sizeof m->avatar, "%s", my_avatar); return; }
  VUser *u = vuser(m->user_id);
  if (u && u->state == 2) { if (!m->username[0]) snprintf(m->username, sizeof m->username, "%s", u->name); if (!m->avatar[0]) snprintf(m->avatar, sizeof m->avatar, "%s", u->avatar); gw_fill_member(m->user_id, u->name, u->avatar); return; }
  if (!m->username[0]) for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].uid, m->user_id)) snprintf(m->username, sizeof m->username, "%s", dms[k].name);
}
/* outgoing DM call: "Ringing..." until someone picks up (CALL_* events tell us who is still being rung) */
static int ring_out_state(const GwInfo *g) {          /* 0 none, 1 ringing, 2 no answer */
  if (!voice_chan[0] || voice_guild[0] || strcmp(oring_chan, voice_chan)) return 0;
  for (int i = 0; i < g->nmembers; i++) if (!g->members[i].self && strcmp(g->members[i].user_id, my_user_id)) { oring_chan[0] = 0; return 0; }   /* they picked up */
  if (oring_n == 0 || nx_ticks() - oring_t0 > 60000) return 2; return 1;
}
static void draw_voice(void) {
  nx_rect(CHAT_X, 0, CHAT_W, NX_H, C_DARK); nx_rect(CHAT_X, HDR_H - 1, CHAT_W, 1, C_BG);
  nx_text(CHAT_X + 20, 14, "))", 1 | NX_BOLD, C_GREEN, 0); nx_text(CHAT_X + 50, 14, voice_name, 1 | NX_BOLD, C_WHITE, CHAT_W - 260);
  VcInfo v; voice_snapshot(&v); GwInfo g; gw_snapshot(&g); vcv_begin();   /* vcv_*: camera / Go Live video (vcview.c) */
  const char *st = !voice_started ? (g.state == GW_READY ? "Asking Discord for a voice server..." : g.state == GW_ERROR ? "Gateway error" : "Connecting to Discord gateway...") :
    v.state == VC_CONNECTING ? "Connecting to voice server..." : v.state == VC_UDP ? "Setting up audio..." : v.state == VC_CONNECTED ? "Voice Connected" : v.state == VC_ERROR ? "Voice failed" : "Disconnected";
  NxColor sc = v.state == VC_CONNECTED ? C_GREEN : v.state == VC_ERROR ? C_RED : RGB(240, 178, 50);
  nx_circle(CHAT_X + 32, 86, 7, sc); nx_text(CHAT_X + 50, 74, st, 1 | NX_BOLD, sc, 0);
  if (v.state == VC_ERROR && v.err[0]) nx_wrap(CHAT_X + 50, 100, v.err, 0, C_RED, CHAT_W - 80, 2, true);
  else if (g.state == GW_ERROR && g.err[0]) nx_wrap(CHAT_X + 50, 100, g.err, 0, C_RED, CHAT_W - 80, 2, true);
  else if (v.state == VC_CONNECTED) { char b[160]; snprintf(b, sizeof b, "%s   ping %d ms   %s   rx %u / tx %u", v.mode[0] ? v.mode : "", v.ping_ms, v.mic_ok ? "GamePad mic on" : "no mic (receive only)", v.rx_packets, v.tx_packets); nx_text(CHAT_X + 50, 100, b, 0, C_MUTED, CHAT_W - 80); }
  if (v.dave_version > 0) { char b[96]; snprintf(b, sizeof b, v.dave_active ? "E2EE on (DAVE v%d)" : v.dave_group ? "E2EE (DAVE v%d): joined, waiting for the switch-over..." : "E2EE (DAVE v%d): negotiating keys...", v.dave_version); nx_wrap(CHAT_X + 50, 124, b, 0, v.dave_active ? C_GREEN : RGB(240, 178, 50), CHAT_W - 80, 2, true); }
  /* members: gateway voice states, with speaking from the audio stream */
  int y = 170; nx_text(CHAT_X + 24, y, "IN THIS CHANNEL", 0 | NX_BOLD, C_MUTED, 0); y += 30;
  int cols = 4, cw = (CHAT_W - 48) / cols; int shown = 0;
  int rstate = ring_out_state(&g);
  for (int i = 0; i < g.nmembers; i++) { GwVoiceMember *m = &g.members[i]; member_fill(m); bool talk = false; if (m->self) talk = v.speaking; else for (int k = 0; k < v.npeers; k++) if (!strcmp(v.peers[k].user_id, m->user_id)) talk = v.peers[k].speaking;
    int cx = CHAT_X + 24 + (shown % cols) * cw, cy = y + (shown / cols) * 150; nx_round(cx, cy, cw - 12, 136, C_SIDE);
    if (talk) nx_circle(cx + (cw - 12) / 2, cy + 52, 36, C_GREEN);
    if (vcv_tile(m->user_id, m->username, m->stream && !m->self, cx, cy, cw - 12, 136)) { if (talk) { nx_rect(cx, cy, cw - 12, 3, C_GREEN); nx_rect(cx, cy + 133, cw - 12, 3, C_GREEN); nx_rect(cx, cy, 3, 136, C_GREEN); nx_rect(cx + cw - 15, cy, 3, 136, C_GREEN); } }
    else { SDL_Texture *mt = NULL; if (m->avatar[0]) { char mp[120]; snprintf(mp, sizeof mp, "avatars/%s/%s.png?size=128", m->user_id, m->avatar); mt = av_get(mp, 64); }
    if (mt) nx_draw_tex(mt, cx + (cw - 12) / 2 - 32, cy + 20, 64, 64);
    else { nx_circle(cx + (cw - 12) / 2, cy + 52, 32, UC(name_color(m->user_id))); char b[2] = {m->username[0] ? m->username[0] : '?', 0}; if (b[0] >= 'a' && b[0] <= 'z') b[0] -= 32; text_center(cx + (cw - 12) / 2, cy + 30, b, 2 | NX_BOLD, C_WHITE); } }
    if (m->video) nx_text(cx + cw - 70, cy + 8, "cam", 0 | NX_BOLD, C_GREEN, 0); else if (m->stream) nx_text(cx + cw - 70, cy + 8, "LIVE", 0 | NX_BOLD, C_RED, 0);
    if (nhits < 200) hits[nhits++] = (Hit){-1 - i, cx, cy, cw - 12, 136, 3, 0};
    char nm[80]; snprintf(nm, sizeof nm, "%s%s", m->username[0] ? m->username : "...", m->self ? " (you)" : ""); int tw = nx_text_w(nm, 1); if (tw > cw - 30) tw = cw - 30; nx_text(cx + ((cw - 12) - tw) / 2, cy + 96, nm, 1, C_TEXT, cw - 30);
    if (m->mute || (m->self && v.muted)) nx_text(cx + 10, cy + 8, "muted", 0, C_RED, 0); shown++; }
  for (int i = 0; i < v.nusers && shown < 16; i++) { const char *uid = v.users[i]; bool have = false; for (int k = 0; k < g.nmembers; k++) if (!strcmp(g.members[k].user_id, uid)) have = true;
    if (have || !strcmp(uid, my_user_id)) continue; VUser *u = vuser(uid); bool talk = false; for (int k = 0; k < v.npeers; k++) if (!strcmp(v.peers[k].user_id, uid)) talk = v.peers[k].speaking;
    int cx = CHAT_X + 24 + (shown % cols) * cw, cy = y + (shown / cols) * 150; nx_round(cx, cy, cw - 12, 136, C_SIDE);
    if (talk) nx_circle(cx + (cw - 12) / 2, cy + 52, 36, C_GREEN);
    if (!vcv_tile(uid, u && u->state == 2 ? u->name : uid, false, cx, cy, cw - 12, 136)) {
    SDL_Texture *mt = NULL; if (u && u->avatar[0]) { char mp[120]; snprintf(mp, sizeof mp, "avatars/%s/%s.png?size=128", uid, u->avatar); mt = av_get(mp, 64); }
    if (mt) nx_draw_tex(mt, cx + (cw - 12) / 2 - 32, cy + 20, 64, 64); else nx_circle(cx + (cw - 12) / 2, cy + 52, 32, UC(name_color(uid))); }
    const char *nmn = u && u->state == 2 ? u->name : "..."; int tw = nx_text_w(nmn, 1); if (tw > cw - 30) tw = cw - 30; nx_text(cx + ((cw - 12) - tw) / 2, cy + 96, nmn, 1, C_TEXT, cw - 30);
    if (nhits < 200) hits[nhits++] = (Hit){-1000 - i, cx, cy, cw - 12, 136, 3, 0};
    shown++; }
  if (rstate) { Chan *dc = NULL; for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].id, voice_chan)) dc = &dms[k];
    int cx = CHAT_X + 24 + (shown % cols) * cw, cy = y + (shown / cols) * 150, mid = cx + (cw - 12) / 2; nx_round(cx, cy, cw - 12, 136, C_SIDE);
    if (rstate == 1) { int pulse = (int)(6 * (1 + sinf(nx_ticks() / 220.0f))); nx_circle(mid, cy + 52, 34 + pulse, RGBA(35, 165, 90, 110)); }
    SDL_Texture *t = dc ? av_get(dc->av, 34) : NULL; if (t) nx_draw_tex(t, mid - 32, cy + 20, 64, 64); else nx_circle(mid, cy + 52, 32, C_BLURPLE);
    char rb[40]; if (rstate == 1) { int dots = (nx_ticks() / 400) % 4; snprintf(rb, sizeof rb, "Ringing%.*s", dots, "..."); } else snprintf(rb, sizeof rb, "No answer");
    int tw = nx_text_w(rb, 1 | NX_BOLD); nx_text(mid - tw / 2, cy + 96, rb, 1 | NX_BOLD, rstate == 1 ? C_GREEN : C_MUTED, 0); shown++;
    char cb[120]; snprintf(cb, sizeof cb, rstate == 1 ? "Calling %s..." : "%s didn't pick up - stay to wait, or B to hang up", dc ? dc->name : cur_name); nx_text(CHAT_X + 24, y + ((shown + cols - 1) / cols) * 150 + 4, cb, 1, C_MUTED, CHAT_W - 48); }
  if (!shown) nx_text(CHAT_X + 24, y + 8, voice_started ? "Nobody else here yet" : "...", 1, C_MUTED, 0);
  nx_rrect(CHAT_X + CHAT_W - 200, 14, 180, 40, 10, C_SEL); text_center(CHAT_X + CHAT_W - 110, 20, "Y  Text chat", 1 | NX_BOLD, C_WHITE);
  /* bottom controls */
  int by = 648; nx_round(CHAT_X + 16, by, CHAT_W - 32, 58, C_INPUT);
  nx_round(CHAT_X + 32, by + 10, 190, 38, v.muted ? C_RED : RGB(64, 66, 73)); text_center(CHAT_X + 32 + 95, by + 18, v.muted ? "X  Unmute" : "X  Mute", 1 | NX_BOLD, C_WHITE);
  nx_round(CHAT_X + 240, by + 10, 200, 38, C_RED); text_center(CHAT_X + 340, by + 18, "B  Leave call", 1 | NX_BOLD, C_WHITE);
  { bool cam = voice_camera_on(); nx_round(CHAT_X + 458, by + 10, 170, 38, cam ? C_GREEN : RGB(64, 66, 73)); text_center(CHAT_X + 543, by + 18, cam ? "R  Camera off" : "R  Camera", 1 | NX_BOLD, C_WHITE); }   /* camera toggle (voice_set_camera) */
  nx_text(CHAT_X + 646, by + 18, "Talk into the GamePad mic. Pick a text channel on the left to keep chatting.", 0, C_MUTED, CHAT_W - 676);
  vcv_full(CHAT_X + 16, 150, CHAT_W - 32, 490);   /* full-size camera / stream view, if one is open */
}
/* composer text for display: <:name:id> -> inline emoji image, <@id> -> @name */
static const char *compose_disp(void) {
  static char out[2000]; size_t o = 0; const char *p = compose;
  while (*p && o < sizeof out - 40) {
    if (*p == '<') { const char *e = strchr(p, '>');
      if (e && (p[1] == ':' || (p[1] == 'a' && p[2] == ':'))) { const char *lc = e; while (lc > p && *lc != ':') lc--; if (lc > p + 2) { o += snprintf(out + o, sizeof out - o, "\x01" "c:%.*s\x02", (int)(e - lc - 1), lc + 1); p = e + 1; continue; } }
      if (e && p[1] == '@') { char id[24]; snprintf(id, sizeof id, "%.*s", (int)(e - p - 2) > 23 ? 23 : (int)(e - p - 2), p + 2); const char *nmn = NULL; for (int k = 0; k < nment; k++) if (!strcmp(ment_id[k], id)) nmn = ment_name[k];
        if (nmn) { o += snprintf(out + o, sizeof out - o, "@%s", nmn); p = e + 1; continue; } } }
    out[o++] = *p++;
  }
  out[o] = 0; return out;
}
static void chat_select_visible(void) {        /* keep the selected message inside the view */
  if (msel < 0 || msel >= nm) return; int below = 0; for (int k = msel + 1; k < nm; k++) below += msg_height(k);
  int bottom = MSG_BOT + (int)chat_scroll - below, top = bottom - msg_height(msel);
  if (bottom > MSG_BOT) chat_scroll -= bottom - MSG_BOT; if (top < HDR_H) chat_scroll += HDR_H - top;
}
static void draw_chat(void) {
  if (show_voice && voice_chan[0]) { nhits = 0; draw_voice(); return; }
  nx_rect(CHAT_X, 0, CHAT_W, NX_H, C_BG);
  nx_rect(CHAT_X, HDR_H - 1, CHAT_W, 1, C_DARK);
  if (cur_chan[0]) {
    nx_rect(CHAT_X, HDR_H, CHAT_W, 2, RGBA(0, 0, 0, 40));
    if (vc_chat) ico_speaker(CHAT_X + 20, 20, RGB(128, 132, 142)); else if (in_dms) { Chan *dmc = NULL; for (int k = 0; k < ndm; k++) if (!strcmp(dms[k].id, cur_chan)) dmc = &dms[k]; SDL_Texture *ht = dmc ? av_get(dmc->av, 34) : NULL; if (ht) nx_draw_tex(ht, CHAT_X + 16, 11, 30, 30); else nx_text(CHAT_X + 20, 12, "@", 2, RGB(128, 132, 142), 0); }
    else ico_hash(CHAT_X + 20, 19, RGB(128, 132, 142));
    if (vc_chat && voice_chan[0] && !strcmp(voice_chan, cur_chan)) { nx_rrect(CHAT_X + CHAT_W - 220, 14, 200, 40, 10, C_GREEN); text_center(CHAT_X + CHAT_W - 120, 20, "Y  Back to call", 1 | NX_BOLD, C_WHITE); }
    else if (in_dms && !vc_chat) { bool inc = voice_chan[0] && !strcmp(voice_chan, cur_chan); nx_rrect(CHAT_X + CHAT_W - 190, 14, 170, 40, 10, inc ? C_GREEN : RGB(64, 66, 73)); text_center(CHAT_X + CHAT_W - 105, 20, inc ? "In call" : "R  Call", 1 | NX_BOLD, C_WHITE); }
    int hw = nx_text(CHAT_X + 54, 14, cur_name, 1 | NX_BOLD, C_WHITE, CHAT_W - 300);
    if (cur_topic[0] && !in_dms) { nx_rect(CHAT_X + 66 + hw, 18, 1, 22, RGB(63, 65, 71)); nx_text(CHAT_X + 78 + hw, 17, cur_topic, 0, C_MUTED, CHAT_W - 330 - hw); }
    if (job || img[0].job || img[1].job) nx_text(CHAT_X + CHAT_W - 70, 16, "...", 1, C_MUTED, 0);
  } else nx_text(CHAT_X + 20, 14, "Discord: For the Nintendo Wii U", 1 | NX_BOLD, C_WHITE, 0);
  if (!cur_chan[0]) {                                           /* empty state */
    nx_circle(CHAT_X + CHAT_W / 2, 250, 56, C_SIDE); text_center(CHAT_X + CHAT_W / 2, 214, "#", 3 | NX_BOLD, C_MUTED);
    text_center(CHAT_X + CHAT_W / 2, 330, "Pick a channel to start chatting", 2 | NX_BOLD, C_WHITE);
    text_center(CHAT_X + CHAT_W / 2, 380, "D-pad + A, or just tap one on the left", 1, C_MUTED);
  } else {
    nx_clip(CHAT_X, HDR_H, CHAT_W, MSG_BOT - HDR_H); nhits = 0;
    int total = 0; for (int i = 0; i < nm; i++) total += msg_height(i);
    chat_total = total; int view_h = MSG_BOT - HDR_H; float maxs = total > view_h ? (float)(total - view_h) : 0;
    if (chat_scroll > maxs) chat_scroll = maxs; if (chat_scroll < 0) chat_scroll = 0;
    int y = MSG_BOT + (int)chat_scroll;
    for (int i = nm - 1; i >= 0 && y > HDR_H - 500; i--) { int h = msg_height(i); y -= h; if (y < MSG_BOT) draw_msg(i, y + 4); }
    if (!nm) { if (job) text_center(CHAT_X + CHAT_W / 2, 300, "Loading messages...", 1, C_MUTED); else { text_center(CHAT_X + CHAT_W / 2, 280, cur_name, 2 | NX_BOLD, C_WHITE); text_center(CHAT_X + CHAT_W / 2, 330, "This is the beginning of the conversation.", 1, C_MUTED); } }
    else if (total > view_h) { int bh = view_h * view_h / total; if (bh < 30) bh = 30; int by = HDR_H + (int)((maxs - chat_scroll) * (view_h - bh) / (maxs > 0 ? maxs : 1)); nx_round(CHAT_X + CHAT_W - 9, by, 5, bh, RGBA(26, 27, 30, 200)); }
    nx_unclip();
  }
  /* composer */
  int cx = CHAT_X + 16, cw = CHAT_W - 32;
  { char tl[200] = ""; int nt = 0; uint32_t now2 = nx_ticks(); for (int k = 0; k < ntyping; k++) if (typing[k].until > now2) { if (nt++) strncat(tl, ", ", sizeof tl - strlen(tl) - 1); strncat(tl, typing[k].name, sizeof tl - strlen(tl) - 1); }
    if (nt && cur_chan[0] && !reply_to[0]) { strncat(tl, nt == 1 ? " is typing..." : " are typing...", sizeof tl - strlen(tl) - 1); nx_text(cx + 6, 618, tl, 0 | NX_BOLD, C_TEXT, cw - 20); } }
  if (edit_id[0] && cur_chan[0]) { nx_rrect(cx, 606, cw, 40, 8, C_SIDE); nx_text(cx + 16, 614, "Editing message", 0 | NX_BOLD, C_TEXT, 0); nx_text(cx + cw - 150, 614, "B  cancel", 0, C_MUTED, 0); }
  else if (reply_to[0] && cur_chan[0]) { nx_rrect(cx, 606, cw, 40, 8, C_SIDE); char rb[140]; snprintf(rb, sizeof rb, "Replying to %s", reply_name); nx_text(cx + 16, 614, rb, 0, C_TEXT, cw - 200); nx_text(cx + cw - 150, 614, "B  cancel", 0, C_MUTED, 0); }
  nx_rrect(cx, 648, cw, 58, 10, C_INPUT);
  if (cur_chan[0]) { nx_circle(cx + 30, 677, 14, C_ICON); nx_rect(cx + 23, 676, 14, 3, C_INPUT); nx_rect(cx + 29, 670, 3, 14, C_INPUT); }   /* "+" */
  char ph[110]; snprintf(ph, sizeof ph, "Message %s%s", in_dms ? "@" : "#", cur_name);
  if (cur_chan[0]) { nx_rrect(cx + cw - 214, 664, 44, 26, 5, C_ICON); nx_text(cx + cw - 207, 664, "GIF", 0 | NX_BOLD, C_INPUT, 0); }
  if (cur_chan[0]) { nx_text(cx + cw - 152, 660, "\xF0\x9F\x99\x82", 1, C_WHITE, 0); }   /* emoji button */
  int tx0 = cur_chan[0] ? cx + 58 : cx + 20;
  if (!cur_chan[0]) nx_text(cx + 20, 664, "Select a channel first", 1, RGB(110, 117, 126), 0);
  else if (compose[0]) { const char *t = compose_disp(); if (nx_text_w(t, 1) > cw - 300) { size_t l = strlen(t); t += l > 46 ? l - 46 : 0; while ((*t & 0xC0) == 0x80) t++; } nx_text(tx0, 664, t, 1, C_TEXT, cw - 340); nx_round(cx + cw - 104, 656, 96, 42, send_pending ? C_MUTED : C_BLURPLE); text_center(cx + cw - 56, 664, send_pending ? "..." : "Send", 1 | NX_BOLD, C_WHITE); }
  else { nx_text(tx0, 664, ph, 1, RGB(110, 117, 126), cw - 340); nx_text(cx + cw - 92, 668, "X: type", 0, RGB(110, 117, 126), 0); }
}
static void draw_status(void) {
  if (status[0] && nx_ticks() < status_until) {
    int w = nx_text_w(status, 1) + 70; if (w > NX_W - 60) w = NX_W - 60;
    nx_round((NX_W - w) / 2, NX_H - 150, w, 50, RGBA(17, 18, 20, 240)); nx_circle((NX_W - w) / 2 + 24, NX_H - 125, 7, RGB(240, 178, 50)); nx_text((NX_W - w) / 2 + 42, NX_H - 138, status, 1, C_TEXT, w - 54);
  }
}

/* ---------------- login + QR screens ---------------- */
#define BTN_QR_X 340
#define BTN_QR_Y 372
#define BTN_QR_W 600
#define BTN_QR_H 64
#define BTN_TOK_Y 450
#define BTN_TOK_H 56
static void draw_login(void) {
  nx_clear(RGB(36, 37, 41));
  nx_round(220, 70, 840, 580, C_BG); nx_round(222, 72, 836, 576, C_BG);
  nx_circle(640, 160, 46, C_BLURPLE); text_center(640, 130, "D", 3 | NX_BOLD, C_WHITE);
  text_center(640, 226, "Welcome back!", 2 | NX_BOLD, C_WHITE);
  text_center(640, 276, "Chat on Discord straight from your Wii U", 1, C_MUTED);
  nx_round(BTN_QR_X, BTN_QR_Y, BTN_QR_W, BTN_QR_H, C_BLURPLE); text_center(640, BTN_QR_Y + 16, "Log in with QR code", 2 | NX_BOLD, C_WHITE); nx_text(BTN_QR_X + 566 - 6, BTN_QR_Y + 20, "Y", 1 | NX_BOLD, RGB(200, 205, 255), 0);
  nx_round(BTN_QR_X, BTN_TOK_Y, BTN_QR_W, BTN_TOK_H, RGB(78, 80, 88)); text_center(640, BTN_TOK_Y + 12, "Enter a token instead", 1 | NX_BOLD, C_WHITE); nx_text(BTN_QR_X + 566 - 6, BTN_TOK_Y + 14, "A", 1 | NX_BOLD, C_MUTED, 0);
  nx_wrap(260, 528, "Token file: SD:/wiiu/apps/MingusDiscord/token.txt    |    Using a personal account in a third-party client is against Discord's Terms of Service and can get the account actioned.", 0, RGB(120, 126, 136), 760, 3, true);
}
static uint8_t qr_buf[qrcodegen_BUFFER_LEN_MAX], qr_tmp[qrcodegen_BUFFER_LEN_MAX]; static char qr_for[100]; static bool qr_ok;
static void step(int n, int y, const char *t) { nx_circle(660, y + 16, 15, C_BLURPLE); char b[4]; snprintf(b, sizeof b, "%d", n); text_center(660, y + 3, b, 0 | NX_BOLD, C_WHITE); nx_wrap(692, y + 2, t, 1, C_TEXT, 520, 2, true); }
static void draw_qr(void) {
  const RaState *r = ra_get();
  nx_clear(RGB(36, 37, 41)); nx_round(60, 90, 1160, 540, C_BG);
  text_center(640, 110, "Log in with QR code", 2 | NX_BOLD, C_WHITE);
  nx_round(100, 170, 420, 420, C_WHITE);
  if (r->state == RA_QR || r->state == RA_SCANNED || r->state == RA_LOGIN) {
    if (strcmp(qr_for, r->fingerprint)) { char payload[160]; snprintf(payload, sizeof payload, "https://discord.com/ra/%s", r->fingerprint);
      qr_ok = qrcodegen_encodeText(payload, qr_tmp, qr_buf, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true); snprintf(qr_for, sizeof qr_for, "%s", r->fingerprint); }
    if (qr_ok) { int n = qrcodegen_getSize(qr_buf), px = 372 / n; if (px < 3) px = 3; int tot = n * px, ox = 100 + (420 - tot) / 2, oy = 170 + (420 - tot) / 2;
      for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) if (qrcodegen_getModule(qr_buf, x, y)) nx_rect(ox + x * px, oy + y * px, px, px, RGB(0, 0, 0));
      if (r->state != RA_QR) { nx_rect(100, 170, 420, 420, RGBA(255, 255, 255, 215)); nx_circle(310, 380, 44, C_GREEN); text_center(310, 354, "OK", 2 | NX_BOLD, C_WHITE); } }
  } else text_center(310, 360, r->state == RA_ERROR ? "!" : "...", 3 | NX_BOLD, RGB(150, 154, 162));
  int tx = 640;
  if (r->state == RA_KEYGEN) { nx_text(tx + 20, 190, "Creating a secure key...", 2 | NX_BOLD, C_WHITE, 560); nx_wrap(tx + 20, 250, "This only happens once and can take a minute on the Wii U. Please keep this screen open.", 1, C_MUTED, 520, 0, true); }
  else if (r->state == RA_CONNECTING) nx_text(tx + 20, 190, "Connecting to Discord...", 2 | NX_BOLD, C_WHITE, 560);
  else if (r->state == RA_QR) {
    nx_text(tx + 20, 170, "Scan with your phone", 2 | NX_BOLD, C_WHITE, 560);
    step(1, 240, "Open Discord on your phone"); step(2, 296, "Tap You, then Scan QR Code"); step(3, 352, "Point the camera at this screen"); step(4, 408, "Confirm the login on your phone");
  } else if (r->state == RA_SCANNED) { nx_text(tx + 20, 190, "Almost there", 2 | NX_BOLD, C_GREEN, 560); char b[200]; snprintf(b, sizeof b, "Hi %s! Tap \"Yes, log in\" on your phone to finish.", r->user[0] ? r->user : "there"); nx_wrap(tx + 20, 250, b, 1, C_TEXT, 540, 0, true); }
  else if (r->state == RA_LOGIN) nx_text(tx + 20, 190, "Logging you in...", 2 | NX_BOLD, C_GREEN, 560);
  else if (r->state == RA_ERROR) { nx_text(tx + 20, 190, "Couldn't log in", 2 | NX_BOLD, C_RED, 560); nx_wrap(tx + 20, 250, r->err, 1, C_TEXT, 540, 6, true); nx_round(tx + 20, 450, 300, 58, C_BLURPLE); text_center(tx + 170, 462, "Try again   Y", 1 | NX_BOLD, C_WHITE); }
  nx_text(100, 598, "B  Back", 1, C_MUTED, 0);
  nx_wrap(tx + 20, 560, "Uses Discord's own QR login. Third-party clients aren't allowed by Discord's Terms of Service.", 0, RGB(120, 126, 136), 540, 2, true);
}

/* ---------------- input ---------------- */
static void ensure_vis_guild(void) { int per = (NX_H - 14) / GUILD_PITCH; if (gsel < gscroll) gscroll = gsel; if (gsel >= gscroll + per - 1) gscroll = gsel - per + 2; if (gscroll < 0) gscroll = 0; }
static void move_chan(int dir) {
  Chan *v = view(); int n = view_n(); int i = csel + dir; while (i >= 0 && i < n && v[i].header) i += dir; if (i < 0 || i >= n) return; csel = i;
  int y = 0; for (int k = cscroll; k <= i; k++) y += row_h(&v[k]);
  while (y > list_bot() - LIST_Y - 8 && cscroll < i) { y -= row_h(&v[cscroll]); cscroll++; } if (i < cscroll) cscroll = i;
}
static void commit_guild(void) { rail_activate(rsel); }
static void commit_chan(void) { Chan *v = view(); if (csel >= 0 && csel < view_n() && !v[csel].header) open_channel(&v[csel]); }
static void send_compose(void) { if (!compose[0] || send_pending || !cur_chan[0]) return; snprintf(send_buf, sizeof send_buf, "%s", compose); send_pending = true; }
static int sw_down, sw_x, sw_y, sw_moved, sw_acc;   /* rail / channel list: 1 or 2 while a finger is down */
static bool hit(const NxInput *in, int x, int y, int w, int h) { return in->touch_pressed && in->tx >= x && in->tx < x + w && in->ty >= y && in->ty < y + h; }

static void react_toggle(const char *msg_id, const char *code, bool remove) {
  if (rjob || !cur_chan[0]) return; char enc[200], path[320]; dc_urlenc(code, enc, sizeof enc);
  snprintf(path, sizeof path, "/channels/%s/messages/%s/reactions/%s/@me", cur_chan, msg_id, enc); rjob = dc_request(remove ? "DELETE" : "PUT", path, "");
}
static void chat_tap(int x, int y) {               /* a tap (not a drag) in the message area */
  for (int k = nhits - 1; k >= 0; k--) { Hit *h = &hits[k]; if (h->kind == 3 || h->i < 0 || x < h->x || x >= h->x + h->w || y < h->y || y >= h->y + h->h || h->i >= nm) continue;
    Msg *m = &msgs[h->i];
    if (h->kind == 2) { React *re = &m->re[h->ri]; react_toggle(m->id, re->code, re->me); return; }
    if (h->kind == 4) { mv_open(&m->at[h->ri]); return; }
    if (h->kind == 1) { ov_profile_perms(my_perms()), ov_open_profile(m->authorid, in_dms ? "" : view_guild); return; }
    msel = h->i; ov_actions_mod((my_perms() & P_MANAGE_MSG) != 0); ov_open_actions_ex(m->id, m->authorid, m->author, !strcmp(m->authorid, my_user_id)); return; }
}
static void do_logout(void) { voice_leave(); gw_stop(); gw_started = false; my_user_id[0] = 0; dc_logout(); go_login("Logged out"); }
static void open_member_menu(const char *uid) {        /* volume / mute / moderation for someone in the call (also reachable from their video tile) */
  if (!uid || !uid[0] || !strcmp(uid, my_user_id)) return;
  unsigned long long pm = 0; for (int q = 1; q < ng; q++) if (!strcmp(guilds[q].id, voice_guild)) pm = guilds[q].perms;
  GwInfo g; gw_snapshot(&g); for (int i = 0; i < g.nmembers; i++) if (!strcmp(g.members[i].user_id, uid)) { member_fill(&g.members[i]); ov_open_member(uid, g.members[i].username[0] ? g.members[i].username : "...", voice_guild, pm, true, g.members[i].mute, g.members[i].deaf); return; }
  VUser *u = vuser(uid); ov_open_member(uid, u && u->state == 2 ? u->name : "...", voice_guild, pm, true, false, false);
}
static void ov_result(const OvResult *r) {
  switch (r->kind) {
    case OVR_LOGOUT: do_logout(); break;
    case OVR_PIN: if (!rjob && cur_chan[0]) { char p2[120]; snprintf(p2, sizeof p2, "/channels/%s/pins/%s", cur_chan, r->msg_id); rjob = dc_request("PUT", p2, ""); say("Pinned", 1500); } break;
    case OVR_OPEN_MEDIA: for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, r->msg_id)) for (int k = 0; k < msgs[i].nat; k++) { Attach *a = &msgs[i].at[k]; if (a->audio || a->video || a->image) { mv_open(a); break; } } break;
    case OVR_EDIT: for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, r->msg_id)) { snprintf(compose, sizeof compose, "%s", msgs[i].raw ? msgs[i].raw : msgs[i].text); snprintf(edit_id, sizeof edit_id, "%s", r->msg_id); reply_to[0] = 0; msel = -1; pane = 2; } break;
    case OVR_DELETE: if (!rjob && cur_chan[0]) { char p2[120]; snprintf(p2, sizeof p2, "/channels/%s/messages/%s", cur_chan, r->msg_id); rjob = dc_request("DELETE", p2, ""); msel = -1; } break;
    case OVR_INSERT: strncat(compose, r->text, sizeof compose - strlen(compose) - 1); break;
    case OVR_SEND_TEXT: if (!send_pending && cur_chan[0]) { snprintf(send_buf, sizeof send_buf, "%s", r->text); send_pending = true; send_keep_compose = true; } break;
    case OVR_REACT: { bool mine = false; for (int i = 0; i < nm; i++) if (!strcmp(msgs[i].id, r->msg_id)) for (int k = 0; k < msgs[i].nre; k++) if (!strcmp(msgs[i].re[k].code, r->code) && msgs[i].re[k].me) mine = true;
      if (!mine) react_toggle(r->msg_id, r->code, false); break; }
    case OVR_REPLY: snprintf(reply_to, sizeof reply_to, "%s", r->msg_id); snprintf(reply_name, sizeof reply_name, "%s", r->name); msel = -1; pane = 2; break;
    case OVR_MENTION: { char t[40]; snprintf(t, sizeof t, "<@%s> ", r->user_id); strncat(compose, t, sizeof compose - strlen(compose) - 1); if (nment < 8) { snprintf(ment_id[nment], 24, "%s", r->user_id); snprintf(ment_name[nment], 80, "%s", r->name); nment++; } msel = -1; break; }
    case OVR_PROFILE: ov_profile_perms(my_perms()), ov_open_profile(r->user_id, in_dms ? "" : view_guild); break;
    case OVR_EMOJI_REACT: ov_emoji_guilds(guilds + 1, ng - 1), ov_open_emoji(EMO_REACT, in_dms ? "" : view_guild, r->msg_id); break;
    case OVR_OPEN_DM: { select_guild(0); Chan ch; memset(&ch, 0, sizeof ch); snprintf(ch.id, 24, "%s", r->chan_id); snprintf(ch.name, sizeof ch.name, "%s", r->name); ch.type = 1; open_channel(&ch); break; }
  }
}
static void handle_input(NxInput *in) {
  if (screen == S_LOGIN) {
    if (in->pressed[B_A] || hit(in, BTN_QR_X, BTN_TOK_Y, BTN_QR_W, BTN_TOK_H)) nx_start_text("Discord token", "");
    if (in->text[0]) { nx_stop_text(); char t[400]; snprintf(t, sizeof t, "%s", in->text); for (char *p = t; *p; p++) if (*p == '\n') *p = 0; if (strlen(t) > 20) { dc_set_token(t); screen = S_MAIN; want_guilds = true; want_me = true; status[0] = 0; } else say("That doesn't look like a token", 3000); }
    if (in->pressed[B_Y] || hit(in, BTN_QR_X, BTN_QR_Y, BTN_QR_W, BTN_QR_H)) { qr_for[0] = 0; ra_start(); screen = S_QR; }
    return;
  }
  if (screen == S_QR) {
    const RaState *r = ra_get();
    if (r->state == RA_DONE) { dc_set_token(r->token); ra_cancel(); screen = S_MAIN; want_guilds = true; want_me = true; status[0] = 0; return; }
    if (in->pressed[B_B] || hit(in, 100, 590, 200, 40)) { ra_cancel(); screen = S_LOGIN; return; }
    if (r->state == RA_ERROR && (in->pressed[B_Y] || hit(in, 660, 450, 300, 58))) { qr_for[0] = 0; ra_start(); }
    return;
  }
  if (tap_down) { if (in->touch && !in->touch_released) { int ddx = abs(in->tx - tap_x), ddy = abs(in->ty - tap_y); if (ddx > tap_moved) tap_moved = ddx; if (ddy > tap_moved) tap_moved = ddy; } if (in->touch_released) { tap_down = false; if (tap_moved < 30) chat_tap(tap_x, tap_y); } else if (!in->touch) tap_down = false; }
  if (in->text[0] && cur_chan[0]) {               /* the keyboard was opened with the draft as its initial text, so its result replaces the draft */
    nx_stop_text(); char *nl = strchr(in->text, '\n'); if (nl) *nl = 0;
    snprintf(compose, sizeof compose, "%s", in->text);
    if (nl) send_compose();
  }
  if (show_voice && voice_chan[0] && (vcv_fullscreen_active() || in->tx >= CHAT_X || !in->touch_pressed) && vcv_input(in, pane == 2)) { if (in->touch_pressed) pane = 2; { char mu[24]; if (vcv_take_menu(mu)) open_member_menu(mu); } return; }   /* video (vcview.c): focus / full screen / Watch; takes A,B,Left,Right only while a video is focused */
  if (show_voice && voice_chan[0] && pane == 2 && in->pressed[B_Y]) { open_vc_chat_cur(); return; }
  if (show_voice && voice_chan[0] && pane == 2 && in->pressed[B_PLUS]) { ov_open_mic(); return; }
  if (!show_voice && in_dms && cur_chan[0] && pane == 2 && in->pressed[B_R]) { if (voice_chan[0] && !strcmp(voice_chan, cur_chan)) { show_voice = true; } else call_start(); return; }
  if (show_voice && voice_chan[0] && pane == 2 && in->pressed[B_ZL]) { deaf_on = !deaf_on; voice_set_deaf(deaf_on); say(deaf_on ? "Deafened" : "Undeafened", 1500); return; }
  if (show_voice && voice_chan[0] && pane == 2 && in->pressed[B_A]) {      /* people in the call -> member menu (volume / mute for me / moderation) */
    GwInfo g; gw_snapshot(&g); VcInfo vv; voice_snapshot(&vv); static char ids[48][24], names[48][80]; static bool mu[48], de[48]; int n = 0;
    for (int k = 0; k < g.nmembers && n < 48; k++) { if (!strcmp(g.members[k].user_id, my_user_id)) continue; member_fill(&g.members[k]); snprintf(ids[n], 24, "%s", g.members[k].user_id); snprintf(names[n], 80, "%s", g.members[k].username[0] ? g.members[k].username : "..."); mu[n] = g.members[k].mute; de[n] = g.members[k].deaf; n++; }
    for (int k = 0; k < vv.nusers && n < 48; k++) { bool have = !strcmp(vv.users[k], my_user_id); for (int q = 0; q < n; q++) if (!strcmp(ids[q], vv.users[k])) have = true; if (have) continue; VUser *u = vuser(vv.users[k]); snprintf(ids[n], 24, "%s", vv.users[k]); snprintf(names[n], 80, "%s", u && u->state == 2 ? u->name : vv.users[k]); mu[n] = de[n] = false; n++; }
    unsigned long long pm = 0; for (int k = 1; k < ng; k++) if (!strcmp(guilds[k].id, voice_guild)) pm = guilds[k].perms;
    ov_open_people(n, ids, names, mu, de, voice_guild, pm); return; }
  if (!show_voice && vc_chat && voice_chan[0] && !strcmp(voice_chan, cur_chan) && in->pressed[B_Y]) { show_voice = true; pane = 2; return; }
  if (pane == 1 && in->pressed[B_X] && !in_dms) { Chan *vv = view(); if (csel >= 0 && csel < view_n() && vv[csel].type == 2) { open_vc_chat(vv[csel].id, vv[csel].name); return; } }
  if (show_voice && voice_chan[0] && pane == 2) {
    if (in->pressed[B_X] || hit(in, CHAT_X + 32, 658, 190, 38)) { voice_muted = !voice_muted; voice_set_mute(voice_muted); }
    if (in->pressed[B_B] || hit(in, CHAT_X + 240, 658, 200, 38)) { voice_leave(); return; }
    if (in->pressed[B_R] || hit(in, CHAT_X + 458, 658, 170, 38)) { bool on = !voice_camera_on(); voice_set_camera(on); say(voice_camera_on() ? "Camera on" : on ? "Camera: not connected yet" : "Camera off", 1500); return; }   /* camera toggle */
  }
  if (pane == 2 && cur_chan[0] && !(show_voice && voice_chan[0]) && in->pressed[B_PLUS]) { att_on = true; att_sel = 0; return; }
  if (pane == 2 && cur_chan[0] && !(show_voice && voice_chan[0])) {    /* message selection: Up/Down pick a message, A = actions */
    if (in->pressed[B_UP]) { msel = msel < 0 ? nm - 1 : (msel > 0 ? msel - 1 : 0); chat_select_visible(); in->pressed[B_UP] = false; }
    else if (in->pressed[B_DOWN] && msel >= 0) { msel++; if (msel >= nm) msel = -1; else chat_select_visible(); in->pressed[B_DOWN] = false; }
    if (in->pressed[B_A] && msel >= 0 && msel < nm) { bool med = false; for (int k = 0; k < msgs[msel].nat; k++) if (msgs[msel].at[k].audio || msgs[msel].at[k].video || msgs[msel].at[k].image) med = true; ov_actions_media(med); ov_actions_mod((my_perms() & P_MANAGE_MSG) != 0); ov_open_actions_ex(msgs[msel].id, msgs[msel].authorid, msgs[msel].author, !strcmp(msgs[msel].authorid, my_user_id)); return; }
    if (in->pressed[B_B] && edit_id[0]) { edit_id[0] = 0; compose[0] = 0; return; }
    if (in->pressed[B_B] && msel >= 0) { msel = -1; return; }
    if (in->pressed[B_B] && reply_to[0]) { reply_to[0] = 0; return; }
  }
  int dir = 0; uint32_t now = nx_ticks();
  if (in->pressed[B_DOWN]) dir = 1; if (in->pressed[B_UP]) dir = -1;
  if (fabsf(in->ly) > 0.6f && now - stick_t > 160) { dir = in->ly > 0 ? 1 : -1; stick_t = now; }
  if (pane == 0 && dir) { int s = rsel + dir; if (s >= 0 && s < nrail) { rsel = s; rail_visible(); } }
  if (pane == 1 && dir) move_chan(dir);
  if (pane == 2 && dir) chat_scroll -= dir * 80;
  if (pane == 2 && fabsf(in->ry) > 0.2f) chat_scroll -= in->ry * 14;
  if (pane == 2 && in->tdy) chat_scroll += in->tdy;
  if (in->touch && in->tx < RAIL_W && in->tdy) { static int racc; racc -= in->tdy; while (racc > GUILD_PITCH) { racc -= GUILD_PITCH; if (rscroll < nrail - 1) rscroll++; } while (racc < -GUILD_PITCH) { racc += GUILD_PITCH; if (rscroll > 0) rscroll--; } }
  if (sw_down) { if (in->touch && !in->touch_released) { int ddx = abs(in->tx - sw_x), ddy = abs(in->ty - sw_y); if (ddx > sw_moved) sw_moved = ddx; if (ddy > sw_moved) sw_moved = ddy; }
    if (sw_down == 2 && in->tdy) { sw_acc -= in->tdy; while (sw_acc > 40) { sw_acc -= 40; { Chan *vv = view(); int rest = 0; for (int q = cscroll; q < view_n(); q++) rest += row_h(&vv[q]); if (rest > list_bot() - LIST_Y) cscroll++; } } while (sw_acc < -40) { sw_acc += 40; if (cscroll > 0) cscroll--; } }
    if (in->touch_released || !in->touch) { int k0 = sw_down; sw_down = 0;
      if (in->touch_released && sw_moved < 30) {
        if (k0 == 1) { int yy = 14; for (int k = rscroll; k < nrail; k++) { if (sw_y >= yy && sw_y < yy + 56) { rsel = k; rail_activate(k); break; } yy += rail_h(k); } }
        else { int yy = LIST_Y; Chan *v = view(); for (int i = cscroll; i < view_n(); i++) { int h = row_h(&v[i]); if (sw_y >= yy && sw_y < yy + h && !v[i].header) { csel = i; commit_chan(); break; } yy += h; } } } } }
  if (in->pressed[B_ZR]) chat_scroll -= 420; if (in->pressed[B_ZL]) chat_scroll += 420;
  if (in->pressed[B_A]) { if (pane == 0) commit_guild(); else if (pane == 1) commit_chan(); else if (compose[0]) send_compose(); else nx_start_text("Message", compose); }
  if (in->pressed[B_X] && !(show_voice && voice_chan[0] && pane == 2)) { if (cur_chan[0]) { pane = 2; nx_start_text("Message", compose); } }
  if (in->pressed[B_B]) { if (pane == 2 && compose[0]) compose[0] = 0; else if (pane > 0) pane--; }
  if (in->pressed[B_RIGHT] || in->pressed[B_R]) { if (pane == 0) commit_guild(); else if (pane < 2 && cur_chan[0]) pane++; }
  if (in->pressed[B_LEFT] || in->pressed[B_L]) { if (pane > 0) pane--; }
  if (in->pressed[B_Y]) { want_guilds = true; sett_ready = rel_ready = false; if (gsel == 0) want_dms = true; else snprintf(want_chans, sizeof want_chans, "%s", view_guild); if (cur_chan[0]) want_msgs = true; say("Refreshing...", 1200); }
  if (in->pressed[B_MINUS] && !(show_voice && voice_chan[0])) say("Log out is in Settings (gear icon, bottom left)", 2500);
  if (in->touch_pressed) {
    int x = in->tx, y = in->ty;
    if (x < RAIL_W && y >= NX_H - BIRD_H) { bird_poke(); say("chirp!", 900); }
    else if (x < RAIL_W) { sw_down = 1; sw_x = x; sw_y = y; sw_moved = 0; }   /* tap = on release, a swipe scrolls */
    else if (x >= CH_X && x < CHAT_X && y >= NX_H - USER_H) {                /* user panel: mute / deafen / settings */
      if (x >= CH_X + CH_W - 108 && x < CH_X + CH_W - 76) { voice_muted = !voice_muted; voice_set_mute(voice_muted); say(voice_muted ? "Muted" : "Unmuted", 1200); }
      else if (x >= CH_X + CH_W - 74 && x < CH_X + CH_W - 42) { deaf_on = !deaf_on; voice_set_deaf(deaf_on); say(deaf_on ? "Deafened" : "Undeafened", 1200); }
      else if (x >= CH_X + CH_W - 40) ov_open_mic(); }
    else if (x >= CH_X && x < CHAT_X && voice_chan[0] && y >= list_bot()) {  /* voice panel: disconnect / open the call */
      if (x >= CH_X + CH_W - 52) voice_leave(); else { show_voice = true; pane = 2; } }
    else if (x < CHAT_X && y >= LIST_Y && y < list_bot()) { sw_down = 2; sw_x = x; sw_y = y; sw_moved = 0; sw_acc = 0; }
    else if (show_voice && voice_chan[0] && x >= CHAT_X) { pane = 2; GwInfo g; gw_snapshot(&g); for (int k = 0; k < nhits; k++) if (hits[k].kind == 3 && x >= hits[k].x && x < hits[k].x + hits[k].w && y >= hits[k].y && y < hits[k].y + hits[k].h) { int mi = -1 - hits[k].i; if (hits[k].i <= -1000) { VcInfo vv; voice_snapshot(&vv); int ui = -1000 - hits[k].i; unsigned long long pm = 0; for (int q = 1; q < ng; q++) if (!strcmp(guilds[q].id, voice_guild)) pm = guilds[q].perms;
      if (ui >= 0 && ui < vv.nusers) { VUser *u = vuser(vv.users[ui]); ov_open_member(vv.users[ui], u && u->state == 2 ? u->name : vv.users[ui], voice_guild, pm, true, false, false); } } else if (mi >= 0 && mi < g.nmembers) { unsigned long long pm = 0; for (int q = 1; q < ng; q++) if (!strcmp(guilds[q].id, voice_guild)) pm = guilds[q].perms; ov_open_member(g.members[mi].user_id, g.members[mi].username, voice_guild, pm, true, g.members[mi].mute, g.members[mi].deaf); } break; } }
    if (show_voice && voice_chan[0] && x >= CHAT_X + CHAT_W - 200 && y < 60) open_vc_chat_cur();
    else if (!show_voice && vc_chat && voice_chan[0] && !strcmp(voice_chan, cur_chan) && x >= CHAT_X + CHAT_W - 220 && y < 60) { show_voice = true; pane = 2; }
    else if (!show_voice && in_dms && cur_chan[0] && x >= CHAT_X + CHAT_W - 190 && y < 60) { if (voice_chan[0] && !strcmp(voice_chan, cur_chan)) show_voice = true; else call_start(); pane = 2; }
    else if (x >= CHAT_X && y >= 648 && cur_chan[0]) { int cxx = CHAT_X + 16, cww = CHAT_W - 32; if (x < cxx + 50) { att_on = true; att_sel = 0; } else if (x >= cxx + cww - 160 && x < cxx + cww - 110) ov_emoji_guilds(guilds + 1, ng - 1), ov_open_emoji(EMO_INSERT, in_dms ? "" : view_guild, NULL); else if (x >= cxx + cww - 220 && x < cxx + cww - 166) ov_open_gifs(); else if (compose[0] && x > cxx + cww - 104) send_compose(); else nx_start_text("Message", compose); }
    else if (x >= CHAT_X && y >= 606 && y < 646 && reply_to[0]) reply_to[0] = 0;
    else if (x >= CHAT_X && y > HDR_H && y < MSG_BOT && cur_chan[0] && !(show_voice && voice_chan[0])) { pane = 2; tap_down = true; tap_x = x; tap_y = y; tap_moved = 0; }
    else if (x >= CHAT_X) pane = 2;
  }
}

int main(void) {
  if (!nx_init("Discord: For the Nintendo Wii U")) return 1;
  nx_set_inline_provider(inline_provider); ov_init(av_get); ov_load_volumes(); anim_set_log(anim_log);
  dc_init(); dlog_init(dc_dir()); netmem_start();   /* before any network use: own socket memory for UDP voice/video */
  nxnet_init(); rs_load();
  if (dc_has_token()) { screen = S_MAIN; want_guilds = true; want_me = true; }
  NxInput in = {0}; strcpy(guilds[0].name, "Direct Messages");
  while (!in.quit) {
    nx_poll(&in);
    if (voice_chan[0]) voice_ptt(in.down[B_ZR]);
    if (screen != S_MAIN) handle_input(&in); else { if (inc_on) incoming_input(&in); else if (rec_ui) rec_input(&in); else if (vrec_ui) vrec_input(&in); else if (att_on) att_input(&in); else if (mv_on) mv_input(&in); else if (ov_active()) { OvResult r; ov_input(&in, &r); if (r.kind) ov_result(&r); } else handle_input(&in); net_tick(); }
    if (screen == S_LOGIN) draw_login();
    else if (screen == S_QR) draw_qr();
    else { nx_clear(C_BG); draw_chat(); draw_channels(); draw_guilds(); vcv_fullscreen(); ov_draw(); if (att_on) draw_att(); if (mv_on) mv_draw(); if (rec_ui) draw_rec(); if (vrec_ui) draw_vrec(); if (inc_on) draw_incoming(); anims_sweep(); }
    if (in.typing) { nx_rect(0, 0, NX_W, 44, RGBA(0, 0, 0, 200)); text_center(NX_W / 2, 8, "Typing on the GamePad keyboard - press OK when done", 1, C_WHITE); }
    draw_status(); nx_present(); SDL_Delay(4);
  }
  dlog("[exit] leaving voice"); voice_leave(); dlog("[exit] stopping gateway"); gw_stop(); dlog("[exit] stopping network"); netmem_stop(); dlog("[exit] shutting down UI"); nx_shutdown(); dlog("[exit] done"); return 0;
}
