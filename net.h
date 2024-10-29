#ifndef __NET_H__
#define __NET_H__

#define LEN_CLIENTID 11
#define LEN_PACKET_NORMAL 1024
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
    bool dropped;
} NetNode;

typedef struct {
    NetNode base;

    //time_t ping;                /* last time when receive ping packet */
} NetHornNode;

typedef struct {
    NetNode base;
    char id[LEN_CLIENTID];
    struct _net_binary_node *binary;

    uint8_t *buf;               /* receive buffer */
    ssize_t recvlen;
    uint8_t bufsend[LEN_PACKET_NORMAL];

    MLIST *bees;                /* list of BeeEntry* */
    MLIST *channels;            /* list of Channel* */
} NetClientNode;

typedef struct _net_binary_node {
    NetNode base;
    NetClientNode *contrl;

    uint8_t *buf;               /* receive buffer */
    ssize_t recvlen;

    bool in_business;
} NetBinaryNode;

MERR* netExposeME();

void netNodeFree(NetNode *node);

bool SSEND(int fd, uint8_t *buf, size_t len);

#endif  /* __NET_H__ */
