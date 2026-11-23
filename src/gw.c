#include "gw.h"
#include "ws.h"
#include "dlog.h"
#include "golive.h"
#include "vrecv.h"
#include "dc.h"
#include "voice.h"
#include <SDL2/SDL.h>
#include <jansson.h>
#include <coreinit/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <time.h>

#ifndef GW_CAPS
#define GW_CAPS (1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 1024 | 2048 | 4096 | 8192)   /* not 256 (token refresh) or 512 (settings proto) */
#endif
static char gids[300][24]; static volatile int ngids = -1;
void gw_set_guild_ids(const char *const *ids, int n) { if (n > 300) n = 300; for (int i = 0; i < n; i++) snprintf(gids[i], 24, "%s", ids[i]); ngids = n; }
static GwInfo I; static SDL_mutex *mx; static SDL_Thread *th; static volatile bool stop_flag;
static char token[420], my_id[24];
static char req_guild[24], req_chan[24]; static bool req_mute, req_deaf; static volatile int req_pending;   /* 1 join, 2 leave */
static char cur_guild[24], cur_chan[24];
static volatile bool self_video; static volatile bool req_video;
/* Go Live watching (gw thread) */
static SDL_mutex *smx; static char st_req[96]; static volatile int st_req_kind;   /* 1 watch, 2 stop */
static char st_key[96], st_srv[24], st_chan[24], st_tok[160], st_ep[160]; static bool st_have_create, st_have_server, st_started;
static void stream_state(int s, const char *reason) { SDL_LockMutex(mx); I.stream_state = s; if (reason) snprintf(I.stream_reason, sizeof I.stream_reason, "%s", reason); snprintf(I.stream_key, sizeof I.stream_key, "%s", st_key); I.gen++; SDL_UnlockMutex(mx); }
static void stream_try_start(void) {
  if (!st_have_create || !st_have_server || st_started || !st_ep[0]) return; char sess[80]; SDL_LockMutex(mx); snprintf(sess, sizeof sess, "%s", I.session_id); SDL_UnlockMutex(mx);
  golive_start(st_key, st_srv, st_chan, my_id, sess, st_tok, st_ep); st_started = true; stream_state(GWS_CONNECTING, "");
}
static void stream_stop_local(int newstate, const char *reason) { if (st_started) golive_stop(); st_started = st_have_create = st_have_server = false; stream_state(newstate, reason); }
static void stream_event(Ws *w, const char *t, json_t *d) {
  const char *k = json_string_value(json_object_get(d, "stream_key")); char *dump = json_dumps(d, JSON_COMPACT);
  if (dump) { char *tk = strstr(dump, "\"token\":\""); if (tk) { tk += 9; while (*tk && *tk != '"') *tk++ = '*'; } dlog("[gw] %s %.400s", t, dump); free(dump); }   /* token masked */
  (void)w; if (!k || !st_key[0] || strcmp(k, st_key)) return;
  if (!strcmp(t, "STREAM_CREATE")) { snprintf(st_srv, sizeof st_srv, "%s", json_string_value(json_object_get(d, "rtc_server_id")) ? json_string_value(json_object_get(d, "rtc_server_id")) : ""); snprintf(st_chan, sizeof st_chan, "%s", json_string_value(json_object_get(d, "rtc_channel_id")) ? json_string_value(json_object_get(d, "rtc_channel_id")) : "");
    st_have_create = st_srv[0] != 0; stream_state(GWS_CREATED, json_is_true(json_object_get(d, "paused")) ? "paused" : ""); stream_try_start(); }
  else if (!strcmp(t, "STREAM_SERVER_UPDATE")) { const char *ep = json_string_value(json_object_get(d, "endpoint")), *tk = json_string_value(json_object_get(d, "token"));
    if (!ep) { if (st_started) { golive_stop(); st_started = false; } st_have_server = false; stream_state(GWS_CREATED, "stream server moving..."); return; }
    if (st_started && strcmp(ep, st_ep)) { golive_stop(); st_started = false; }
    snprintf(st_ep, sizeof st_ep, "%s", ep); snprintf(st_tok, sizeof st_tok, "%s", tk ? tk : ""); st_have_server = true; stream_try_start(); }
  else if (!strcmp(t, "STREAM_UPDATE")) { stream_state(I.stream_state, json_is_true(json_object_get(d, "paused")) ? "paused" : ""); }
  else if (!strcmp(t, "STREAM_DELETE")) { const char *r = json_string_value(json_object_get(d, "reason")); stream_stop_local(GWS_ENDED, r ? r : "stream ended"); st_key[0] = 0; }
}
static void stream_requests(Ws *w) {      /* gw thread, idle loop */
  if (!st_req_kind || !smx) return; SDL_LockMutex(smx); int kind = st_req_kind; char key[96]; snprintf(key, sizeof key, "%s", st_req); st_req_kind = 0; SDL_UnlockMutex(smx);
  if (st_key[0]) { char b[160]; snprintf(b, sizeof b, "{\"op\":19,\"d\":{\"stream_key\":\"%s\"}}", st_key); ws_send_text(w, b, strlen(b)); dlog("[gw] stop watching %s", st_key); stream_stop_local(GWS_NONE, ""); st_key[0] = 0; stream_state(GWS_NONE, ""); }
  if (kind == 1 && key[0] && !strchr(key, '"') && !strchr(key, '\\')) { snprintf(st_key, sizeof st_key, "%s", key); char b[160]; snprintf(b, sizeof b, "{\"op\":20,\"d\":{\"stream_key\":\"%s\"}}", st_key); ws_send_text(w, b, strlen(b)); dlog("[gw] watch stream %s", st_key); stream_state(GWS_REQUESTED, ""); }
}
void gw_watch_stream(const char *key) { if (!smx) smx = SDL_CreateMutex(); SDL_LockMutex(smx); snprintf(st_req, sizeof st_req, "%s", key ? key : ""); st_req_kind = key && key[0] ? 1 : 2; SDL_UnlockMutex(smx); }
void gw_set_self_video(bool on) { if (self_video != on) { self_video = on; req_video = true; } }
static char watch[24]; static SDL_mutex *evmx; typedef struct Ev { struct Ev *next; int kind; char *json; } Ev; static Ev *ev_h, *ev_t; static int ev_n;
void gw_watch_channel(const char *id) { if (!evmx) evmx = SDL_CreateMutex(); SDL_LockMutex(evmx); snprintf(watch, sizeof watch, "%s", id ? id : ""); SDL_UnlockMutex(evmx); }
static void ev_push(int kind, json_t *d) {
  if (!evmx) return; char *s = json_dumps(d, JSON_COMPACT); if (!s) return; Ev *e = calloc(1, sizeof *e); if (!e) { free(s); return; } e->kind = kind; e->json = s;
  SDL_LockMutex(evmx); if (ev_n > 200) { Ev *o = ev_h; ev_h = o->next; if (!ev_h) ev_t = NULL; free(o->json); free(o); ev_n--; }
  if (ev_t) ev_t->next = e; else ev_h = e; ev_t = e; ev_n++; SDL_UnlockMutex(evmx);
}
bool gw_pop_event(int *kind, char **json) {
  if (!evmx) return false; SDL_LockMutex(evmx); Ev *e = ev_h; if (e) { ev_h = e->next; if (!ev_h) ev_t = NULL; ev_n--; } SDL_UnlockMutex(evmx);
  if (!e) return false; *kind = e->kind; *json = e->json; free(e); return true;
}
bool gw_ready(void) { bool r; if (!mx) return false; SDL_LockMutex(mx); r = I.state == GW_READY; SDL_UnlockMutex(mx); return r; }

static const char *js(json_t *o, const char *k) { json_t *v = json_object_get(o, k); return json_is_string(v) ? json_string_value(v) : ""; }
#define LOCK() SDL_LockMutex(mx)
#define UNLOCK() SDL_UnlockMutex(mx)
static void set_state(int s, const char *err) { LOCK(); I.state = s; if (err) snprintf(I.err, sizeof I.err, "%s", err); I.gen++; UNLOCK(); }

static void send_json(Ws *w, json_t *o) { char *s = json_dumps(o, JSON_COMPACT); if (s) { ws_send_text(w, s, strlen(s)); free(s); } json_decref(o); }
static void send_voice_state(Ws *w, const char *g, const char *c, bool mute, bool deaf) {
  json_t *d = json_object(); json_object_set_new(d, "guild_id", g[0] ? json_string(g) : json_null()); json_object_set_new(d, "channel_id", c[0] ? json_string(c) : json_null());
  json_object_set_new(d, "self_mute", json_boolean(mute)); json_object_set_new(d, "self_deaf", json_boolean(deaf)); json_object_set_new(d, "self_video", json_boolean(self_video));
  json_t *o = json_object(); json_object_set_new(o, "op", json_integer(4)); json_object_set_new(o, "d", d); send_json(w, o);
  dlog("[gw] voice state -> guild=%s chan=%s", g[0] ? g : "-", c[0] ? c : "-");
}

/* ---- voice states of everyone (sidebar) ---- */
#define MAX_VS 3000
static GwVS VS[MAX_VS]; static int nvs; static unsigned vs_gen;
static void vs_set(json_t *d, const char *gid_default) {   /* caller holds mx */
  const char *uid = js(d, "user_id"), *chan = js(d, "channel_id"), *gid = js(d, "guild_id"); if (!gid[0]) gid = gid_default ? gid_default : ""; if (!uid[0]) return;
  int at = -1; for (int i = 0; i < nvs; i++) if (!strcmp(VS[i].user, uid)) at = i;
  if (!chan[0]) { if (at >= 0) { VS[at] = VS[--nvs]; vs_gen++; } return; }
  if (at < 0) { if (nvs >= MAX_VS) return; at = nvs++; memset(&VS[at], 0, sizeof VS[at]); snprintf(VS[at].user, 24, "%s", uid); }
  GwVS *v = &VS[at]; snprintf(v->chan, 24, "%s", chan); snprintf(v->guild, 24, "%s", gid);
  json_t *mem = json_object_get(d, "member"); json_t *user = mem ? json_object_get(mem, "user") : NULL;
  const char *nm = mem ? js(mem, "nick") : ""; if (!nm[0] && user) nm = js(user, "global_name"); if (!nm[0] && user) nm = js(user, "username"); if (nm[0]) snprintf(v->name, sizeof v->name, "%s", nm);
  const char *av = user ? js(user, "avatar") : ""; if (av[0]) snprintf(v->avatar, sizeof v->avatar, "%s", av);
  v->mute = json_is_true(json_object_get(d, "self_mute")) || json_is_true(json_object_get(d, "mute")); v->deaf = json_is_true(json_object_get(d, "self_deaf")) || json_is_true(json_object_get(d, "deaf"));
  v->video = json_is_true(json_object_get(d, "self_video")); v->stream = json_is_true(json_object_get(d, "self_stream")); vs_gen++;
}
static void vs_array(json_t *arr, const char *gid) { size_t i; json_t *e; LOCK(); json_array_foreach(arr, i, e) if (json_is_object(e)) vs_set(e, gid); UNLOCK(); }
int gw_voice_table(GwVS *out, int max) { if (!mx) return 0; LOCK(); int n = nvs < max ? nvs : max; memcpy(out, VS, n * sizeof *out); UNLOCK(); return n; }
unsigned gw_voice_gen(void) { return vs_gen; }
void gw_fill_member(const char *uid, const char *name, const char *avatar) {
  if (!mx || !uid || !uid[0]) return; LOCK();
  for (int i = 0; i < I.nmembers; i++) if (!strcmp(I.members[i].user_id, uid)) { if (!I.members[i].username[0] && name && name[0]) snprintf(I.members[i].username, sizeof I.members[i].username, "%s", name); if (!I.members[i].avatar[0] && avatar && avatar[0]) snprintf(I.members[i].avatar, sizeof I.members[i].avatar, "%s", avatar); }
  for (int i = 0; i < nvs; i++) if (!strcmp(VS[i].user, uid)) { if (!VS[i].name[0] && name && name[0]) { snprintf(VS[i].name, sizeof VS[i].name, "%s", name); vs_gen++; } if (!VS[i].avatar[0] && avatar && avatar[0]) snprintf(VS[i].avatar, sizeof VS[i].avatar, "%s", avatar); }
  UNLOCK();
}
/* READY is megabytes and gets skipped: stream-scan it for every "voice_states":[...] array (guild voice states have no guild_id; the channel id is enough) */
static struct { int pi, depth; bool on, instr, esc; char *buf; size_t n, cap; int arrays, states; } SC;
static const char SC_PAT[] = "\"voice_states\":[";
/* ---- session resume: keep calls alive across a gateway drop (a fresh login ends the voice session with 4006) ---- */
static char g_sess[100], g_resume[220]; static long long g_seq = -1; static volatile bool g_rejoin; static bool rs_on;
static struct { int depth; bool instr, esc, expect_key, inkey, inval; int want; char key[32]; int kn; char val[220]; int vn; } RS;
static void rs_feed(const unsigned char *p, size_t n) {   /* READY streamed past us: pick d.session_id and d.resume_gateway_url */
  if (!p) { memset(&RS, 0, sizeof RS); return; }
  for (size_t i = 0; i < n; i++) { char c = (char)p[i];
    if (RS.instr) {
      if (RS.esc) { RS.esc = false; if (RS.inval && RS.vn < 218) RS.val[RS.vn++] = c; continue; }
      if (c == '\\') { RS.esc = true; continue; }
      if (c == '"') { RS.instr = false;
        if (RS.inkey) { RS.inkey = false; RS.key[RS.kn] = 0; RS.want = !strcmp(RS.key, "session_id") ? 1 : !strcmp(RS.key, "resume_gateway_url") ? 2 : 0; }
        else if (RS.inval) { RS.inval = false; RS.val[RS.vn] = 0; if (RS.want == 1) snprintf(g_sess, sizeof g_sess, "%s", RS.val); else if (RS.want == 2) snprintf(g_resume, sizeof g_resume, "%s", RS.val); RS.want = 0; }
        continue; }
      if (RS.inkey && RS.kn < 30) RS.key[RS.kn++] = c; else if (RS.inval && RS.vn < 218) RS.val[RS.vn++] = c; continue; }
    if (c == '"') { RS.instr = true; if (RS.depth == 2 && RS.expect_key) { RS.inkey = true; RS.kn = 0; } else if (RS.depth == 2 && RS.want) { RS.inval = true; RS.vn = 0; } }
    else if (c == '{' || c == '[') { if (RS.depth == 2) RS.want = 0; RS.depth++; if (RS.depth == 2 && c == '{') RS.expect_key = true; }
    else if (c == '}' || c == ']') RS.depth--;
    else if (RS.depth == 2) { if (c == ',') { RS.expect_key = true; RS.want = 0; } else if (c == ':') RS.expect_key = false; } }
}
static void scan_cb(void *ud, const unsigned char *p, size_t n) {
  (void)ud; if (rs_on) { rs_feed(p, n); if (!p) rs_on = false; }
  if (!p) { if (SC.arrays || SC.states) dlog("[gw] READY scan: %d voice_states arrays, %d people in voice", SC.arrays, SC.states); SC.pi = 0; SC.on = false; SC.n = 0; SC.arrays = SC.states = 0; return; }
  for (size_t i = 0; i < n; i++) { char ch = (char)p[i];
    if (!SC.on) { if (ch == SC_PAT[SC.pi]) { if (!SC_PAT[++SC.pi]) { SC.on = true; SC.pi = 0; SC.depth = 1; SC.instr = SC.esc = false; SC.n = 0; if (!SC.buf) { SC.cap = 512 * 1024; SC.buf = malloc(SC.cap); } if (SC.buf) SC.buf[SC.n++] = '['; } } else SC.pi = ch == SC_PAT[0] ? 1 : 0; continue; }
    if (SC.buf && SC.n + 1 < SC.cap) SC.buf[SC.n++] = ch; else SC.n = SC.cap;   /* too big: dropped */
    if (SC.instr) { if (SC.esc) SC.esc = false; else if (ch == '\\') SC.esc = true; else if (ch == '"') SC.instr = false; continue; }
    if (ch == '"') SC.instr = true; else if (ch == '[' || ch == '{') SC.depth++; else if (ch == ']' || ch == '}') { if (--SC.depth == 0) { SC.on = false; SC.arrays++;
      if (SC.buf && SC.n < SC.cap) { SC.buf[SC.n] = 0; json_t *a = json_loads(SC.buf, 0, NULL); if (json_is_array(a)) { SC.states += (int)json_array_size(a); vs_array(a, NULL); } json_decref(a); } } }
  }
}
static void seed_members(void) {        /* mx held: people already in the channel we are joining */
  I.nmembers = 0; for (int i = 0; i < nvs && I.nmembers < GW_MAX_MEMBERS; i++) { GwVS *v = &VS[i]; if (strcmp(v->chan, req_chan)) continue; GwVoiceMember *m = &I.members[I.nmembers++]; memset(m, 0, sizeof *m);
    snprintf(m->user_id, 24, "%s", v->user); snprintf(m->username, sizeof m->username, "%s", v->name); snprintf(m->avatar, sizeof m->avatar, "%s", v->avatar); snprintf(m->channel_id, 24, "%s", v->chan);
    m->mute = v->mute; m->deaf = v->deaf; m->video = v->video; m->stream = v->stream; m->self = !strcmp(v->user, my_id); }
}
/* ---- zlib-stream transport: READY is ~16 MB of JSON; compressed it is a fraction (less to download and decrypt) ---- */
static z_stream zs; static bool zs_on; static unsigned char ztail[4]; static size_t ztn;
static char *acc; static const size_t ACC_CAP = 240 * 1024; static size_t acc_n; static bool acc_big, big_done; static size_t zout_total;
static unsigned char zout[32768];
static void scan_cb(void *ud, const unsigned char *p, size_t n);
static void z_reset(void) { if (zs_on) inflateEnd(&zs); memset(&zs, 0, sizeof zs); zs_on = inflateInit(&zs) == Z_OK; acc_n = 0; acc_big = big_done = false; ztn = 0; zout_total = 0; if (!acc) acc = malloc(ACC_CAP + 1); }
static void z_out(const unsigned char *p, size_t n) {
  zout_total += n;
  if (!acc_big && acc && acc_n + n <= ACC_CAP) { memcpy(acc + acc_n, p, n); acc_n += n; return; }
  if (!acc_big) { acc_big = true; if (acc_n) scan_cb(NULL, (const unsigned char *)acc, acc_n); acc_n = 0; }   /* too big to keep: stream it through the READY scanner */
  scan_cb(NULL, p, n);
}
/* feed compressed bytes; returns 1 when a whole (small) message is in acc, NUL-terminated */
static int z_feed(const unsigned char *p, size_t n, bool frame_end) {
  if (!zs_on) return 0;
  if (n >= 4) { memcpy(ztail, p + n - 4, 4); ztn = 4; } else for (size_t i = 0; i < n; i++) { memmove(ztail, ztail + 1, 3); ztail[3] = p[i]; if (ztn < 4) ztn++; }
  if (n) { zs.next_in = (unsigned char *)p; zs.avail_in = (uInt)n;
    for (;;) { zs.next_out = zout; zs.avail_out = sizeof zout; int r = inflate(&zs, Z_SYNC_FLUSH); size_t got = sizeof zout - zs.avail_out; if (got) z_out(zout, got);
      if (r != Z_OK && r != Z_BUF_ERROR) { dlog("[gw] inflate error %d", r); return 0; }
      if (zs.avail_in == 0 && zs.avail_out != 0) break; if (r == Z_BUF_ERROR && !got) break; } }
  if (!frame_end || ztn < 4 || ztail[0] || ztail[1] || ztail[2] != 0xFF || ztail[3] != 0xFF) return 0;
  if (acc_big) { scan_cb(NULL, NULL, 0); acc_big = false; acc_n = 0; big_done = true; return 0; }
  if (!acc) return 0; acc[acc_n] = 0; return 1;
}
static void zskip_cb(void *ud, const unsigned char *p, size_t n) { (void)ud; if (p) z_feed(p, n, false); else z_feed(NULL, 0, true); }
static void member_update(json_t *d) {
  const char *uid = js(d, "user_id"), *chan = js(d, "channel_id"), *gid = js(d, "guild_id");
  json_t *mem = json_object_get(d, "member"); json_t *user = mem ? json_object_get(mem, "user") : NULL; const char *name = mem ? js(mem, "nick") : ""; if (!name[0] && user) name = js(user, "global_name"); if (!name[0] && user) name = js(user, "username");
  LOCK(); vs_set(d, NULL);
  bool ours = !strcmp(uid, my_id);
  if (ours) { if (chan[0] && !strcmp(chan, cur_chan)) { snprintf(I.session_id, sizeof I.session_id, "%s", js(d, "session_id")); snprintf(I.channel_id, sizeof I.channel_id, "%s", chan); snprintf(I.guild_id, sizeof I.guild_id, "%s", gid); I.have_state = true; } else if (!chan[0]) { I.have_state = false; I.nmembers = 0; } }
  /* keep the member list for the channel we are in */
  if (cur_chan[0] && (!cur_guild[0] || !strcmp(gid, cur_guild))) {
    int found = -1; for (int i = 0; i < I.nmembers; i++) if (!strcmp(I.members[i].user_id, uid)) found = i;
    bool in_chan = chan[0] && !strcmp(chan, cur_chan);
    if (in_chan) { if (found < 0 && I.nmembers < GW_MAX_MEMBERS) { found = I.nmembers++; memset(&I.members[found], 0, sizeof I.members[found]); snprintf(I.members[found].user_id, 24, "%s", uid); }
      if (found >= 0) { GwVoiceMember *m = &I.members[found]; if (name[0]) snprintf(m->username, sizeof m->username, "%s", name); { const char *av = user ? js(user, "avatar") : ""; if (av[0]) snprintf(m->avatar, sizeof m->avatar, "%s", av); } m->video = json_is_true(json_object_get(d, "self_video")); m->stream = json_is_true(json_object_get(d, "self_stream")); m->mute = json_is_true(json_object_get(d, "self_mute")) || json_is_true(json_object_get(d, "mute")); m->deaf = json_is_true(json_object_get(d, "self_deaf")) || json_is_true(json_object_get(d, "deaf")); m->self = ours; } }
    else if (found >= 0) { memmove(&I.members[found], &I.members[found + 1], (I.nmembers - found - 1) * sizeof I.members[0]); I.nmembers--; }
  }
  I.gen++; UNLOCK();
}

static int worker(void *arg) {
  (void)arg; OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0); OSSetThreadPriority(OSGetCurrentThread(), 18);
  static char msg[262144];
  while (!stop_flag) {
    Ws w; set_state(GW_CONNECTING, "");
    bool resuming = g_sess[0] && g_resume[0] && g_seq >= 0; char gurl[300];
    if (resuming) snprintf(gurl, sizeof gurl, "%s/?v=10&encoding=json&compress=zlib-stream", g_resume); else snprintf(gurl, sizeof gurl, "wss://gateway.discord.gg/?v=10&encoding=json&compress=zlib-stream");
    if (!ws_open(&w, gurl, "https://discord.com", 15000)) { if (resuming) { g_sess[0] = 0; dlog("[gw] resume server unreachable: will log in fresh"); } dlog("[gw] %s", w.err); set_state(GW_ERROR, w.err); for (int i = 0; i < 500 && !stop_flag; i++) SDL_Delay(10); continue; }
    dlog("[gw] connected (zlib-stream)%s", resuming ? ", resuming the session" : ""); z_reset(); w.skip_cb = zskip_cb; rs_on = !resuming; if (!resuming) { LOCK(); nvs = 0; vs_gen++; UNLOCK(); }
    uint32_t hb_iv = 41250, last_hb = SDL_GetTicks(); long long seq = resuming ? g_seq : -1; bool identified = false, hello = false; int backoff = 0; uint32_t id_t0 = 0, prog_t = 0; int pre_ready = 0;
    while (!stop_flag) {
      bool zbin = false; size_t zlen = 0; int r = ws_recv2(&w, msg, sizeof msg, &zbin, &zlen);
      if (r == 1 && zbin) { if (z_feed((const unsigned char *)msg, zlen, true) == 1 && acc_n < sizeof msg) { memcpy(msg, acc, acc_n + 1); acc_n = 0; } else continue; }
      if (r < 0) { dlog("[gw] closed: %s", w.err); if (strstr(w.err, "close 4007") || strstr(w.err, "close 4009") || strstr(w.err, "close 1000")) { g_sess[0] = 0; g_seq = -1; } if (strstr(w.err, "close 4004") || strstr(w.err, "close 4013") || strstr(w.err, "close 4014")) { set_state(GW_ERROR, "Discord rejected this login for the gateway (voice needs a user account token)"); ws_close(&w); goto done; } break; }
      if (r == 1) {
        json_t *j = json_loads(msg, 0, NULL); if (!j) continue;
        int op = (int)json_integer_value(json_object_get(j, "op")); json_t *d = json_object_get(j, "d"); json_t *s = json_object_get(j, "s"); if (json_is_integer(s)) seq = g_seq = json_integer_value(s);
        const char *t = js(j, "t");
        if (op == 10) { hello = true; json_t *hi = d ? json_object_get(d, "heartbeat_interval") : NULL; if (json_is_number(hi)) hb_iv = (uint32_t)json_number_value(hi); last_hb = SDL_GetTicks();
          json_t *props = json_object(); json_object_set_new(props, "os", json_string("Windows")); json_object_set_new(props, "browser", json_string("Chrome")); json_object_set_new(props, "device", json_string(""));
          json_t *id = json_object(); json_object_set_new(id, "token", json_string(token)); json_object_set_new(id, "properties", props); json_object_set_new(id, "compress", json_false()); json_object_set_new(id, "capabilities", json_integer(GW_CAPS));   /* dedupe users + prioritized READY (guild data moves to READY_SUPPLEMENTAL) + client state v2: READY went from ~17 MB to a fraction */
          json_t *pres = json_object(); json_object_set_new(pres, "status", json_string("online")); json_object_set_new(pres, "since", json_integer(0)); json_object_set_new(pres, "activities", json_array()); json_object_set_new(pres, "afk", json_false()); json_object_set_new(id, "presence", pres);
          json_t *o = json_object(); if (resuming) { json_decref(id); json_t *rd = json_object(); json_object_set_new(rd, "token", json_string(token)); json_object_set_new(rd, "session_id", json_string(g_sess)); json_object_set_new(rd, "seq", json_integer(g_seq)); json_object_set_new(o, "op", json_integer(6)); json_object_set_new(o, "d", rd); }
          else { json_object_set_new(o, "op", json_integer(2)); json_object_set_new(o, "d", id); } send_json(&w, o); dlog("[gw] hello, %s sent (hb %u, seq %lld)", resuming ? "resume" : "identify", hb_iv, g_seq); id_t0 = prog_t = SDL_GetTicks(); }
        else if (op == 11) { }
        else if (op == 1) { char b[64]; snprintf(b, sizeof b, "{\"op\":1,\"d\":%lld}", seq); ws_send_text(&w, b, strlen(b)); }
        else if (op == 7 || op == 9) { if (op == 9 && !json_is_true(d)) { g_sess[0] = 0; g_seq = -1; } dlog("[gw] op %d: reconnect%s", op, op == 9 ? " (session invalid)" : ""); json_decref(j); break; }
        else if (op == 0) {
          if (!identified && pre_ready++ < 8) dlog("[gw] before READY: %s (%u bytes)", t[0] ? t : "?", (unsigned)strlen(msg));
          if (!strcmp(t, "READY")) { if (d) { snprintf(g_sess, sizeof g_sess, "%s", js(d, "session_id")); snprintf(g_resume, sizeof g_resume, "%s", js(d, "resume_gateway_url")); } g_rejoin = false; { json_t *gs = d ? json_object_get(d, "guilds") : NULL; size_t gi; json_t *ge; json_array_foreach(gs, gi, ge) vs_array(json_object_get(ge, "voice_states"), js(ge, "id")); }
            identified = true; set_state(GW_READY, ""); dlog("[gw] READY (%u KB after %u ms)", (unsigned)(strlen(msg) / 1024), (unsigned)(SDL_GetTicks() - id_t0)); if (req_pending == 1) { snprintf(cur_guild, 24, "%s", req_guild); snprintf(cur_chan, 24, "%s", req_chan); LOCK(); I.have_state = I.have_server = false; seed_members(); I.gen++; UNLOCK(); send_voice_state(&w, req_guild, req_chan, req_mute, req_deaf); req_pending = 0; } }
          else if (!strcmp(t, "RESUMED")) { identified = true; set_state(GW_READY, ""); dlog("[gw] RESUMED after %u ms: calls keep going", (unsigned)(SDL_GetTicks() - id_t0)); if (g_rejoin) { req_pending = 0; g_rejoin = false; } }
          else if (!strcmp(t, "READY_SUPPLEMENTAL") && d) { json_t *gs = json_object_get(d, "guilds"); size_t gi; json_t *ge; int nst = 0; json_array_foreach(gs, gi, ge) { json_t *vsa = json_object_get(ge, "voice_states"); nst += (int)json_array_size(vsa); vs_array(vsa, js(ge, "id")); } dlog("[gw] READY_SUPPLEMENTAL: %d guilds, %d people in voice (%u bytes)", (int)json_array_size(gs), nst, (unsigned)strlen(msg)); }
          else if (!strcmp(t, "VOICE_STATE_UPDATE") && d) member_update(d);
          else if (!strncmp(t, "STREAM_", 7) && d) stream_event(&w, t, d);
          else if (d && !strncmp(t, "CALL_", 5)) { int k = !strcmp(t, "CALL_CREATE") ? GWE_CALL_CREATE : !strcmp(t, "CALL_UPDATE") ? GWE_CALL_UPDATE : !strcmp(t, "CALL_DELETE") ? GWE_CALL_DELETE : 0; if (k) { ev_push(k, d); dlog("[gw] %s", t); } }
          else if (d && (!strncmp(t, "MESSAGE_", 8) || !strcmp(t, "TYPING_START"))) {
            int k = !strcmp(t, "MESSAGE_CREATE") ? GWE_CREATE : !strcmp(t, "MESSAGE_UPDATE") ? GWE_UPDATE : !strcmp(t, "MESSAGE_DELETE") ? GWE_DELETE : !strcmp(t, "TYPING_START") ? GWE_TYPING : !strncmp(t, "MESSAGE_REACTION_", 17) ? GWE_REACT : 0;
            if (k && evmx) { SDL_LockMutex(evmx); bool w = watch[0] && !strcmp(js(d, "channel_id"), watch); SDL_UnlockMutex(evmx); if (w) ev_push(k, d); else if (k == GWE_CREATE && !json_object_get(d, "guild_id")) ev_push(GWE_DM, d); } }
          else if (!strcmp(t, "VOICE_SERVER_UPDATE") && d) { LOCK(); snprintf(I.vtoken, sizeof I.vtoken, "%s", js(d, "token")); snprintf(I.endpoint, sizeof I.endpoint, "%s", js(d, "endpoint")); I.have_server = I.endpoint[0] != 0; I.gen++; UNLOCK(); dlog("[gw] voice server %s", I.endpoint); }
        }
        json_decref(j);
      } else {
        /* a READY that was too big to keep still means we are identified */
        if (!identified && hello && (big_done || (w.skip_op == 1 && !w.skip && w.skipped_total))) { identified = true; set_state(GW_READY, ""); g_rejoin = false; dlog("[gw] resume info %s (session id %u chars '%.4s...', url %s, seq %lld)", g_sess[0] && g_resume[0] ? "captured" : "missing", (unsigned)strlen(g_sess), g_sess, g_resume, g_seq); dlog("[gw] READY (large, scanned) after %u ms: %u KB over the network, %u KB of JSON", (unsigned)(SDL_GetTicks() - id_t0), (unsigned)(w.skipped_total / 1024), (unsigned)(zout_total / 1024)); }
        { static int drop_test = -1; if (drop_test < 0) { FILE *f = fopen("/vol/external01/wiiu/apps/MingusDiscord/gwdrop.txt", "rb"); drop_test = f ? 1 : 0; if (f) fclose(f); }   /* test switch: drop the gateway once, 20 s in */
          if (drop_test == 1 && identified && SDL_GetTicks() - id_t0 > 20000) { drop_test = 0; dlog("[gw] test: dropping the connection (gwdrop.txt)"); break; } }
        if (!identified && hello && id_t0) { uint32_t nw = SDL_GetTicks();
          if (nw - prog_t >= 3000) { prog_t = nw; dlog("[gw] waiting for READY: %u s, %u KB received so far%s", (unsigned)((nw - id_t0) / 1000), (unsigned)(w.skipped_total / 1024), w.skip ? " (mid-frame)" : ""); }
          static size_t last_got; static uint32_t last_prog; if (w.skipped_total != last_got || !last_prog) { last_got = w.skipped_total; last_prog = nw; }
          if (nw - last_prog > 25000) { last_prog = 0; dlog("[gw] READY stalled (nothing for 25 s): reconnecting"); set_state(GW_CONNECTING, "Discord didn't answer the login, retrying..."); break; } }
        if (identified && req_pending == 1) { snprintf(cur_guild, 24, "%s", req_guild); snprintf(cur_chan, 24, "%s", req_chan); LOCK(); I.have_state = I.have_server = false; seed_members(); I.gen++; UNLOCK(); send_voice_state(&w, req_guild, req_chan, req_mute, req_deaf); req_pending = 0; }
        else if (identified && req_pending == 2) { if (st_key[0]) { char b[160]; snprintf(b, sizeof b, "{\"op\":19,\"d\":{\"stream_key\":\"%s\"}}", st_key); ws_send_text(&w, b, strlen(b)); st_key[0] = 0; } stream_stop_local(GWS_NONE, ""); send_voice_state(&w, cur_guild, "", false, false); cur_chan[0] = 0; LOCK(); I.have_state = I.have_server = false; I.nmembers = 0; I.gen++; UNLOCK(); req_pending = 0; }
        if (identified) stream_requests(&w);
        if (identified && req_video && cur_chan[0]) { req_video = false; send_voice_state(&w, cur_guild, cur_chan, req_mute, req_deaf); }
        if (hello && SDL_GetTicks() - last_hb >= hb_iv) { char b[64]; snprintf(b, sizeof b, "{\"op\":1,\"d\":%s}", seq < 0 ? "null" : ""); if (seq >= 0) snprintf(b, sizeof b, "{\"op\":1,\"d\":%lld}", seq); ws_send_text(&w, b, strlen(b)); last_hb = SDL_GetTicks(); }
        SDL_Delay(w.skip ? 1 : 15);   /* mid-READY: keep reading */
      }
    }
    if (stop_flag) ws_close(&w); else ws_close_code(&w, 4000);   /* reconnecting: keep the session resumable */
    if (st_key[0] || st_started) { stream_stop_local(GWS_ENDED, "gateway reconnected"); st_key[0] = 0; }
    if (stop_flag) break; set_state(GW_CONNECTING, ""); backoff = backoff < 5 ? backoff + 1 : 5; for (int i = 0; i < backoff * 200 && !stop_flag; i++) SDL_Delay(10);
    if (cur_chan[0]) { snprintf(req_guild, 24, "%s", cur_guild); snprintf(req_chan, 24, "%s", cur_chan); req_pending = 1; g_rejoin = true; }   /* rejoin after a drop (cancelled if the session resumes) */
  }
  set_state(GW_OFF, ""); return 0;
  done: return 0;
}

void gw_start(const char *tok, const char *user_id) {
  if (!mx) mx = SDL_CreateMutex(); if (th) return;
  snprintf(token, sizeof token, "%s", tok); snprintf(my_id, sizeof my_id, "%s", user_id); stop_flag = false;
  th = SDL_CreateThreadWithStackSize(worker, "gw", 256 * 1024, NULL);
  vr_selftest_start(dc_dir(), my_id);   /* only runs when SD:/wiiu/apps/MingusDiscord/vtest.h264 exists */
  voice_camera_selftest_start(my_id);   /* only with SD:/wiiu/apps/MingusDiscord/vsend_selftest.txt, no call: nothing is sent */
}
void gw_stop(void) { if (!th) return; stop_flag = true; SDL_WaitThread(th, NULL); th = NULL; }
void gw_join_voice(const char *g, const char *c, bool mute, bool deaf) { snprintf(req_guild, 24, "%s", g ? g : ""); snprintf(req_chan, 24, "%s", c); req_mute = mute; req_deaf = deaf; req_pending = 1; }
void gw_leave_voice(void) { req_pending = 2; }
void gw_snapshot(GwInfo *out) { if (!mx) { memset(out, 0, sizeof *out); return; } LOCK(); *out = I; UNLOCK(); }
