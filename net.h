#ifndef __NET_H__
#define __NET_H__

#define LEN_PACKET_NORMAL 512
#define CONTRL_PACKET_MAX_LEN 10485760

typedef enum {
    NET_CONTRL = 0,
    NET_BINARY,
    NET_HORN,
    NET_TIMER,
    NET_CLIENT_CONTRL,
    NET_CLIENT_BINARY,
} NetNodeType;

typedef struct {
    int fd;
    NetNodeType type;
} NetNode;

typedef struct {
    NetNode base;

    time_t ping;                /* last time when receive ping packet */
} NetHornNode;

typedef struct {
    NetNode base;

    uint8_t *buf;               /* receive buffer */
    ssize_t recvlen;
    uint8_t bufsend[LEN_PACKET_NORMAL];

    bool in_business;
    bool dropped;
} NetClientNode;

MERR* netExposeME();

void netNodeFree(NetNode *node);
void netHornPing();

bool SSEND(int fd, uint8_t *buf, size_t len);

#endif  /* __NET_H__ */
