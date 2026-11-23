#ifndef GOLIVE_H
#define GOLIVE_H
#include <stdbool.h>
/* Go Live viewer: the second RTC connection a stream needs (own voice websocket, UDP, transport crypto and DAVE
 * group), receive-only. Started by gw.c once STREAM_CREATE + STREAM_SERVER_UPDATE arrived for the watched stream.
 * Video goes to vrecv (VR_CONN_STREAM, mapped to the streamer's user id); stream audio is not played yet. */
enum { GL_OFF, GL_CONNECTING, GL_CONNECTED, GL_ERROR };
void golive_start(const char *stream_key, const char *rtc_server_id, const char *rtc_channel_id, const char *user_id,
                  const char *session_id, const char *token, const char *endpoint);
void golive_stop(void);                       /* any thread except the golive threads */
int golive_state(char *err, int errcap);      /* GL_*; err gets the last error text */
const char *golive_owner(void);               /* user id of the streamer being watched ("" when off) */
#endif
