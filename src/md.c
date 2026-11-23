/* Discord markdown layout on top of nx text (word-wrapped, per-atom styles) */
#include "md.h"
#include "nx.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define C_TEXT RGB(219, 222, 225)
#define C_LINK RGB(0, 168, 252)
#define C_CODEBG RGB(43, 45, 49)
#define C_QUOTE RGB(78, 80, 88)
#define C_MENTBG RGBA(88, 101, 242, 70)
#define C_MENT RGB(201, 205, 251)

enum { S_B = 1, S_I = 2, S_U = 4, S_S = 8, S_CODE = 16, S_SPOIL = 32, S_LINK = 64, S_MENT = 128 };

static int font_of(int st, int hdr) {
  int px = hdr == 1 ? 32 : hdr == 2 ? 28 : 24; bool b = (st & S_B) || hdr;
  if (st & S_I) return NX_PX(px, b, 1);
  if (hdr == 1) return 2 | NX_BOLD;
  if (hdr == 2) return NX_PX(28, 1, 0);
  return 1 | (b ? NX_BOLD : 0);
}
/* count occurrences of a marker in s (for "only open if it also closes") */
static int count_mark(const char *s, const char *mk) { int n = 0; size_t l = strlen(mk); for (const char *p = s; (p = strstr(p, mk)); p += l) n++; return n; }

typedef struct { int x, y, lh, w, x0, lines; bool draw; int hdr; } Cur;
static void newline(Cur *c) { c->x = c->x0; c->y += c->lh; c->lines++; }
static void emit_atom(Cur *c, const char *s, int st) {
  if (!*s) return; int f = font_of(st, c->hdr);
  int aw = nx_text_w(s, f); int sp = nx_text_w(" ", f);
  if (c->x > c->x0 && c->x + aw > c->x0 + c->w) newline(c);
  if (aw > c->w) {                                             /* a single atom wider than the box: break it by characters */
    char piece[512]; int pn = 0; int i = 0;
    while (s[i]) { uint32_t cp; int l = nx_utf8_next(s, i, &cp); if (pn + l >= (int)sizeof piece - 1) break; memcpy(piece + pn, s + i, l); piece[pn + l] = 0;
      if (pn && nx_text_w(piece, f) > c->x0 + c->w - c->x) { piece[pn] = 0; emit_atom(c, piece, st); newline(c); pn = 0; continue; }
      pn += l; i += l; }
    piece[pn] = 0; if (pn) emit_atom(c, piece, st); return;
  }
  if (c->draw) {
    int h = nx_text_h(f);
    if (st & S_CODE) nx_rrect(c->x - 2, c->y + 1, aw + 4, h - 2, 4, C_CODEBG);
    if (st & S_SPOIL) nx_rrect(c->x - 2, c->y + 1, aw + 4, h - 2, 4, RGB(30, 31, 34));
    if (st & S_MENT) nx_rrect(c->x - 2, c->y + 1, aw + 4, h - 2, 4, C_MENTBG);
    NxColor col = (st & S_LINK) ? C_LINK : (st & S_MENT) ? C_MENT : (st & S_SPOIL) ? RGB(150, 152, 160) : C_TEXT;
    nx_text(c->x, c->y + (c->lh - h), s, f, col, 0);
    if (st & S_U) nx_rect(c->x, c->y + c->lh - 4, aw, 2, col);
    if (st & S_S) nx_rect(c->x, c->y + c->lh / 2 + 1, aw, 2, col);
  }
  c->x += aw; (void)sp;
}
static void emit_space(Cur *c, int st) { int f = font_of(st, c->hdr); int sp = nx_text_w(" ", f); if (c->x > c->x0) { if (c->draw && (st & (S_CODE | S_SPOIL | S_MENT))) { int h = nx_text_h(f); nx_rect(c->x, c->y + 1, sp, h - 2, (st & S_MENT) ? C_MENTBG : (st & S_CODE) ? C_CODEBG : RGB(30, 31, 34)); } c->x += sp; if (c->x > c->x0 + c->w) newline(c); } }

/* inline markdown of one line */
static void do_inline(Cur *c, const char *line) {
  int st = 0; char atom[1024]; int an = 0; const char *p = line;
  #define FLUSH() do { atom[an] = 0; emit_atom(c, atom, st); an = 0; } while (0)
  while (*p) {
    if (*p == '\\' && p[1] && strchr("*_~`|>#[]()\\", p[1])) { if (an < 1000) atom[an++] = p[1]; p += 2; continue; }
    if (*p == '`' && !(st & S_CODE) && strchr(p + 1, '`')) { FLUSH(); st |= S_CODE; p++; continue; }
    if (*p == '`' && (st & S_CODE)) { FLUSH(); st &= ~S_CODE; p++; continue; }
    if (st & S_CODE) { if (*p == ' ') { FLUSH(); emit_space(c, st); p++; } else { if (an < 1000) atom[an++] = *p; p++; } continue; }
    if (!strncmp(p, "**", 2) && ((st & S_B) || count_mark(p, "**") >= 2)) { FLUSH(); st ^= S_B; p += 2; continue; }
    if (!strncmp(p, "__", 2) && ((st & S_U) || count_mark(p, "__") >= 2)) { FLUSH(); st ^= S_U; p += 2; continue; }
    if (!strncmp(p, "~~", 2) && ((st & S_S) || count_mark(p, "~~") >= 2)) { FLUSH(); st ^= S_S; p += 2; continue; }
    if (!strncmp(p, "||", 2) && ((st & S_SPOIL) || count_mark(p, "||") >= 2)) { FLUSH(); st ^= S_SPOIL; p += 2; continue; }
    if ((*p == '*' || *p == '_') && p[1] != *p) {
      bool open = !(st & S_I), at_start = p == line || !isalnum((unsigned char)p[-1]), at_end = !p[1] || !isalnum((unsigned char)p[1]);
      char mk[2] = {*p, 0};
      if ((open && (*p == '*' || at_start) && p[1] && p[1] != ' ' && count_mark(p, mk) >= 2) || (!open && (*p == '*' || at_end))) { FLUSH(); st ^= S_I; p++; continue; }
    }
    if (*p == '[') {                                                /* [masked](link) */
      const char *rb = strstr(p, "]("), *cl = rb ? strchr(rb, ')') : NULL;
      if (rb && cl && !strncmp(rb + 2, "http", 4)) { FLUSH(); char t[400]; snprintf(t, sizeof t, "%.*s", (int)(rb - p - 1), p + 1);
        int save = st; st |= S_LINK; char *w = t; while (*w) { char *sp = strchr(w, ' '); if (sp) *sp = 0; emit_atom(c, w, st); if (!sp) break; emit_space(c, st); w = sp + 1; } st = save; p = cl + 1; continue; }
    }
    if ((p == line || p[-1] == ' ') && (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8))) {   /* bare link */
      FLUSH(); int save = st; st |= S_LINK; while (*p && *p != ' ') { if (an < 1000) atom[an++] = *p; p++; } FLUSH(); st = save; continue; }
    if (*p == '@' && (p == line || p[-1] == ' ')) { FLUSH(); int save = st; st |= S_MENT; while (*p && *p != ' ') { if (an < 1000) atom[an++] = *p; p++; } FLUSH(); st = save; continue; }
    if (*p == ' ') { FLUSH(); emit_space(c, st); p++; continue; }
    if (an < 1000) atom[an++] = *p; p++;
  }
  FLUSH();
  #undef FLUSH
}
static bool emoji_only(const char *s, int *count) {     /* only emoji / custom emoji / spaces -> Discord shows them "jumbo" */
  int n = 0, i = 0; bool any = false;
  while (s[i]) { if (s[i] == 1) { while (s[i] && s[i] != 2) i++; if (s[i]) i++; n++; any = true; continue; }
    uint32_t cp; int l = nx_utf8_next(s, i, &cp); i += l;
    if (cp == ' ' || cp == 0xFE0F || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF) || cp == 0x20E3) continue;
    if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xE0020 && cp <= 0xE007F)) { n++; any = true; continue; }
    return false; }
  *count = n; return any;
}
int md_layout(int x, int y, const char *text, int w, bool draw) {
  int ne = 0; if (emoji_only(text, &ne) && ne <= 27) return nx_wrap(x, y, text, 3, C_TEXT, w, 0, draw) + 4;
  Cur c = {x, y, nx_text_h(1), w, x, 0, draw, 0}; int y0 = y; bool code = false; int code_top = 0;
  char line[2048]; const char *p = text;
  while (*p || p == text) {
    const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); if (l > 2047) l = 2047; memcpy(line, p, l); line[l] = 0;
    if (!strncmp(line, "```", 3)) {                                /* code fence: the block is drawn as plain lines on a dark box */
      if (!code) { code = true; code_top = c.y; } else { code = false; if (draw) { (void)code_top; } c.y += 6; }
      p = e ? e + 1 : p + l; if (!e) break; continue;
    }
    c.hdr = 0; c.x0 = x; c.w = w; c.x = x; c.lh = nx_text_h(1);
    if (code) {
      if (draw) nx_rect(x - 6, c.y, w + 12, c.lh, C_CODEBG);
      Cur cc = c; cc.draw = draw; int f = 1; (void)f; if (draw && line[0]) nx_text(x + 4, c.y, line, 1, RGB(200, 204, 210), w - 8); c.y += c.lh;
      (void)cc; p = e ? e + 1 : p + l; if (!e) break; continue;
    }
    const char *s = line; bool quote = false;
    if (!strncmp(s, "> ", 2) || !strcmp(s, ">")) { quote = true; s += s[1] ? 2 : 1; c.x0 = x + 16; c.x = c.x0; c.w = w - 16; }
    if (s[0] == '#') { int lv = 0; while (s[lv] == '#' && lv < 4) lv++; if (lv <= 3 && s[lv] == ' ') { c.hdr = lv; s += lv + 1; c.lh = lv == 1 ? nx_text_h(2) + 6 : lv == 2 ? 38 : nx_text_h(1) + 4; } }
    if ((s[0] == '-' || s[0] == '*') && s[1] == ' ') { if (draw) nx_text(c.x0 + 4, c.y, "\xE2\x80\xA2", 1, C_TEXT, 0); c.x0 += 26; c.x = c.x0; c.w -= 26; s += 2; }
    int ly = c.y;
    if (!*s) c.y += c.lh / 2 + 4;                                   /* blank line: half-height gap */
    else { do_inline(&c, s); c.y += c.lh; }
    if (quote && draw) nx_rect(x, ly, 4, c.y - ly, C_QUOTE);
    p = e ? e + 1 : p + l; if (!e) break;
  }
  return c.y - y0;
}
