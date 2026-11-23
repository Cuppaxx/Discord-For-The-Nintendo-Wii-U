#include "dc.h"
#include "anim.h"
#include <jansson.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <whb/sdcard.h>
#include <whb/file.h>

static char token[400]; static char api[200] = "https://discord.com/api/v10"; static char note[200];
static char sdpath[200]; static char ra_url[200] = "https://remote-auth-gateway.discord.gg/?v=2";
const char *dc_ra_url(void) { return ra_url; }
static char cdn[200] = "https://cdn.discordapp.com"; static char autojoin[64];
const char *dc_autojoin(void) { return autojoin; }
static bool fakemic; bool dc_fakemic(void) { return fakemic; }   /* test aid: config.txt "autojoin=<guild>:<voice channel>" */
void dc_avatar_url(const char *userid, const char *hash, char *out, size_t cap) { snprintf(out, cap, "%s/avatars/%s/%s.png?size=64", cdn, userid, hash); }
void dc_cdn_url(const char *path, char *out, size_t cap) { if (strchr(path, '?')) snprintf(out, cap, "%s/%s", cdn, path); else snprintf(out, cap, "%s/%s.png?size=64", cdn, path); }   /* a path with its own ".png?size=N" is used as-is */
static long iso_ts(const char *s) {                         /* "2026-10-04T22:23:11..." -> unix seconds (UTC) */
  int Y, M, D, h, m, sec; if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  Y -= M <= 2; long era = (Y >= 0 ? Y : Y - 399) / 400; unsigned yoe = (unsigned)(Y - era * 400); unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + (long)doe - 719468; return days * 86400 + h * 3600 + m * 60 + sec;
}

static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0; char *p = s; while (*p == ' ' || *p == '\t' || *p == '"') p++; memmove(s, p, strlen(p) + 1); n = strlen(s); if (n && s[n - 1] == '"') s[n - 1] = 0; }

static void read_text(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "rb"); if (!f) return; size_t n = fread(out, 1, cap - 1, f); out[n] = 0; fclose(f);
}

void dc_init(void) {
  WHBMountSdCard();
  const char *m = WHBGetSdCardMountPath(); if (!m) m = "fs:/vol/external01";
  snprintf(sdpath, sizeof sdpath, "%s/wiiu/apps/MingusDiscord", m);
  char buf[2048] = "";
  /* config.txt (content = test builds, SD = user): lines api=<base url>, token=<token> */
  const char *cfgs[2] = {"/vol/content/config.txt", NULL}; char sdcfg[260]; snprintf(sdcfg, sizeof sdcfg, "%s/config.txt", sdpath); cfgs[1] = sdcfg;
  for (int i = 0; i < 2; i++) {
    buf[0] = 0; read_text(cfgs[i], buf, sizeof buf); char *p = buf;
    while (*p) { char *nl = strchr(p, '\n'); if (nl) *nl = 0;
      if (!strncmp(p, "api=", 4)) { snprintf(api, sizeof api, "%s", p + 4); trim(api); }
      else if (!strncmp(p, "cdn=", 4)) { snprintf(cdn, sizeof cdn, "%s", p + 4); trim(cdn); }
      else if (!strncmp(p, "fakemic=", 8)) fakemic = p[8] == '1';
      else if (!strncmp(p, "autojoin=", 9)) { snprintf(autojoin, sizeof autojoin, "%s", p + 9); trim(autojoin); }
      else if (!strncmp(p, "ra=", 3)) { snprintf(ra_url, sizeof ra_url, "%s", p + 3); trim(ra_url); }
      else if (!strncmp(p, "token=", 6)) { snprintf(token, sizeof token, "%s", p + 6); trim(token); }
      p = nl ? nl + 1 : p + strlen(p); }
  }
  char tp[260]; snprintf(tp, sizeof tp, "%s/token.txt", sdpath);
  if (!token[0]) { read_text(tp, token, sizeof token); trim(token); }
  snprintf(note, sizeof note, "Put your token in SD:/wiiu/apps/MingusDiscord/token.txt");
}
const char *dc_token(void) { return token; }
const char *dc_dir(void) { return sdpath; }
const char *dc_api(void) { return api; }
bool dc_has_token(void) { return token[0] != 0; }
const char *dc_config_note(void) { return note; }
void dc_set_token(const char *t) {
  snprintf(token, sizeof token, "%s", t); trim(token);
  char d1[260]; snprintf(d1, sizeof d1, "%s/wiiu", sdpath); (void)d1;
  char parent[260]; snprintf(parent, sizeof parent, "%.*s", (int)(strlen(sdpath) - strlen("/MingusDiscord")), sdpath); mkdir(parent, 0777); mkdir(sdpath, 0777);
  char tp[260]; snprintf(tp, sizeof tp, "%s/token.txt", sdpath); FILE *f = fopen(tp, "wb"); if (f) { fputs(token, f); fclose(f); }
}
void dc_logout(void) { token[0] = 0; char tp[260]; snprintf(tp, sizeof tp, "%s/token.txt", sdpath); FILE *f = fopen(tp, "wb"); if (f) fclose(f); }

static NxJob *start(const char *method, const char *path, const char *body) {
  char url[700]; snprintf(url, sizeof url, "%s%s", api, path);
  char auth[460]; snprintf(auth, sizeof auth, "Authorization: %s", token);
  bool bot = !strncmp(token, "Bot ", 4);
  const char *h[] = {auth, "Content-Type: application/json", "Accept: application/json",
    bot ? "User-Agent: DiscordBot (https://github.com/mingus/wiiu, 1.0)" : "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppData/1.0 Chrome/126.0 Safari/537.36", NULL};
  return nxnet_start(method, url, h, body, 0, 4u << 20);
}
NxJob *dc_get(const char *path) { return start("GET", path, NULL); }
NxJob *dc_request(const char *method, const char *path, const char *body) { return start(method, path, body); }
void dc_urlenc(const char *in, char *out, size_t cap) {
  size_t o = 0; for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == ':') out[o++] = (char)*p;
    else o += snprintf(out + o, cap - o, "%%%02X", *p); }
  out[o] = 0;
}
NxJob *dc_upload(const char *chan, const char *payload, const char *fname, const char *ctype, const unsigned char *data, size_t len) {
  static const char *B = "----MingusWiiUBoundary7d2f9a";
  char head[1400]; int hn = snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n%s\r\n--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n", B, payload, B, fname, ctype);
  char tail[80]; int tn = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", B);
  size_t total = (size_t)hn + len + (size_t)tn; char *body = malloc(total); if (!body) return NULL;
  memcpy(body, head, hn); memcpy(body + hn, data, len); memcpy(body + hn + len, tail, tn);
  char url[300]; snprintf(url, sizeof url, "%s/channels/%s/messages", api, chan);
  char auth[460]; snprintf(auth, sizeof auth, "Authorization: %s", token); char ct[120]; snprintf(ct, sizeof ct, "Content-Type: multipart/form-data; boundary=%s", B);
  const char *h[] = {auth, ct, "Accept: application/json", "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppData/1.0 Chrome/126.0 Safari/537.36", NULL};
  NxJob *j = nxnet_start("POST", url, h, body, total, 1u << 20); free(body); return j;
}
NxJob *dc_send(const char *chan, const char *text, const char *reply_to) {
  json_t *o = json_object(); json_object_set_new(o, "content", json_string(text));
  if (reply_to && reply_to[0]) { json_t *r = json_object(); json_object_set_new(r, "message_id", json_string(reply_to)); json_object_set_new(r, "fail_if_not_exists", json_false()); json_object_set_new(o, "message_reference", r); }
  char *body = json_dumps(o, JSON_COMPACT); json_decref(o);
  char path[80]; snprintf(path, sizeof path, "/channels/%s/messages", chan);
  NxJob *j = start("POST", path, body); free(body); return j;
}
NxJob *dc_post_message(const char *chan, const char *text) {
  json_t *o = json_object(); json_object_set_new(o, "content", json_string(text));
  char *body = json_dumps(o, JSON_COMPACT); json_decref(o);
  char path[80]; snprintf(path, sizeof path, "/channels/%s/messages", chan);
  NxJob *j = start("POST", path, body); free(body); return j;
}

const char *dc_error_text(const NxResp *r) {
  static char b[220];
  if (r->status == 0) { snprintf(b, sizeof b, "Network error: %.150s", r->err[0] ? r->err : "no response"); return b; }
  if (r->status == 401) return "Token rejected (401). Check token.txt";
  if (r->status == 403) return "Forbidden (403): no access";
  if (r->status == 429) return "Rate limited (429) - slow down";
  snprintf(b, sizeof b, "HTTP %d", r->status); return b;
}

static const char *js(json_t *o, const char *k) { json_t *v = json_object_get(o, k); return json_is_string(v) ? json_string_value(v) : ""; }
static void cp(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

int dc_parse_guilds(const char *jsn, Guild *out, int max) {
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return -1; }
  int n = 0; size_t i; json_t *g;
  json_array_foreach(r, i, g) { if (n >= max) break; cp(out[n].id, 24, js(g, "id")); cp(out[n].name, 72, js(g, "name")); cp(out[n].icon, 40, json_is_string(json_object_get(g, "icon")) ? js(g, "icon") : ""); out[n].perms = strtoull(js(g, "permissions"), NULL, 10); out[n].owner = json_is_true(json_object_get(g, "owner")); if (out[n].owner || (out[n].perms & P_ADMIN)) out[n].perms = ~0ULL; n++; }
  json_decref(r); return n;
}

static int chan_cmp_pos;
int dc_parse_channels(const char *jsn, Chan *out, int max) {
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return -1; }
  size_t cnt = json_array_size(r); Chan *all = calloc(cnt + 1, sizeof(Chan)); int na = 0; size_t i; json_t *c;
  json_array_foreach(r, i, c) {
    all[na].type = (int)json_integer_value(json_object_get(c, "type")); all[na].pos = (int)json_integer_value(json_object_get(c, "position"));
    all[na].last = json_is_string(json_object_get(c, "last_message_id")) ? strtoull(js(c, "last_message_id"), NULL, 10) : 0;
    { const char *tp = js(c, "topic"); size_t k = 0; for (; tp[k] && k < sizeof all[na].topic - 1; k++) all[na].topic[k] = tp[k] == '\n' ? ' ' : tp[k]; all[na].topic[k] = 0; }
    cp(all[na].id, 24, js(c, "id")); cp(all[na].name, 72, js(c, "name")); cp(all[na].parent, 24, json_is_string(json_object_get(c, "parent_id")) ? js(c, "parent_id") : ""); na++;
  }
  (void)chan_cmp_pos; int n = 0;
  /* uncategorised text channels first, then each category (sorted by position) with its text channels */
  #define TEXTY(t) ((t) == 0 || (t) == 5 || (t) == 2)   /* text, announcement, voice */
  for (int pass = 0; pass < 2; pass++) {
    for (int p = 0; p < 500 && n < max; p++) {                                  /* simple position sweep */
      for (int a = 0; a < na && n < max; a++) {
        if (pass == 0 && !all[a].parent[0] && TEXTY(all[a].type) && all[a].pos == p) { out[n] = all[a]; out[n].header = false; n++; }
      }
    }
  }
  int lastpos = -1;
  for (int step = 0; step < na && n < max; step++) {                            /* categories in position order */
    int best = -1; for (int a = 0; a < na; a++) if (all[a].type == 4 && all[a].pos > lastpos && (best < 0 || all[a].pos < all[best].pos)) best = a;
    if (best < 0) break; lastpos = all[best].pos;
    int hdr = n; out[n] = all[best]; out[n].header = true; n++; int kids = 0;
    for (int p = 0; p < 500 && n < max; p++) for (int a = 0; a < na && n < max; a++)
      if (TEXTY(all[a].type) && !strcmp(all[a].parent, all[best].id) && all[a].pos == p) { out[n] = all[a]; out[n].header = false; n++; kids++; }
    if (!kids) n = hdr;                                                           /* drop empty categories */
  }
  free(all); json_decref(r); return n;
}

static unsigned long long *blocked_ids; static int n_blocked;
void dc_set_blocked(const char *jsn) {
  free(blocked_ids); blocked_ids = NULL; n_blocked = 0;
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return; }
  size_t cnt = json_array_size(r); blocked_ids = calloc(cnt + 1, sizeof *blocked_ids); size_t i; json_t *e;
  json_array_foreach(r, i, e) if ((int)json_integer_value(json_object_get(e, "type")) == 2) { const char *id = js(e, "id"); if (!id[0]) { json_t *u = json_object_get(e, "user"); id = u ? js(u, "id") : ""; } if (id[0]) blocked_ids[n_blocked++] = strtoull(id, NULL, 10); }
  json_decref(r);
}
static bool is_blocked(const char *id) { unsigned long long v = strtoull(id, NULL, 10); for (int i = 0; i < n_blocked; i++) if (blocked_ids[i] == v) return true; return false; }

int dc_parse_guild_order(const char *jsn, char ids[][24], int max) {
  json_t *r = json_loads(jsn, 0, NULL); if (!r) return 0; int n = 0;
  json_t *gf = json_object_get(r, "guild_folders"); size_t i, k; json_t *f, *g;
  json_array_foreach(gf, i, f) { json_t *gi = json_object_get(f, "guild_ids"); json_array_foreach(gi, k, g) { if (n >= max) break; if (json_is_string(g)) { cp(ids[n], 24, json_string_value(g)); n++; } } }
  json_decref(r); return n;
}

static int dm_cmp(const void *a, const void *b) { unsigned long long x = ((const Chan *)a)->last, y = ((const Chan *)b)->last; return x < y ? 1 : x > y ? -1 : 0; }   /* newest first */
int dc_parse_dms(const char *jsn, Chan *out, int max) {
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return -1; }
  int n = 0; size_t i; json_t *c;
  json_array_foreach(r, i, c) {
    if (n >= max) break; int t = (int)json_integer_value(json_object_get(c, "type")); if (t != 1 && t != 3) continue;
    json_t *rec = json_object_get(c, "recipients");
    if (t == 1) { json_t *u0 = json_array_get(rec, 0); if (u0 && is_blocked(js(u0, "id"))) continue; }     /* hide DMs with blocked users */
    memset(&out[n], 0, sizeof out[n]);
    cp(out[n].id, 24, js(c, "id")); out[n].type = t; out[n].header = false; out[n].parent[0] = 0; out[n].pos = 0;
    out[n].last = json_is_string(json_object_get(c, "last_message_id")) ? strtoull(js(c, "last_message_id"), NULL, 10) : 0;
    char nm[72] = ""; size_t k; json_t *u;
    if (json_is_string(json_object_get(c, "name")) && js(c, "name")[0]) cp(nm, 72, js(c, "name"));
    else json_array_foreach(rec, k, u) { const char *dn = json_is_string(json_object_get(u, "global_name")) ? js(u, "global_name") : js(u, "username"); if (nm[0]) strncat(nm, ", ", 71 - strlen(nm)); strncat(nm, dn, 71 - strlen(nm)); }
    cp(out[n].name, 72, nm[0] ? nm : "Unknown");
    if (t == 1) { json_t *u0 = json_array_get(rec, 0); if (u0) { dc_parse_style(u0, &out[n].ns); cp(out[n].uid, 24, js(u0, "id")); } }
    if (t == 1) { json_t *u0 = json_array_get(rec, 0); if (u0 && json_is_string(json_object_get(u0, "avatar"))) snprintf(out[n].av, sizeof out[n].av, "avatars/%s/%s", js(u0, "id"), js(u0, "avatar")); }
    else if (json_is_string(json_object_get(c, "icon"))) snprintf(out[n].av, sizeof out[n].av, "channel-icons/%s/%s", js(c, "id"), js(c, "icon"));
    n++;
  }
  qsort(out, n, sizeof *out, dm_cmp);
  json_decref(r); return n;
}

/* Discord media-proxy URL scaled to fit 520x360 WITHOUT cropping (passing a fixed w+h box crops) */
static void proxy_img_url(char *dst, size_t cap, const char *u, int w, int h) {
  if (!strstr(u, "discordapp")) { snprintf(dst, cap, "%s", u); return; }
  const char *sep = strchr(u, '?') ? "&" : "?";
  if (w > 0 && h > 0) { if (w > 520) { h = h * 520 / w; w = 520; } if (h > 360) { w = w * 360 / h; h = 360; } if (w < 1) w = 1; if (h < 1) h = 1; snprintf(dst, cap, "%s%swidth=%d&height=%d&format=jpeg", u, sep, w, h); }
  else snprintf(dst, cap, "%s%swidth=520&format=jpeg", u, sep);
}

static unsigned hash_color(const char *s) {
  static const unsigned pal[] = {0xf0b232, 0x5865f2, 0x57f287, 0xeb459e, 0xed4245, 0x3ba55d, 0x00b0f4, 0xfee75c, 0xc27cff, 0xff8a4c};
  unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return pal[h % 10];
}

/* turn <@id> <#id> <:emoji:id> markup into readable text */
static char *clean(const char *in, json_t *mentions) {
  /* the buffer grows as needed (it used to stop copying when n + 80 >= cap, which emptied every message under ~8 bytes: "ok", a lone emoji...) */
  size_t cap = strlen(in) * 2 + 256; char *o = malloc(cap); size_t n = 0; const char *p = in;
  while (*p) {
    if (n + 200 > cap) { cap = cap * 2 + 256; char *g = realloc(o, cap); if (!g) break; o = g; }
    if (p[0] == '<') {
      const char *e = strchr(p, '>');
      if (e && e - p < 90) {
        char tag[100]; snprintf(tag, sizeof tag, "%.*s", (int)(e - p - 1), p + 1);
        if (tag[0] == '@' || (tag[0] == '@' && tag[1] == '!')) {
          const char *id = tag + 1; if (*id == '!' || *id == '&') id++; const char *nm = "user"; size_t k; json_t *u;
          json_array_foreach(mentions, k, u) if (!strcmp(js(u, "id"), id)) { nm = json_is_string(json_object_get(u, "global_name")) ? js(u, "global_name") : js(u, "username"); break; }
          n += snprintf(o + n, cap - n, "@%s", nm); p = e + 1; continue;
        }
        if (tag[0] == '#') { n += snprintf(o + n, cap - n, "#channel"); p = e + 1; continue; }
        if (tag[0] == ':' || (tag[0] == 'a' && tag[1] == ':')) {     /* custom emoji <:name:id> / <a:name:id> -> inline image token \x01c:<id>\x02 */
          const char *a = strchr(tag, ':'); const char *b = a ? strchr(a + 1, ':') : NULL;
          if (a && b && b[1]) { n += snprintf(o + n, cap - n, "\x01" "c:%s\x02", b + 1); p = e + 1; continue; } }
        if (tag[0] == 't' && tag[1] == ':') {                          /* <t:unix[:style]> -> readable UTC date/time */
          long long ts = strtoll(tag + 2, NULL, 10); time_t tt = (time_t)ts; struct tm tmv; char tb[40] = "[time]";
          if (ts > 0 && gmtime_r(&tt, &tmv)) strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tmv);
          n += snprintf(o + n, cap - n, "%s", tb); p = e + 1; continue; }
      }
    }
    o[n++] = *p++;
  }
  o[n] = 0; return o;
}

char *dc_clean_text(const char *in) { return clean(in, NULL); }
/* guild roles: name color = color of the highest-positioned role that has one */
typedef struct { unsigned long long id; unsigned color; int pos; } Role;
static Role *roles; static int nroles; static char roles_guild[24];
void dc_set_roles(const char *gid, const char *jsn) {
  free(roles); roles = NULL; nroles = 0; snprintf(roles_guild, sizeof roles_guild, "%s", gid);
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return; }
  roles = calloc(json_array_size(r) + 1, sizeof *roles); size_t i; json_t *e;
  json_array_foreach(r, i, e) { Role *o = &roles[nroles++]; o->id = strtoull(js(e, "id"), NULL, 10); json_t *cs = json_object_get(e, "colors");
    o->color = (unsigned)json_integer_value(json_object_get(e, "color")); if (!o->color && json_is_object(cs)) o->color = (unsigned)json_integer_value(json_object_get(cs, "primary_color"));
    o->pos = (int)json_integer_value(json_object_get(e, "position")); }
  json_decref(r);
}
const char *dc_roles_guild(void) { return roles_guild; }
static unsigned member_color(json_t *mem) {
  json_t *rl = json_object_get(mem, "roles"); size_t i; json_t *v; int best = -1; unsigned col = 0;
  json_array_foreach(rl, i, v) { unsigned long long id = strtoull(json_string_value(v) ? json_string_value(v) : "0", NULL, 10);
    for (int k = 0; k < nroles; k++) if (roles[k].id == id && roles[k].color && roles[k].pos > best) { best = roles[k].pos; col = roles[k].color; } }
  return col;
}
void dc_parse_style(json_t *u, NxNameStyle *o) {
  memset(o, 0, sizeof *o); json_t *d = u ? json_object_get(u, "display_name_styles") : NULL; if (!json_is_object(d)) return;
  o->font = (short)json_integer_value(json_object_get(d, "font_id")); o->effect = (short)json_integer_value(json_object_get(d, "effect_id"));
  json_t *cs = json_object_get(d, "colors"); size_t k; json_t *v; json_array_foreach(cs, k, v) { if (o->ncol >= 5) break; o->col[o->ncol++] = (uint32_t)json_integer_value(v); }
}
static const char *user_dn(json_t *u) { return json_is_string(json_object_get(u, "global_name")) && js(u, "global_name")[0] ? js(u, "global_name") : js(u, "username"); }
/* attachments + embeds + stickers of a message (or of a forwarded message snapshot) */
static void parse_media(json_t *m, Msg *o, bool *embed_img, bool *gif_embed) {
  json_t *ats = json_object_get(m, "attachments"); size_t k; json_t *a;
  json_array_foreach(ats, k, a) {
    if (o->nat >= MAX_AT) break; Attach *at = &o->at[o->nat++]; cp(at->name, 56, js(a, "filename"));
    const char *ct = js(a, "content_type"); at->image = !strncmp(ct, "image/", 6) && strncmp(ct, "image/svg", 9) != 0;
    at->video = !strncmp(ct, "video/", 6); at->gif = !strncmp(ct, "image/gif", 9); cp(at->ctype, sizeof at->ctype, ct);
    { const char *fn = js(a, "filename"), *dot = strrchr(fn, '.'); at->audio = !strncmp(ct, "audio/", 6) || (dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".opus") || !strcasecmp(dot, ".wav") || !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".aac"))); }
    at->w = (int)json_integer_value(json_object_get(a, "width")); at->h = (int)json_integer_value(json_object_get(a, "height"));
    const char *u = json_is_string(json_object_get(a, "proxy_url")) ? js(a, "proxy_url") : js(a, "url");
    cp(at->src, sizeof at->src, js(a, "url")[0] ? js(a, "url") : u);
    if (at->gif) cp(at->anim_url, sizeof at->anim_url, at->src);
    { json_t *ds = json_object_get(a, "duration_secs"); const char *wf = js(a, "waveform");
      if (json_is_number(ds) && wf[0]) { at->voice = true; at->audio = true; at->dur = (float)json_number_value(ds); size_t ol = 0; if (mbedtls_base64_decode(at->wave, sizeof at->wave, &ol, (const unsigned char *)wf, strlen(wf)) == 0) at->nwave = (int)ol; } }
    if (at->video) { at->image = true; proxy_img_url(at->url, sizeof at->url, u, at->w, at->h); }   /* media proxy returns a poster frame for videos */
    else if (at->image) proxy_img_url(at->url, sizeof at->url, u, at->w, at->h); else cp(at->url, sizeof at->url, u);
  }
  json_t *embs = json_object_get(m, "embeds"); size_t ei; json_t *e;       /* embed previews: GIFs (gifv), images, link cards */
  json_array_foreach(embs, ei, e) {
    const char *et = js(e, "type");
    if (!o->card && strcmp(et, "gifv") && strcmp(et, "image")) {        /* link card text */
      const char *t = js(e, "title"), *d = js(e, "description"), *pn = js(json_object_get(e, "provider"), "name"), *an = js(json_object_get(e, "author"), "name");
      if (t[0] || d[0]) { size_t cap = strlen(t) + strlen(d) + strlen(pn) + strlen(an) + 8; char *cd = malloc(cap); snprintf(cd, cap, "%s%s%s%s%s%s%.*s", pn[0] ? pn : an, (pn[0] || an[0]) ? "\n" : "", t, t[0] ? "\n" : "", "", "", 400, d); o->card = cd; } }
    if (o->nat >= MAX_AT) continue;
    json_t *src = json_object_get(e, "image"); if (!json_is_object(src)) src = json_object_get(e, "thumbnail"); if (!json_is_object(src)) continue;
    const char *u = json_is_string(json_object_get(src, "proxy_url")) ? js(src, "proxy_url") : js(src, "url"); if (!u[0]) continue;
    Attach *at = &o->at[o->nat++]; bool gv = !strcmp(et, "gifv"); cp(at->name, 56, gv ? "GIF" : "image");
    at->image = true; at->gif = gv; at->w = (int)json_integer_value(json_object_get(src, "width")); at->h = (int)json_integer_value(json_object_get(src, "height"));
    { const char *ou = js(src, "url"); const char *q = strchr(ou, '?'); size_t ul = q ? (size_t)(q - ou) : strlen(ou);
      if (gv && ul > 5 && (!strncasecmp(ou + ul - 5, ".webp", 5) || !strncasecmp(ou + ul - 4, ".gif", 4))) cp(at->anim_url, sizeof at->anim_url, ou); }   /* Klipy/Tenor: animated WebP/GIF thumbnail */
    json_t *vid = json_object_get(e, "video"); if (gv && json_is_object(vid)) cp(at->src, sizeof at->src, json_is_string(json_object_get(vid, "proxy_url")) ? js(vid, "proxy_url") : js(vid, "url"));
    proxy_img_url(at->url, sizeof at->url, u, at->w, at->h); *embed_img = true; if (gv || !strcmp(et, "image")) *gif_embed = true;
  }
  json_t *sts = json_object_get(m, "sticker_items"); size_t si; json_t *s;           /* stickers (PNG/APNG/GIF); Lottie stickers can't be shown */
  json_array_foreach(sts, si, s) { int ft = (int)json_integer_value(json_object_get(s, "format_type")); if (o->nat >= MAX_AT || ft == 3) continue;
    Attach *at = &o->at[o->nat++]; cp(at->name, 56, "sticker"); at->image = true; at->w = at->h = 160;
    snprintf(at->url, sizeof at->url, ft == 4 ? "https://media.discordapp.net/stickers/%s.gif?size=160" : "%s/stickers/%s.png?size=160", ft == 4 ? js(s, "id") : cdn, ft == 4 ? "" : js(s, "id"));
    if (ft == 4) snprintf(at->url, sizeof at->url, "https://media.discordapp.net/stickers/%s.png?size=160", js(s, "id"));
    *embed_img = true; }
}
static char *one_line(const char *s, int maxc) {
  size_t n = strlen(s); char *o = malloc(n + 8); int k = 0; for (size_t i = 0; i < n && k < maxc; i++) o[k++] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
  while (k && ((unsigned char)o[k - 1] & 0xC0) == 0x80 && k == maxc) k--; o[k] = 0; if (n > (size_t)maxc) strcat(o, "..."); return o;
}
int dc_parse_messages(const char *jsn, Msg *out, int max) {
  json_t *r = json_loads(jsn, 0, NULL); if (!json_is_array(r)) { json_decref(r); return -1; }
  int total = (int)json_array_size(r); int n = 0;
  for (int i = total - 1; i >= 0 && n < max; i--) {                /* API returns newest first */
    json_t *m = json_array_get(r, i); Msg *o = &out[n]; memset(o, 0, sizeof *o);
    cp(o->id, 24, js(m, "id")); json_t *au = json_object_get(m, "author"); json_t *mem = json_object_get(m, "member");
    const char *nick = mem && json_is_string(json_object_get(mem, "nick")) ? js(mem, "nick") : "";
    cp(o->author, 56, nick[0] ? nick : user_dn(au)); cp(o->authorid, 24, js(au, "id")); o->color = 0xF2F3F5; if (mem) { unsigned rc = member_color(mem); if (rc) o->color = rc; }   /* Discord: role color, else default white */ dc_parse_style(au, &o->ns);
    const char *ts = js(m, "timestamp"); if (strlen(ts) >= 16) snprintf(o->time, 8, "%.5s", ts + 11); o->ts = iso_ts(ts);
    if (json_is_string(json_object_get(au, "avatar"))) cp(o->avatar, 48, js(au, "avatar"));
    o->text = clean(js(m, "content"), json_object_get(m, "mentions")); o->raw = strdup(js(m, "content"));
    bool embed_img = false, gif_embed = false; parse_media(m, o, &embed_img, &gif_embed);
    /* reply header (type 19): referenced_message is the original, or null when it was deleted */
    json_t *mref = json_object_get(m, "message_reference"); int rtype = mref ? (int)json_integer_value(json_object_get(mref, "type")) : 0;
    if ((int)json_integer_value(json_object_get(m, "type")) == 19 || (mref && rtype == 0 && json_object_get(m, "referenced_message"))) {
      json_t *rm = json_object_get(m, "referenced_message"); o->has_reply = true;
      if (json_is_object(rm)) { json_t *ra = json_object_get(rm, "author"); cp(o->reply_author, 56, user_dn(ra)); cp(o->reply_authorid, 24, js(ra, "id")); dc_parse_style(ra, &o->reply_ns);
        char *rc = clean(js(rm, "content"), json_object_get(rm, "mentions"));
        if (!rc[0]) { free(rc); rc = strdup(json_array_size(json_object_get(rm, "attachments")) ? "Click to see attachment" : json_array_size(json_object_get(rm, "embeds")) ? "[embed]" : json_array_size(json_object_get(rm, "sticker_items")) ? "[sticker]" : "..."); }
        o->reply = one_line(rc, 120); free(rc); }
    }
    /* forwarded message (message_reference.type 1): show the snapshot */
    json_t *snaps = json_object_get(m, "message_snapshots");
    if (rtype == 1 && json_is_array(snaps) && json_array_size(snaps)) {
      json_t *sm = json_object_get(json_array_get(snaps, 0), "message"); o->forwarded = true;
      if (json_is_object(sm)) { if (!o->text[0]) { free(o->text); o->text = clean(js(sm, "content"), json_object_get(sm, "mentions")); } parse_media(sm, o, &embed_img, &gif_embed); }
    }
    if (gif_embed && o->text[0] && !strncmp(o->text, "http", 4) && !strpbrk(o->text, " \n")) o->text[0] = 0;   /* a bare GIF/image link: the preview replaces the URL text */
    if (gif_embed && o->card) { free(o->card); o->card = NULL; }
    { json_t *rs = json_object_get(m, "reactions"); size_t ri; json_t *rr;                /* reactions */
      json_array_foreach(rs, ri, rr) { if (o->nre >= MAX_REACT) break; json_t *em = json_object_get(rr, "emoji"); React *x = &o->re[o->nre];
        const char *eid = json_is_string(json_object_get(em, "id")) ? js(em, "id") : "", *en = js(em, "name"); if (!en[0] && !eid[0]) continue;
        if (eid[0]) { snprintf(x->key, sizeof x->key, "\x01" "c:%s\x02", eid); snprintf(x->code, sizeof x->code, "%s:%s", en[0] ? en : "_", eid); }
        else { snprintf(x->key, sizeof x->key, "%s", en); snprintf(x->code, sizeof x->code, "%s", en); }
        x->count = (int)json_integer_value(json_object_get(rr, "count")); x->me = json_is_true(json_object_get(rr, "me")); o->nre++; } }
    int mt = (int)json_integer_value(json_object_get(m, "type"));         /* system messages get readable text */
    if (!o->text[0] && !o->nat && !o->card) {
      const char *sys = mt == 1 ? "added someone to the group." : mt == 2 ? "removed someone from the group." : mt == 3 ? "started a call." : mt == 4 ? "changed the channel name." : mt == 5 ? "changed the channel icon." :
        mt == 6 ? "pinned a message." : mt == 7 ? "joined the server." : mt == 8 ? "boosted the server!" : mt == 18 ? "started a thread." : mt == 46 ? "[poll result]" : NULL;
      if (sys) { free(o->text); o->text = strdup(sys); }
    }
    n++;
  }
  json_decref(r); return n;
}
void dc_free_msg(Msg *m) { { extern void dlog(const char *, ...); if (m->nat && m->at[0].voice) dlog("[uaf] free voice msg %s text %p", m->id, (void *)m->text); } for (int i = 0; i < m->nat; i++) if (m->at[i].anim) { anim_close((Anim *)m->at[i].anim); m->at[i].anim = NULL; }
  free(m->text); m->text = NULL; free(m->raw); m->raw = NULL; free(m->reply); m->reply = NULL; free(m->card); m->card = NULL; for (int i = 0; i < m->nat; i++) if (m->at[i].tex) { SDL_DestroyTexture(m->at[i].tex); m->at[i].tex = NULL; } }
