/* vrtest.c - offline self test of the video receive path (runs when SD:/wiiu/apps/MingusDiscord/vtest.h264 exists).
 * Annex-B H.264 file -> access units -> DAVE video encrypt (real 2-member MLS group) -> RFC 6184 packetiser ->
 * simulated network (reordering, one packet only via RTX, one lost packet) -> vr_rtp() on VR_CONN_TEST ->
 * depacketiser -> DAVE decrypt (checked bit-exact against the original frame) -> hardware decoder -> texture.
 * Also logs one encrypted frame + its key so tools/dave_check.py can re-verify it with an independent
 * little-endian AES-GCM implementation. */
#include "vrecv.h"
#include "voice.h"
#include "dlog.h"
#include "dave_c.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <coreinit/time.h>
#include "yuv.h"

#define MAXAU 400
static uint8_t *au[MAXAU]; static size_t aun[MAXAU]; static int nau;
static dave_session_t *ta, *tb; static dave_encryptor_t *te; static dave_decryptor_t *td;
static char uid_show[24]; static char dir_[256];
static volatile int cmp_ok, cmp_bad; static volatile int pli_n;
static const char *UA = "1111222233334444", *UB = "5555666677778888";
#define TSSRC 7001u
#define TRTX 7002u

static int test_decrypt(int conn, const char *uid, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *w) {
  (void)conn; (void)uid; SDL_mutex *m = voice_dave_mutex(); SDL_LockMutex(m); int rc = td ? dave_decrypt_video(td, in, n, out, cap, w) : -1; SDL_UnlockMutex(m);
  if (rc == 0) {   /* compare with the original access unit (frames are matched by size + content scan) */
    bool hit = false; for (int i = 0; i < nau && !hit; i++) if (aun[i] == *w && !memcmp(au[i], out, *w)) hit = true;
    if (hit) cmp_ok++; else { cmp_bad++; if (cmp_bad <= 3) dlog("[vtest] decrypted frame (%u bytes) matches no original access unit", (unsigned)*w); } }
  return rc;
}
static void test_pli(int conn, uint32_t ssrc) { (void)conn; pli_n++; dlog("[vtest] PLI callback for ssrc %u (#%d)", (unsigned)ssrc, pli_n); }

typedef struct { uint8_t d[1500]; int n; bool m; } Pkt;
static Pkt pk[512]; static int npk;
static void on_pkt(void *u, const uint8_t *pl, int len, bool marker) { (void)u; if (npk < 512 && len <= 1500) { memcpy(pk[npk].d, pl, len); pk[npk].n = len; pk[npk].m = marker; npk++; } }

static void hexlog(const char *tag, const uint8_t *p, size_t n) {
  char line[200]; for (size_t o = 0; o < n; o += 64) { int k = 0; for (size_t i = o; i < n && i < o + 64; i++) k += snprintf(line + k, sizeof line - k, "%02x", p[i]); dlog("[vtest] %s %u %s", tag, (unsigned)o, line); }
}
static bool load(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) return false; fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > (8 << 20)) { fclose(f); return false; } uint8_t *d = malloc(sz); if (!d || fread(d, 1, sz, f) != (size_t)sz) { fclose(f); free(d); return false; } fclose(f);
  /* split at access unit delimiters (type 9; the file is made with ffmpeg -bsf:v h264_metadata=aud=insert); AUDs are left out like WebRTC does */
  size_t i = 0; uint8_t *cur = NULL; size_t cn = 0, cc = 0;
  while (i + 3 <= (size_t)sz) {
    size_t s = i; while (s + 3 <= (size_t)sz && !(d[s] == 0 && d[s + 1] == 0 && d[s + 2] == 1)) s++; if (s + 3 > (size_t)sz) break; s += 3;
    size_t e = s; while (e + 3 <= (size_t)sz && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (d[e + 2] == 0 && e + 3 < (size_t)sz && d[e + 3] == 1)))) e++; if (e + 3 > (size_t)sz) e = sz;
    int t = d[s] & 31;
    if (t == 9) { if (cur && cn && nau < MAXAU) { au[nau] = cur; aun[nau] = cn; nau++; cur = NULL; cn = cc = 0; } }
    else { if (cn + (e - s) + 4 > cc) { cc = (cn + (e - s) + 4) * 2; cur = realloc(cur, cc); } memcpy(cur + cn, "\0\0\0\1", 4); cn += 4; memcpy(cur + cn, d + s, e - s); cn += e - s; }
    i = e; }
  if (cur && cn && nau < MAXAU) { au[nau] = cur; aun[nau] = cn; nau++; } else free(cur);
  free(d); return nau > 0;
}
static uint64_t us(void) { return (uint64_t)OSTicksToMicroseconds(OSGetSystemTime()); }
/* converter micro benchmark on a synthetic 1280x720 NV12 frame: old per-pixel code vs the table/2x2 converter */
static void old_step(const uint8_t *y, const uint8_t *uv, int pitch, int ow, int oh, int step, uint8_t *out) {   /* the first vrecv tile converter */
  for (int j = 0; j < oh; j++) { int sy = j * step; const uint8_t *yr = y + sy * pitch, *ur = uv + (sy / 2) * pitch; uint8_t *o = out + (size_t)j * ow * 4;
    for (int i = 0; i < ow; i++) { int sx = i * step, cx = sx & ~1; int u = ur[cx] - 128, v = ur[cx + 1] - 128; int yy = 1192 * (yr[sx] - 16); int r = (yy + 1634 * v) >> 10, g = (yy - 833 * u - 400 * v) >> 10, b = (yy + 2066 * u) >> 10;
      o[0] = r < 0 ? 0 : r > 255 ? 255 : r; o[1] = g < 0 ? 0 : g > 255 ? 255 : g; o[2] = b < 0 ? 0 : b > 255 ? 255 : b; o[3] = 255; o += 4; } }
}
static void bench_converter(void) {
  const int W = 1280, H = 720; uint8_t *nv = malloc(W * H * 3 / 2), *a = malloc(W * H * 4), *b = malloc(W * H * 4); if (!nv || !a || !b) { free(nv); free(a); free(b); return; }
  uint32_t s = 12345; for (int i = 0; i < W * H * 3 / 2; i++) { s = s * 1103515245 + 12345; nv[i] = (uint8_t)((i * 7 >> 4) + (s >> 28)); }
  const uint8_t *y = nv, *uv = nv + W * H; uint64_t t;
  t = us(); for (int k = 0; k < 4; k++) yuv_nv12_to_rgba_ref(y, uv, W, W, H, a); double ref = (us() - t) / 4000.0;
  t = us(); for (int k = 0; k < 4; k++) yuv_nv12_to_rgba(y, uv, W, W, H, b, W * 4, W, H, 1); double n1 = (us() - t) / 4000.0;
  bool same = !memcmp(a, b, (size_t)W * H * 4);
  t = us(); for (int k = 0; k < 4; k++) yuv_nv12_to_rgba(y, uv, W, W, H, b, W * 4, W, H, 2); double n2 = (us() - t) / 4000.0;
  bool same2 = !memcmp(a, b, (size_t)W * H * 4);
  t = us(); for (int k = 0; k < 20; k++) old_step(y, uv, W, 320, 180, 4, a); double os = (us() - t) / 20000.0;
  t = us(); for (int k = 0; k < 20; k++) yuv_nv12_to_rgba(y, uv, W, W, H, b, 320 * 4, 320, 180, 1); double ns = (us() - t) / 20000.0;
  t = us(); for (int k = 0; k < 20; k++) old_step(y, uv, W, 640, 360, 2, a); double os2 = (us() - t) / 20000.0;
  t = us(); for (int k = 0; k < 20; k++) yuv_nv12_to_rgba(y, uv, W, W, H, b, 640 * 4, 640, 360, 1); double ns2 = (us() - t) / 20000.0;
  dlog("[vtest] BENCH NV12->RGBA 1280x720: old %.2f ms, new %.2f ms (bit-exact %d), new 2 cores %.2f ms (bit-exact %d) | to 320x180: old %.2f ms, new %.2f ms | to 640x360: old %.2f ms, new %.2f ms",
       ref, n1, same, n2, same2, os, ns, os2, ns2);
  free(nv); free(a); free(b);
}
static void free_aus(void) { for (int i = 0; i < nau; i++) free(au[i]); nau = 0; }
/* send every access unit of the loaded clip through encrypt -> packetise -> (impaired) network -> vr_rtp at fps */
static uint16_t seq = 40000; static uint32_t ts = 123456; static uint8_t enc[1 << 20];
static int frames, enc_fail, sent, lost_frame = -1; static bool logged_le;
static void feed(SDL_mutex *m, int loops, int frame_ms, bool impair) {
  uint32_t next = SDL_GetTicks();
  for (int L = 0; L < loops; L++) for (int f = 0; f < nau; f++, frames++) {
    size_t w = 0; SDL_LockMutex(m); int erc = dave_encrypt_video(te, TSSRC, DAVE_C_CODEC_H264, au[f], aun[f], enc, sizeof enc, &w); SDL_UnlockMutex(m);
    if (erc || !w) { enc_fail++; if (enc_fail <= 3) dlog("[vtest] encrypt rc=%d", erc); continue; }
    if (impair && !logged_le && L == 0 && f >= 1 && aun[f] < 1500 && !(au[f][4] == 0x67 || (au[f][4] & 31) == 7)) {   /* one small non-IDR frame for the PC-side check */
      uint8_t key[16]; SDL_LockMutex(m); int krc = dave_test_get_key(ta, UA, 0, key); SDL_UnlockMutex(m);
      if (krc == 0) { logged_le = true; dlog("[vtest] LE-CHECK frame %d plain %u enc %u", f, (unsigned)aun[f], (unsigned)w); hexlog("LE-KEY", key, 16); hexlog("LE-PLAIN", au[f], aun[f]); hexlog("LE-ENC", enc, w); } }
    if (frames == 1) { hexlog("first-encrypted-frame-head", enc, w < 48 ? w : 48); }
    npk = 0; vr_h264_packetize(enc, w, 1000, on_pkt, NULL);
    int order[512]; for (int i = 0; i < npk; i++) order[i] = i;
    if (impair && npk >= 3 && f % 4 == 0) { int t = order[0]; order[0] = order[1]; order[1] = t; }       /* reorder inside a frame */
    int drop = -1, via_rtx = -1; if (impair && L == 0 && f == 20) { drop = npk >= 2 ? 1 : 0; lost_frame = frames; }   /* one lost packet */
    if (impair && L == 0 && f == 8) via_rtx = 0;
    for (int i = 0; i < npk; i++) { int q = order[i]; uint16_t sq = (uint16_t)(seq + q);
      if (q == drop) continue;
      if (q == via_rtx) { uint8_t r[1502]; r[0] = sq >> 8; r[1] = (uint8_t)sq; memcpy(r + 2, pk[q].d, pk[q].n); vr_rtp(VR_CONN_TEST, TRTX, VR_PT_H264_RTX, (uint16_t)(5000 + f), ts, pk[q].m, r, pk[q].n + 2); }
      else vr_rtp(VR_CONN_TEST, TSSRC, VR_PT_H264, sq, ts, pk[q].m, pk[q].d, pk[q].n);
      sent++; }
    if (impair && L == 0 && f == 3) { uint8_t pad[2] = {0, 0}; vr_rtp(VR_CONN_TEST, TRTX, VR_PT_H264_RTX, 4000, ts, false, pad, 0); }   /* RTX padding probe: must be ignored */
    seq = (uint16_t)(seq + npk); ts += (uint32_t)(frame_ms * 90);
    next += frame_ms; int32_t wait = (int32_t)(next - SDL_GetTicks()); if (wait > 0) SDL_Delay(wait);
  }
}
static void phase_log(const char *name, const VrStats *a, const VrStats *b, uint32_t ms) {
  uint32_t de = b->n_exec - a->n_exec, dc = b->n_conv - a->n_conv; float secs = ms / 1000.0f;
  dlog("[vtest] PERF %s: %.1f fps decoded, %.1f fps shown, decode %.2f ms/frame, convert %.2f ms/frame (to %dx%d), late drops %u",
       name, dc / secs, (b->presented - a->presented) / secs, de ? (b->exec_us - a->exec_us) / 1000.0 / de : 0.0, dc ? (b->conv_us - a->conv_us) / 1000.0 / dc : 0.0, b->w, b->h, (unsigned)(b->late_drops - a->late_drops));
}
static int test_thread(void *arg) {
  (void)arg; SDL_Delay(6000); char path[300]; snprintf(path, sizeof path, "%s/vtest.h264", dir_);
  bench_converter();
  if (!load(path)) { dlog("[vtest] could not load %s", path); return 0; }
  dlog("[vtest] loaded %d access units from %s", nau, path);
  SDL_mutex *m = voice_dave_mutex(); SDL_LockMutex(m);
  uint32_t t0 = SDL_GetTicks(); int rc = dave_test_make_pair(987654321ULL, UA, UB, &ta, &tb);
  if (rc == 0) { te = dave_encryptor_new(); td = dave_decryptor_new(); if (dave_encryptor_set_ratchet(te, ta, UA) || dave_decryptor_set_ratchet(td, tb, UA)) rc = 50; dave_encryptor_set_passthrough(te, 0); dave_decryptor_set_passthrough(td, 0); }
  SDL_UnlockMutex(m); dlog("[vtest] MLS pair rc=%d (%u ms)", rc, (unsigned)(SDL_GetTicks() - t0)); if (rc) return 0;
  vr_set_conn(VR_CONN_TEST, test_decrypt, test_pli); vr_set_dave(VR_CONN_TEST, true);
  uint32_t ss[1] = {TSSRC}, rr[1] = {TRTX}; vr_map(VR_CONN_TEST, uid_show, ss, rr, 1);
  vr_request_ui_bench();
  VrStats p0, p1; vr_stats(VR_CONN_TEST, uid_show, &p0); uint32_t tp = SDL_GetTicks();
  feed(m, 3, 66, true);                                   /* phase A: 320x240 @ 15 fps with reordering, RTX and one lost packet */
  SDL_Delay(1500);
  VrStats st; vr_stats(VR_CONN_TEST, uid_show, &st); phase_log("320x240 tile", &p0, &st, SDL_GetTicks() - tp);
  dlog("[vtest] RESULT frames %d (enc fail %d) packets sent %d | rx pkts %u rtx %u frames %u drops %u | decrypt ok %u fail %u | bit-exact %d mismatched %d | decoded %u (%dx%d shown) errors %u | PLIs %u (callback %d) lost frame #%d",
       frames, enc_fail, sent, st.pkts, st.rtx_pkts, st.frames, st.drops, st.decrypt_ok, st.decrypt_fail, cmp_ok, cmp_bad, st.decoded, st.w, st.h, st.errors, st.plis, pli_n, lost_frame);
  bool pass = enc_fail == 0 && cmp_bad == 0 && cmp_ok > 0 && st.decoded > 0 && st.plis > 0 && st.decrypt_fail == 0;
  dlog("[vtest] %s", pass ? "PASS" : "FAIL");
  snprintf(path, sizeof path, "%s/vtest720.h264", dir_); free_aus();
  if (load(path)) {                                      /* phase B: 1280x720 @ 30 fps, first as a tile, then focused (full-size conversion) */
    dlog("[vtest] loaded %d access units from %s", nau, path); int bad0 = cmp_bad;
    vr_set_focus(0, NULL); feed(m, 1, 33, false);           /* warm-up: decoder reopen + first-keyframe quirk */
    vr_set_focus(0, NULL); vr_stats(VR_CONN_TEST, uid_show, &p0); tp = SDL_GetTicks(); feed(m, 1, 33, false); SDL_Delay(300); vr_stats(VR_CONN_TEST, uid_show, &p1); phase_log("1280x720 tile", &p0, &p1, SDL_GetTicks() - tp);
    vr_set_focus(VR_CONN_TEST, uid_show);
    vr_stats(VR_CONN_TEST, uid_show, &p0); tp = SDL_GetTicks(); feed(m, 1, 33, false); SDL_Delay(300); vr_stats(VR_CONN_TEST, uid_show, &p1); phase_log("1280x720 focused", &p0, &p1, SDL_GetTicks() - tp);
    vr_set_focus(0, NULL); dlog("[vtest] 720p frames decrypted bit-exact: %s", cmp_bad == bad0 ? "yes" : "NO");
  }
  SDL_Delay(15000);   /* keep the last frame on screen for a while */
  vr_map(VR_CONN_TEST, uid_show, NULL, NULL, 0);
  SDL_LockMutex(m); dave_encryptor_free(te); dave_decryptor_free(td); te = NULL; td = NULL; dave_session_free(ta); dave_session_free(tb); SDL_UnlockMutex(m);
  return 0;
}
void vr_selftest_start(const char *dir, const char *show_as) {
  static bool once; if (once) return; char p[300]; snprintf(p, sizeof p, "%s/vtest.h264", dir); FILE *f = fopen(p, "rb"); if (!f) return; fclose(f); once = true;
  snprintf(dir_, sizeof dir_, "%s", dir); snprintf(uid_show, sizeof uid_show, "%s", show_as && show_as[0] ? show_as : "selftest");
  vr_init(); voice_dave_mutex(); dlog("[vtest] self test scheduled (%s, shown as user %s)", p, uid_show);
  SDL_CreateThreadWithStackSize(test_thread, "vtest", 1024 * 1024, NULL);
}
