/* vcview.c - call-screen video UI: see vcview.h
 * Modes: GRID (videos inside the member tiles) -> FOCUS (one video large in the call area) -> FULL (whole 1280x720). */
#include "vcview.h"
#include "vrecv.h"
#include "golive.h"
#include "gw.h"
#include "dlog.h"
#include <stdio.h>
#include <string.h>

enum { K_FOCUS = 1, K_WATCH, K_STOP, K_MENU };
static char menu_req[24]; static int menu_box[4];
bool vcv_take_menu(char *out) { if (!menu_req[0]) return false; snprintf(out, 24, "%s", menu_req); menu_req[0] = 0; return true; }
enum { M_GRID, M_FOCUS, M_FULL };
typedef struct { int kind, conn; char uid[24]; int x, y, w, h; } Box;
static Box box[40]; static int nbox;
typedef struct { int conn; char uid[24]; char name[64]; bool live; } Vis;   /* videos shown this frame (cycling, cursor) */
static Vis vis[16]; static int nvis;
static int mode = M_GRID; static int f_conn = -1; static char f_uid[24], f_name[64]; static bool f_live;
static int fx, fy, fw, fh;                 /* focus view rectangle (call area) */
static bool drawn_this_frame;              /* draw_voice ran this frame (full screen only then) */
static uint32_t osd_t;                     /* full screen overlay shown since */
static int cursor = -1; static uint32_t cursor_t;
static char ended_msg[64]; static uint32_t ended_t;
static int stop_box[4];                    /* "Stop watching" chip in focus / full view */

static void add_box(int kind, int conn, const char *uid, int x, int y, int w, int h) { if (nbox < 40) { Box *b = &box[nbox++]; b->kind = kind; b->conn = conn; snprintf(b->uid, sizeof b->uid, "%s", uid); b->x = x; b->y = y; b->w = w; b->h = h; } }
static void set_focus(int conn, const char *uid, const char *name, int m) {
  if (conn < 0 || !uid) { if (mode != M_GRID) dlog("[vcv] back to grid"); mode = M_GRID; f_conn = -1; f_uid[0] = 0; vr_set_focus(0, NULL); return; }
  f_conn = conn; snprintf(f_uid, sizeof f_uid, "%s", uid); snprintf(f_name, sizeof f_name, "%s", name && name[0] ? name : uid); vr_set_focus(conn, uid); mode = m; osd_t = SDL_GetTicks(); ended_t = 0;
  f_live = conn == VR_CONN_STREAM; for (int i = 0; i < nvis; i++) if (vis[i].conn == conn && !strcmp(vis[i].uid, uid)) f_live = vis[i].conn == VR_CONN_STREAM;
  dlog("[vcv] %s: conn %d user %s", m == M_FULL ? "full screen" : "focus", conn, uid);
}
static void chip(int x, int y, int w, int h, NxColor c, const char *t) { nx_rrect(x, y, w, h, 8, c); int tw = nx_text_w(t, 0); nx_text(x + (w - tw) / 2, y + (h - nx_text_h(0)) / 2, t, 0 | NX_BOLD, RGB(255, 255, 255), 0); }
static void fit(SDL_Texture *t, int tw, int th, int x, int y, int w, int h) {
  if (tw <= 0 || th <= 0) return; float s = (float)w / tw; if (th * s > h) s = (float)h / th; int dw = (int)(tw * s), dh = (int)(th * s); nx_draw_tex(t, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh);
}
static int source_for(const char *uid, bool prefer_stream) {   /* which connection has video for this user */
  if (prefer_stream && vr_active(VR_CONN_STREAM, uid)) return VR_CONN_STREAM;
  if (vr_active(VR_CONN_VOICE, uid)) return VR_CONN_VOICE;
  if (vr_active(VR_CONN_STREAM, uid)) return VR_CONN_STREAM;
  if (vr_active(VR_CONN_SELF, uid)) return VR_CONN_SELF;       /* our own camera preview */
  if (vr_active(VR_CONN_TEST, uid)) return VR_CONN_TEST;
  return -1;
}
static bool watching(const char *uid) { const char *o = golive_owner(); GwInfo g; gw_snapshot(&g); return (o[0] && !strcmp(o, uid)) || (g.stream_state != GWS_NONE && g.stream_state != GWS_ENDED && g.stream_key[0] && strstr(g.stream_key, uid)); }
static void add_vis(int conn, const char *uid, const char *name) {
  for (int i = 0; i < nvis; i++) if (vis[i].conn == conn && !strcmp(vis[i].uid, uid)) return;
  if (nvis < 16) { vis[nvis].conn = conn; snprintf(vis[nvis].uid, 24, "%s", uid); snprintf(vis[nvis].name, 64, "%s", name && name[0] ? name : uid); nvis++; }
}
/* is the focused source still there? if not: "ended" message, then back to the grid */
static bool focus_alive(void) {
  if (f_conn < 0) return false;
  bool alive = vr_active(f_conn, f_uid) || (f_conn == VR_CONN_STREAM && watching(f_uid));
  if (alive) { ended_t = 0; return true; }
  if (!ended_t) { ended_t = SDL_GetTicks(); GwInfo g; gw_snapshot(&g);
    snprintf(ended_msg, sizeof ended_msg, "%s", f_conn == VR_CONN_STREAM ? (g.stream_state == GWS_ENDED && g.stream_reason[0] && strcmp(g.stream_reason, "stream_ended") ? g.stream_reason : "Stream ended") : "Camera turned off");
    dlog("[vcv] focused video gone: %s", ended_msg); }
  if (SDL_GetTicks() - ended_t > 2500) { set_focus(-1, NULL, NULL, M_GRID); return false; }
  return true;   /* keep showing the message for a moment */
}
static void cycle(int dir) {
  if (nvis <= 0) return; int cur = -1; for (int i = 0; i < nvis; i++) if (f_conn == vis[i].conn && !strcmp(f_uid, vis[i].uid)) cur = i;
  int n = cur < 0 ? (dir > 0 ? 0 : nvis - 1) : (cur + dir + nvis) % nvis; set_focus(vis[n].conn, vis[n].uid, vis[n].name, mode == M_GRID ? M_FOCUS : mode);
}
static void watch(const char *uid, const char *name) {
  GwInfo g; gw_snapshot(&g); char key[96];
  if (g.guild_id[0]) snprintf(key, sizeof key, "guild:%s:%s:%s", g.guild_id, g.channel_id, uid); else snprintf(key, sizeof key, "call:%s:%s", g.channel_id, uid);
  for (int i = 0; i < g.nmembers; i++) if (!strcmp(g.members[i].user_id, uid) && g.members[i].username[0]) name = g.members[i].username;
  gw_watch_stream(key); set_focus(VR_CONN_STREAM, uid, name, M_FOCUS);
}
static void stop_watching(void) { gw_watch_stream(NULL); if (f_conn == VR_CONN_STREAM) set_focus(-1, NULL, NULL, M_GRID); }

void vcv_begin(void) { vr_tick(); nbox = 0; nvis = 0; drawn_this_frame = true; }
bool vcv_tile(const char *uid, const char *name, bool live, int x, int y, int w, int h) {
  int conn = source_for(uid, false); bool drew = false;
  if (conn >= 0) {
    int tw = 0, th = 0; SDL_Texture *t = vr_texture(conn, uid, &tw, &th); add_vis(conn, uid, name);
    if (t) { nx_rect(x, y, w, h, RGB(0, 0, 0)); fit(t, tw, th, x, y, w, h); nx_rect(x, y + h - 44, w, 44, RGBA(0, 0, 0, 150)); drew = true; }
    const char *st = vr_status(conn, uid);
    if (!t || st[0]) nx_text(x + 8, y + 30, st[0] ? st : conn == VR_CONN_STREAM ? "starting stream..." : "starting video...", 0, RGB(240, 178, 50), w - 16);
    if (conn == VR_CONN_TEST) nx_text(x + 8, y + 8, "self-test", 0 | NX_BOLD, RGB(240, 178, 50), 0);
    if (conn == VR_CONN_SELF) nx_text(x + 8, y + 8, "your camera", 0 | NX_BOLD, RGB(35, 165, 90), 0);
    if (conn != VR_CONN_SELF && conn != VR_CONN_TEST) { chip(x + w - 46, y + h - 34, 38, 26, RGB(64, 66, 73), "..."); add_box(K_MENU, conn, uid, x, y + h - 44, w, 44); }   /* name strip / "...": volume & options */
    add_box(K_FOCUS, conn, uid, x, y, w, h);
    if (cursor >= 0 && cursor < nvis && vis[cursor].conn == conn && !strcmp(vis[cursor].uid, uid) && SDL_GetTicks() - cursor_t < 6000) {
      NxColor c = RGB(88, 101, 242); nx_rect(x, y, w, 4, c); nx_rect(x, y + h - 4, w, 4, c); nx_rect(x, y, 4, h, c); nx_rect(x + w - 4, y, 4, h, c); nx_text(x + 8, y + h - 66, "A: open", 0 | NX_BOLD, RGB(255, 255, 255), 0); }
  }
  if (live) {
    if (vr_active(VR_CONN_STREAM, uid) && conn != VR_CONN_STREAM) add_vis(VR_CONN_STREAM, uid, name);   /* camera in the tile, stream reachable by cycling */
    if (watching(uid)) { chip(x + w - 70, y + 34, 62, 26, RGB(64, 66, 73), "Stop"); add_box(K_STOP, VR_CONN_STREAM, uid, x + w - 70, y + 34, 62, 26); }
    else { chip(x + w - 78, y + 34, 70, 26, RGB(88, 101, 242), "Watch"); add_box(K_WATCH, VR_CONN_STREAM, uid, x + w - 78, y + 34, 70, 26); }
  }
  return drew;
}
static void draw_video(int x, int y, int w, int h, bool full) {
  int tw = 0, th = 0; SDL_Texture *t = ended_t ? NULL : vr_texture(f_conn, f_uid, &tw, &th);
  if (t) fit(t, tw, th, x, y, w, h);
  else { char gl[120] = ""; const char *msg;
    if (ended_t) msg = ended_msg;
    else { const char *st = vr_status(f_conn, f_uid); GwInfo g; gw_snapshot(&g);
      if (f_conn == VR_CONN_STREAM) { int s = golive_state(gl, sizeof gl); if (!gl[0] || s != GL_ERROR) snprintf(gl, sizeof gl, "%s", s == GL_CONNECTED ? "connected, waiting for video..." : g.stream_state == GWS_REQUESTED ? "asking Discord for the stream..." : g.stream_state == GWS_ENDED ? g.stream_reason : "connecting to the stream server..."); }
      msg = st[0] ? st : gl[0] ? gl : "waiting for video..."; }
    int tw2 = nx_text_w(msg, full ? 2 : 1); nx_text(x + (w - tw2) / 2, y + h / 2 - 16, msg, (full ? 2 : 1) | NX_BOLD, ended_t ? RGB(237, 66, 69) : RGB(240, 178, 50), w - 40); }
}
bool vcv_full(int x, int y, int w, int h) {
  fx = x; fy = y; fw = w; fh = h; stop_box[2] = 0; menu_box[2] = 0;
  if (mode == M_GRID || !focus_alive()) return false;
  if (mode == M_FULL) return true;                    /* drawn by vcv_fullscreen() over everything */
  nx_rrect(x, y, w, h, 12, RGB(0, 0, 0)); draw_video(x + 6, y + 6, w - 12, h - 46, false);
  char b[160]; VrStats vs; vr_stats(f_conn, f_uid, &vs);
  snprintf(b, sizeof b, "%s  -  %s   %dx%d", f_name, f_conn == VR_CONN_STREAM ? "LIVE" : f_conn == VR_CONN_TEST ? "self-test" : "cam", vs.w, vs.h);
  nx_text(x + 16, y + h - 34, b, 0 | NX_BOLD, RGB(219, 222, 225), w - 560);
  if (f_conn == VR_CONN_STREAM && !ended_t) { chip(x + w - 560, y + h - 38, 120, 30, RGB(64, 66, 73), "Stop watching"); stop_box[0] = x + w - 560; stop_box[1] = y + h - 38; stop_box[2] = 120; stop_box[3] = 30; }
  if (f_conn != VR_CONN_SELF && f_conn != VR_CONN_TEST) { int mx2 = f_conn == VR_CONN_STREAM && !ended_t ? x + w - 690 : x + w - 560; chip(mx2, y + h - 38, 120, 30, RGB(88, 101, 242), "- Options"); menu_box[0] = mx2; menu_box[1] = y + h - 38; menu_box[2] = 120; menu_box[3] = 30; }
  nx_text(x + w - 420, y + h - 34, "A/tap: full screen   B: back   <- ->: next", 0, RGB(148, 155, 164), 0);
  return true;
}
void vcv_fullscreen(void) {
  bool drawn = drawn_this_frame; drawn_this_frame = false;
  if (!drawn || mode != M_FULL || !focus_alive()) return;
  nx_rect(0, 0, NX_W, NX_H, RGB(0, 0, 0)); draw_video(0, 0, NX_W, NX_H, true);
  uint32_t age = SDL_GetTicks() - osd_t; stop_box[2] = 0; menu_box[2] = 0;
  if (age < 3500) {                                   /* overlay, fades out */
    int a = age < 2800 ? 255 : (int)(255 * (3500 - age) / 700); NxColor bg = RGBA(0, 0, 0, a * 150 / 255), fg = RGBA(255, 255, 255, a), mu = RGBA(180, 185, 192, a);
    nx_rect(0, 0, NX_W, 64, bg); nx_text(24, 18, f_name, 1 | NX_BOLD, fg, 600);
    int nw = nx_text_w(f_name, 1 | NX_BOLD); bool live = f_conn == VR_CONN_STREAM; nx_rrect(36 + (nw > 600 ? 600 : nw), 18, live ? 54 : 46, 26, 6, live ? RGBA(237, 66, 69, a) : RGBA(35, 165, 90, a));
    nx_text(44 + (nw > 600 ? 600 : nw), 21, live ? "LIVE" : f_conn == VR_CONN_TEST ? "test" : "cam", 0 | NX_BOLD, fg, 0);
    nx_rect(0, NX_H - 52, NX_W, 52, bg); nx_text(24, NX_H - 38, live ? "B: back   <- ->: next video   ZR: stop watching   tap: show controls" : "B: back   <- ->: next video   tap: show controls", 0, mu, 0);
    if (f_conn != VR_CONN_SELF && f_conn != VR_CONN_TEST) { chip(NX_W - 190, 15, 170, 34, RGBA(88, 101, 242, a), "- Volume / options"); menu_box[0] = NX_W - 190; menu_box[1] = 15; menu_box[2] = 170; menu_box[3] = 34; }
    if (live && !ended_t) { chip(NX_W - 170, NX_H - 44, 150, 34, RGBA(64, 66, 73, a), "Stop watching"); stop_box[0] = NX_W - 170; stop_box[1] = NX_H - 44; stop_box[2] = 150; stop_box[3] = 34; }
  }
}
static bool in_rect(const NxInput *in, int x, int y, int w, int h) { return in->tx >= x && in->tx < x + w && in->ty >= y && in->ty < y + h; }
bool vcv_input(const NxInput *in, bool buttons) {
  if (mode != M_GRID && focus_alive() && f_conn != VR_CONN_SELF && f_conn != VR_CONN_TEST) {   /* focused / full: minus or the Options chip -> this person's menu */
    if ((buttons || mode == M_FULL) && in->pressed[B_MINUS]) { snprintf(menu_req, sizeof menu_req, "%s", f_uid); return true; }
    if (in->touch_pressed && menu_box[2] && in_rect(in, menu_box[0], menu_box[1], menu_box[2], menu_box[3])) { snprintf(menu_req, sizeof menu_req, "%s", f_uid); return true; } }
  if (mode == M_FULL && !focus_alive()) {}
  if (mode == M_FULL) {                                /* the whole screen is ours: every key we use wins */
    if (in->pressed[B_B]) { mode = M_FOCUS; dlog("[vcv] full screen -> focus"); return true; }
    if (in->pressed[B_LEFT] || in->pressed[B_RIGHT]) { cycle(in->pressed[B_RIGHT] ? 1 : -1); return true; }
    if (in->pressed[B_ZR] && f_conn == VR_CONN_STREAM) { stop_watching(); return true; }
    if (in->touch_pressed) { if (stop_box[2] && in_rect(in, stop_box[0], stop_box[1], stop_box[2], stop_box[3])) { stop_watching(); return true; } osd_t = SDL_GetTicks(); return true; }
    if (in->pressed[B_A]) { osd_t = SDL_GetTicks(); return true; }
    return false;
  }
  if (mode == M_FOCUS) {
    if (buttons && in->pressed[B_B]) { set_focus(-1, NULL, NULL, M_GRID); return true; }
    if (buttons && in->pressed[B_A]) { mode = M_FULL; osd_t = SDL_GetTicks(); dlog("[vcv] focus -> full screen"); return true; }
    if (buttons && (in->pressed[B_LEFT] || in->pressed[B_RIGHT])) { cycle(in->pressed[B_LEFT] ? -1 : 1); return true; }
    if (buttons && in->pressed[B_ZR] && f_conn == VR_CONN_STREAM) { stop_watching(); return true; }
    if (in->touch_pressed && in_rect(in, fx, fy, fw, fh)) {
      if (stop_box[2] && in_rect(in, stop_box[0], stop_box[1], stop_box[2], stop_box[3])) { stop_watching(); return true; }
      mode = M_FULL; osd_t = SDL_GetTicks(); dlog("[vcv] focus -> full screen (tap)"); return true; }
    return false;
  }
  /* grid */
  if (buttons && nvis > 0 && (in->pressed[B_UP] || in->pressed[B_DOWN])) {   /* tile cursor over the videos */
    if (cursor < 0 || SDL_GetTicks() - cursor_t >= 6000) cursor = in->pressed[B_DOWN] ? 0 : nvis - 1; else cursor = (cursor + (in->pressed[B_DOWN] ? 1 : -1) + nvis) % nvis;
    cursor_t = SDL_GetTicks(); return true; }
  if (buttons && in->pressed[B_A] && nvis > 0 && cursor >= 0 && cursor < nvis && SDL_GetTicks() - cursor_t < 6000) { int c = cursor; cursor = -1;   /* A only while the tile cursor is shown (else A = people list) */ set_focus(vis[c].conn, vis[c].uid, vis[c].name, M_FOCUS); return true; }
  if (in->touch_pressed) {
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nbox; i++) { Box *b = &box[i];   /* chips first, then whole tiles */
        if ((pass == 0) != (b->kind != K_FOCUS)) continue;
        if (!in_rect(in, b->x, b->y, b->w, b->h)) continue;
        const char *nm = b->uid; for (int k = 0; k < nvis; k++) if (!strcmp(vis[k].uid, b->uid)) nm = vis[k].name;
        if (b->kind == K_WATCH) { watch(b->uid, nm); return true; }
        if (b->kind == K_STOP) { stop_watching(); return true; }
        if (b->kind == K_MENU) { snprintf(menu_req, sizeof menu_req, "%s", b->uid); return true; }
        set_focus(b->conn, b->uid, nm, M_FOCUS); return true; }
  }
  return false;
}
bool vcv_fullscreen_active(void) { return mode == M_FULL && f_conn >= 0; }
