#include "nxnet.h"
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <whb/file.h>
#include <coreinit/thread.h>

typedef struct { char *p; size_t n, cap, max; struct NxJob *job; } Buf;
struct NxJob {
  char *method, *url, *body; char **headers; int nheaders; size_t body_len, max_bytes;
  SDL_Thread *th; volatile int done, cancelled; NxResp resp;
 NxPostFn post; void *ud; SDL_mutex *bmx; Buf *live; volatile size_t total; char dbg[1500]; };

static size_t on_write(char *d, size_t s, size_t n, void *u) {
  Buf *b = u; size_t k = s * n;
  if (b->n + k > b->max) return 0;                       /* too large: abort transfer */
  if (b->job) SDL_LockMutex(b->job->bmx);
  if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; if (b->cap < 4096) b->cap = 4096; char *np = realloc(b->p, b->cap); if (!np) { if (b->job) SDL_UnlockMutex(b->job->bmx); return 0; } b->p = np; }
  memcpy(b->p + b->n, d, k); b->n += k; b->p[b->n] = 0;
  if (b->job) SDL_UnlockMutex(b->job->bmx); return k;
}

int (*nxnet_cookie_get)(const char *url, char *out, size_t cap);
void (*nxnet_cookie_set)(const char *url, const char *setcookie);

typedef struct { NxResp *r; const char *cur; char *h; size_t hn, hcap; char loc[2600]; } Hdr;
static size_t on_header(char *d, size_t s, size_t n, void *u) {
  size_t k = s * n; Hdr *H = u;
  if (k >= 5 && !strncasecmp(d, "HTTP/", 5)) { H->hn = 0; H->loc[0] = 0; H->r->ctype[0] = 0; if (H->h) H->h[0] = 0; return k; }
  if (H->hn + k + 1 < 20000) { if (H->hn + k + 1 > H->hcap) { H->hcap = (H->hn + k + 1) * 2 + 256; H->h = realloc(H->h, H->hcap); } memcpy(H->h + H->hn, d, k); H->hn += k; H->h[H->hn] = 0; }
  char line[1500]; size_t m = k < sizeof line - 1 ? k : sizeof line - 1; memcpy(line, d, m); line[m] = 0; char *e = strpbrk(line, "\r\n"); if (e) *e = 0;
  if (!strncasecmp(line, "content-type:", 13)) { const char *v = line + 13; while (*v == ' ') v++; snprintf(H->r->ctype, sizeof H->r->ctype, "%s", v); }
  else if (!strncasecmp(line, "location:", 9)) { const char *v = line + 9; while (*v == ' ') v++; snprintf(H->loc, sizeof H->loc, "%s", v); }
  else if (!strncasecmp(line, "set-cookie:", 11) && nxnet_cookie_set) { const char *v = line + 11; while (*v == ' ') v++; nxnet_cookie_set(H->cur, v); }
  return k;
}

static SDL_mutex *cmx; static char *ca_data; static uint32_t ca_len;
/* one share handle: connections, TLS sessions and DNS are reused across requests and threads */
static CURLSH *share; static SDL_mutex *shmx[CURL_LOCK_DATA_LAST + 1];
static void sh_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { (void)h; (void)a; (void)u; if (d <= CURL_LOCK_DATA_LAST && shmx[d]) SDL_LockMutex(shmx[d]); }
static void sh_unlock(CURL *h, curl_lock_data d, void *u) { (void)h; (void)u; if (d <= CURL_LOCK_DATA_LAST && shmx[d]) SDL_UnlockMutex(shmx[d]); }
void nxnet_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); cmx = SDL_CreateMutex();
  for (int i = 0; i <= CURL_LOCK_DATA_LAST; i++) shmx[i] = SDL_CreateMutex();
  share = curl_share_init(); if (share) { curl_share_setopt(share, CURLSHOPT_LOCKFUNC, sh_lock); curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, sh_unlock); curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT); curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION); curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS); } uint32_t n = 0; char *raw = WHBReadWholeFile("/vol/content/cacert.pem", &n);
  if (raw) { ca_data = malloc(n + 1); memcpy(ca_data, raw, n); ca_data[n] = 0; ca_len = n + 1; WHBFreeWholeFile(raw); } }   /* mbedtls wants the PEM NUL-terminated and counted */
int nxnet_ca_len(void) { return (int)ca_len; }
const char *nxnet_ca_data(void) { return ca_data; }
void nxnet_free(NxResp *r) { free(r->body); r->body = NULL; r->len = 0; free(r->headers); r->headers = NULL; }

static void resolve_loc(const char *base, const char *ref, char *out, size_t cap) {
  if (!strncasecmp(ref, "http://", 7) || !strncasecmp(ref, "https://", 8)) { snprintf(out, cap, "%s", ref); return; }
  const char *sp = strstr(base, "://"); char scheme[10] = "https"; if (sp && sp - base < 9) snprintf(scheme, sizeof scheme, "%.*s", (int)(sp - base), base);
  if (ref[0] == '/' && ref[1] == '/') { snprintf(out, cap, "%s:%s", scheme, ref); return; }
  const char *after = sp ? sp + 3 : base; const char *pe = strpbrk(after, "/?#"); size_t ol = pe ? (size_t)(pe - base) : strlen(base);
  if (ref[0] == '/') { snprintf(out, cap, "%.*s%s", (int)ol, base, ref); return; }
  char dir[1100]; snprintf(dir, sizeof dir, "%s", pe ? pe : "/"); char *q = strpbrk(dir, "?#"); if (q) *q = 0; char *ls = strrchr(dir, '/'); if (ls) ls[1] = 0; else strcpy(dir, "/");
  snprintf(out, cap, "%.*s%s%s", (int)ol, base, dir, ref);
}

static int on_dbg(CURL *h, curl_infotype t, char *d, size_t n, void *u) { (void)h; NxJob *j = u; if (!j) return 0; if (t == CURLINFO_HEADER_OUT || t == CURLINFO_HEADER_IN) { SDL_LockMutex(j->bmx); size_t have = strlen(j->dbg); if (have + n + 1 < sizeof j->dbg) { memcpy(j->dbg + have, d, n); j->dbg[have + n] = 0; } SDL_UnlockMutex(j->bmx); } return 0; }
static int on_xfer(void *u, curl_off_t dlt, curl_off_t dln, curl_off_t ult, curl_off_t uln) { (void)dln; (void)ult; (void)uln; NxJob *j = u; if (j && dlt > 0) j->total = (size_t)dlt; return 0; }
static void nxnet_request_j(NxResp *out, const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes, NxJob *job);
void nxnet_request(NxResp *out, const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes) { nxnet_request_j(out, method, url, headers, body, body_len, max_bytes, NULL); }
static void nxnet_request_j(NxResp *out, const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes, NxJob *job) {
  memset(out, 0, sizeof *out);
  char cur[2600]; snprintf(cur, sizeof cur, "%s", url); char meth[16]; snprintf(meth, sizeof meth, "%s", method ? method : "GET");
  const char *bodyp = body; size_t blen = body ? (body_len ? body_len : strlen(body)) : 0;
  bool caller_cookie = false; for (int i = 0; headers && headers[i]; i++) if (!strncasecmp(headers[i], "cookie:", 7)) caller_cookie = true;
  Hdr H; memset(&H, 0, sizeof H); H.r = out;
  for (int hop = 0; hop < 10; hop++) {
    CURL *c = curl_easy_init(); if (!c) { strcpy(out->err, "curl init failed"); break; }
    Buf b = {0}; b.max = max_bytes ? max_bytes : 8u << 20; b.job = job; if (job) { SDL_LockMutex(job->bmx); job->live = &b; job->total = 0; SDL_UnlockMutex(job->bmx); } H.cur = cur; H.hn = 0; H.loc[0] = 0; out->ctype[0] = 0;
    struct curl_slist *hl = NULL; for (int i = 0; headers && headers[i]; i++) hl = curl_slist_append(hl, headers[i]);
    char ck[4200]; ck[0] = 0; if (!caller_cookie && nxnet_cookie_get && nxnet_cookie_get(cur, ck + 8, sizeof ck - 8)) { memcpy(ck, "Cookie: ", 8); hl = curl_slist_append(hl, ck); }
    curl_easy_setopt(c, CURLOPT_URL, cur);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);                       /* redirects are handled here so cookies set on each hop are honoured */
    curl_easy_setopt(c, CURLOPT_CAPATH, "/vol/content/certs");   /* default is the console's scerts dir, which fails to load and aborts the blob too */
    CURLcode so = CURLE_OK; char ebuf[CURL_ERROR_SIZE] = ""; curl_easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    if (ca_data) { struct curl_blob bl = {ca_data, ca_len, CURL_BLOB_COPY}; so = curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &bl); }
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L + (long)(blen / 40000));   /* big uploads (video clips) get extra time: ~40 KB/s minimum */
    if (job) { curl_easy_setopt(c, CURLOPT_VERBOSE, 1L); curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, on_dbg); curl_easy_setopt(c, CURLOPT_DEBUGDATA, job); curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_xfer); curl_easy_setopt(c, CURLOPT_XFERINFODATA, job); }
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); if (share) curl_easy_setopt(c, CURLOPT_SHARE, share);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L); curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write); curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header); curl_easy_setopt(c, CURLOPT_HEADERDATA, &H);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (strcmp(meth, "GET")) {
      curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, meth);
      if (bodyp) { curl_easy_setopt(c, CURLOPT_POSTFIELDS, bodyp); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)blen); }
    }
    CURLcode rc = curl_easy_perform(c);
    long st = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st); out->status = (int)st;
    snprintf(out->final_url, sizeof out->final_url, "%s", cur);
    curl_slist_free_all(hl); curl_easy_cleanup(c);
    if (rc == CURLE_OK && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && H.loc[0]) {
      char nu[2600]; resolve_loc(cur, H.loc, nu, sizeof nu); snprintf(cur, sizeof cur, "%s", nu);
      if (st == 303 || ((st == 301 || st == 302) && strcmp(meth, "GET") && strcmp(meth, "HEAD"))) { strcpy(meth, "GET"); bodyp = NULL; blen = 0; }
      if (job) { SDL_LockMutex(job->bmx); job->live = NULL; SDL_UnlockMutex(job->bmx); } free(b.p); continue;
    }
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && b.n)) { snprintf(out->err, sizeof out->err, "%s [rc=%d blob=%d] %s", curl_easy_strerror(rc), (int)rc, (int)so, ebuf); out->status = 0; }
    if (job) SDL_LockMutex(job->bmx); out->body = b.p ? b.p : calloc(1, 1); out->len = b.n; if (job) { job->live = NULL; SDL_UnlockMutex(job->bmx); } break;
  }
  if (!out->body) out->body = calloc(1, 1);
  out->headers = H.h ? H.h : strdup("");
}

/* ---------- in-memory response cache: css / js / images / fonts, LRU, 24 MB ---------- */
typedef struct CEnt { char *url; char *body; size_t len; char ctype[96]; char final_url[1024]; char *headers; uint32_t used; struct CEnt *next; } CEnt;
static CEnt *cache_head; static size_t cache_bytes; static uint32_t cache_clock; static SDL_mutex *cachemx;
#define CACHE_MAX (24u << 20)
static bool cacheable(const NxResp *r, const char *method) {
  if (strcmp(method, "GET") || r->status != 200 || !r->len || r->len > (3u << 20)) return false;
  const char *t = r->ctype; bool ok = strstr(t, "css") || strstr(t, "javascript") || strstr(t, "ecmascript") || strncmp(t, "image/", 6) == 0 || strstr(t, "font") || strstr(t, "svg");
  if (!ok) return false;
  if (r->headers) { const char *cc = r->headers; for (;;) { const char *l = cc; while (*l == ' ') l++; if (!strncasecmp(l, "cache-control:", 14) && (strstr(l, "no-store") || strstr(l, "private") || strstr(l, "max-age=0"))) { const char *e = strchr(l, '\n'); char tmp[200]; snprintf(tmp, sizeof tmp, "%.*s", e ? (int)(e - l) : 150, l); if (strstr(tmp, "no-store") || strstr(tmp, "max-age=0")) return false; } cc = strchr(cc, '\n'); if (!cc) break; cc++; } }
  return true;
}
static void cache_put(const char *url, const NxResp *r) {
  if (!cachemx) cachemx = SDL_CreateMutex(); SDL_LockMutex(cachemx);
  for (CEnt *e = cache_head; e; e = e->next) if (!strcmp(e->url, url)) { SDL_UnlockMutex(cachemx); return; }
  while (cache_bytes + r->len > CACHE_MAX && cache_head) {                     /* evict least recently used */
    CEnt **pv = NULL, **p = &cache_head; uint32_t best = 0xFFFFFFFFu; for (; *p; p = &(*p)->next) if ((*p)->used < best) { best = (*p)->used; pv = p; }
    if (!pv) break; CEnt *v = *pv; *pv = v->next; cache_bytes -= v->len; free(v->url); free(v->body); free(v->headers); free(v); }
  CEnt *e = calloc(1, sizeof *e); e->url = strdup(url); e->body = malloc(r->len + 1); memcpy(e->body, r->body, r->len + 1); e->len = r->len; snprintf(e->ctype, sizeof e->ctype, "%s", r->ctype); snprintf(e->final_url, sizeof e->final_url, "%s", r->final_url); e->headers = strdup(r->headers ? r->headers : ""); e->used = ++cache_clock; e->next = cache_head; cache_head = e; cache_bytes += r->len;
  SDL_UnlockMutex(cachemx);
}
static bool cache_get(const char *url, NxResp *out) {
  if (!cachemx) return false; SDL_LockMutex(cachemx); bool hit = false;
  for (CEnt *e = cache_head; e; e = e->next) if (!strcmp(e->url, url)) { memset(out, 0, sizeof *out); out->status = 200; out->body = malloc(e->len + 1); memcpy(out->body, e->body, e->len + 1); out->len = e->len; snprintf(out->ctype, sizeof out->ctype, "%s", e->ctype); snprintf(out->final_url, sizeof out->final_url, "%s", e->final_url); out->headers = strdup(e->headers); e->used = ++cache_clock; hit = true; break; }
  SDL_UnlockMutex(cachemx); return hit;
}
void nxnet_cache_clear(void) { if (!cachemx) return; SDL_LockMutex(cachemx); while (cache_head) { CEnt *v = cache_head; cache_head = v->next; free(v->url); free(v->body); free(v->headers); free(v); } cache_bytes = 0; SDL_UnlockMutex(cachemx); }

static int worker(void *arg) {
  NxJob *j = arg; const char **h = (const char **)j->headers;
  /* Wii U threads of equal priority on one core are cooperative: keep network work off the UI core and below it */
  OSThread *self = OSGetCurrentThread();
  OSSetThreadAffinity(self, OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2);
  OSSetThreadPriority(self, 20);
  if (!cache_get(j->url, &j->resp)) { nxnet_request_j(&j->resp, j->method, j->url, h, j->body, j->body_len, j->max_bytes, j); if (cacheable(&j->resp, j->method)) cache_put(j->url, &j->resp); }
  if (j->post) j->post(&j->resp, j->ud);
  j->done = 1; return 0;
}
NxJob *nxnet_start(const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes) { return nxnet_start_ex(method, url, headers, body, body_len, max_bytes, NULL, NULL); }
NxJob *nxnet_start_ex(const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes, NxPostFn post, void *ud) {
  NxJob *j = calloc(1, sizeof *j); j->post = post; j->ud = ud; j->bmx = SDL_CreateMutex(); j->method = strdup(method ? method : "GET"); j->url = strdup(url);
  if (body) { j->body = malloc(body_len ? body_len : strlen(body) + 1); memcpy(j->body, body, body_len ? body_len : strlen(body) + 1); j->body_len = body_len ? body_len : strlen(body); }
  int n = 0; while (headers && headers[n]) n++;
  j->headers = calloc(n + 1, sizeof(char *)); for (int i = 0; i < n; i++) j->headers[i] = strdup(headers[i]);
  j->max_bytes = max_bytes;
  j->th = SDL_CreateThreadWithStackSize(worker, "nxnet", 256 * 1024, j);
  if (!j->th) { strcpy(j->resp.err, "thread failed"); j->resp.body = calloc(1, 1); j->done = 1; }
  return j;
}
bool nxnet_job_done(NxJob *j) { return j->done; }
static void job_free(NxJob *j) {
  if (j->th) SDL_WaitThread(j->th, NULL);
  free(j->method); free(j->url); free(j->body); for (int i = 0; j->headers && j->headers[i]; i++) free(j->headers[i]); free(j->headers); if (j->bmx) SDL_DestroyMutex(j->bmx); free(j);
}
void nxnet_job_take(NxJob *j, NxResp *out) { *out = j->resp; j->resp.body = NULL; job_free(j); }
static int reaper(void *arg) { NxJob *j = arg; while (!j->done) SDL_Delay(50); nxnet_free(&j->resp); job_free(j); return 0; }
void nxnet_job_cancel(NxJob *j) { if (j->done) { nxnet_free(&j->resp); job_free(j); } else { SDL_Thread *t = SDL_CreateThread(reaper, "reap", j); if (t) SDL_DetachThread(t); } }



/* progressive access to a transfer in flight: bytes available so far (and the total if known) */
size_t nxnet_job_avail(NxJob *j, size_t *total) {
  if (!j) return 0; SDL_LockMutex(j->bmx); size_t n = j->live ? j->live->n : (j->done ? j->resp.len : 0); if (total) *total = j->total ? j->total : (j->done ? j->resp.len : 0); SDL_UnlockMutex(j->bmx); return n;
}
size_t nxnet_job_copy(NxJob *j, size_t off, void *dst, size_t max) {
  if (!j) return 0; SDL_LockMutex(j->bmx); const char *src = j->live ? j->live->p : (j->done ? j->resp.body : NULL); size_t n = j->live ? j->live->n : (j->done ? j->resp.len : 0);
  size_t k = 0; if (src && off < n) { k = n - off; if (k > max) k = max; memcpy(dst, src + off, k); } SDL_UnlockMutex(j->bmx); return k;
}
int nxnet_job_status(NxJob *j) { if (!j) return -1; SDL_LockMutex(j->bmx); int st = j->done ? j->resp.status : 0; SDL_UnlockMutex(j->bmx); return st; }
const char *nxnet_job_headers(NxJob *j) { if (!j || !j->done) return ""; return j->dbg[0] ? j->dbg : (j->resp.headers ? j->resp.headers : j->resp.err); }
