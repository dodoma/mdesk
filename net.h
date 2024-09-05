#ifndef __NET_H__
#define __NET_H__

typedef enum {
    NET_STREAM = 0,
    NET_STREAM_SSL,
    NET_HORN,
    NET_TIMER,
    NET_CLIENT,
    NET_CLIENT_SSL,
} NetNodeType;

typedef struct {
    int fd;
    NetNodeType type;
} NetNode;

typedef struct {
    NetNode base;

    time_t pong;                /* last time when receive heartbeat packet */
} NetHornNode;

typedef struct {
    NetNode base;
} NetClientNode;

MERR* netExposeME();
void netNodeFree(NetNode *node);

#endif  /* __NET_H__ */
