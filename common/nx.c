#include "nx.h"
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <whb/file.h>
#include <math.h>

static SDL_Window *win; static SDL_Renderer *ren;
static TTF_Font *fonts[8]; static const int fsz[4] = {20, 24, 32, 52};
static TTF_Font *pxfont(int code);
#define F(size) ((size) >= 1000 ? pxfont(size) : fonts[(((size) & NX_BOLD) ? 4 : 0) + ((size) & 3)])
static SDL_Joystick *joy;
static char *fontdata, *fontdata_b; static uint32_t fontlen, fontlen_b;

/* ---- text cache: one texture per (size,color,string), small LRU ---- */
#define TC_N 768
typedef struct { char *s; int size; uint32_t col; SDL_Texture *t; int w, h; uint32_t used; int yoff; bool pending; uint32_t gen; } TC;
static TC tc[TC_N]; static uint32_t tick;
static SDL_Surface *rich_render(const char *s, int size, SDL_Color c, bool *pending, int *yoff);
static int rich_width(const char *s, int size);
static bool needs_rich(const char *s);
static uint32_t inline_gen;

bool nx_init(const char *title) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) return false;
  win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, NX_W, NX_H, 0);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  if (!win || !ren) return false;
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  SDL_RenderSetLogicalSize(ren, NX_W, NX_H);
  TTF_Init(); IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
  fontdata = WHBReadWholeFile("/vol/content/font.ttf", &fontlen);
  fontdata_b = WHBReadWholeFile("/vol/content/font_bold.ttf", &fontlen_b);
  if (fontdata) for (int i = 0; i < 4; i++) fonts[i] = TTF_OpenFontRW(SDL_RWFromConstMem(fontdata, fontlen), 0, fsz[i]);
  for (int i = 0; i < 4; i++) fonts[4 + i] = fontdata_b ? TTF_OpenFontRW(SDL_RWFromConstMem(fontdata_b, fontlen_b), 0, fsz[i]) : fonts[i];
  SDL_JoystickEventState(SDL_ENABLE); SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE); SDL_StopTextInput();   /* SDL starts with text input "active": only our own nx_start_text opens the keyboard */   /* swkbd OK / cancel notifications */
  if (SDL_NumJoysticks() > 0) joy = SDL_JoystickOpen(0);
  return true;
}
void nx_shutdown(void) { TTF_Quit(); IMG_Quit(); SDL_Quit(); }
SDL_Renderer *nx_renderer(void) { return ren; }
uint32_t nx_ticks(void) { return SDL_GetTicks(); }

/* SDL's Wii U GamePad button order (VPAD): verified at runtime through NxInput.last_raw */
static int map_btn(int raw) {
  static const int m[] = {B_A, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN};   /* SDL2 wiiu: A B X Y LS RS L R ZL ZR + - Left Up Right Down */
  return raw >= 0 && raw < 16 ? m[raw] : -1;
}

/* ---- test-only scripted input: /vol/content/script.txt lines "<ms> <cmd> [args]" (absent in release builds) ---- */
typedef struct { uint32_t t; char cmd[16]; char arg[100]; } ScriptLine;
static ScriptLine *script; static int script_n, script_i; static uint32_t script_t0; static bool script_loaded;
static const char *bnames[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "UP", "DOWN", "LEFT", "RIGHT", "LS", "RS"};
static void script_load(void) {
  script_loaded = true; uint32_t len = 0; char *d = WHBReadWholeFile("/vol/content/script.txt", &len); if (!d) return;
  script = calloc(512, sizeof *script); char *p = d, *end = d + len;
  while (p < end && script_n < 512) {
    char *nl = memchr(p, '\n', end - p); int l = nl ? (int)(nl - p) : (int)(end - p); char ln[160]; snprintf(ln, sizeof ln, "%.*s", l > 159 ? 159 : l, p);
    unsigned t; char cmd[16], arg[100] = ""; if (sscanf(ln, "%u %15s %99[^\r\n]", &t, cmd, arg) >= 2) { script[script_n].t = t; strcpy(script[script_n].cmd, cmd); strcpy(script[script_n].arg, arg); script_n++; }
    p = nl ? nl + 1 : end;
  }
  free(d); script_t0 = SDL_GetTicks();
}
static void script_run(NxInput *in) {
  if (!script_loaded) script_load();
  uint32_t now = SDL_GetTicks() - script_t0;
  while (script_i < script_n && script[script_i].t <= now) {
    ScriptLine *s = &script[script_i++]; int x, y;
    if (!strcmp(s->cmd, "tap") && sscanf(s->arg, "%d %d", &x, &y) == 2) { in->tx = x; in->ty = y; in->touch_pressed = true; in->touch = true; }
    else if (!strcmp(s->cmd, "up") && sscanf(s->arg, "%d %d", &x, &y) == 2) { in->tx = x; in->ty = y; in->touch = false; in->touch_released = true; }
    else if (!strcmp(s->cmd, "drag") && sscanf(s->arg, "%d %d", &x, &y) == 2) { in->tdx += x; in->tdy += y; in->touch = true; }
    else if (!strcmp(s->cmd, "text")) { strncat(in->text, s->arg, sizeof in->text - strlen(in->text) - 1); }
    else if (!strcmp(s->cmd, "stick")) { float a, b, c, d2; if (sscanf(s->arg, "%f %f %f %f", &a, &b, &c, &d2) == 4) { in->lx = a; in->ly = b; in->rx = c; in->ry = d2; } }
    else if (!strcmp(s->cmd, "quit")) in->quit = true;
    else if (!strcmp(s->cmd, "btn")) for (int b = 0; b < B_COUNT; b++) if (!strcmp(s->arg, bnames[b])) { in->pressed[b] = true; }
  }
}

static uint32_t kb_since; static bool kb_open;   /* the keyboard we opened is on screen */
void nx_poll(NxInput *in) {
  memset(in->pressed, 0, sizeof in->pressed); in->touch_pressed = in->touch_released = false; in->text[0] = 0; in->tdx = in->tdy = 0;
  script_run(in);
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: in->quit = true; break;
      case SDL_JOYBUTTONDOWN: { in->last_raw = e.jbutton.button; int b = map_btn(e.jbutton.button); if (b >= 0) { in->down[b] = true; in->pressed[b] = true; } break; }
      case SDL_JOYBUTTONUP: { int b = map_btn(e.jbutton.button); if (b >= 0) in->down[b] = false; break; }
      case SDL_JOYHATMOTION: { int v = e.jhat.value; bool u = v & SDL_HAT_UP, d = v & SDL_HAT_DOWN, l = v & SDL_HAT_LEFT, r = v & SDL_HAT_RIGHT;
        if (u && !in->down[B_UP]) in->pressed[B_UP] = true; if (d && !in->down[B_DOWN]) in->pressed[B_DOWN] = true;
        if (l && !in->down[B_LEFT]) in->pressed[B_LEFT] = true; if (r && !in->down[B_RIGHT]) in->pressed[B_RIGHT] = true;
        in->down[B_UP] = u; in->down[B_DOWN] = d; in->down[B_LEFT] = l; in->down[B_RIGHT] = r; break; }
      case SDL_JOYAXISMOTION: { float v = e.jaxis.value / 32767.0f; if (v > -0.12f && v < 0.12f) v = 0;
        switch (e.jaxis.axis) { case 0: in->lx = v; break; case 1: in->ly = v; break; case 2: in->rx = v; break; case 3: in->ry = v; break; } break; }
      case SDL_FINGERDOWN: in->touch = in->touch_pressed = true; in->tx = (int)(e.tfinger.x * NX_W); in->ty = (int)(e.tfinger.y * NX_H); break;
      case SDL_FINGERMOTION: in->tdx += (int)(e.tfinger.dx * NX_W); in->tdy += (int)(e.tfinger.dy * NX_H); in->tx = (int)(e.tfinger.x * NX_W); in->ty = (int)(e.tfinger.y * NX_H); break;
      case SDL_FINGERUP: in->touch = false; in->touch_released = true; in->tx = (int)(e.tfinger.x * NX_W); in->ty = (int)(e.tfinger.y * NX_H); break;
      case SDL_MOUSEBUTTONDOWN: in->touch = in->touch_pressed = true; in->tx = e.button.x; in->ty = e.button.y; break;
      case SDL_MOUSEBUTTONUP: in->touch = false; in->touch_released = true; in->tx = e.button.x; in->ty = e.button.y; break;
      case SDL_MOUSEMOTION: if (in->touch) { in->tdx += e.motion.xrel; in->tdy += e.motion.yrel; } in->tx = e.motion.x; in->ty = e.motion.y; break;
      case SDL_TEXTINPUT: strncat(in->text, e.text.text, sizeof in->text - strlen(in->text) - 1); break;
      case SDL_KEYDOWN: if (e.key.keysym.sym == SDLK_RETURN) { strncat(in->text, "\n", sizeof in->text - strlen(in->text) - 1); } break;
#if defined(__WIIU__) && defined(SDL_VIDEO_DRIVER_WIIU)
      case SDL_SYSWMEVENT: if (e.syswm.msg && e.syswm.msg->subsystem == SDL_SYSWM_WIIU) { unsigned ev = e.syswm.msg->msg.wiiu.event; if (ev == SDL_WIIU_SYSWM_SWKBD_CANCEL_EVENT || ev == SDL_WIIU_SYSWM_SWKBD_OK_FINISH_EVENT) { SDL_StopTextInput(); kb_open = false; } } break;
#endif
    }
  }
  if (kb_open && !SDL_IsTextInputActive()) kb_open = false;
  in->typing = kb_open;
  if (in->typing) { if (!kb_since) kb_since = SDL_GetTicks();
    memset(in->pressed, 0, sizeof in->pressed); in->touch_pressed = in->touch_released = false; in->tdx = in->tdy = 0; }
  else kb_since = 0;
}

void nx_clear(NxColor c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); SDL_RenderClear(ren); }
void nx_present(void) { SDL_RenderPresent(ren); }
void nx_rect(int x, int y, int w, int h, NxColor c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); SDL_Rect r = {x, y, w, h}; SDL_RenderFillRect(ren, &r); }
void nx_round(int x, int y, int w, int h, NxColor c) {
  nx_rect(x + 3, y, w - 6, h, c); nx_rect(x, y + 3, w, h - 6, c); nx_rect(x + 1, y + 1, w - 2, h - 2, c);
}
void nx_rrect(int x, int y, int w, int h, int r, NxColor c) {
  if (w <= 0 || h <= 0 || c.a == 0) return; int m = (w < h ? w : h) / 2; if (r > m) r = m;
  if (r <= 1) { nx_rect(x, y, w, h, c); return; }
  nx_rect(x, y + r, w, h - 2 * r, c);
  for (int i = 0; i < r; i++) { float dy = r - i - 0.5f; int inset = (int)(r - sqrtf((float)r * r - dy * dy) + 0.5f); nx_rect(x + inset, y + i, w - 2 * inset, 1, c); nx_rect(x + inset, y + h - 1 - i, w - 2 * inset, 1, c); }
}
void nx_clip(int x, int y, int w, int h) { SDL_Rect r = {x, y, w, h}; SDL_RenderSetClipRect(ren, &r); }
void nx_unclip(void) { SDL_RenderSetClipRect(ren, NULL); }

static TC *tc_get(const char *s, int size, NxColor c) {
  uint32_t col = (c.r << 24) | (c.g << 16) | (c.b << 8) | c.a; tick++;
  int v = -1;
  for (int i = 0; i < TC_N; i++) if (tc[i].s && tc[i].size == size && tc[i].col == col && !strcmp(tc[i].s, s)) {
    tc[i].used = tick; if (!tc[i].pending || tc[i].gen == inline_gen) return &tc[i];
    v = i; free(tc[v].s); if (tc[v].t) SDL_DestroyTexture(tc[v].t); tc[v].s = NULL; tc[v].t = NULL; goto have;   /* an inline image arrived: re-render */
  }
  v = 0; for (int i = 0; i < TC_N; i++) if (!tc[i].s) { v = i; goto have; }
  for (int i = 1; i < TC_N; i++) if (tc[i].used < tc[v].used) v = i;
  free(tc[v].s); if (tc[v].t) SDL_DestroyTexture(tc[v].t); tc[v].s = NULL; tc[v].t = NULL;
have:
  if (!F(size)) return NULL;
  SDL_Color sc = {c.r, c.g, c.b, c.a};
  bool pend = false; int yoff = 0;
  SDL_Surface *sf = needs_rich(s) ? rich_render(s, size, sc, &pend, &yoff) : TTF_RenderUTF8_Blended(F(size), s[0] ? s : " ", sc);
  if (!sf) return NULL;
  tc[v].t = SDL_CreateTextureFromSurface(ren, sf); tc[v].w = sf->w; tc[v].h = sf->h; SDL_FreeSurface(sf);
  if (tc[v].t) SDL_SetTextureBlendMode(tc[v].t, SDL_BLENDMODE_BLEND);
  tc[v].s = strdup(s); tc[v].size = size; tc[v].col = col; tc[v].used = tick; tc[v].yoff = yoff; tc[v].pending = pend; tc[v].gen = inline_gen; return &tc[v];
}
int nx_text_w(const char *s, int size) {
  if (needs_rich(s)) return rich_width(s, size);
  int w = 0, h = 0; TTF_Font *f = F(size); if (!f) return 0;
  for (const unsigned char *q = (const unsigned char *)s; *q; q++) if (*q >= 128) {      /* render once first: measuring alone caches metrics without the bitmap and composite glyphs then draw blank */
    SDL_Color k = {0, 0, 0, 255}; SDL_Surface *t = TTF_RenderUTF8_Blended(f, s, k); if (t) SDL_FreeSurface(t); break; }
  TTF_SizeUTF8(f, s, &w, &h); return w;
}
/* fast width for mostly-ASCII strings: cached per-size glyph advances (no kerning) */
static TTF_Font *pxf[800];
static uint32_t pxuse[800]; static uint32_t pxclock; static int pxopen;
#define PX_MAX_OPEN 12                    /* FreeType runs out of resources (composite glyphs render blank) with many faces open */
static TTF_Font *pxfont(int code) {
  int c = code - 1000; if (c < 0 || c >= 800) return NULL; pxuse[c] = ++pxclock; if (pxf[c]) return pxf[c];
  if (pxopen >= PX_MAX_OPEN) { int v = -1; for (int i = 0; i < 800; i++) if (pxf[i] && (v < 0 || pxuse[i] < pxuse[v])) v = i; if (v >= 0) { TTF_CloseFont(pxf[v]); pxf[v] = NULL; pxopen--; } }
  int px = c % 200, bold = (c / 200) & 1, ital = (c / 400) & 1; if (px < 6) px = 6; if (px > 120) px = 120;
  char *d = bold && fontdata_b ? fontdata_b : fontdata; uint32_t l = bold && fontdata_b ? fontlen_b : fontlen; if (!d) return NULL;
  TTF_Font *f = TTF_OpenFontRW(SDL_RWFromConstMem(d, l), 0, px); if (f) TTF_SetFontHinting(f, TTF_HINTING_NONE); if (f && ital) TTF_SetFontStyle(f, TTF_STYLE_ITALIC); pxf[c] = f; if (f) pxopen++; return f;
}
static short *pxadv[800];
static short adv[8][128]; static bool adv_ok[8];
int nx_text_w_fast(const char *s, int size) {
  if (size >= 1000) {
    TTF_Font *f = pxfont(size); if (!f) return 0; int c = size - 1000; if (c < 0 || c >= 800) return 0;
    if (!pxadv[c]) { pxadv[c] = malloc(128 * sizeof(short)); for (int ch = 0; ch < 128; ch++) { int a = 0; if (ch < 32 || ch == 127 || TTF_GlyphMetrics(f, (Uint16)ch, NULL, NULL, NULL, NULL, &a) != 0) a = 0; pxadv[c][ch] = (short)a; } }
    int w = 0; for (const unsigned char *q = (const unsigned char *)s; *q; q++) { if (*q >= 128 || *q < 32) return nx_text_w(s, size); w += pxadv[c][*q]; } return w;
  }
  if (!F(size)) return 0; int ix = ((size & NX_BOLD) ? 4 : 0) + (size & 3);
  if (!adv_ok[ix]) { for (int c = 32; c < 127; c++) { int a = 0; if (TTF_GlyphMetrics(F(size), (Uint16)c, NULL, NULL, NULL, NULL, &a) != 0) a = fsz[size & 3] / 2; adv[ix][c] = (short)a; } adv_ok[ix] = true; }
  int w = 0; for (const unsigned char *p = (const unsigned char *)s; *p; p++) { if (*p >= 128 || *p < 32) return nx_text_w(s, size); w += *p < 127 ? adv[ix][*p] : 0; }
  return w;
}
int nx_text_h(int size) { return F(size) ? TTF_FontLineSkip(F(size)) : fsz[size & 3]; }
int nx_text(int x, int y, const char *s, int size, NxColor c, int maxw) {
  if (!s || !*s) return 0;
  static char buf[2048]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  if (maxw > 0 && nx_text_w(buf, size) > maxw) {            /* truncate with ellipsis (never inside a UTF-8 sequence or an inline-image token) */
    int n = (int)strlen(buf), ew = nx_text_w("...", size);
    while (n > 1 && nx_text_w(buf, size) > maxw - ew) {
      n--; while (n > 1 && (buf[n] & 0xC0) == 0x80) n--;
      int open = -1; for (int k = 0; k < n; k++) { if (buf[k] == 1) open = k; else if (buf[k] == 2) open = -1; } if (open >= 0) n = open;
      buf[n] = 0; }
    strcat(buf, "...");
  }
  TC *t = tc_get(buf, size, c); if (!t || !t->t) return 0;
  SDL_Rect d = {x, y - t->yoff, t->w, t->h}; SDL_RenderCopy(ren, t->t, NULL, &d); return t->w;
}
/* one code point (or a whole \x01..\x02 token) starting at byte i: its length in bytes */
static int glyph_len(const char *s, int i) {
  if (s[i] == 1) { int k = i + 1; while (s[k] && s[k] != 2) k++; return s[k] == 2 ? k - i + 1 : k - i; }
  uint32_t cp; return nx_utf8_next(s, i, &cp);
}
/* word wrap; returns height used. lines are drawn if draw. Words wider than the box are broken by character
 * (long URLs used to run past the edge and get clipped - the "half text" bug). */
int nx_wrap(int x, int y, const char *s, int size, NxColor c, int w, int maxlines, bool draw) {
  int lh = nx_text_h(size), lines = 0; static char line[2048], word[1024], trial[3100]; line[0] = 0; const char *p = s;
  #define EMIT() do { if (draw) nx_text(x, y + lines * lh, line, size, c, 0); lines++; line[0] = 0; } while (0)
  while (*p && (maxlines <= 0 || lines < maxlines)) {
    int n = 0;
    while (*p && *p != ' ' && *p != '\n' && n < (int)sizeof word - 8) word[n++] = *p++;
    word[n] = 0;
    snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
    if (nx_text_w(trial, size) <= w) { snprintf(line, sizeof line, "%s", trial); }
    else {
      if (line[0]) { EMIT(); if (maxlines > 0 && lines >= maxlines) break; }
      if (nx_text_w(word, size) <= w) snprintf(line, sizeof line, "%s", word);
      else {                                                    /* break the long word into pieces that fit */
        int i = 0; char piece[1024]; int pn = 0; piece[0] = 0;
        while (word[i]) { int gl = glyph_len(word, i); char t2[1024]; memcpy(t2, piece, pn); memcpy(t2 + pn, word + i, gl); t2[pn + gl] = 0;
          if (pn && nx_text_w(t2, size) > w) { snprintf(line, sizeof line, "%s", piece); EMIT(); if (maxlines > 0 && lines >= maxlines) break; pn = 0; piece[0] = 0; continue; }
          memcpy(piece + pn, word + i, gl); pn += gl; piece[pn] = 0; i += gl; }
        snprintf(line, sizeof line, "%s", piece);
      }
    }
    if (*p == '\n') { EMIT(); p++; }
    else if (*p == ' ') p++;
  }
  if (line[0] && (maxlines <= 0 || lines < maxlines)) EMIT();
  #undef EMIT
  return lines * lh;
}

/* ================= rich text: fallback fonts + inline images ================= */
static NxInlineProvider inl_provider;
void nx_set_inline_provider(NxInlineProvider p) { inl_provider = p; }
void nx_inline_changed(void) { inline_gen++; }
int nx_utf8_next(const char *str, int i, uint32_t *cp) {
  const unsigned char *s = (const unsigned char *)str + i; unsigned c = s[0];
  if (c < 0x80) { *cp = c; return 1; }
  int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; if (n == 1) { *cp = 0xFFFD; return 1; }
  uint32_t v = c & (0x3F >> (n - 1)); for (int k = 1; k < n; k++) { if ((s[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return k; } v = (v << 6) | (s[k] & 0x3F); }
  *cp = v; return n;
}
#define FB_MAX 16
static char *fbdata[FB_MAX]; static uint32_t fblen[FB_MAX]; static int nfb = -1; static char fbname[FB_MAX][64]; static bool fbtried[FB_MAX];
static TTF_Font *fbfont[FB_MAX][8];
static void fb_list(void) {
  if (nfb >= 0) return; nfb = 0; uint32_t len = 0; char *d = WHBReadWholeFile("/vol/content/fb/list.txt", &len); if (!d) return;
  char *p = d, *end = d + len;
  while (p < end && nfb < FB_MAX) { char *nl = memchr(p, '\n', end - p); int l = nl ? (int)(nl - p) : (int)(end - p); while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) l--;
    if (l > 0 && l < 60) { memcpy(fbname[nfb], p, l); fbname[nfb][l] = 0; nfb++; } p = nl ? nl + 1 : end; }
  free(d);
}
static int slot_of(int size) { if (size >= 1000) { int px = (size - 1000) % 200, best = 0; for (int i = 1; i < 4; i++) if (abs(fsz[i] - px) < abs(fsz[best] - px)) best = i; return ((size - 1000) / 200 & 1 ? 4 : 0) + best; } return ((size & NX_BOLD) ? 4 : 0) + (size & 3); }
static TTF_Font *fb_font(int i, int size) {      /* fallback i at the size of "size" (px sizes snap to the nearest standard size) */
  if (i < 0 || i >= nfb) return NULL; int sl = slot_of(size); if (fbfont[i][sl]) return fbfont[i][sl];
  if (!fbdata[i]) { if (fbtried[i]) return NULL; fbtried[i] = true; char path[120]; snprintf(path, sizeof path, "/vol/content/fb/%s", fbname[i]); fbdata[i] = WHBReadWholeFile(path, &fblen[i]); if (!fbdata[i]) return NULL; }
  TTF_Font *f = TTF_OpenFontRW(SDL_RWFromConstMem(fbdata[i], fblen[i]), 0, fsz[sl & 3]); if (f && (sl & 4)) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
  fbfont[i][sl] = f; return f;
}
/* which font draws code point cp: -1 main font, 0..nfb-1 a fallback, -2 nobody (tofu with the main font). Cached per code point. */
static struct { uint32_t cp; signed char f; } fcache[4096];
static int font_for(uint32_t cp) {
  if (cp < 0x80) return -1;
  unsigned h = (cp * 2654435761u) >> 20; if (fcache[h].cp == cp && cp) return fcache[h].f;
  int r = -2; TTF_Font *m = fonts[1];
  if (m && TTF_GlyphIsProvided32(m, cp)) r = -1;
  else { fb_list(); for (int i = 0; i < nfb; i++) { TTF_Font *f = fb_font(i, 1); if (f && TTF_GlyphIsProvided32(f, cp)) { r = i; break; } } }
  fcache[h].cp = cp; fcache[h].f = (signed char)r; return r;
}
static bool is_mark(uint32_t cp) { return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D || cp == 0x200C || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0x1DC0 && cp <= 0x1DFF); }
static bool is_emoji_cp(uint32_t cp, uint32_t next) {
  if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;                       /* pictographs, faces, flags (regional indicators), etc. */
  if (next == 0xFE0F || next == 0x20E3) return true;                      /* explicit emoji presentation / keycaps */
  if ((cp >= 0x2300 && cp <= 0x2BFF) && font_for(cp) == -2) return true; /* symbol-block emoji no font has */
  return false;
}
static bool needs_rich(const char *s) { for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p >= 0x80 || *p == 1) return true; return false; }

static TTF_Font *g_pref;              /* preferred font for the current call (styled names); -3 in Seg.font */
typedef struct { int kind; int font; int a, b; char key[96]; } Seg;   /* kind 0: text bytes [a,b) in font; 1: inline image */
static int segment(const char *s, Seg *out, int max) {
  int n = 0, i = 0, len = (int)strlen(s);
  while (i < len && n < max) {
    if (s[i] == 1) { int k = i + 1; while (s[k] && s[k] != 2) k++; Seg *g = &out[n++]; g->kind = 1; g->a = i; g->b = s[k] ? k + 1 : k; snprintf(g->key, sizeof g->key, "%.*s", k - i - 1 > 90 ? 90 : k - i - 1, s + i + 1); i = g->b; continue; }
    uint32_t cp, nx = 0; int l = nx_utf8_next(s, i, &cp); if (i + l < len) nx_utf8_next(s, i + l, &nx);
    if (cp >= 0x80 && is_emoji_cp(cp, nx)) {                               /* one emoji cluster -> twemoji key */
      uint32_t cps[16]; int nc = 0, j = i; bool zwj = false; cps[nc++] = cp; j += l;
      bool ri = cp >= 0x1F1E6 && cp <= 0x1F1FF;
      while (j < len && nc < 15) { uint32_t c2; int l2 = nx_utf8_next(s, j, &c2);
        if (c2 == 0xFE0F || c2 == 0x20E3 || (c2 >= 0x1F3FB && c2 <= 0x1F3FF) || (c2 >= 0xE0020 && c2 <= 0xE007F)) { cps[nc++] = c2; j += l2; continue; }
        if (ri && c2 >= 0x1F1E6 && c2 <= 0x1F1FF && nc == 1) { cps[nc++] = c2; j += l2; continue; }
        if (c2 == 0x200D && j + l2 < len) { uint32_t c3; int l3 = nx_utf8_next(s, j + l2, &c3); cps[nc++] = c2; cps[nc++] = c3; zwj = true; j += l2 + l3; continue; }
        break; }
      Seg *g = &out[n++]; g->kind = 1; g->a = i; g->b = j; int kp = snprintf(g->key, sizeof g->key, "t:");
      bool first = true; for (int k = 0; k < nc; k++) { if (cps[k] == 0xFE0F && !zwj) continue; kp += snprintf(g->key + kp, sizeof g->key - kp, "%s%x", first ? "" : "-", (unsigned)cps[k]); first = false; }
      i = j; continue;
    }
    int f; if (g_pref && cp >= 0x20 && !is_mark(cp) && TTF_GlyphIsProvided32(g_pref, cp)) f = -3;
    else { f = (cp < 0x80 || is_mark(cp)) ? (n && out[n - 1].kind == 0 && is_mark(cp) ? out[n - 1].font : -1) : font_for(cp); if (f == -2) f = -1; }
    if (n && out[n - 1].kind == 0 && out[n - 1].font == f && out[n - 1].b == i) out[n - 1].b = i + l;
    else { Seg *g = &out[n++]; g->kind = 0; g->font = f; g->a = i; g->b = i + l; }
    i += l;
  }
  return n;
}
static TTF_Font *seg_font(const Seg *g, int size) { if (g->font == -3 && g_pref) return g_pref; TTF_Font *f = g->font >= 0 ? fb_font(g->font, size) : NULL; return f ? f : F(size); }
static int em_size(int size) { TTF_Font *f = F(size); return f ? TTF_FontHeight(f) - 2 : 22; }
static int seg_w(const char *s, const Seg *g, int size) {
  if (g->kind == 1) return em_size(size) + 2;
  char tmp[2048]; int l = g->b - g->a; if (l > 2047) l = 2047; memcpy(tmp, s + g->a, l); tmp[l] = 0;
  TTF_Font *f = seg_font(g, size); int w = 0, h = 0;
  SDL_Color k = {0, 0, 0, 255}; SDL_Surface *t = TTF_RenderUTF8_Blended(f, tmp, k); if (t) { w = t->w; SDL_FreeSurface(t); return w; }   /* render to measure: see nx_text_w */
  TTF_SizeUTF8(f, tmp, &w, &h); return w;
}
/* width cache: wrapping measures the same strings over and over */
static struct { char *s; int size; int w; void *pref; } wc[512];
static int rich_width(const char *s, int size) {
  unsigned h = 5381; for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = h * 33 + *p; h = (h ^ (unsigned)size) & 511;
  h = (h ^ (unsigned)(uintptr_t)g_pref) & 511;
  if (wc[h].s && wc[h].size == size && wc[h].pref == (void *)g_pref && !strcmp(wc[h].s, s)) return wc[h].w;
  static Seg seg[256]; int n = segment(s, seg, 256), w = 0; for (int i = 0; i < n; i++) w += seg_w(s, &seg[i], size);
  free(wc[h].s); wc[h].s = strdup(s); wc[h].size = size; wc[h].w = w; wc[h].pref = g_pref; return w;
}
static SDL_Surface *rich_render(const char *s, int size, SDL_Color c, bool *pending, int *yoff) {
  static Seg seg[256]; int n = segment(s, seg, 256); TTF_Font *mf = F(size); if (!mf) return NULL;
  int masc = TTF_FontAscent(mf), asc = masc, desc = TTF_FontHeight(mf) - masc, W = 0;
  for (int i = 0; i < n; i++) { if (seg[i].kind == 0) { TTF_Font *f = seg_font(&seg[i], size); int a = TTF_FontAscent(f), d = TTF_FontHeight(f) - a; if (a > asc) asc = a; if (d > desc) desc = d; } W += seg_w(s, &seg[i], size); }
  if (W < 1) W = 1; int H = asc + desc;
  SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888); if (!out) return NULL;
  SDL_FillRect(out, NULL, SDL_MapRGBA(out->format, c.r, c.g, c.b, 0));
  int x = 0, em = em_size(size);
  for (int i = 0; i < n; i++) {
    Seg *g = &seg[i]; int w = seg_w(s, g, size);
    if (g->kind == 0) {
      char tmp[2048]; int l = g->b - g->a; if (l > 2047) l = 2047; memcpy(tmp, s + g->a, l); tmp[l] = 0;
      TTF_Font *f = seg_font(g, size); SDL_Surface *t = TTF_RenderUTF8_Blended(f, tmp, c);
      if (t) { SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_NONE); SDL_Rect d = {x, asc - TTF_FontAscent(f), t->w, t->h}; SDL_BlitSurface(t, NULL, out, &d); SDL_FreeSurface(t); }
    } else {
      bool pend = false; SDL_Surface *img = inl_provider ? inl_provider(g->key, &pend) : NULL; if (pend) *pending = true;
      if (img) { SDL_BlendMode bm; SDL_GetSurfaceBlendMode(img, &bm); SDL_SetSurfaceBlendMode(img, SDL_BLENDMODE_NONE);
        SDL_Rect d = {x + 1, asc - masc + (TTF_FontHeight(mf) - em) / 2, em, em}; SDL_BlitScaled(img, NULL, out, &d); SDL_SetSurfaceBlendMode(img, bm); }
    }
    x += w;
  }
  *yoff = asc - masc; return out;
}

SDL_Texture *nx_tex_from_mem(const void *data, int len, int *w, int *h) {
  SDL_Surface *sf = IMG_Load_RW(SDL_RWFromConstMem(data, len), 1); if (!sf) return NULL;
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf); if (w) *w = sf->w; if (h) *h = sf->h; SDL_FreeSurface(sf); return t;
}
SDL_Texture *nx_tex_from_rgba(const void *rgba, int w, int h) {
  SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormatFrom((void *)rgba, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32); if (!sf) return NULL;
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf); SDL_FreeSurface(sf); if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); return t;
}
void nx_draw_tex(SDL_Texture *t, int x, int y, int w, int h) { SDL_Rect d = {x, y, w, h}; SDL_RenderCopy(ren, t, NULL, &d); }

void nx_start_text(const char *hint, const char *initial) {
#if defined(__WIIU__)
  if (hint && *hint) SDL_WiiUSetSWKBDHintText(hint);
  if (initial && *initial) SDL_WiiUSetSWKBDInitialText(initial);
#else
  (void)hint; (void)initial;
#endif
  SDL_Rect r = {0, NX_H - 80, NX_W, 80}; SDL_SetTextInputRect(&r);
  SDL_StartTextInput(); kb_open = true;
}
void nx_stop_text(void) { SDL_StopTextInput(); kb_open = false; }

void nx_circle(int cx, int cy, int r, NxColor c) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  for (int dy = -r; dy <= r; dy++) { int dx = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f); SDL_Rect q = {cx - dx, cy + dy, dx * 2, 1}; SDL_RenderFillRect(ren, &q); }
}
/* decode an image and mask it to an anti-aliased circle of the given diameter */
SDL_Texture *nx_circle_tex_from_mem(const void *data, int len, int d) {
  SDL_Surface *src = IMG_Load_RW(SDL_RWFromConstMem(data, len), 1); if (!src) return NULL;
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(src); if (!rgba) return NULL;
  SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, d, d, 32, SDL_PIXELFORMAT_ABGR8888); if (!out) { SDL_FreeSurface(rgba); return NULL; }
  SDL_SetSurfaceBlendMode(rgba, SDL_BLENDMODE_NONE); SDL_BlitScaled(rgba, NULL, out, NULL); SDL_FreeSurface(rgba);
  uint32_t *px = out->pixels; float rr = d / 2.0f;
  for (int y = 0; y < d; y++) for (int x = 0; x < d; x++) {
    float dx = x + 0.5f - rr, dy = y + 0.5f - rr, dist = sqrtf(dx * dx + dy * dy), a = rr - dist; if (a < 0) a = 0; if (a > 1) a = 1;
    uint32_t p = px[y * (out->pitch / 4) + x]; uint32_t al = (uint32_t)(((p >> 24) & 255) * a); px[y * (out->pitch / 4) + x] = (p & 0x00FFFFFF) | (al << 24); }
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, out); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_FreeSurface(out); return t;
}

/* ================= Discord display-name styles ================= */
static char *nmdata[17]; static uint32_t nmlen[17]; static bool nmtried[17]; static TTF_Font *nmfont[17][8];
static TTF_Font *name_font(int id, int size) {
  if (id <= 0 || id > 16 || id == 11) return NULL; int sl = slot_of(size); if (nmfont[id][sl]) return nmfont[id][sl];
  if (!nmdata[id]) { if (nmtried[id]) return NULL; nmtried[id] = true; char p[80]; snprintf(p, sizeof p, "/vol/content/names/%d.ttf", id); nmdata[id] = WHBReadWholeFile(p, &nmlen[id]); if (!nmdata[id]) return NULL; }
  return nmfont[id][sl] = TTF_OpenFontRW(SDL_RWFromConstMem(nmdata[id], nmlen[id]), 0, fsz[sl & 3]);
}
static uint32_t mixc(uint32_t a, uint32_t b, float t) { int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t), g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), bl = (int)((a & 255) * (1 - t) + (b & 255) * t); return (uint32_t)(r << 16 | g << 8 | bl); }
static uint32_t grad_at(const uint32_t *col, int n, float t) { if (n <= 1) return col[0]; float p = t * (n - 1); int i = (int)p; if (i >= n - 1) return col[n - 1]; return mixc(col[i], col[i + 1], p - i); }
static void put_px(uint32_t *d, uint32_t rgb, uint32_t sa) {      /* "over" blend of rgb with alpha sa onto ARGB pixel d */
  if (!sa) return; uint32_t da = *d >> 24; uint32_t oa = sa + da * (255 - sa) / 255; if (!oa) return;
  uint32_t w2 = da * (255 - sa) / 255;
  uint32_t r = (((rgb >> 16) & 255) * sa + ((*d >> 16) & 255) * w2) / oa, g = (((rgb >> 8) & 255) * sa + ((*d >> 8) & 255) * w2) / oa, b = ((rgb & 255) * sa + (*d & 255) * w2) / oa;
  *d = oa << 24 | r << 16 | g << 8 | b;
}
#define NPAD 4
typedef struct { char *s; int size; NxNameStyle st; uint32_t ph; SDL_Texture *t; int w, h, lw, yoff; uint32_t used; bool pending; uint32_t gen; } NC;
static NC ncache[192]; static uint32_t ntick;
static SDL_Surface *name_render(const char *s, int size, const NxNameStyle *st, NxColor plain, int *lw, int *yoff, bool *pending) {
  g_pref = name_font(st->font, size);
  SDL_Color white = {255, 255, 255, 255}; SDL_Surface *m = rich_render(s, size, white, pending, yoff);   /* glyph mask (emoji keep their colors) */
  g_pref = NULL; if (!m) return NULL; *lw = m->w;
  int W = m->w + 2 * NPAD, H = m->h + 2 * NPAD;
  SDL_Surface *o = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888); if (!o) { SDL_FreeSurface(m); return NULL; }
  SDL_FillRect(o, NULL, 0);
  uint32_t col[5]; int n = st->ncol > 5 ? 5 : st->ncol; for (int i = 0; i < n; i++) col[i] = st->col[i] & 0xFFFFFF;
  if (!n) { col[0] = (uint32_t)(plain.r << 16 | plain.g << 8 | plain.b); n = 1; }
  int ef = st->effect; if (ef > 1000) ef = 1;
  uint32_t *mp = m->pixels, *op = o->pixels; int mpitch = m->pitch / 4, opitch = o->pitch / 4;
  #define MA(x, y) (((x) < 0 || (y) < 0 || (x) >= m->w || (y) >= m->h) ? 0u : (mp[(y) * mpitch + (x)] >> 24))
  if (ef == 3) {                                                           /* NEON: soft glow in the color, light core */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int sx = x - NPAD, sy = y - NPAD; unsigned acc = 0, cnt = 0; for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) { acc += MA(sx + dx, sy + dy); cnt++; } unsigned a = acc / cnt * 2; if (a > 230) a = 230; put_px(&op[y * opitch + x], col[0], a); }
  } else if (ef == 5) {                                                    /* POP: colored drop shadow */
    uint32_t sh = mixc(col[0], 0, 0.55f); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) put_px(&op[y * opitch + x], sh, MA(x - NPAD - 2, y - NPAD - 2));
  } else if (ef == 4) {                                                    /* TOON: dark outline */
    uint32_t ol = mixc(col[0], 0, 0.65f); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { unsigned a = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { unsigned v = MA(x - NPAD + dx, y - NPAD + dy); if (v > a) a = v; } put_px(&op[y * opitch + x], ol, a); }
  }
  for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++) {          /* the glyphs themselves */
    uint32_t px = mp[y * mpitch + x]; uint32_t a = px >> 24; if (!a) continue; uint32_t rgb = px & 0xFFFFFF;
    if (rgb == 0xFFFFFF) {                                                 /* glyph pixel (emoji pixels are colored: keep them) */
      float tx = m->w > 1 ? (float)x / (m->w - 1) : 0, ty = m->h > 1 ? (float)y / (m->h - 1) : 0;
      if (ef == 2 || ef == 6 || ef == 7 || ef == 8) rgb = grad_at(col, n, tx);
      else if (ef == 4) rgb = mixc(mixc(col[0], 0xFFFFFF, 0.35f), mixc(col[0], 0, 0.25f), ty);
      else if (ef == 3) rgb = mixc(col[0], 0xFFFFFF, 0.7f);
      else rgb = col[0];
    }
    put_px(&op[(y + NPAD) * opitch + x + NPAD], rgb, a);
  }
  #undef MA
  SDL_FreeSurface(m); return o;
}
static NC *name_get(const char *s, int size, const NxNameStyle *st, NxColor plain) {
  uint32_t ph = (uint32_t)(plain.r << 16 | plain.g << 8 | plain.b); ntick++;
  int v = -1;
  for (int i = 0; i < 192; i++) { NC *e = &ncache[i]; if (e->s && e->size == size && e->ph == ph && !memcmp(&e->st, st, sizeof *st) && !strcmp(e->s, s)) { e->used = ntick; if (!e->pending || e->gen == inline_gen) return e; v = i; break; } }
  if (v < 0) { v = 0; for (int i = 0; i < 192; i++) { if (!ncache[i].s) { v = i; break; } if (ncache[i].used < ncache[v].used) v = i; } }
  NC *e = &ncache[v]; free(e->s); if (e->t) SDL_DestroyTexture(e->t); memset(e, 0, sizeof *e);
  int lw = 0, yoff = 0; bool pend = false; SDL_Surface *sf = name_render(s, size, st, plain, &lw, &yoff, &pend); if (!sf) return NULL;
  e->t = SDL_CreateTextureFromSurface(ren, sf); if (e->t) SDL_SetTextureBlendMode(e->t, SDL_BLENDMODE_BLEND); e->w = sf->w; e->h = sf->h; SDL_FreeSurface(sf);
  e->s = strdup(s); e->size = size; e->st = *st; e->ph = ph; e->lw = lw; e->yoff = yoff; e->used = ntick; e->pending = pend; e->gen = inline_gen; return e;
}
static bool plain_style(const NxNameStyle *st) { return !st || (st->effect == 0 && st->ncol == 0 && (st->font <= 0 || st->font == 11)); }
int nx_name_w(const char *s, int size, const NxNameStyle *st) {
  if (plain_style(st)) return nx_text_w(s, size);
  g_pref = name_font(st->font, size); int w = rich_width(s, size); g_pref = NULL; return w;
}
int nx_name(int x, int y, const char *s, int size, const NxNameStyle *st, NxColor plain, int maxw) {
  if (!s || !*s) return 0;
  if (plain_style(st)) return nx_text(x, y, s, size, plain, maxw);
  static char buf[512]; snprintf(buf, sizeof buf, "%s", s);
  if (maxw > 0 && nx_name_w(buf, size, st) > maxw) { int n = (int)strlen(buf), ew = nx_text_w("...", size);
    while (n > 1 && nx_name_w(buf, size, st) > maxw - ew) { n--; while (n > 1 && (buf[n] & 0xC0) == 0x80) n--; int open = -1; for (int k = 0; k < n; k++) { if (buf[k] == 1) open = k; else if (buf[k] == 2) open = -1; } if (open >= 0) n = open; buf[n] = 0; }
    strcat(buf, "..."); }
  NC *e = name_get(buf, size, st, plain); if (!e || !e->t) return nx_text(x, y, buf, size, plain, 0);
  SDL_Rect d = {x - NPAD, y - e->yoff - NPAD, e->w, e->h}; SDL_RenderCopy(ren, e->t, NULL, &d); return e->lw;
}

SDL_Texture *nx_rrect_tex_from_mem(const void *data, int len, int d, int rad) {
  SDL_Surface *src = IMG_Load_RW(SDL_RWFromConstMem(data, len), 1); if (!src) return NULL;
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(src); if (!rgba) return NULL;
  SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, d, d, 32, SDL_PIXELFORMAT_ABGR8888); if (!out) { SDL_FreeSurface(rgba); return NULL; }
  SDL_SetSurfaceBlendMode(rgba, SDL_BLENDMODE_NONE); SDL_BlitScaled(rgba, NULL, out, NULL); SDL_FreeSurface(rgba);
  uint32_t *px = out->pixels;
  for (int y = 0; y < d; y++) for (int x = 0; x < d; x++) {
    float cx = x + 0.5f < rad ? rad : (x + 0.5f > d - rad ? d - rad : x + 0.5f), cy = y + 0.5f < rad ? rad : (y + 0.5f > d - rad ? d - rad : y + 0.5f);
    float dx = x + 0.5f - cx, dy = y + 0.5f - cy, a = rad - sqrtf(dx * dx + dy * dy) + 0.5f; if (a < 0) a = 0; if (a > 1) a = 1;
    uint32_t p = px[y * (out->pitch / 4) + x]; uint32_t al = (uint32_t)(((p >> 24) & 255) * a); px[y * (out->pitch / 4) + x] = (p & 0x00FFFFFF) | (al << 24); }
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, out); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_FreeSurface(out); return t;
}
