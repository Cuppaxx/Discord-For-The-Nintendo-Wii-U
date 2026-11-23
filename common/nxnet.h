#ifndef NXNET_H
#define NXNET_H
#include <stddef.h>
#include <stdbool.h>

typedef struct {
  int status;            /* HTTP status, 0 = transport error */
  char *body; size_t len;/* malloc'd, NUL-terminated; caller frees with nxnet_free */
  char ctype[96];        /* Content-Type */
  char final_url[2600];  /* after redirects */
  char err[160];
  char *headers;         /* all response headers of the final hop, "Name: value\r\n" lines (malloc'd) */
  void *user;            /* set by a post callback (nxnet_start_ex) */
} NxResp;

typedef struct NxJob NxJob;
/* optional cookie hooks (browser installs them): return 1 and fill out with "a=b; c=d" if cookies apply */
extern int (*nxnet_cookie_get)(const char *url, char *out, size_t cap);
extern void (*nxnet_cookie_set)(const char *url, const char *setcookie);

void nxnet_init(void);
int nxnet_ca_len(void); const char *nxnet_ca_data(void);
void nxnet_free(NxResp *r);
/* blocking */
void nxnet_request(NxResp *out, const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes);
/* async: runs on a worker thread; poll with nxnet_job_done */
NxJob *nxnet_start(const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes);
typedef void (*NxPostFn)(NxResp *r, void *ud);   /* runs on the worker thread after the transfer (decode an image there, not on the UI thread) */
NxJob *nxnet_start_ex(const char *method, const char *url, const char **headers, const char *body, size_t body_len, size_t max_bytes, NxPostFn post, void *ud);
bool nxnet_job_done(NxJob *j);
void nxnet_job_take(NxJob *j, NxResp *out);   /* only after done; frees the job */
void nxnet_job_cancel(NxJob *j);              /* result discarded when finished */
void nxnet_cache_clear(void);
int nxnet_job_status(NxJob *j);
const char *nxnet_job_headers(NxJob *j);                     /* response headers once done (debugging) */                              /* HTTP status once done, 0 while running */
size_t nxnet_job_avail(NxJob *j, size_t *total);           /* progressive: bytes downloaded so far, total if known */
size_t nxnet_job_copy(NxJob *j, size_t off, void *dst, size_t max);   /* copy bytes [off, off+max) of what has arrived */                 /* drop the in-memory response cache (css/js/images) */              /* result discarded when finished */
#endif

