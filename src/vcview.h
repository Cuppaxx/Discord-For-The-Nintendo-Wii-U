#ifndef VCVIEW_H
#define VCVIEW_H
/* Video on the call screen (used by main.c draw_voice / handle_input / the render loop). Main thread only.
 * GRID: cameras / watched streams inside the member tiles (+ Watch/Stop chips for Go Live).
 * FOCUS: one video large in the call area.  FULL: that video fills the whole 1280x720 screen.
 * Input: tap a video tile (or Up/Down tile cursor + A) = FOCUS; A or tap = FULL; B steps back FULL -> FOCUS -> GRID;
 * Left/Right cycle videos while focused/full; ZR stops watching a stream. A is only taken while a video is focused or
 * the tile cursor is shown, so the grid keeps A for the people list, Left/Right for pane navigation and R for the camera. */
#include <stdbool.h>
#include "nx.h"
void vcv_begin(void);                      /* start of draw_voice: uploads decoded frames, resets hit boxes */
/* draw this member's video (camera, watched stream or self-test) into the tile; adds a Watch/Stop chip if live.
 * Returns true when video was drawn (the caller then skips the avatar). */
bool vcv_tile(const char *user_id, const char *name, bool live, int x, int y, int w, int h);
bool vcv_full(int x, int y, int w, int h); /* end of draw_voice: the FOCUS view (true = something drawn in that area) */
void vcv_fullscreen(void);                 /* render loop, after the sidebars: the FULL view over everything (no-op otherwise) */
/* call-screen input; buttons = the call pane has the controller focus. true = consumed, caller returns. */
bool vcv_input(const NxInput *in, bool buttons);
bool vcv_fullscreen_active(void);
bool vcv_take_menu(char *uid_out);         /* the user asked for this person's volume/options menu (tile "..." / Options chip / minus) */
#endif
