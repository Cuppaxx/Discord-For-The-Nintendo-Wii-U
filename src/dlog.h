#ifndef DLOG_H
#define DLOG_H
/* one mutexed log file (SD:/wiiu/apps/MingusDiscord/log.txt); safe from any thread, never call per audio frame */
void dlog_init(const char *dir);
void dlog(const char *fmt, ...);
#endif
