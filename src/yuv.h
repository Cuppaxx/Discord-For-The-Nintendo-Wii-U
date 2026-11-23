#ifndef YUV_H
#define YUV_H
/* Fast NV12 -> RGBA (bytes R,G,B,A; SDL_PIXELFORMAT_RGBA32) for decoded H.264 frames.
 * Integer BT.601 limited range via lookup tables, one UV pair shared by each 2x2 block, 32-bit stores,
 * optional nearest-neighbour downscale straight to the display size, optional second core for big frames. */
#include <stdint.h>
#include <stdbool.h>
/* src: y/uv planes (NV12, same pitch), sw x sh visible pixels; dst: ow x oh RGBA, dst_pitch bytes per row.
 * ow <= sw, oh <= sh (downscale only). threads: 1 = this thread, 2 = split rows with a helper thread on another core. */
void yuv_nv12_to_rgba(const uint8_t *y, const uint8_t *uv, int pitch, int sw, int sh, uint8_t *dst, int dst_pitch, int ow, int oh, int threads);
/* the plain per-pixel converter the apps used before (reference for tests / benchmarks) */
void yuv_nv12_to_rgba_ref(const uint8_t *y, const uint8_t *uv, int pitch, int w, int h, uint8_t *out);
#endif
