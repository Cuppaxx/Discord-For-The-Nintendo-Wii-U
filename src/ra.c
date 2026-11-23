#include "ra.h"
#include "dc.h"
#include "nxnet.h"
#include "dlog.h"
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <jansson.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <coreinit/thread.h>
#include <whb/file.h>
#include <whb/sdcard.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static RaState S; static volatile int cancel_flag; static SDL_Thread *th; static volatile int running;

static void fail(const char *m) { snprintf(S.err, sizeof S.err, "%s", m); S.state = RA_ERROR; }

/* ---------- small helpers ---------- */
static int b64enc(const unsigned char *in, size_t n, char *out, size_t cap) { size_t o = 0; if (mbedtls_base64_encode((unsigned char *)out, cap, &o, in, n)) return -1; out[o] = 0; return (int)o; }
static int b64dec(const char *in, unsigned char *out, size_t cap) { size_t o = 0; if (mbedtls_base64_decode(out, cap, &o, (const unsigned char *)in, strlen(in))) return -1; return (int)o; }

/* ---------- key handling ---------- */
static mbedtls_entropy_context ent; static mbedtls_ctr_drbg_context drbg; static mbedtls_pk_context pk;
static char keypath[260];

static bool load_or_make_key(void) {
  mbedtls_pk_init(&pk);
  FILE *f = fopen(keypath, "rb");
  if (f) { unsigned char buf[2600]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n > 100 && mbedtls_pk_parse_key(&pk, buf, n, NULL, 0) == 0 && mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA) return true;
    mbedtls_pk_free(&pk); mbedtls_pk_init(&pk); }
  S.state = RA_KEYGEN;
  if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) { fail("pk setup failed"); return false; }
  if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random, &drbg, 2048, 65537) != 0) { fail("RSA key generation failed"); return false; }
  static unsigned char out[2600]; int n = mbedtls_pk_write_key_der(&pk, out, sizeof out);
  if (n > 0) { char dir[260]; snprintf(dir, sizeof dir, "%s", keypath); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; char parent[260]; snprintf(parent, sizeof parent, "%s", dir); char *s2 = strrchr(parent, '/'); if (s2) { *s2 = 0; mkdir(parent, 0777); } mkdir(dir, 0777); }
    FILE *w = fopen(keypath, "wb"); if (w) { fwrite(out + sizeof out - n, 1, n, w); fclose(w); } }
  return true;
}

/* ---------- WebSocket over libcurl's connect-only TLS socket ---------- */
typedef struct { CURL *c; unsigned char rb[65536]; size_t rn; } Ws;

static bool ws_send_raw(Ws *w, const unsigned char *d, size_t n) {
  size_t off = 0; int spins = 0;
  while (off < n) { size_t sent = 0; CURLcode rc = curl_easy_send(w->c, d + off, n - off, &sent); if (rc == CURLE_OK) { off += sent; spins = 0; } else if (rc == CURLE_AGAIN) { SDL_Delay(10); if (++spins > 500) return false; } else return false; }
  return true;
}
static bool ws_send(Ws *w, int opcode, const char *payload, size_t n) {
  unsigned char hdr[14]; int h = 0; hdr[h++] = 0x80 | opcode;
  if (n < 126) hdr[h++] = 0x80 | (unsigned char)n; else { hdr[h++] = 0x80 | 126; hdr[h++] = (unsigned char)(n >> 8); hdr[h++] = (unsigned char)n; }
  unsigned char mask[4]; mbedtls_ctr_drbg_random(&drbg, mask, 4); memcpy(hdr + h, mask, 4); h += 4;
  unsigned char *buf = malloc(h + n); memcpy(buf, hdr, h); for (size_t i = 0; i < n; i++) buf[h + i] = (unsigned char)payload[i] ^ mask[i & 3];
  bool ok = ws_send_raw(w, buf, h + n); free(buf); return ok;
}
/* returns 1 = text message in out, 0 = nothing yet, -1 = closed/error */
static int ws_poll(Ws *w, char *out, size_t cap) {
  for (;;) {
    if (w->rn >= 2) {
      unsigned char op = w->rb[0] & 0x0F; size_t len = w->rb[1] & 0x7F, hl = 2;
      if (len == 126) { if (w->rn < 4) goto more; len = (w->rb[2] << 8) | w->rb[3]; hl = 4; }
      else if (len == 127) { if (w->rn < 10) goto more; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | w->rb[2 + i]; hl = 10; }
      if (len > sizeof w->rb - 16) return -1;
      if (w->rn < hl + len) goto more;
      int result = 0;
      if (op == 1 || op == 0) { size_t c = len < cap - 1 ? len : cap - 1; memcpy(out, w->rb + hl, c); out[c] = 0; result = 1; }
      else if (op == 8) { dlog("[ra] close frame: code %d reason \"%.*s\"", len >= 2 ? (w->rb[hl] << 8 | w->rb[hl + 1]) : -1, (int)(len > 2 ? len - 2 : 0), (const char *)w->rb + hl + 2); result = -1; }
      else if (op == 9) ws_send(w, 0xA, (const char *)w->rb + hl, len);
      memmove(w->rb, w->rb + hl + len, w->rn - hl - len); w->rn -= hl + len;
      if (result) return result; continue;
    }
    more: { size_t got = 0; CURLcode rc = curl_easy_recv(w->c, w->rb + w->rn, sizeof w->rb - w->rn, &got);
      if (rc == CURLE_OK) { if (got == 0) return -1; w->rn += got; continue; } if (rc == CURLE_AGAIN) return 0; return -1; }
  }
}

static bool ws_connect(Ws *w, const char *url, const char *origin) {
  memset(w, 0, sizeof *w); w->c = curl_easy_init(); if (!w->c) return false;
  curl_easy_setopt(w->c, CURLOPT_URL, url); curl_easy_setopt(w->c, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(w->c, CURLOPT_CAPATH, "/vol/content/certs"); curl_easy_setopt(w->c, CURLOPT_CONNECTTIMEOUT, 15L); curl_easy_setopt(w->c, CURLOPT_NOSIGNAL, 1L);
  const char *ca = nxnet_ca_data(); if (ca) { struct curl_blob bl = {(void *)ca, (size_t)nxnet_ca_len(), CURL_BLOB_COPY}; curl_easy_setopt(w->c, CURLOPT_CAINFO_BLOB, &bl); }
  char eb[CURL_ERROR_SIZE] = ""; curl_easy_setopt(w->c, CURLOPT_ERRORBUFFER, eb);
  CURLcode rc = curl_easy_perform(w->c);
  if (rc != CURLE_OK) { char m[220]; snprintf(m, sizeof m, "Can't reach Discord login server: %s %s", curl_easy_strerror(rc), eb); fail(m); return false; }
  const char *p = strstr(url, "://"); p = p ? p + 3 : url; const char *sl = strchr(p, '/'); char host[200]; snprintf(host, sizeof host, "%.*s", (int)(sl ? sl - p : (int)strlen(p)), p); const char *path = sl ? sl : "/";
  unsigned char kb[16]; mbedtls_ctr_drbg_random(&drbg, kb, 16); char key[40]; b64enc(kb, 16, key, sizeof key);
  char req[900]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36\r\n\r\n", path, host, key, origin);
  if (!ws_send_raw(w, (unsigned char *)req, n)) { fail("Could not send WebSocket handshake"); return false; }
  char resp[2048]; size_t rn = 0; uint32_t t0 = SDL_GetTicks();
  while (rn < sizeof resp - 1 && SDL_GetTicks() - t0 < 15000 && !cancel_flag) {
    size_t got = 0; CURLcode r2 = curl_easy_recv(w->c, resp + rn, sizeof resp - 1 - rn, &got);
    if (r2 == CURLE_OK && got) { rn += got; resp[rn] = 0; char *e = strstr(resp, "\r\n\r\n"); if (e) { size_t used = (e + 4) - resp; if (rn > used) { memcpy(w->rb, resp + used, rn - used); w->rn = rn - used; } break; } }
    else if (r2 == CURLE_AGAIN) SDL_Delay(20); else { fail("Connection closed during handshake"); return false; }
  }
  if (strncmp(resp, "HTTP/1.1 101", 12)) { char m[200]; snprintf(m, sizeof m, "WebSocket upgrade refused: %.60s", resp); char *nl = strpbrk(m, "\r\n"); if (nl) *nl = 0; fail(m); return false; }
  return true;
}

/* ---------- protocol ---------- */
static const char *js(json_t *o, const char *k) { json_t *v = json_object_get(o, k); return json_is_string(v) ? json_string_value(v) : ""; }

static bool rsa_decrypt_b64(const char *b64, unsigned char *out, size_t cap, size_t *olen) {
  unsigned char in[600]; int n = b64dec(b64, in, sizeof in); if (n <= 0) return false;
  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk); mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
  return mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_RSA_PRIVATE, NULL, 0, olen, in, out, cap) == 0;
}

static void send_json(Ws *w, json_t *o) { char *s = json_dumps(o, JSON_COMPACT); ws_send(w, 1, s, strlen(s)); free(s); json_decref(o); }

static void do_login(const char *ticket) {
  S.state = RA_LOGIN;
  json_t *o = json_object(); json_object_set_new(o, "ticket", json_string(ticket)); char *body = json_dumps(o, JSON_COMPACT); json_decref(o);
  char url[300]; snprintf(url, sizeof url, "%s/users/@me/remote-auth/login", dc_api());
  const char *h[] = {"Content-Type: application/json", "Accept: application/json", "Origin: https://discord.com", "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/124.0 Safari/537.36", NULL};
  NxResp r; nxnet_request(&r, "POST", url, h, body, 0, 1 << 16); free(body);
  if (r.status == 200) {
    json_t *j = json_loads(r.body, 0, NULL); const char *et = j ? js(j, "encrypted_token") : "";
    unsigned char tok[500]; size_t tl = 0;
    if (et[0] && rsa_decrypt_b64(et, tok, sizeof tok - 1, &tl) && tl > 20 && tl < 400) { memcpy(S.token, tok, tl); S.token[tl] = 0; S.state = RA_DONE; }
    else fail("Could not decrypt the login token");
    json_decref(j);
  } else if (r.status == 400 && strstr(r.body, "captcha")) fail("Discord asked for a captcha. Use a token file instead.");
  else { char m[200]; snprintf(m, sizeof m, "Login failed (HTTP %d) %.100s", r.status, r.err); fail(m); }
  nxnet_free(&r);
}

static int worker(void *arg) {
  (void)arg;
  OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2); OSSetThreadPriority(OSGetCurrentThread(), 20);
  mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
  if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"mingus-ra", 9) != 0) { fail("No entropy source for key generation"); goto out; }
  WHBMountSdCard(); const char *m = WHBGetSdCardMountPath(); if (!m) m = "fs:/vol/external01";
  snprintf(keypath, sizeof keypath, "%s/wiiu/apps/MingusDiscord/ra_key.der", m);
  S.state = RA_CONNECTING;
  if (!load_or_make_key()) goto out;
  if (cancel_flag) goto out;
  S.state = RA_CONNECTING;
  Ws w; const char *url = dc_ra_url();
  dlog("[ra] connecting %s", url); if (!ws_connect(&w, url, "https://discord.com")) { dlog("[ra] connect failed: %s", S.err); if (w.c) curl_easy_cleanup(w.c); goto out; }
  uint32_t hb_iv = 41250, last_hb = SDL_GetTicks(), start = SDL_GetTicks(); unsigned char nonce[256]; (void)nonce;
  char msg[8192];
  while (!cancel_flag && S.state != RA_DONE && S.state != RA_ERROR) {
    int r = ws_poll(&w, msg, sizeof msg);
    if (r < 0) { dlog("[ra] ws closed (state %d)", (int)S.state); if (S.state != RA_DONE && S.state != RA_LOGIN) fail("Login server closed the connection"); break; }
    if (r == 1) {
      json_t *j = json_loads(msg, 0, NULL); if (j) {
        const char *op = js(j, "op"); dlog("[ra] recv op=%s", op);
        if (!strcmp(op, "hello")) {
          json_t *iv = json_object_get(j, "heartbeat_interval"); if (json_is_integer(iv)) hb_iv = (uint32_t)json_integer_value(iv);
          unsigned char der[600]; int n = mbedtls_pk_write_pubkey_der(&pk, der, sizeof der); char b64[900];
          if (n > 0 && b64enc(der + sizeof der - n, n, b64, sizeof b64) > 0) { json_t *o = json_object(); json_object_set_new(o, "op", json_string("init")); json_object_set_new(o, "encoded_public_key", json_string(b64)); send_json(&w, o); }
          else { fail("Could not encode public key"); }
        } else if (!strcmp(op, "nonce_proof")) {
          unsigned char nn[300]; size_t nl = 0;
          if (rsa_decrypt_b64(js(j, "encrypted_nonce"), nn, sizeof nn, &nl)) { dlog("[ra] nonce decrypted, %zu bytes", nl);
            char p[500]; if (b64enc(nn, nl, p, sizeof p) < 0) { fail("Nonce too large"); json_decref(j); break; }   /* v2: proof = base64url(decrypted nonce), no hashing */
            for (char *c = p; *c; c++) { if (*c == '+') *c = '-'; else if (*c == '/') *c = '_'; } char *eq = strchr(p, '='); while (eq && *eq == '=') *eq++ = 0;
            json_t *o = json_object(); json_object_set_new(o, "op", json_string("nonce_proof")); json_object_set_new(o, "nonce", json_string(p)); send_json(&w, o);
          } else fail("Could not decrypt server nonce");
        } else if (!strcmp(op, "pending_remote_init")) { snprintf(S.fingerprint, sizeof S.fingerprint, "%s", js(j, "fingerprint")); S.state = RA_QR; }
        else if (!strcmp(op, "pending_ticket")) {
          unsigned char up[400]; size_t ul = 0; S.user[0] = 0;
          if (rsa_decrypt_b64(js(j, "encrypted_user_payload"), up, sizeof up - 1, &ul)) { up[ul] = 0; char *f = strchr((char *)up, ':'); for (int k = 0; k < 2 && f; k++) f = strchr(f + 1, ':'); snprintf(S.user, sizeof S.user, "%s", f ? f + 1 : (char *)up); }
          S.state = RA_SCANNED;
        } else if (!strcmp(op, "pending_login")) { char tk[600]; snprintf(tk, sizeof tk, "%s", js(j, "ticket")); json_decref(j); do_login(tk); break; }
        else if (!strcmp(op, "cancel")) { fail("Login was cancelled on the phone"); }
        json_decref(j);
      }
    }
    uint32_t now = SDL_GetTicks();
    if (now - last_hb >= hb_iv) { ws_send(&w, 1, "{\"op\":\"heartbeat\"}", 18); last_hb = now; }
    if (now - start > 125000 && S.state != RA_LOGIN) { fail("QR code expired"); break; }
    SDL_Delay(25);
  }
  if (w.c) curl_easy_cleanup(w.c);
out:
  mbedtls_pk_free(&pk); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
  running = 0; return 0;
}

void ra_start(void) {
  if (running) { cancel_flag = 1; if (th) { SDL_WaitThread(th, NULL); th = NULL; } }
  memset(&S, 0, sizeof S); cancel_flag = 0; running = 1; S.state = RA_CONNECTING;
  th = SDL_CreateThreadWithStackSize(worker, "ra", 512 * 1024, NULL);
  if (!th) { running = 0; fail("Could not start login thread"); }
}
void ra_cancel(void) { cancel_flag = 1; if (th) { SDL_WaitThread(th, NULL); th = NULL; } S.state = RA_IDLE; running = 0; }
const RaState *ra_get(void) { return &S; }
