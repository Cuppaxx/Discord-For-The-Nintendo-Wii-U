#ifndef RA_H
#define RA_H
/* Discord QR-code login ("remote auth" v2): scan the QR with the Discord phone app to log this device in. */
enum { RA_IDLE, RA_KEYGEN, RA_CONNECTING, RA_QR, RA_SCANNED, RA_LOGIN, RA_DONE, RA_ERROR };
typedef struct {
  volatile int state;
  char fingerprint[100];     /* QR payload = https://discord.com/ra/<fingerprint> */
  char user[100];            /* who scanned (shown while waiting for phone confirmation) */
  char token[420];           /* set when state == RA_DONE */
  char err[220];
} RaState;

void ra_start(void);          /* begins (or restarts) a login attempt on a worker thread */
void ra_cancel(void);
const RaState *ra_get(void);
#endif
