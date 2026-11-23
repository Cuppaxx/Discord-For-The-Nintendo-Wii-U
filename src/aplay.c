/* aplay.c - audio-only attachment player (see aplay.h). The whole file is downloaded into RAM (progressively);
 * libavformat reads it through a custom AVIOContext that waits for bytes still downloading. */
#include "aplay.h"
#include "video.h"
#include "nxnet.h"
#include <coreinit/thread.h>
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define FIFO_SECS 1.0

struct APlay {
  char url[2200]; size_t maxdl; char err[200]; volatile int state; char codec[80];
  NxJob *job; uint8_t *buf; size_t len, cap, total; volatile bool done; SDL_mutex *mx;
  SDL_Thread *th; volatile bool stop;
  /* decoder side */
  AVFormatContext *fmt; AVIOContext *avio; AVCodecContext *dec; int sidx; uint64_t rpos; double dur;
  volatile int rate, ch; volatile bool eof;
  uint8_t *fifo; size_t flen, fcap;                             /* decoded S16 interleaved, guarded by mx */
  volatile bool seekreq; volatile double seek_to; volatile bool seek_done; double drop_until;
  /* output side (main thread) */
  SDL_AudioDeviceID adev; uint64_t queued; double base; bool started, paused;
};

static void (*g_log)(const char *line);
void ap_set_log(void (*fn)(const char *line)) { g_log = fn; }
static void plog(const char *fmt, ...) { if (!g_log) return; char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_log(b); }
static void seterr(APlay *p, const char *m) { if (p->state != VID_ERROR) { snprintf(p->err, sizeof p->err, "%s", m); p->state = VID_ERROR; plog("[aplay %p] error: %s", (void *)p, m); } }

static void pump(APlay *p) {
  if (!p->job) return; size_t tot = 0, have = nxnet_job_avail(p->job, &tot); if (tot) p->total = tot;
  if (have > p->len) { if (have > p->cap) { size_t nc; if (tot >= have && tot <= p->maxdl) nc = tot; else { nc = p->cap ? p->cap : (1u << 20); while (nc < have) nc *= 2; if (nc > p->maxdl) nc = p->maxdl; }
      if (nc > p->cap) { SDL_LockMutex(p->mx); uint8_t *nb = realloc(p->buf, nc); if (nb) { p->buf = nb; p->cap = nc; } SDL_UnlockMutex(p->mx); if (!nb) return; } }
    if (have > p->cap) have = p->cap; if (have > p->len) p->len += nxnet_job_copy(p->job, p->len, p->buf + p->len, have - p->len); }
  if (nxnet_job_done(p->job) && (p->len >= nxnet_job_avail(p->job, NULL) || p->len >= p->maxdl)) { int st = nxnet_job_status(p->job); plog("[aplay %p] download done: HTTP %d, %u bytes", (void *)p, st, (unsigned)p->len);
    if (st >= 300 || (st == 0 && !p->len)) { char m[80]; snprintf(m, sizeof m, st ? "download failed (HTTP %d)" : "download failed (network)", st); seterr(p, m); }
    nxnet_job_cancel(p->job); p->job = NULL; p->done = true; }
}
static int io_read(void *ud, uint8_t *dst, int n) {
  APlay *p = ud;
  for (;;) { if (p->stop || p->state == VID_ERROR) return AVERROR_EXIT; pump(p); if (p->rpos < p->len) break; if (p->done) return AVERROR_EOF; SDL_Delay(10); }
  size_t k = p->len - (size_t)p->rpos; if (k > (size_t)n) k = (size_t)n; memcpy(dst, p->buf + p->rpos, k); p->rpos += k; return (int)k;
}
static int64_t io_seek(void *ud, int64_t off, int whence) {
  APlay *p = ud; whence &= ~AVSEEK_FORCE;
  if (whence == AVSEEK_SIZE) return p->done ? (int64_t)p->len : p->total ? (int64_t)p->total : -1;
  int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)p->rpos : whence == SEEK_END ? (int64_t)(p->done ? p->len : p->total) : -1;
  if (base < 0 || (whence == SEEK_END && !p->done && !p->total)) return -1;
  int64_t np = base + off; if (np < 0) return -1; p->rpos = (uint64_t)np; return np;
}
static float smp(const AVFrame *f, int c, int i, int ch) {
  const uint8_t *const *d = (const uint8_t *const *)f->extended_data;
  switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return ((const float *)d[c])[i];
    case AV_SAMPLE_FMT_FLT: return ((const float *)d[0])[i * ch + c];
    case AV_SAMPLE_FMT_DBLP: return (float)((const double *)d[c])[i];
    case AV_SAMPLE_FMT_DBL: return (float)((const double *)d[0])[i * ch + c];
    case AV_SAMPLE_FMT_S16P: return ((const int16_t *)d[c])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16: return ((const int16_t *)d[0])[i * ch + c] / 32768.0f;
    case AV_SAMPLE_FMT_S32P: return ((const int32_t *)d[c])[i] / 2147483648.0f;
    case AV_SAMPLE_FMT_S32: return ((const int32_t *)d[0])[i * ch + c] / 2147483648.0f;
    case AV_SAMPLE_FMT_U8P: return (d[c][i] - 128) / 128.0f;
    case AV_SAMPLE_FMT_U8: return (d[0][i * ch + c] - 128) / 128.0f;
    default: return 0; }
}
static void push_frame(APlay *p, const AVFrame *f) {
  int ch = f->ch_layout.nb_channels, n = f->nb_samples; if (ch < 1 || n < 1) return; int oc = ch > 2 ? 2 : ch;
  if (!p->rate) { p->rate = f->sample_rate; p->ch = oc; }
  if (f->pts != AV_NOPTS_VALUE && p->drop_until > 0) { double t = f->pts * av_q2d(p->fmt->streams[p->sidx]->time_base); if (t + (double)n / f->sample_rate < p->drop_until) return; }
  size_t bytes = (size_t)n * p->ch * 2; SDL_LockMutex(p->mx);
  if (p->flen + bytes > p->fcap) { size_t nc = (p->flen + bytes) * 2; uint8_t *nb = realloc(p->fifo, nc); if (!nb) { SDL_UnlockMutex(p->mx); return; } p->fifo = nb; p->fcap = nc; }
  int16_t *o = (int16_t *)(p->fifo + p->flen);
  for (int i = 0; i < n; i++) for (int c = 0; c < p->ch; c++) { float x = smp(f, c < ch ? c : 0, i, ch) * 32767.0f; *o++ = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
  p->flen += bytes; SDL_UnlockMutex(p->mx);
}
static int dec_thread(void *arg) {
  APlay *p = arg; OSThread *self = OSGetCurrentThread(); OSSetThreadAffinity(self, OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(self, 22);
  /* wait for the first bytes so probing sees real data; without a Content-Length the size (and so the duration of
   * ogg/adts/vbr files) is unknown until the end, so small files are fetched completely first */
  while (!p->stop && p->state != VID_ERROR && !p->done && (p->len < 4096 || (!p->total && p->len < (16u << 20)))) { pump(p); SDL_Delay(10); }
  if (p->stop || p->state == VID_ERROR) return 0;
  unsigned char *iob = av_malloc(32768); p->avio = avio_alloc_context(iob, 32768, 0, p, io_read, NULL, io_seek);
  p->fmt = avformat_alloc_context(); if (!p->avio || !p->fmt) { seterr(p, "out of memory (demuxer)"); return 0; }
  p->fmt->pb = p->avio; p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  int r = avformat_open_input(&p->fmt, NULL, NULL, NULL);
  if (r < 0) { char e[64]; av_strerror(r, e, sizeof e); char m[160]; snprintf(m, sizeof m, "Unsupported audio format (%s)", e); seterr(p, m); return 0; }
  p->fmt->probesize = 1 << 20; avformat_find_stream_info(p->fmt, NULL);
  const AVCodec *codec = NULL; p->sidx = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (p->sidx < 0 || !codec) { const char *cn = "?"; for (unsigned i = 0; i < p->fmt->nb_streams; i++) if (p->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) cn = avcodec_get_name(p->fmt->streams[i]->codecpar->codec_id);
    char m[160]; snprintf(m, sizeof m, p->sidx == AVERROR_STREAM_NOT_FOUND ? "No audio track" : "Unsupported audio codec (%s)", cn); seterr(p, m); return 0; }
  AVStream *st = p->fmt->streams[p->sidx]; for (unsigned i = 0; i < p->fmt->nb_streams; i++) if ((int)i != p->sidx) p->fmt->streams[i]->discard = AVDISCARD_ALL;
  p->dec = avcodec_alloc_context3(codec); if (!p->dec || avcodec_parameters_to_context(p->dec, st->codecpar) < 0 || avcodec_open2(p->dec, codec, NULL) < 0) { seterr(p, "audio decoder failed to open"); return 0; }
  p->dur = p->fmt->duration > 0 ? p->fmt->duration / (double)AV_TIME_BASE : st->duration > 0 ? st->duration * av_q2d(st->time_base) : 0;
  snprintf(p->codec, sizeof p->codec, "%s/%s %d Hz %d ch", p->fmt->iformat->name, codec->name, st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
  plog("[aplay %p] %s, duration %.2f s", (void *)p, p->codec, p->dur);
  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  while (!p->stop && p->state != VID_ERROR) {
    if (p->seekreq) { int64_t ts = (int64_t)(p->seek_to * AV_TIME_BASE); int sr = av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD); if (sr < 0) sr = av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_ANY);
      avcodec_flush_buffers(p->dec); SDL_LockMutex(p->mx); p->flen = 0; SDL_UnlockMutex(p->mx); p->drop_until = p->seek_to; p->eof = false; p->seekreq = false; p->seek_done = true; plog("[aplay %p] seek %.2f -> %d", (void *)p, p->seek_to, sr); }
    if (p->eof) { SDL_Delay(20); continue; }
    if (p->rate && p->flen > (size_t)(p->rate * p->ch * 2 * FIFO_SECS)) { SDL_Delay(10); continue; }
    int rr = av_read_frame(p->fmt, pkt);
    if (rr < 0) { if (rr == AVERROR_EXIT) break; avcodec_send_packet(p->dec, NULL); while (avcodec_receive_frame(p->dec, fr) >= 0) push_frame(p, fr); avcodec_flush_buffers(p->dec); p->eof = true; plog("[aplay %p] end of stream", (void *)p); continue; }
    if (pkt->stream_index == p->sidx && avcodec_send_packet(p->dec, pkt) >= 0) while (avcodec_receive_frame(p->dec, fr) >= 0) push_frame(p, fr);
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt); av_frame_free(&fr); return 0;
}

APlay *ap_open(const char *url, size_t max_bytes) {
  APlay *p = calloc(1, sizeof *p); if (!p) return NULL; snprintf(p->url, sizeof p->url, "%s", url); p->maxdl = max_bytes ? max_bytes : (96u << 20); p->mx = SDL_CreateMutex(); p->state = VID_LOADING;
  const char *h[] = {"User-Agent: MingusDiscord/1.0 (Nintendo Wii U homebrew)", "Accept: */*", "Accept-Encoding: identity", NULL};
  plog("[aplay %p] open %.150s", (void *)p, url);
  p->job = nxnet_start("GET", url, h, NULL, 0, p->maxdl);
  p->th = SDL_CreateThreadWithStackSize(dec_thread, "adec", 256 * 1024, p); if (!p->th) seterr(p, "could not start the audio thread");
  return p;
}
static double out_clock(const APlay *p) { if (!p->adev || !p->rate) return p->base; uint32_t q = SDL_GetQueuedAudioSize(p->adev); return p->base + (double)(p->queued > q ? p->queued - q : 0) / (p->rate * p->ch * 2); }
void ap_tick(APlay *p) {
  if (!p || p->state == VID_ERROR) return;
  if (p->seek_done) { p->seek_done = false; if (p->adev) SDL_ClearQueuedAudio(p->adev); p->queued = 0; p->base = p->seek_to; if (p->state == VID_ENDED) p->state = p->paused ? VID_PAUSED : VID_PLAYING; }
  if (p->seekreq) return;
  if (!p->adev && p->rate) { if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want); want.freq = p->rate; want.format = AUDIO_S16SYS; want.channels = (Uint8)p->ch; want.samples = 2048;
    p->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); if (!p->adev) { seterr(p, "could not open the audio device"); return; } plog("[aplay %p] audio device %d Hz %d ch", (void *)p, have.freq, have.channels); }
  if (!p->adev) return;
  uint32_t bps = (uint32_t)(p->rate * p->ch * 2);
  if (SDL_GetQueuedAudioSize(p->adev) < bps / 2) { SDL_LockMutex(p->mx); size_t k = p->flen; if (k > bps / 2) k = (bps / 2) & ~3u; if (k) { SDL_QueueAudio(p->adev, p->fifo, (uint32_t)k); p->queued += k; memmove(p->fifo, p->fifo + k, p->flen - k); p->flen -= k; } SDL_UnlockMutex(p->mx); }
  if (p->state == VID_LOADING && (SDL_GetQueuedAudioSize(p->adev) >= bps / 4 || p->eof)) { p->started = true; p->state = p->paused ? VID_PAUSED : VID_PLAYING; if (!p->paused) SDL_PauseAudioDevice(p->adev, 0); }
  if (p->state == VID_PLAYING && p->eof && !p->flen && SDL_GetQueuedAudioSize(p->adev) == 0) p->state = VID_ENDED;}
int ap_state(const APlay *p) { return p ? p->state : VID_ERROR; }
const char *ap_error(const APlay *p) { return p ? p->err : "no player"; }
const char *ap_codec(const APlay *p) { return p ? p->codec : ""; }
void ap_seek(APlay *p, double rel) {
  if (!p || p->state == VID_ERROR || p->state == VID_LOADING || p->seekreq) return;
  double t = out_clock(p) + rel; if (t < 0) t = 0; if (p->dur > 0 && t > p->dur - 0.25) t = p->dur - 0.25; if (t < 0) t = 0;
  p->seek_to = t; p->seekreq = true;
}
void ap_toggle(APlay *p) {
  if (!p || p->state == VID_ERROR) return;
  if (p->state == VID_ENDED) { p->paused = false; p->seek_to = 0; p->seekreq = true; if (p->adev) SDL_PauseAudioDevice(p->adev, 0); return; }
  p->paused = !p->paused; if (p->state == VID_LOADING) return;
  if (p->adev) SDL_PauseAudioDevice(p->adev, p->paused ? 1 : 0); p->state = p->paused ? VID_PAUSED : VID_PLAYING;
}
double ap_time(const APlay *p) { if (!p) return 0; if (p->state == VID_ENDED && p->dur > 0) return p->dur; double t = p->seekreq ? p->seek_to : out_clock(p); if (p->dur > 0 && t > p->dur) t = p->dur; return t; }
double ap_duration(const APlay *p) { return p ? p->dur : 0; }
int ap_loaded_pct(const APlay *p) { if (!p) return 0; if (p->done) return 100; if (p->total) return (int)(100.0 * p->len / p->total); return p->len ? 50 : 0; }
void ap_close(APlay *p) {
  if (!p) return; p->stop = true; if (p->th) SDL_WaitThread(p->th, NULL);
  if (p->adev) SDL_CloseAudioDevice(p->adev);
  if (p->dec) avcodec_free_context(&p->dec);
  if (p->fmt) avformat_close_input(&p->fmt);
  if (p->avio) { av_freep(&p->avio->buffer); avio_context_free(&p->avio); }
  if (p->job) nxnet_job_cancel(p->job);
  free(p->buf); free(p->fifo); SDL_DestroyMutex(p->mx); free(p);
}
