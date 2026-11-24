/* nx - tiny shared UI layer for the Discord / browser Wii U apps (SDL2 + SDL2_ttf + SDL2_image) */
#ifndef NX_H
#define NX_H
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdint.h>
#include <stdbool.h>

#define NX_BOLD 16      /* OR into a font size to get the bold face */
#define NX_PX(px, bold, italic) (1000 + (int)(px) + ((bold) ? 200 : 0) + ((italic) ? 400 : 0))   /* pass as "size" to the text functions */
#define NX_W 1280
#define NX_H 720

/* logical buttons, independent of SDL numbering */
enum { B_A, B_B, B_X, B_Y, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_LS, B_RS, B_COUNT };

typedef struct {
  bool down[B_COUNT], pressed[B_COUNT];   /* pressed = went down this frame */
  float lx, ly, rx, ry;                   /* sticks -1..1, y up = negative */
  bool touch, touch_pressed, touch_released; int tx, ty; int tdx, tdy;   /* touch in NX_W x NX_H coords */
  char text[2048];                        /* SDL_TEXTINPUT this frame (software keyboard; long text arrives in 32-byte chunks) */
  bool typing;                            /* the on-screen keyboard is open: buttons/touch were swallowed this frame */
  bool quit; int last_raw;                /* last raw joystick button index (diagnostics) */
} NxInput;

typedef struct { uint8_t r, g, b, a; } NxColor;

bool nx_init(const char *title);
void nx_shutdown(void);
SDL_Renderer *nx_renderer(void);
void nx_poll(NxInput *in);                /* pump events, fill input */
void nx_clear(NxColor c);
void nx_present(void);
void nx_rect(int x, int y, int w, int h, NxColor c);
void nx_round(int x, int y, int w, int h, NxColor c);
void nx_rrect(int x, int y, int w, int h, int r, NxColor c);   /* true rounded rect */   /* faux-rounded rect */
/* size: 0 small(16) 1 normal(20) 2 large(28) 3 huge(44); returns width drawn */
int nx_text(int x, int y, const char *s, int size, NxColor c, int maxw);
int nx_text_w(const char *s, int size);
int nx_text_w_fast(const char *s, int size);
int nx_text_h(int size);
int nx_wrap(int x, int y, const char *s, int size, NxColor c, int w, int maxlines, bool draw);   /* returns height used */
void nx_circle(int cx, int cy, int r, NxColor c);
SDL_Texture *nx_circle_tex_from_mem(const void *data, int len, int d);
SDL_Texture *nx_rrect_tex_from_mem(const void *data, int len, int d, int radius);   /* square image with rounded corners */
void nx_clip(int x, int y, int w, int h); void nx_unclip(void);
SDL_Texture *nx_tex_from_mem(const void *data, int len, int *w, int *h);
SDL_Texture *nx_tex_from_rgba(const void *rgba, int w, int h);   /* straight-alpha RGBA bytes */
void nx_draw_tex(SDL_Texture *t, int x, int y, int w, int h);

/* ---- rich text: font fallback + inline images ----
 * Characters the main font lacks are drawn with fallback fonts listed (in priority order, one file name per line) in
 * /vol/content/fb/list.txt, loaded lazily. Emoji (Unicode) become inline images keyed "t:<twemoji code points>", e.g.
 * "t:1f600" or "t:1f468-200d-1f469"; an app can also embed any image in text with the token "\x01<key>\x02"
 * (e.g. "\x01c:123456\x02" for a Discord custom emoji). Images come from the provider (NULL = text only). */
typedef SDL_Surface *(*NxInlineProvider)(const char *key, bool *pending);   /* return a surface (provider keeps ownership) or NULL; *pending = still loading */
void nx_set_inline_provider(NxInlineProvider p);
void nx_inline_changed(void);              /* a pending image arrived: cached text that used it is re-rendered */
int nx_utf8_next(const char *s, int i, uint32_t *cp);   /* decode one code point at byte i; returns bytes used (>=1) */

/* Discord display-name styles: font from /vol/content/names/<font_id>.ttf (per-glyph fallback to the normal fonts),
 * effect_id 1 solid, 2 gradient, 3 neon, 4 toon, 5 pop, 6 glow, 7 prism, 8 gummy (6-8 drawn as static gradients),
 * colors 0xRRGGBB. font_id 0/11 = default font; ncol 0 = use "plain". Returns the logical width drawn. */
typedef struct { short font, effect; uint32_t col[5]; unsigned char ncol; } NxNameStyle;
int nx_name(int x, int y, const char *s, int size, const NxNameStyle *st, NxColor plain, int maxw);
int nx_name_w(const char *s, int size, const NxNameStyle *st);
void nx_start_text(const char *hint, const char *initial);   /* opens Wii U software keyboard */
void nx_stop_text(void);
uint32_t nx_ticks(void);

#define RGB(r, g, b) ((NxColor){r, g, b, 255})
#define RGBA(r, g, b, a) ((NxColor){r, g, b, a})
#endif
