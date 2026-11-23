/* overlays: message actions, emoji picker, user profile */
#include "overlay.h"
#include "dc.h"
#include "nxnet.h"
#include "anim.h"
#include <jansson.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <webp/demux.h>

#define C_BG RGB(49, 51, 56)
#define C_SIDE RGB(43, 45, 49)
#define C_DARK RGB(30, 31, 34)
#define C_SEL RGB(64, 66, 73)
#define C_BLURPLE RGB(88, 101, 242)
#define C_TEXT RGB(219, 222, 225)
#define C_MUTED RGB(148, 155, 164)
#define C_WHITE RGB(255, 255, 255)
#define C_GREEN RGB(35, 165, 90)
#define UC(c) RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

static OvImgFn imgfn; static int kind;            /* 0 none, 1 actions, 2 emoji, 3 profile */
static bool hitp(const NxInput *in, int x, int y, int w, int h) { return in->touch_pressed && in->tx >= x && in->tx < x + w && in->ty >= y && in->ty < y + h; }
static int tcenter(int cx, int y, const char *s, int size, NxColor c) { int w = nx_text_w(s, size); return nx_text(cx - w / 2, y, s, size, c, 0); }
static int utf8_put(uint32_t cp, char *o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; } if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
  if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
  o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}
void ov_init(OvImgFn img) { imgfn = img; }
bool ov_active(void) { return kind != 0; }

/* ======================= message actions ======================= */
static char a_msg[24], a_user[24], a_name[80]; static int a_sel; static bool a_own, a_confirm;
static const char *a_items_all[] = {"Reply", "Add reaction", "Mention", "View profile", "Edit message", "Delete message", "Close", "Open attachment", "Pin message"};
static int a_map[10]; static int A_N; static bool a_media, a_mod;
void ov_actions_media(bool m) { a_media = m; }
void ov_actions_mod(bool m) { a_mod = m; }
void ov_open_actions_ex(const char *msg_id, const char *user_id, const char *name, bool own) { snprintf(a_msg, 24, "%s", msg_id); snprintf(a_user, 24, "%s", user_id); snprintf(a_name, 80, "%s", name); a_sel = 0; kind = 1; a_own = own; a_confirm = false;
  A_N = 0; if (a_media) a_map[A_N++] = 7; for (int i = 0; i < 7; i++) { if (i == 4 && !own) continue; if (i == 5 && !own && !a_mod) continue; if (i == 6 && a_mod) a_map[A_N++] = 8; a_map[A_N++] = i; } a_media = false; a_mod = false; }
void ov_open_actions(const char *msg_id, const char *user_id, const char *name) { ov_open_actions_ex(msg_id, user_id, name, false); }
static void act_pick(int i, OvResult *r) {
  kind = 0; snprintf(r->msg_id, 24, "%s", a_msg); snprintf(r->user_id, 24, "%s", a_user); snprintf(r->name, 80, "%s", a_name);
  i = a_map[i]; if (i == 0) r->kind = OVR_REPLY; else if (i == 1) r->kind = OVR_EMOJI_REACT; else if (i == 2) r->kind = OVR_MENTION; else if (i == 3) r->kind = OVR_PROFILE; else if (i == 4) r->kind = OVR_EDIT; else if (i == 5) r->kind = OVR_DELETE; else if (i == 7) r->kind = OVR_OPEN_MEDIA; else if (i == 8) r->kind = OVR_PIN;
}
#define AX 470
#define AY (A_N > 7 ? 60 : 170)
static void act_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 150));
  nx_rrect(AX, AY, 340, 70 + A_N * 56, 12, C_DARK);
  nx_text(AX + 20, AY + 18, a_name, 1 | NX_BOLD, C_WHITE, 300);
  if (a_confirm) { nx_text(AX + 20, AY + 70, "Delete this message?", 1, C_TEXT, 0); nx_text(AX + 20, AY + 110, "A  delete      B  cancel", 1, RGB(237, 66, 69), 0); return; }
  for (int i = 0; i < A_N; i++) { int y = AY + 60 + i * 56; bool del = a_map[i] == 5; if (i == a_sel) nx_rrect(AX + 10, y, 320, 48, 8, del ? RGB(237, 66, 69) : C_BLURPLE); nx_text(AX + 28, y + 11, a_items_all[a_map[i]], 1, i == a_sel ? C_WHITE : del ? RGB(237, 66, 69) : C_TEXT, 0); }
}
static bool act_input(const NxInput *in, OvResult *r) {
  if (a_confirm) { if (in->pressed[B_A]) { r->kind = OVR_DELETE; snprintf(r->msg_id, 24, "%s", a_msg); kind = 0; } else if (in->pressed[B_B] || in->touch_pressed) a_confirm = false; return true; }
  if (in->pressed[B_DOWN] && a_sel < A_N - 1) a_sel++; if (in->pressed[B_UP] && a_sel > 0) a_sel--;
  if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_A]) { if (a_sel == A_N - 1) kind = 0; else if (a_map[a_sel] == 5) a_confirm = true; else act_pick(a_sel, r); return true; }
  if (in->touch_pressed) { for (int i = 0; i < A_N; i++) if (hitp(in, AX + 10, AY + 60 + i * 56, 320, 48)) { if (i == A_N - 1) kind = 0; else if (a_map[i] == 5) a_confirm = true; else act_pick(i, r); return true; }
    if (!hitp(in, AX, AY, 340, 70 + A_N * 56)) kind = 0; }
  return true;
}

/* ======================= emoji picker ======================= */
typedef struct { char draw[48]; char ins[96]; char code[96]; short gi; } Emo;   /* gi: server index (All servers) */
typedef struct { const char *name; int start, n; } ECat;
static Emo *emo; static int nemo, emocap; static ECat cats[12]; static int ncats; static int c_server = -1, c_recent = -1, c_all = -1, c_people = -1;
static int e_cat, e_sel, e_scroll, e_purpose; static char e_msg[24], e_guild[24]; static NxJob *e_job; static char e_job_guild[24];
static Emo recent[32]; static int nrecent;
static Emo *emo_add(void) { if (nemo >= emocap) { emocap = emocap ? emocap * 2 : 1024; emo = realloc(emo, emocap * sizeof *emo); } memset(&emo[nemo], 0, sizeof *emo); return &emo[nemo++]; }
static void add_cp_seq(const uint32_t *cps, int n) {
  Emo *e = emo_add(); int k = 0; for (int i = 0; i < n; i++) k += utf8_put(cps[i], e->draw + k);
  if (n == 1 && cps[0] < 0x10000) k += utf8_put(0xFE0F, e->draw + k);   /* BMP symbols need the emoji presentation selector */
  e->draw[k] = 0; snprintf(e->ins, sizeof e->ins, "%s", e->draw); snprintf(e->code, sizeof e->code, "%s", e->draw);
}
static void add_ranges(const char *name, const uint32_t (*r)[2], int nr) {
  ECat *c = &cats[ncats++]; c->name = name; c->start = nemo;
  for (int i = 0; i < nr; i++) for (uint32_t cp = r[i][0]; cp <= r[i][1]; cp++) add_cp_seq(&cp, 1);
  c->n = nemo - c->start;
}
static const uint32_t R_FACES[][2] = {{0x1F600, 0x1F64F}, {0x1F910, 0x1F92F}, {0x1F970, 0x1F97A}, {0x1F9D0, 0x1F9D0}, {0x1FAE0, 0x1FAE8}, {0x1F47B, 0x1F47D}, {0x1F480, 0x1F480}, {0x1F4A9, 0x1F4A9}, {0x1F446, 0x1F450}, {0x1F4AA, 0x1F4AA}, {0x1F918, 0x1F91F}, {0x270A, 0x270D}};
static const uint32_t R_NATURE[][2] = {{0x1F400, 0x1F43F}, {0x1F980, 0x1F9AE}, {0x1FAB0, 0x1FAB6}, {0x1F330, 0x1F344}, {0x1F490, 0x1F490}, {0x1F30D, 0x1F31F}, {0x2600, 0x2601}, {0x26A1, 0x26A1}, {0x2744, 0x2744}};
static const uint32_t R_FOOD[][2] = {{0x1F345, 0x1F37F}, {0x1F950, 0x1F96F}, {0x1F9C0, 0x1F9CB}, {0x1FAD0, 0x1FADB}};
static const uint32_t R_ACT[][2] = {{0x1F380, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x26BD, 0x26BE}, {0x1F945, 0x1F945}, {0x1F94A, 0x1F94F}, {0x1F9E9, 0x1F9E9}, {0x1FA80, 0x1FA86}};
static const uint32_t R_TRAVEL[][2] = {{0x1F680, 0x1F6C5}, {0x1F3E0, 0x1F3F0}, {0x1F5FB, 0x1F5FF}, {0x1F6F4, 0x1F6FC}, {0x2708, 0x2708}};
static const uint32_t R_OBJ[][2] = {{0x1F4A0, 0x1F4FF}, {0x1F50A, 0x1F53D}, {0x1F9F0, 0x1F9FF}, {0x1FA90, 0x1FAAC}, {0x231A, 0x231B}, {0x23F0, 0x23F3}};
static const uint32_t R_SYM[][2] = {{0x2764, 0x2764}, {0x1F493, 0x1F49F}, {0x1F5A4, 0x1F5A4}, {0x1F90D, 0x1F90E}, {0x1F9E1, 0x1F9E1}, {0x2705, 0x2705}, {0x274C, 0x274C}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x1F4AF, 0x1F4AF}, {0x1F51E, 0x1F51E}, {0x1F6AB, 0x1F6AB}, {0x26A0, 0x26A0}, {0x267B, 0x267B}, {0x2622, 0x2623}, {0x262E, 0x262F}, {0x1F7E0, 0x1F7EB}, {0x2B1B, 0x2B1C}};
static void emo_build(void) {
  if (emo) return;
  c_recent = ncats; { ECat *c = &cats[ncats++]; c->name = "Recent"; c->start = 0; c->n = 0; }
  c_server = ncats; { ECat *c = &cats[ncats++]; c->name = "Server"; c->start = 0; c->n = 0; }
  c_all = ncats; { ECat *c = &cats[ncats++]; c->name = "All servers"; c->start = 0; c->n = 0; }
  c_people = ncats;
  add_ranges("People", R_FACES, sizeof R_FACES / sizeof R_FACES[0]);
  add_ranges("Nature", R_NATURE, sizeof R_NATURE / sizeof R_NATURE[0]);
  add_ranges("Food", R_FOOD, sizeof R_FOOD / sizeof R_FOOD[0]);
  add_ranges("Activities", R_ACT, sizeof R_ACT / sizeof R_ACT[0]);
  add_ranges("Travel", R_TRAVEL, sizeof R_TRAVEL / sizeof R_TRAVEL[0]);
  add_ranges("Objects", R_OBJ, sizeof R_OBJ / sizeof R_OBJ[0]);
  add_ranges("Symbols", R_SYM, sizeof R_SYM / sizeof R_SYM[0]);
  { ECat *c = &cats[ncats++]; c->name = "Flags"; c->start = nemo;
    static const char *cc[] = {"US", "GB", "CA", "MX", "BR", "AR", "JP", "KR", "CN", "PH", "VN", "TH", "IN", "AU", "NZ", "FR", "DE", "IT", "ES", "PT", "NL", "BE", "CH", "AT", "SE", "NO", "FI", "DK", "IE", "PL", "UA", "RU", "TR", "GR", "IL", "SA", "EG", "NG", "ZA", "CO", "CL", "PE"};
    for (size_t i = 0; i < sizeof cc / sizeof cc[0]; i++) { uint32_t p[2] = {0x1F1E6 + (uint32_t)(cc[i][0] - 'A'), 0x1F1E6 + (uint32_t)(cc[i][1] - 'A')}; add_cp_seq(p, 2); }
    static const uint32_t rainbow[] = {0x1F3F3, 0xFE0F, 0x200D, 0x1F308}, trans[] = {0x1F3F3, 0xFE0F, 0x200D, 0x26A7, 0xFE0F}, pirate[] = {0x1F3F4, 0x200D, 0x2620, 0xFE0F}, chk[] = {0x1F3C1};
    add_cp_seq(rainbow, 4); add_cp_seq(trans, 5); add_cp_seq(pirate, 4); add_cp_seq(chk, 1);
    c->n = nemo - c->start; }
}
static void recent_path(char *p, size_t n) { snprintf(p, n, "%s/recent_emoji.txt", dc_dir()); }
static void recent_load(void) {
  nrecent = 0; char p[300]; recent_path(p, sizeof p); FILE *f = fopen(p, "rb"); if (!f) return; char ln[300];
  while (nrecent < 32 && fgets(ln, sizeof ln, f)) { char *a = strtok(ln, "\t\r\n"), *b = strtok(NULL, "\t\r\n"), *c = strtok(NULL, "\t\r\n"); if (!a || !b || !c) continue;
    Emo *e = &recent[nrecent++]; snprintf(e->draw, sizeof e->draw, "%s", a); snprintf(e->ins, sizeof e->ins, "%s", b); snprintf(e->code, sizeof e->code, "%s", c);
    for (char *q = e->draw; *q; q++) if (*q == '\x03') *q = '\x01'; else if (*q == '\x04') *q = '\x02'; }   /* tokens are stored escaped */
  fclose(f);
}
static void recent_push(const Emo *e) {
  int at = -1; for (int i = 0; i < nrecent; i++) if (!strcmp(recent[i].ins, e->ins)) at = i;
  if (at < 0) { if (nrecent < 32) nrecent++; at = nrecent - 1; }
  memmove(&recent[1], &recent[0], at * sizeof *recent); recent[0] = *e;
  char p[300]; recent_path(p, sizeof p); FILE *f = fopen(p, "wb"); if (!f) return;
  for (int i = 0; i < nrecent; i++) { char d[48]; snprintf(d, sizeof d, "%s", recent[i].draw); for (char *q = d; *q; q++) if (*q == '\x01') *q = '\x03'; else if (*q == '\x02') *q = '\x04'; fprintf(f, "%s\t%s\t%s\n", d, recent[i].ins, recent[i].code); }
  fclose(f);
}
static Emo *server_emo; static int nserver;
/* every server's emoji, fetched one server at a time in the background */
typedef struct { char id[24], name[72]; } EG;
static EG *eg; static int eg_n, eg_next = -1, eg_job_i; static NxJob *eg_job; static Emo *all_emo; static int nall, allcap;
void ov_emoji_guilds(const Guild *g, int n) {
  if (eg || n <= 0) return; eg = calloc(n, sizeof *eg); if (!eg) return;
  for (int i = 0; i < n; i++) { snprintf(eg[i].id, 24, "%s", g[i].id); snprintf(eg[i].name, sizeof eg[i].name, "%s", g[i].name); } eg_n = n;
}
static void parse_emojis(const char *body, Emo **arr, int *cnt, int *cap, int gi) {
  json_t *a = json_loads(body, 0, NULL); size_t i; json_t *e;
  json_array_foreach(a, i, e) { const char *id = json_string_value(json_object_get(e, "id")), *nm = json_string_value(json_object_get(e, "name")); if (!id || !nm) continue;
    if (json_is_false(json_object_get(e, "available"))) continue;
    if (*cnt >= *cap) { int nc = *cap ? *cap * 2 : 256; Emo *g2 = realloc(*arr, nc * sizeof **arr); if (!g2) break; *arr = g2; *cap = nc; }
    bool an = json_is_true(json_object_get(e, "animated")); Emo *x = &(*arr)[(*cnt)++]; memset(x, 0, sizeof *x); x->gi = (short)gi;
    snprintf(x->draw, sizeof x->draw, "\x01" "c:%s\x02", id); snprintf(x->ins, sizeof x->ins, "<%s:%s:%s>", an ? "a" : "", nm, id); snprintf(x->code, sizeof x->code, "%s:%s", nm, id); }
  json_decref(a);
}
static bool cat_hidden(int c) { return (c == c_server && !e_guild[0]) || (c == c_all && !eg_n); }
static Emo *cat_item(int c, int i) { if (c == c_recent) return &recent[i]; if (c == c_server) return &server_emo[i]; if (c == c_all) return &all_emo[i]; return &emo[cats[c].start + i]; }
static int cat_n(int c) { if (c == c_recent) return nrecent; if (c == c_server) return nserver; if (c == c_all) return nall; return cats[c].n; }
void ov_open_emoji(int purpose, const char *guild_id, const char *msg_id) {
  emo_build(); recent_load(); e_purpose = purpose; snprintf(e_msg, 24, "%s", msg_id ? msg_id : ""); e_sel = 0; e_scroll = 0; kind = 2;
  if (strcmp(e_guild, guild_id ? guild_id : "")) { snprintf(e_guild, 24, "%s", guild_id ? guild_id : ""); free(server_emo); server_emo = NULL; nserver = 0;
    if (e_guild[0] && !e_job) { char p[80]; snprintf(p, sizeof p, "/guilds/%s/emojis", e_guild); e_job = dc_get(p); snprintf(e_job_guild, 24, "%s", e_guild); } }
  if (eg_next < 0 && eg_n) eg_next = 0;   /* start fetching every server's emoji */
  e_cat = nrecent ? c_recent : (e_guild[0] ? c_server : (eg_n ? c_all : c_people));
}
#define EX 360
#define EY 112
#define EW 880
#define EH 540
#define ECOLS 14
#define ETILE 60
#define EROWS 6
static void emo_pick(Emo *e, OvResult *r) {
  recent_push(e);
  if (e_purpose == EMO_REACT) { r->kind = OVR_REACT; snprintf(r->code, sizeof r->code, "%s", e->code); snprintf(r->msg_id, 24, "%s", e_msg); kind = 0; }
  else { r->kind = OVR_INSERT; snprintf(r->text, sizeof r->text, "%s", e->ins); }   /* the picker stays open to add more */
}
static void emo_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 120));
  nx_rrect(EX, EY, EW, EH, 12, C_DARK);
  nx_text(EX + 20, EY + 14, e_purpose == EMO_REACT ? "Add a reaction" : "Emoji", 1 | NX_BOLD, C_WHITE, 0);
  nx_text(EX + EW - 420, EY + 18, e_purpose == EMO_INSERT ? "L/R category   Y GIFs   A pick   B close" : "L/R category   A pick   B close", 0, C_MUTED, 0);
  int tx = EX + 16;                                                       /* category tabs */
  for (int c = 0; c < ncats; c++) { if (cat_hidden(c)) continue; int w = nx_text_w(cats[c].name, 0) + 22;
    if (c == e_cat) nx_rrect(tx, EY + 52, w, 32, 8, C_BLURPLE); nx_text(tx + 11, EY + 56, cats[c].name, 0, c == e_cat ? C_WHITE : C_MUTED, 0); tx += w + 6; }
  int n = cat_n(e_cat), gx = EX + (EW - ECOLS * ETILE) / 2, gy = EY + 100;
  if (e_cat == c_all && eg_next >= 0 && eg_next <= eg_n && (eg_job || eg_next < eg_n)) { char pb[64]; snprintf(pb, sizeof pb, "loading %d/%d servers", eg_next, eg_n); nx_text(EX + 160, EY + 18, pb, 0, C_MUTED, 0); }
  if (!n) { tcenter(EX + EW / 2, gy + 120, e_cat == c_server ? (e_job ? "Loading server emoji..." : "This server has no custom emoji") : e_cat == c_all ? (eg_job || eg_next < eg_n ? "Loading emoji from your servers..." : "None of your servers have custom emoji") : "Nothing here yet", 1, C_MUTED); return; }
  nx_clip(EX, gy, EW, EROWS * ETILE);
  for (int i = e_scroll * ECOLS; i < n && i < (e_scroll + EROWS) * ECOLS; i++) {
    int r = i / ECOLS - e_scroll, col = i % ECOLS, x = gx + col * ETILE, y = gy + r * ETILE;
    if (i == e_sel) nx_rrect(x + 2, y + 2, ETILE - 4, ETILE - 4, 8, C_SEL);
    Emo *e = cat_item(e_cat, i); int w = nx_text_w(e->draw, 2); nx_text(x + (ETILE - w) / 2, y + 12, e->draw, 2, C_WHITE, 0);
  }
  nx_unclip();
  Emo *s = cat_item(e_cat, e_sel < n ? e_sel : 0); char lab[120];
  if (s->ins[0] == '<') { const char *a = strchr(s->ins, ':'); const char *b = a ? strchr(a + 1, ':') : NULL; snprintf(lab, sizeof lab, ":%.*s:", b ? (int)(b - a - 1) : 0, a ? a + 1 : "");
    if (e_cat == c_all && s->gi >= 0 && s->gi < eg_n) { size_t L = strlen(lab); snprintf(lab + L, sizeof lab - L, "   from %s", eg[s->gi].name); } } else snprintf(lab, sizeof lab, "%s", s->draw);
  nx_text(EX + 24, EY + EH - 42, lab, 1, C_TEXT, EW - 60);
}
static void emo_fix_scroll(void) { int n = cat_n(e_cat); if (e_sel >= n) e_sel = n ? n - 1 : 0; int row = e_sel / ECOLS; if (row < e_scroll) e_scroll = row; if (row >= e_scroll + EROWS) e_scroll = row - EROWS + 1; }
static void emo_cat(int d) { int c = e_cat; for (int k = 0; k < ncats; k++) { c = (c + d + ncats) % ncats; if (cat_hidden(c)) continue; break; } e_cat = c; e_sel = 0; e_scroll = 0; }
static bool emo_input(const NxInput *in, OvResult *r) {
  int n = cat_n(e_cat);
  if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_Y] && e_purpose == EMO_INSERT) { ov_open_gifs(); return true; }
  if (in->pressed[B_R] || in->pressed[B_ZR]) emo_cat(1); if (in->pressed[B_L] || in->pressed[B_ZL]) emo_cat(-1);
  if (in->pressed[B_RIGHT] && e_sel + 1 < n) e_sel++; if (in->pressed[B_LEFT] && e_sel > 0) e_sel--;
  if (in->pressed[B_DOWN] && e_sel + ECOLS < n) e_sel += ECOLS; if (in->pressed[B_UP] && e_sel - ECOLS >= 0) e_sel -= ECOLS;
  if (fabsf(in->ry) > 0.5f) { static uint32_t t; if (nx_ticks() - t > 120) { t = nx_ticks(); e_scroll += in->ry > 0 ? 1 : -1; if (e_scroll < 0) e_scroll = 0; if (e_scroll * ECOLS >= n) e_scroll = n / ECOLS; e_sel = e_scroll * ECOLS; } }
  emo_fix_scroll();
  if (in->pressed[B_A] && n) emo_pick(cat_item(e_cat, e_sel), r);
  if (in->touch_pressed) {
    int tx = EX + 16; for (int c = 0; c < ncats; c++) { if (cat_hidden(c)) continue; int w = nx_text_w(cats[c].name, 0) + 22; if (hitp(in, tx, EY + 52, w, 32)) { e_cat = c; e_sel = 0; e_scroll = 0; return true; } tx += w + 6; }
    int gx = EX + (EW - ECOLS * ETILE) / 2, gy = EY + 100;
    if (in->tx >= gx && in->tx < gx + ECOLS * ETILE && in->ty >= gy && in->ty < gy + EROWS * ETILE) { int i = (e_scroll + (in->ty - gy) / ETILE) * ECOLS + (in->tx - gx) / ETILE; if (i < n) { e_sel = i; emo_pick(cat_item(e_cat, i), r); } }
    else if (!hitp(in, EX, EY, EW, EH)) kind = 0;
  }
  if (in->tdy && in->touch) { static int acc; acc += in->tdy; while (acc > ETILE) { acc -= ETILE; if (e_scroll > 0) e_scroll--; } while (acc < -ETILE) { acc += ETILE; if ((e_scroll + EROWS) * ECOLS < n) e_scroll++; } }
  return true;
}
static void emo_tick(void) {
  if (e_job && nxnet_job_done(e_job)) { NxResp r; nxnet_job_take(e_job, &r); e_job = NULL;
    if (r.status == 200 && !strcmp(e_job_guild, e_guild)) { json_t *a = json_loads(r.body, 0, NULL); size_t n = json_array_size(a), i; json_t *e;
      free(server_emo); server_emo = calloc(n + 1, sizeof *server_emo); nserver = 0; (void)parse_emojis;
      json_array_foreach(a, i, e) { const char *id = json_string_value(json_object_get(e, "id")), *nm = json_string_value(json_object_get(e, "name")); if (!id || !nm) continue;
        if (json_is_false(json_object_get(e, "available"))) continue; bool an = json_is_true(json_object_get(e, "animated")); Emo *o = &server_emo[nserver++]; o->gi = -1;
        snprintf(o->draw, sizeof o->draw, "\x01" "c:%s\x02", id); snprintf(o->ins, sizeof o->ins, "<%s:%s:%s>", an ? "a" : "", nm, id); snprintf(o->code, sizeof o->code, "%s:%s", nm, id); }
      json_decref(a); }
    nxnet_free(&r); }
  /* All servers: one request at a time, after the current server's own list */
  if (eg_job && nxnet_job_done(eg_job)) { NxResp r; nxnet_job_take(eg_job, &r); eg_job = NULL; if (r.status == 200 && r.body) parse_emojis(r.body, &all_emo, &nall, &allcap, eg_job_i); nxnet_free(&r); }
  if (!eg_job && !e_job && eg_next >= 0 && eg_next < eg_n) { char p[80]; eg_job_i = eg_next++; snprintf(p, sizeof p, "/guilds/%s/emojis", eg[eg_job_i].id); eg_job = dc_get(p); }
}

/* ======================= profile ======================= */
typedef struct { char id[24], name[80], user[40], pronouns[40], nick[80], avatar[48], banner[48], gavatar[48]; NxNameStyle ns; uint32_t accent; bool has_accent;
  char *bio; char since[24], joined[24]; int mutual; char badges[6][48]; int nbadges; char conn[6][80]; int nconn; bool loaded, failed, basic; } Prof;
static Prof P; static NxJob *p_job; static int p_stage; static char p_guild[24];
static unsigned long long p_perms; void ov_profile_perms(unsigned long long p) { p_perms = p; }
static NxJob *b_job; static SDL_Texture *b_tex; static char b_for[80]; static NxJob *dm_job; static int p_sel;
static void date_of(time_t t, char *o, size_t n) { struct tm tmv; if (t > 0 && gmtime_r(&t, &tmv)) strftime(o, n, "%b %d, %Y", &tmv); else snprintf(o, n, "?"); }
static time_t iso_t(const char *s) { int Y, M, D, h, mi, se; if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) != 6) return 0; struct tm tmv = {0}; tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se; return mktime(&tmv); }
static void prof_fetch(void) {
  char p[200]; if (p_stage == 0) snprintf(p, sizeof p, "/users/%s/profile?with_mutual_guilds=true&with_mutual_friends=false%s%s", P.id, p_guild[0] ? "&guild_id=" : "", p_guild);
  else snprintf(p, sizeof p, "/users/%s", P.id);
  p_job = dc_get(p);
}
void ov_open_profile(const char *user_id, const char *guild_id) {
  free(P.bio); memset(&P, 0, sizeof P); snprintf(P.id, 24, "%s", user_id); snprintf(p_guild, 24, "%s", guild_id ? guild_id : ""); p_stage = 0; p_sel = 0;
  if (p_job) { nxnet_job_cancel(p_job); p_job = NULL; } prof_fetch(); kind = 3;
  unsigned long long sf = strtoull(user_id, NULL, 10); date_of((time_t)(((sf >> 22) + 1420070400000ULL) / 1000), P.since, sizeof P.since);   /* account age from the snowflake */
}
static void prof_parse(const char *body) {
  json_t *r = json_loads(body, 0, NULL); if (!r) { P.failed = true; return; }
  json_t *u = p_stage == 0 ? json_object_get(r, "user") : r; json_t *up = json_object_get(r, "user_profile"), *gm = json_object_get(r, "guild_member");
  const char *gn = json_string_value(json_object_get(u, "global_name")), *un = json_string_value(json_object_get(u, "username"));
  snprintf(P.name, sizeof P.name, "%s", gn && gn[0] ? gn : un ? un : "?"); snprintf(P.user, sizeof P.user, "%s", un ? un : "");
  const char *av = json_string_value(json_object_get(u, "avatar")); snprintf(P.avatar, sizeof P.avatar, "%s", av ? av : "");
  const char *bn = json_string_value(json_object_get(up, "banner")); if (!bn) bn = json_string_value(json_object_get(u, "banner")); snprintf(P.banner, sizeof P.banner, "%s", bn ? bn : "");
  json_t *ac = json_object_get(up, "accent_color"); if (!json_is_integer(ac)) ac = json_object_get(u, "accent_color"); if (json_is_integer(ac)) { P.accent = (uint32_t)json_integer_value(ac); P.has_accent = true; }
  json_t *tc = json_object_get(up, "theme_colors"); if (json_array_size(tc)) { P.accent = (uint32_t)json_integer_value(json_array_get(tc, 0)); P.has_accent = true; }
  dc_parse_style(u, &P.ns);
  const char *pr = json_string_value(json_object_get(up, "pronouns")); snprintf(P.pronouns, sizeof P.pronouns, "%s", pr ? pr : "");
  const char *bio = json_string_value(json_object_get(up, "bio")); if (!bio) bio = json_string_value(json_object_get(u, "bio"));
  if (bio && bio[0]) P.bio = dc_clean_text(bio);
  if (gm) { const char *nk = json_string_value(json_object_get(gm, "nick")); snprintf(P.nick, sizeof P.nick, "%s", nk ? nk : ""); time_t jt = iso_t(json_string_value(json_object_get(gm, "joined_at"))); if (jt) date_of(jt, P.joined, sizeof P.joined);
    const char *ga = json_string_value(json_object_get(gm, "avatar")); snprintf(P.gavatar, sizeof P.gavatar, "%s", ga ? ga : ""); }
  P.mutual = (int)json_array_size(json_object_get(r, "mutual_guilds"));
  json_t *bs = json_object_get(r, "badges"); size_t i; json_t *b; json_array_foreach(bs, i, b) { if (P.nbadges >= 6) break; const char *d = json_string_value(json_object_get(b, "description")); if (d) snprintf(P.badges[P.nbadges++], 48, "%s", d); }
  json_t *cs = json_object_get(r, "connected_accounts"); json_array_foreach(cs, i, b) { if (P.nconn >= 6) break; const char *t = json_string_value(json_object_get(b, "type")), *n = json_string_value(json_object_get(b, "name")); if (t && n) snprintf(P.conn[P.nconn++], 80, "%s: %s", t, n); }
  P.loaded = true; P.basic = p_stage == 1; json_decref(r);
}
static void prof_tick(void) {
  if (p_job && nxnet_job_done(p_job)) { NxResp r; nxnet_job_take(p_job, &r); p_job = NULL;
    if (r.status == 200) prof_parse(r.body); else if (p_stage == 0) { p_stage = 1; prof_fetch(); } else P.failed = true;
    nxnet_free(&r); }
  if (P.loaded && P.banner[0]) { char key[80]; snprintf(key, sizeof key, "%s/%s", P.id, P.banner);
    if (strcmp(b_for, key)) { snprintf(b_for, sizeof b_for, "%s", key); if (b_tex) { SDL_DestroyTexture(b_tex); b_tex = NULL; } if (b_job) nxnet_job_cancel(b_job);
      char path[160], u[300]; snprintf(path, sizeof path, "banners/%s/%s.png?size=600", P.id, P.banner); dc_cdn_url(path, u, sizeof u); const char *h[] = {"User-Agent: Mozilla/5.0", NULL}; b_job = nxnet_start("GET", u, h, NULL, 0, 4u << 20); } }
  if (b_job && nxnet_job_done(b_job)) { NxResp r; nxnet_job_take(b_job, &r); b_job = NULL; if (r.status == 200 && r.len) b_tex = nx_tex_from_mem(r.body, (int)r.len, NULL, NULL); nxnet_free(&r); }
}
#define PX 340
#define PY 60
#define PW 600
#define PH 600
static void prof_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 160));
  nx_rrect(PX, PY, PW, PH, 14, C_DARK);
  uint32_t ac = P.has_accent ? P.accent : 0x5865F2;
  if (b_tex && P.banner[0]) nx_draw_tex(b_tex, PX, PY, PW, 200); else { nx_rrect(PX, PY, PW, 200, 14, UC(ac)); nx_rect(PX, PY + 186, PW, 14, UC(ac)); }
  nx_circle(PX + 90, PY + 200, 66, C_DARK);
  char ap[160]; SDL_Texture *at = NULL;
  if (P.gavatar[0] && p_guild[0]) { snprintf(ap, sizeof ap, "guilds/%s/users/%s/avatars/%s.png?size=128", p_guild, P.id, P.gavatar); at = imgfn ? imgfn(ap, 120) : NULL; }
  if (!at && P.avatar[0]) { snprintf(ap, sizeof ap, "avatars/%s/%s.png?size=128", P.id, P.avatar); at = imgfn ? imgfn(ap, 120) : NULL; }
  if (at) nx_draw_tex(at, PX + 30, PY + 140, 120, 120); else { nx_circle(PX + 90, PY + 200, 60, C_BLURPLE); char b[2] = {P.name[0] ? P.name[0] : '?', 0}; tcenter(PX + 90, PY + 170, b, 3 | NX_BOLD, C_WHITE); }
  int y = PY + 276;
  if (!P.loaded) { nx_text(PX + 30, y, P.failed ? "Couldn't load this profile" : "Loading profile...", 1, C_MUTED, 0); goto buttons; }
  nx_name(PX + 30, y, P.nick[0] ? P.nick : P.name, 2 | NX_BOLD, &P.ns, C_WHITE, PW - 60); y += 44;
  { char l[160]; snprintf(l, sizeof l, "%s%s%s", P.user, P.pronouns[0] ? "  -  " : "", P.pronouns); nx_text(PX + 30, y, l, 1, C_MUTED, PW - 60); y += 36; }
  if (P.nbadges) { int bx = PX + 30; for (int i = 0; i < P.nbadges; i++) { int w = nx_text_w(P.badges[i], 0) + 20; if (bx + w > PX + PW - 30) break; nx_rrect(bx, y, w, 28, 8, C_SIDE); nx_text(bx + 10, y + 2, P.badges[i], 0, C_TEXT, 0); bx += w + 8; } y += 38; }
  nx_rect(PX + 30, y, PW - 60, 1, C_SEL); y += 12;
  #define PLIM (PY + PH - 84)   /* content stops above the buttons */
  if (P.bio && y + 50 < PLIM) { nx_text(PX + 30, y, "ABOUT ME", 0 | NX_BOLD, C_MUTED, 0); y += 26; int lines = (PLIM - 56 - y) / nx_text_h(0); if (lines > 5) lines = 5; if (lines < 1) lines = 1; y += nx_wrap(PX + 30, y, P.bio, 0, C_TEXT, PW - 60, lines, true) + 8; }
  if (y + 26 < PLIM) { char l[120]; snprintf(l, sizeof l, "Member since %s%s%s", P.since, P.joined[0] ? "   -   Joined server " : "", P.joined); nx_text(PX + 30, y, l, 0, C_TEXT, PW - 60); y += 28; }
  if (P.mutual && y + 24 < PLIM) { char l[60]; snprintf(l, sizeof l, "%d mutual server%s", P.mutual, P.mutual == 1 ? "" : "s"); nx_text(PX + 30, y, l, 0, C_MUTED, 0); y += 26; }
  for (int i = 0; i < P.nconn && y + 22 < PLIM; i++) { nx_text(PX + 30, y, P.conn[i], 0, C_MUTED, PW - 60); y += 24; }
buttons:
  nx_rrect(PX + 24, PY + PH - 66, 176, 46, 10, p_sel == 0 ? C_BLURPLE : C_SEL); tcenter(PX + 112, PY + PH - 56, dm_job ? "Opening..." : "Message", 1 | NX_BOLD, C_WHITE);
  nx_rrect(PX + 212, PY + PH - 66, 176, 46, 10, p_sel == 1 ? C_BLURPLE : C_SEL); tcenter(PX + 300, PY + PH - 56, "Options", 1 | NX_BOLD, C_WHITE);
  nx_rrect(PX + 400, PY + PH - 66, 176, 46, 10, p_sel == 2 ? C_BLURPLE : C_SEL); tcenter(PX + 488, PY + PH - 56, "Close (B)", 1 | NX_BOLD, C_WHITE);
}
static void prof_open_dm(void) {
  if (dm_job) return; char body[80]; snprintf(body, sizeof body, "{\"recipient_id\":\"%s\"}", P.id); dm_job = dc_request("POST", "/users/@me/channels", body);
}
static bool prof_input(const NxInput *in, OvResult *r) {
  if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_LEFT] && p_sel > 0) p_sel--; if (in->pressed[B_RIGHT] && p_sel < 2) p_sel++;
  int pb = -1; if (in->pressed[B_A]) pb = p_sel;
  if (hitp(in, PX + 24, PY + PH - 66, 176, 46)) pb = 0; else if (hitp(in, PX + 212, PY + PH - 66, 176, 46)) pb = 1; else if (hitp(in, PX + 400, PY + PH - 66, 176, 46)) pb = 2; else if (in->touch_pressed && !hitp(in, PX, PY, PW, PH)) kind = 0;
  if (pb == 0) prof_open_dm(); else if (pb == 1) { ov_open_member(P.id, P.nick[0] ? P.nick : P.name, p_guild, p_perms, false, false, false); return true; } else if (pb == 2) kind = 0;
  if (dm_job && nxnet_job_done(dm_job)) { NxResp rr; nxnet_job_take(dm_job, &rr); dm_job = NULL;
    if (rr.status == 200) { json_t *j = json_loads(rr.body, 0, NULL); const char *id = j ? json_string_value(json_object_get(j, "id")) : NULL;
      if (id) { r->kind = OVR_OPEN_DM; snprintf(r->chan_id, 24, "%s", id); snprintf(r->name, sizeof r->name, "%s", P.name); kind = 0; } json_decref(j); }
    nxnet_free(&rr); }
  return true;
}

/* ======================= GIF picker ======================= */
typedef struct { char url[200], prev[260]; int w, h; SDL_Texture *tex; int tw, th; int state; NxJob *job; Anim *an; } Gif;   /* state 0 want, 1 loading, 2 ok, 3 failed */
static Gif gifs[50]; static int ngifs, g_sel, g_scroll; static NxJob *g_job; static char g_query[100], g_shown[100]; static bool g_loaded_once;
#define GCOLS 4
#define GTW 206
#define GTH 116
#define GROWS 3
static void gifs_clear(void) { for (int i = 0; i < ngifs; i++) { if (gifs[i].tex) SDL_DestroyTexture(gifs[i].tex); if (gifs[i].job) nxnet_job_cancel(gifs[i].job); if (gifs[i].an) anim_close(gifs[i].an); } memset(gifs, 0, sizeof gifs); ngifs = 0; g_sel = g_scroll = 0; }
static void gifs_fetch(void) {
  if (g_job) nxnet_job_cancel(g_job); char p[300], q[220];
  if (g_query[0]) { dc_urlenc(g_query, q, sizeof q); snprintf(p, sizeof p, "/gifs/search?q=%s&provider=klipy&locale=en-US&media_format=mp4", q); }
  else snprintf(p, sizeof p, "/gifs/trending-gifs?provider=klipy&locale=en-US&media_format=mp4");
  g_job = dc_get(p); snprintf(g_shown, sizeof g_shown, "%s", g_query);
}
void ov_open_gifs(void) { kind = 4; if (!g_loaded_once) { g_loaded_once = true; gifs_fetch(); } }
/* first frame of an (animated) WebP, scaled to fit w x h -> texture */
static SDL_Texture *webp_first(const uint8_t *d, size_t n, int fw, int fh, int *ow, int *oh) {
  WebPData wd = {d, n}; WebPAnimDecoderOptions o; WebPAnimDecoderOptionsInit(&o); o.color_mode = MODE_RGBA; o.use_threads = 0;
  WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &o); if (!dec) return NULL; WebPAnimInfo ai; WebPAnimDecoderGetInfo(dec, &ai);
  uint8_t *buf = NULL; int ts = 0; SDL_Texture *t = NULL;
  if (WebPAnimDecoderGetNext(dec, &buf, &ts) && buf) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(buf, ai.canvas_width, ai.canvas_height, 32, ai.canvas_width * 4, SDL_PIXELFORMAT_RGBA32);
    float sc = fminf((float)fw / ai.canvas_width, (float)fh / ai.canvas_height); if (sc > 1) sc = 1; int w = (int)(ai.canvas_width * sc), h = (int)(ai.canvas_height * sc); if (w < 1) w = 1; if (h < 1) h = 1;
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (s && dst) { SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE); SDL_BlitScaled(s, NULL, dst, NULL); t = SDL_CreateTextureFromSurface(SDL_GetRenderer(SDL_GL_GetCurrentWindow() ? SDL_GL_GetCurrentWindow() : NULL) ? SDL_GetRenderer(SDL_GL_GetCurrentWindow()) : nx_renderer(), dst); *ow = w; *oh = h; }
    if (s) SDL_FreeSurface(s); if (dst) SDL_FreeSurface(dst);
  }
  WebPAnimDecoderDelete(dec); return t;
}
static void gif_tick(void) {
  if (g_job && nxnet_job_done(g_job)) { NxResp r; nxnet_job_take(g_job, &r); g_job = NULL;
    if (r.status == 200) { gifs_clear(); json_t *a = json_loads(r.body, 0, NULL); size_t i; json_t *e;
      json_array_foreach(a, i, e) { if (ngifs >= 50) break; const char *u = json_string_value(json_object_get(e, "url")), *pv = json_string_value(json_object_get(e, "gif_src")); if (!u || !pv) continue;
        Gif *g = &gifs[ngifs++]; snprintf(g->url, sizeof g->url, "%s", u); snprintf(g->prev, sizeof g->prev, "%s", pv);
        json_t *w = json_object_get(e, "width"), *h = json_object_get(e, "height"); g->w = json_is_integer(w) ? (int)json_integer_value(w) : atoi(json_string_value(w) ? json_string_value(w) : "0"); g->h = json_is_integer(h) ? (int)json_integer_value(h) : atoi(json_string_value(h) ? json_string_value(h) : "0"); }
      json_decref(a); }
    nxnet_free(&r); }
  if (kind != 4) { for (int i = 0; i < ngifs; i++) if (gifs[i].an) { anim_close(gifs[i].an); gifs[i].an = NULL; } return; }
  for (int i = 0; i < ngifs; i++) { Gif *g = &gifs[i]; bool vis = i >= g_scroll * GCOLS && i < (g_scroll + GROWS) * GCOLS;     /* animated previews for the visible tiles */
    if (vis && !g->an && g->state == 2) g->an = anim_open_webp(g->prev, GTW, GTH);
    else if (!vis && g->an) { anim_close(g->an); g->an = NULL; } }
  int loading = 0; for (int i = 0; i < ngifs; i++) if (gifs[i].job) loading++;
  for (int i = 0; i < ngifs; i++) { Gif *g = &gifs[i];
    if (g->job && nxnet_job_done(g->job)) { NxResp r; nxnet_job_take(g->job, &r); g->job = NULL; loading--;
      if (r.status == 200 && r.len) { g->tex = webp_first((const uint8_t *)r.body, r.len, GTW, GTH, &g->tw, &g->th); if (!g->tex) g->tex = nx_tex_from_mem(r.body, (int)r.len, &g->tw, &g->th); }
      g->state = g->tex ? 2 : 3; nxnet_free(&r); }
    else if (g->state == 0 && loading < 4 && i >= g_scroll * GCOLS && i < (g_scroll + GROWS + 1) * GCOLS) { const char *hh[] = {"User-Agent: Mozilla/5.0", NULL}; g->state = 1; g->job = nxnet_start("GET", g->prev, hh, NULL, 0, 6u << 20); loading++; } }
}
#define GX (EX + (EW - GCOLS * (GTW + 10)) / 2)
#define GY (EY + 150)
static void gif_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 120)); nx_rrect(EX, EY, EW, EH, 12, C_DARK);
  nx_rrect(EX + 16, EY + 12, 110, 34, 8, C_SEL); nx_text(EX + 34, EY + 16, "Emoji", 0, C_MUTED, 0);
  nx_rrect(EX + 132, EY + 12, 100, 34, 8, C_BLURPLE); nx_text(EX + 154, EY + 16, "GIFs", 0 | NX_BOLD, C_WHITE, 0);
  nx_text(EX + EW - 360, EY + 18, "Y emoji   X search   A send   B close", 0, C_MUTED, 0);
  nx_rrect(EX + 16, EY + 60, EW - 32, 50, 10, RGB(56, 58, 64));
  nx_text(EX + 34, EY + 72, g_query[0] ? g_query : "Search Klipy (tap or press X)", 1, g_query[0] ? C_TEXT : C_MUTED, EW - 80);
  nx_text(EX + 20, EY + 118, g_shown[0] ? "Results" : "Trending", 0 | NX_BOLD, C_MUTED, 0);
  if (!ngifs) { tcenter(EX + EW / 2, GY + 140, g_job ? "Loading GIFs..." : "No GIFs found", 1, C_MUTED); return; }
  nx_clip(EX, GY, EW, GROWS * (GTH + 10));
  for (int i = g_scroll * GCOLS; i < ngifs && i < (g_scroll + GROWS) * GCOLS; i++) { Gif *g = &gifs[i];
    int r = i / GCOLS - g_scroll, col = i % GCOLS, x = GX + col * (GTW + 10), y = GY + r * (GTH + 10);
    nx_rrect(x, y, GTW, GTH, 8, C_SIDE);
    int aw = 0, ah = 0; SDL_Texture *at = NULL; if (g->an) { anim_tick(g->an); at = anim_frame(g->an, &aw, &ah); if (at) { float s = fminf((float)GTW / aw, (float)GTH / ah); if (s > 1) s = 1; aw = (int)(aw * s); ah = (int)(ah * s); } }
    if (at) nx_draw_tex(at, x + (GTW - aw) / 2, y + (GTH - ah) / 2, aw, ah);
    else if (g->tex) { int w = g->tw, h = g->th; nx_draw_tex(g->tex, x + (GTW - w) / 2, y + (GTH - h) / 2, w, h); } else tcenter(x + GTW / 2, y + GTH / 2 - 10, g->state == 3 ? "x" : "...", 1, C_MUTED);
    if (i == g_sel) { nx_rect(x, y, GTW, 3, C_BLURPLE); nx_rect(x, y + GTH - 3, GTW, 3, C_BLURPLE); nx_rect(x, y, 3, GTH, C_BLURPLE); nx_rect(x + GTW - 3, y, 3, GTH, C_BLURPLE); } }
  nx_unclip();
}
static bool gif_input(const NxInput *in, OvResult *r) {
  if (in->text[0]) { nx_stop_text(); char t[100]; snprintf(t, sizeof t, "%s", in->text); char *nl = strchr(t, '\n'); if (nl) *nl = 0; snprintf(g_query, sizeof g_query, "%s", t); gifs_fetch(); return true; }
  if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_Y]) { kind = 2; return true; }
  if (in->pressed[B_X] || hitp(in, EX + 16, EY + 60, EW - 32, 50)) { nx_start_text("Search GIFs", g_query); return true; }
  if (hitp(in, EX + 16, EY + 12, 110, 34)) { kind = 2; return true; }
  if (in->pressed[B_RIGHT] && g_sel + 1 < ngifs) g_sel++; if (in->pressed[B_LEFT] && g_sel > 0) g_sel--;
  if (in->pressed[B_DOWN] && g_sel + GCOLS < ngifs) g_sel += GCOLS; if (in->pressed[B_UP] && g_sel >= GCOLS) g_sel -= GCOLS;
  { int row = g_sel / GCOLS; if (row < g_scroll) g_scroll = row; if (row >= g_scroll + GROWS) g_scroll = row - GROWS + 1; }
  if (in->tdy && in->touch) { static int acc; acc += in->tdy; while (acc > GTH) { acc -= GTH; if (g_scroll > 0) g_scroll--; } while (acc < -GTH) { acc += GTH; if ((g_scroll + GROWS) * GCOLS < ngifs) g_scroll++; } }
  int pick = -1; if (in->pressed[B_A] && ngifs) pick = g_sel;
  if (in->touch_released && in->tx >= GX && in->ty >= GY && in->ty < GY + GROWS * (GTH + 10)) { int col = (in->tx - GX) / (GTW + 10), row = (in->ty - GY) / (GTH + 10); int i = (g_scroll + row) * GCOLS + col; if (col < GCOLS && i < ngifs) pick = i; }
  else if (in->touch_pressed && !hitp(in, EX, EY, EW, EH)) kind = 0;
  if (pick >= 0) { r->kind = OVR_SEND_TEXT; snprintf(r->text, sizeof r->text, "%s", gifs[pick].url); kind = 0; }
  return true;
}


/* ======================= people in the call + member menu (volume, local mute, moderation) ======================= */
#include "voice.h"
#include "cam.h"
static char m_uid[24], m_name[80], m_guild[24]; static unsigned long long m_perms; static bool m_voice, m_smute, m_sdeaf; static int m_sel; static int m_confirm = -1;
static char m_status[160]; static NxJob *m_job; static float m_prev_gain = 1.0f;
enum { MI_VOL, MI_LMUTE, MI_PROFILE, MI_SMUTE, MI_SDEAF, MI_DISC, MI_TIMEOUT, MI_UNTIMEOUT, MI_KICK, MI_BAN, MI_CLOSE };
static int m_items[16], m_n;
static void vol_save(void) {
  char p[300]; snprintf(p, sizeof p, "%s/uservol.txt", dc_dir()); FILE *f = fopen(p, "wb"); if (!f) return;
  for (int i = 0; i < 96; i++) (void)i;   /* table lives in voice.c; we write the users we know about */
  extern void ov__vol_dump(FILE *f); ov__vol_dump(f); fclose(f);
}
static char vk_ids[96][24]; static int vk_n;                           /* users whose volume we changed (to persist) */
void ov__vol_dump(FILE *f) { fprintf(f, "mic2 %.2f %d\n", voice_get_mic_gain(), voice_get_mic_agc() ? 1 : 0); { float ev, sa; cam_get_adjust(&ev, &sa); fprintf(f, "cam %.2f %.2f\n", ev, sa); } { VoiceSettings s; voice_get_settings(&s); fprintf(f, "vset %d %d %d %d %d %d %d %.2f\n", s.ns, s.echo, s.ptt, s.music, s.fec, s.sens, s.kbps, s.out_gain); } for (int i = 0; i < vk_n; i++) fprintf(f, "%s %.2f\n", vk_ids[i], voice_get_user_gain(vk_ids[i])); }
static void vol_touch(const char *uid) { for (int i = 0; i < vk_n; i++) if (!strcmp(vk_ids[i], uid)) return; if (vk_n < 96) snprintf(vk_ids[vk_n++], 24, "%s", uid); }
void ov_load_volumes(void) {
  char p[300]; snprintf(p, sizeof p, "%s/uservol.txt", dc_dir()); FILE *f = fopen(p, "rb"); if (!f) return; char ln[96];
  while (fgets(ln, sizeof ln, f)) { char id[32]; float g; int a = 0;
    { float ev, sa; if (sscanf(ln, "cam %f %f", &ev, &sa) == 2) { cam_set_adjust(ev, sa); continue; } }
    if (sscanf(ln, "mic2 %f %d", &g, &a) >= 1) { voice_set_mic_gain(g); voice_set_mic_agc(a != 0); continue; }
    if (sscanf(ln, "mic %f %d", &g, &a) >= 1) { voice_set_mic_gain(1.0f); voice_set_mic_agc(a != 0); continue; }   /* old versions read the mic wrong (no cache invalidation): their boosted gain would now be far too loud */
    { int ns, ec, pt, mu, fe, se, kb; float og; if (sscanf(ln, "vset %d %d %d %d %d %d %d %f", &ns, &ec, &pt, &mu, &fe, &se, &kb, &og) == 8) { VoiceSettings s = {ns != 0, ec != 0, pt != 0, mu != 0, fe != 0, se, kb, og}; voice_set_settings(&s); continue; } }
    if (sscanf(ln, "%31s %f", id, &g) == 2) { voice_set_user_gain(id, g); vol_touch(id); } } fclose(f);
}
void ov_open_member(const char *uid, const char *name, const char *guild_id, unsigned long long perms, bool in_voice, bool srv_mute, bool srv_deaf) {
  snprintf(m_uid, 24, "%s", uid); snprintf(m_name, 80, "%s", name ? name : uid); snprintf(m_guild, 24, "%s", guild_id ? guild_id : ""); m_perms = m_guild[0] ? perms : 0;
  m_voice = in_voice; m_smute = srv_mute; m_sdeaf = srv_deaf; m_sel = 0; m_confirm = -1; m_status[0] = 0; kind = 5;
  m_n = 0; m_items[m_n++] = MI_VOL; m_items[m_n++] = MI_LMUTE; m_items[m_n++] = MI_PROFILE;
  if (m_voice && (m_perms & P_MUTE)) m_items[m_n++] = MI_SMUTE;
  if (m_voice && (m_perms & P_DEAFEN)) m_items[m_n++] = MI_SDEAF;
  if (m_voice && (m_perms & P_MOVE)) m_items[m_n++] = MI_DISC;
  if (m_perms & P_TIMEOUT) { m_items[m_n++] = MI_TIMEOUT; m_items[m_n++] = MI_UNTIMEOUT; }
  if (m_perms & P_KICK) m_items[m_n++] = MI_KICK;
  if (m_perms & P_BAN) m_items[m_n++] = MI_BAN;
  m_items[m_n++] = MI_CLOSE;
}
static void m_rest(const char *method, const char *path, const char *body, const char *what) {
  if (m_job) return; m_job = dc_request(method, path, body); snprintf(m_status, sizeof m_status, "%s...", what);
}
static void m_do(int it, OvResult *r) {
  char p[160], b[160];
  switch (it) {
    case MI_LMUTE: { float g = voice_get_user_gain(m_uid); if (g > 0.001f) { m_prev_gain = g; voice_set_user_gain(m_uid, 0); } else voice_set_user_gain(m_uid, m_prev_gain > 0.001f ? m_prev_gain : 1.0f); vol_touch(m_uid); vol_save(); break; }
    case MI_PROFILE: kind = 0; r->kind = OVR_PROFILE; snprintf(r->user_id, 24, "%s", m_uid); break;
    case MI_SMUTE: snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); snprintf(b, sizeof b, "{\"mute\":%s}", m_smute ? "false" : "true"); m_rest("PATCH", p, b, m_smute ? "Unmuting" : "Server muting"); m_smute = !m_smute; break;
    case MI_SDEAF: snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); snprintf(b, sizeof b, "{\"deaf\":%s}", m_sdeaf ? "false" : "true"); m_rest("PATCH", p, b, m_sdeaf ? "Undeafening" : "Server deafening"); m_sdeaf = !m_sdeaf; break;
    case MI_DISC: snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); m_rest("PATCH", p, "{\"channel_id\":null}", "Disconnecting"); break;
    case MI_TIMEOUT: { time_t t = time(NULL) + 600; struct tm tmv; char ts[40] = ""; if (gmtime_r(&t, &tmv)) strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
      snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); snprintf(b, sizeof b, "{\"communication_disabled_until\":\"%s\"}", ts); m_rest("PATCH", p, b, "Timing out (10 min)"); break; }
    case MI_UNTIMEOUT: snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); m_rest("PATCH", p, "{\"communication_disabled_until\":null}", "Removing timeout"); break;
    case MI_KICK: snprintf(p, sizeof p, "/guilds/%s/members/%s", m_guild, m_uid); m_rest("DELETE", p, "", "Kicking"); break;
    case MI_BAN: snprintf(p, sizeof p, "/guilds/%s/bans/%s", m_guild, m_uid); m_rest("PUT", p, "{\"delete_message_seconds\":0}", "Banning"); break;
    case MI_CLOSE: kind = 0; break;
  }
}
static const char *m_label(int it, char *buf, size_t n) {
  switch (it) {
    case MI_VOL: { int pct = (int)(voice_get_user_gain(m_uid) * 100 + 0.5f); snprintf(buf, n, "Volume %d%%", pct); return buf; }
    case MI_LMUTE: return voice_get_user_gain(m_uid) > 0.001f ? "Mute for me" : "Unmute for me";
    case MI_PROFILE: return "View profile";
    case MI_SMUTE: return m_smute ? "Server unmute" : "Server mute";
    case MI_SDEAF: return m_sdeaf ? "Server undeafen" : "Server deafen";
    case MI_DISC: return "Disconnect from voice";
    case MI_TIMEOUT: return "Timeout for 10 minutes";
    case MI_UNTIMEOUT: return "Remove timeout";
    case MI_KICK: return "Kick from server";
    case MI_BAN: return "Ban from server";
    default: return "Close";
  }
}
#define MX 440
#define MY 70
#define MW 400
static void member_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 150));
  int h = 110 + m_n * 46; nx_rrect(MX, MY, MW, h, 12, C_DARK);
  nx_text(MX + 20, MY + 16, m_name, 1 | NX_BOLD, C_WHITE, MW - 40);
  for (int i = 0; i < m_n; i++) { int y = MY + 58 + i * 46, it = m_items[i]; char buf[64]; bool danger = it == MI_KICK || it == MI_BAN;
    if (i == m_sel) nx_rrect(MX + 10, y, MW - 20, 40, 8, danger ? RGB(237, 66, 69) : C_BLURPLE);
    if (it == MI_VOL) { float g = voice_get_user_gain(m_uid); nx_rrect(MX + 230, y + 16, 140, 8, 4, C_SEL); nx_rrect(MX + 230, y + 16, (int)(140 * g / 3.0f), 8, 4, g > 1.0f ? RGB(240, 178, 50) : C_GREEN); }
    nx_text(MX + 26, y + 8, m_label(it, buf, sizeof buf), 1, i == m_sel ? C_WHITE : danger ? RGB(237, 66, 69) : C_TEXT, it == MI_VOL ? 190 : MW - 60); }
  int sy = MY + 58 + m_n * 46 + 6;
  if (m_confirm >= 0) { nx_rrect(MX, sy, MW, 70, 10, RGB(60, 20, 22)); char q[120]; snprintf(q, sizeof q, "%s %s?", m_items[m_confirm] == MI_BAN ? "Ban" : "Kick", m_name); nx_text(MX + 20, sy + 8, q, 1 | NX_BOLD, C_WHITE, MW - 40); nx_text(MX + 20, sy + 38, "A  confirm      B  cancel", 0, RGB(237, 66, 69), 0); }
  else if (m_status[0]) nx_text(MX + 20, sy, m_status, 0, C_MUTED, MW - 40);
  else nx_text(MX + 20, sy, "Left/Right: volume (up to 300%)", 0, C_MUTED, MW - 40);
}
static bool member_input(const NxInput *in, OvResult *r) {
  if (m_job && nxnet_job_done(m_job)) { NxResp rr; nxnet_job_take(m_job, &rr); m_job = NULL; if (rr.status >= 200 && rr.status < 300) snprintf(m_status, sizeof m_status, "Done."); else snprintf(m_status, sizeof m_status, "Failed: %s", dc_error_text(&rr)); nxnet_free(&rr); }
  if (m_confirm >= 0) { if (in->pressed[B_A]) { m_do(m_items[m_confirm], r); m_confirm = -1; } else if (in->pressed[B_B] || in->touch_pressed) m_confirm = -1; return true; }
  if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_DOWN] && m_sel < m_n - 1) m_sel++; if (in->pressed[B_UP] && m_sel > 0) m_sel--;
  if (m_items[m_sel] == MI_VOL && (in->pressed[B_LEFT] || in->pressed[B_RIGHT])) { float g = voice_get_user_gain(m_uid) + (in->pressed[B_RIGHT] ? 0.1f : -0.1f); voice_set_user_gain(m_uid, g); vol_touch(m_uid); vol_save(); }
  int pick = -1; if (in->pressed[B_A]) pick = m_sel;
  if (in->touch_pressed) { for (int i = 0; i < m_n; i++) if (hitp(in, MX + 10, MY + 58 + i * 46, MW - 20, 40)) { m_sel = i; if (m_items[i] == MI_VOL) { float g = (in->tx - (MX + 230)) / 140.0f * 3.0f; if (g >= 0 && in->tx >= MX + 220) { voice_set_user_gain(m_uid, g); vol_touch(m_uid); vol_save(); } } else pick = i; }
    if (!hitp(in, MX, MY, MW, 130 + m_n * 46)) kind = 0; }
  if (pick >= 0 && m_items[pick] != MI_VOL) { if (m_items[pick] == MI_KICK || m_items[pick] == MI_BAN) m_confirm = pick; else m_do(m_items[pick], r); }
  return true;
}
/* voice settings (input volume, AGC, sensitivity, noise suppression, echo, PTT, Opus mode/bitrate/FEC, output volume) */
static int mic_sel;
#define VS_N 14
static bool lo_confirm;   /* log out needs a second press */
static const int kbps_steps[] = {16, 24, 32, 48, 64, 96, 128};
void ov_open_mic(void) { mic_sel = 0; lo_confirm = false; kind = 7; }
static void vs_label(int i, char *b, size_t n) {
  VoiceSettings s; voice_get_settings(&s);
  switch (i) {
    case 0: snprintf(b, n, "Input volume   %d%%", (int)(voice_get_mic_gain() * 100 + 0.5f)); break;
    case 1: snprintf(b, n, "Automatic gain control   %s", voice_get_mic_agc() ? "On" : "Off"); break;
    case 2: if (s.sens) snprintf(b, n, "Input sensitivity   %d", s.sens); else snprintf(b, n, "Input sensitivity   Automatic"); break;
    case 3: snprintf(b, n, "Noise suppression   %s", s.ns ? "On" : "Off"); break;
    case 4: snprintf(b, n, "Echo reduction   %s", s.echo ? "On" : "Off"); break;
    case 5: snprintf(b, n, "Push to talk (hold ZR)   %s", s.ptt ? "On" : "Off"); break;
    case 6: snprintf(b, n, "Opus mode   %s", s.music ? "Music (no voice processing)" : "Voice"); break;
    case 7: snprintf(b, n, "Bitrate   %d kbps", s.kbps); break;
    case 8: snprintf(b, n, "Loss protection (FEC)   %s", s.fec ? "On" : "Off"); break;
    case 9: snprintf(b, n, "Output volume   %d%%", (int)(s.out_gain * 100 + 0.5f)); break;
    case 10: snprintf(b, n, "Mic test (hear yourself)   %s", voice_mic_testing() ? "ON - talk now" : "Off"); break;
    case 11: { float ev, sa; cam_get_adjust(&ev, &sa); snprintf(b, n, "Camera exposure   %+.2f", ev); break; }
    case 12: { float ev, sa; cam_get_adjust(&ev, &sa); snprintf(b, n, "Camera saturation   %d%%", (int)(sa * 100 + 0.5f)); break; }
    default: snprintf(b, n, "%s", lo_confirm ? "Log out?  Press A again to confirm" : "Log out"); break;
  }
}
static void vs_change(int i, int dir) {               /* dir: -1/+1 for sliders, 0 = toggle */
  VoiceSettings s; voice_get_settings(&s);
  switch (i) {
    case 0: voice_set_mic_gain(voice_get_mic_gain() + (dir ? dir * 0.1f : 0.1f)); break;
    case 1: voice_set_mic_agc(!voice_get_mic_agc()); break;
    case 2: s.sens = dir ? s.sens + dir * 5 : (s.sens ? 0 : 20); if (s.sens < 0) s.sens = 0; if (s.sens > 100) s.sens = 100; break;
    case 3: s.ns = !s.ns; break; case 4: s.echo = !s.echo; break; case 5: s.ptt = !s.ptt; break; case 6: s.music = !s.music; break;
    case 7: { int k = 0; for (int q = 0; q < 7; q++) if (kbps_steps[q] <= s.kbps) k = q; k += dir ? dir : 1; if (k < 0) k = 0; if (k > 6) k = dir ? 6 : 0; s.kbps = kbps_steps[k]; break; }
    case 8: s.fec = !s.fec; break;
    case 10: voice_mic_test(!voice_mic_testing()); return;
    case 11: { float ev, sa; cam_get_adjust(&ev, &sa); ev += dir ? dir * 0.25f : 0.25f; if (ev > 1.51f) ev = dir ? 1.5f : -1.5f; cam_set_adjust(ev, sa); vol_save(); return; }
    case 12: { float ev, sa; cam_get_adjust(&ev, &sa); sa += dir ? dir * 0.1f : 0.1f; if (sa > 2.01f) sa = dir ? 2.0f : 0.0f; cam_set_adjust(ev, sa); vol_save(); return; }
    case 13: return;   /* log out: handled in mic_input (needs the result) */
    default: s.out_gain += dir ? dir * 0.1f : 0.1f; if (s.out_gain > 2.0f) s.out_gain = dir ? 2.0f : 0.0f; if (s.out_gain < 0) s.out_gain = 0; break;
  }
  if (i >= 2) voice_set_settings(&s); vol_save();
}
#define VSX 300
#define VSY 20
#define VS_TOP 64
#define VS_RH 39
#define VSW 680
static void mic_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 160)); nx_rrect(VSX, VSY, VSW, 684, 14, C_DARK);
  nx_text(VSX + 24, VSY + 14, "Settings", 2 | NX_BOLD, C_WHITE, 0);
  for (int i = 0; i < VS_N; i++) { int y = VSY + VS_TOP + i * VS_RH; char b[96]; vs_label(i, b, sizeof b);
    if (i == 11) nx_rect(VSX + 24, y - 2, VSW - 48, 1, C_SEL);   /* camera section */
    if (i == 13) nx_rect(VSX + 24, y - 2, VSW - 48, 1, C_SEL);
    if (i == mic_sel) nx_rrect(VSX + 12, y, VSW - 24, VS_RH - 4, 8, i == 13 && lo_confirm ? RGB(218, 55, 60) : C_BLURPLE); nx_text(VSX + 28, y + 6, b, 1, i == 13 && !(lo_confirm && i == mic_sel) ? RGB(250, 120, 124) : C_WHITE, VSW - 60);
    if (i == 6 || i == 7) { } }
  int y = VSY + VS_TOP + VS_N * VS_RH + 8; nx_text(VSX + 28, y, "Mic level", 0 | NX_BOLD, C_MUTED, 0);
  int lv = voice_mic_level(); float lf = lv / 32767.0f; if (lf > 1) lf = 1; nx_rrect(VSX + 140, y + 6, VSW - 180, 14, 6, C_SEL);
  nx_rrect(VSX + 140, y + 6, (int)((VSW - 180) * lf), 14, 6, lv > 26000 ? RGB(237, 66, 69) : lv > voice_vad_threshold() ? C_GREEN : C_MUTED);
  int thx = VSX + 140 + (int)((VSW - 180) * (voice_vad_threshold() / 32767.0f)); nx_rect(thx, y + 2, 2, 22, C_WHITE);
  nx_text(VSX + 28, y + 32, "Up/Down select   Left/Right adjust   A toggle   B close", 0, C_MUTED, 0);
}
static bool mic_input(const NxInput *in, OvResult *r) {
  if (in->pressed[B_B]) { if (lo_confirm) { lo_confirm = false; return true; } kind = 0; vol_save(); if (voice_mic_testing()) voice_mic_test(false); return true; }
  int prev_sel = mic_sel; if (in->pressed[B_DOWN] && mic_sel < VS_N - 1) mic_sel++; if (in->pressed[B_UP] && mic_sel > 0) mic_sel--; if (mic_sel != prev_sel) lo_confirm = false;
  if (mic_sel == 13 && (in->pressed[B_A] || (in->touch_pressed && hitp(in, VSX + 12, VSY + VS_TOP + 13 * VS_RH, VSW - 24, VS_RH - 4)))) { if (lo_confirm) { lo_confirm = false; kind = 0; vol_save(); r->kind = OVR_LOGOUT; } else lo_confirm = true; return true; }
  bool slider = mic_sel == 0 || mic_sel == 2 || mic_sel == 7 || mic_sel == 9 || mic_sel == 11 || mic_sel == 12;
  if (in->pressed[B_LEFT]) vs_change(mic_sel, slider ? -1 : 0); else if (in->pressed[B_RIGHT]) vs_change(mic_sel, slider ? 1 : 0); else if (in->pressed[B_A]) vs_change(mic_sel, 0);
  if (in->touch_pressed) { for (int i = 0; i < VS_N; i++) if (hitp(in, VSX + 12, VSY + VS_TOP + i * VS_RH, VSW - 24, VS_RH - 4)) { if (i == 13) { mic_sel = 13; lo_confirm = true; return true; } bool sl = i == 0 || i == 2 || i == 7 || i == 9 || i == 11 || i == 12; mic_sel = i; lo_confirm = false; vs_change(i, sl ? (in->tx > VSX + VSW / 2 ? 1 : -1) : 0); return true; }
    if (!hitp(in, VSX, VSY, VSW, 684)) { kind = 0; vol_save(); if (voice_mic_testing()) voice_mic_test(false); } }
  return true;
}
/* people in the call: pick one to open the member menu */
static char pp_ids[48][24], pp_names[48][80]; static bool pp_mute[48], pp_deaf[48]; static int pp_n, pp_sel; static char pp_guild[24]; static unsigned long long pp_perms;
void ov_open_people(int n, const char ids[][24], const char names[][80], const bool *mute, const bool *deaf, const char *guild_id, unsigned long long perms) {
  pp_n = n > 48 ? 48 : n; for (int i = 0; i < pp_n; i++) { snprintf(pp_ids[i], 24, "%s", ids[i]); snprintf(pp_names[i], 80, "%s", names[i]); pp_mute[i] = mute ? mute[i] : false; pp_deaf[i] = deaf ? deaf[i] : false; }
  snprintf(pp_guild, 24, "%s", guild_id ? guild_id : ""); pp_perms = perms; pp_sel = 0; kind = 6;
}
static void people_draw(void) {
  nx_rect(0, 0, NX_W, NX_H, RGBA(0, 0, 0, 150)); int h = 80 + (pp_n ? pp_n : 1) * 44; if (h > 640) h = 640; nx_rrect(MX, MY, MW, h, 12, C_DARK);
  nx_text(MX + 20, MY + 16, "People in this call", 1 | NX_BOLD, C_WHITE, 0);
  if (!pp_n) nx_text(MX + 20, MY + 60, "Nobody else here", 1, C_MUTED, 0);
  int first = pp_sel > 11 ? pp_sel - 11 : 0;
  for (int i = first; i < pp_n && i < first + 12; i++) { int y = MY + 56 + (i - first) * 44; if (i == pp_sel) nx_rrect(MX + 10, y, MW - 20, 40, 8, C_BLURPLE);
    char l[120]; float g = voice_get_user_gain(pp_ids[i]); snprintf(l, sizeof l, "%s%s", pp_names[i], g <= 0.001f ? "   (muted for you)" : g > 1.001f || g < 0.999f ? "" : "");
    nx_text(MX + 26, y + 8, l, 1, C_WHITE, MW - 120); if (g > 0.001f && (g > 1.001f || g < 0.999f)) { char pc[16]; snprintf(pc, sizeof pc, "%d%%", (int)(g * 100 + 0.5f)); nx_text(MX + MW - 80, y + 8, pc, 0, C_MUTED, 0); } }
}
static bool people_input(const NxInput *in, OvResult *r) {
  (void)r; if (in->pressed[B_B]) { kind = 0; return true; }
  if (in->pressed[B_DOWN] && pp_sel < pp_n - 1) pp_sel++; if (in->pressed[B_UP] && pp_sel > 0) pp_sel--;
  int pick = -1; if (in->pressed[B_A] && pp_n) pick = pp_sel;
  if (in->touch_pressed) { int first = pp_sel > 11 ? pp_sel - 11 : 0; for (int i = first; i < pp_n && i < first + 12; i++) if (hitp(in, MX + 10, MY + 56 + (i - first) * 44, MW - 20, 40)) pick = i; if (pick < 0 && !hitp(in, MX, MY, MW, 80 + pp_n * 44)) kind = 0; }
  if (pick >= 0) ov_open_member(pp_ids[pick], pp_names[pick], pp_guild, pp_perms, true, pp_mute[pick], pp_deaf[pick]);
  return true;
}

/* ======================= dispatch ======================= */
bool ov_input(const NxInput *in, OvResult *res) {
  memset(res, 0, sizeof *res);
  if (kind == 1) return act_input(in, res);
  if (kind == 2) return emo_input(in, res);
  if (kind == 3) return prof_input(in, res);
  if (kind == 4) return gif_input(in, res);
  if (kind == 5) return member_input(in, res);
  if (kind == 6) return people_input(in, res);
  if (kind == 7) return mic_input(in, res);
  return false;
}
void ov_tick(void) { emo_tick(); prof_tick(); gif_tick(); }
void ov_draw(void) { if (kind == 1) act_draw(); else if (kind == 2) emo_draw(); else if (kind == 3) prof_draw(); else if (kind == 4) gif_draw(); else if (kind == 5) member_draw(); else if (kind == 6) people_draw(); else if (kind == 7) mic_draw(); }
