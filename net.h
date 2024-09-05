#ifndef __NET_H__
#define __NET_H__

typedef struct {
    int fd;
    int fd_ssl;
    int fd_horn;
    int fd_timer;

    time_t pong;                /* last time when receive heartbeat packet */
} NetNode;

MERR* netExposeME();

#endif  /* __NET_H__ */
