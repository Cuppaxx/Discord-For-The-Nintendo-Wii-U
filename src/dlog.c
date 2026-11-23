#include "dlog.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static FILE *lf; static SDL_mutex *mx;
void dlog_init(const char *dir) { char p[300]; snprintf(p, sizeof p, "%s/log.txt", dir); mx = SDL_CreateMutex(); lf = fopen(p, "wb"); }
void dlog(const char *fmt, ...) {
  if (!lf) return; SDL_LockMutex(mx); fprintf(lf, "[%6u] ", (unsigned)SDL_GetTicks()); va_list ap; va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap); fputc('\n', lf); fflush(lf); SDL_UnlockMutex(mx);
}
