#ifndef MD_H
#define MD_H
#include <stdbool.h>
/* Discord-flavoured markdown for message text: **bold** *italic* __underline__ ~~strike~~ `code` ```blocks```
 * ||spoilers|| # headers > quotes - lists [masked](links) and plain links. Measures (draw=false) or draws;
 * returns the height used. Same input + width always gives the same height. */
int md_layout(int x, int y, const char *text, int w, bool draw);
#endif
