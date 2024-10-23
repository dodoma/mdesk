#ifndef __BEE_H__
#define __BEE_H__

/*
 * bee, 音源上的业务逻辑实现
 *
 * 诸如读写音频文件、建立索引、文件比对、硬件设置、读写数据库、播放控制等，分成了不同的业务线程处理
 */

typedef enum {
    SYNC_RAWFILE = 0,
    SYNC_TRACK_COVER,
    SYNC_ARTIST_COVER,
    SYNC_ALBUM_COVER,
    SYNC_PONG,
} SYNC_TYPE;

typedef struct queue_entry {
    uint16_t seqnum;
    uint16_t command;
    NetClientNode *client;
    MDF *nodein;
    MDF *nodeout;

    struct queue_entry *next;
} QueueEntry;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    ssize_t size;
    QueueEntry *top;
    QueueEntry *bottom;
} QueueManager;

typedef struct {
    const char *name;
    MLIST *users;               /* list of NetClientNode* */
} Channel;

typedef struct bee_entry {
    uint8_t id;                 /* 与 FRAME_TYPE 部分对应 */
    const char *name;
    bool running;

    QueueManager *op_queue;
    pthread_t *op_thread;

    MLIST *users;               /* list of NetClientNode* */
    MLIST *channels;

    bool (*process)(struct bee_entry *e, QueueEntry *qe);
    void (*stop)(struct bee_entry *e);
} BeeEntry;

typedef struct {
    uint8_t id;
    const char *name;
    BeeEntry* (*init_driver)(void);
} BeeDriver;

MERR* beeStart();
void beeStop();
BeeEntry* beeFind(uint8_t id);

Channel* channelFind(MLIST *channels, const char *name, bool create);
bool channelEmpty(Channel *slot);
bool channelHas(Channel *slot, NetClientNode *client);
bool channelJoin(Channel *slot, NetClientNode *client);
void channelLeft(Channel *slot, NetClientNode *client);
void channelSend(Channel *slot, uint8_t *bufsend, size_t sendlen);

QueueManager* queueCreate();
void queueFree(QueueManager *queue);

QueueEntry* queueEntryCreate(uint16_t seqnum, uint16_t command, NetClientNode *client, MDF *datanode);
void queueEntryFree(void *p);

QueueEntry* queueEntryGet(QueueManager *queue);
void queueEntryPush(QueueManager *queue, QueueEntry *qe);

void binaryPush(BeeEntry *be, SYNC_TYPE stype, NetBinaryNode *client);

#endif  /* ___BEE_H__ */
