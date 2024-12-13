#ifndef __PACKET_H__
#define __PACKET_H__

#define PACKET_SOF 0xAA         /* start of frame */
#define LEN_IDIOT 2
#define LEN_PREAMBLE 9
#define LEN_HEADER 13

typedef enum {
    IDIOT_PING = 101,
    IDIOT_PONG,
    IDIOT_CONNECT,              /* contrl socket 申请 clientid */
    IDIOT_PLAY_STEP,
    IDIOT_USTICK_MOUNT,
    IDIOT_FREE,                 /*  闲着 */
    IDIOT_BUSY_INDEXING,
} IDIOT_INDICATOR;

typedef enum {
    FRAME_CMD = 0,         /* 硬解命令 */
    FRAME_ACK,             /* 简短回包（command, success, errmsg?） */
    FRAME_RESPONSE,        /* 完整回包（command, success, errmsg?, nodein?) */

    FRAME_HARDWARE,        /* 音源控制相关 */
    FRAME_AUDIO,           /* 播放相关 */
    FRAME_STORAGE
} FRAME_TYPE;

typedef enum {
    CMD_BROADCAST = 0,          /* (cpuid, port_contrl, port_binary) */
    CMD_CONNECT,                /* server 返回、 binary socket 上报 clientid (clientid) */
    CMD_SYNC,                   /* (filename, filelength(8 bytes) [file contents])  */
    CMD_STORE_LIST,             /* () */
} COMMAND_CMD;

typedef enum {
    CMD_WIFI_SET = 0,
    CMD_HOME_INFO,
    CMD_UDISK_INFO,             /* 查看 U 盘目录下媒体信息 */
    CMD_UDISK_COPY,             /* 拷贝 U 盘目录下媒体文件 */
    CMD_STORE_CREATE,           /* 创建媒体库 */
    CMD_STORE_RENAME,
    CMD_STORE_SET_DEFAULT,
    CMD_STORE_DELETE,
    CMD_STORE_MERGE,
    CMD_SET_AUTOPLAY,
} COMMAND_HDARDWAR;

typedef enum {
    CMD_PLAY = 0,               /* 播放（指定媒体文件，或者随机） */
    CMD_PLAY_INFO,              /* 查询当前播放信息 */
    CMD_PAUSE,
    CMD_RESUME,
    CMD_NEXT,
    CMD_PREVIOUS,
    CMD_DRAGTO,
    CMD_SET_SHUFFLE,
    CMD_SET_VOLUME,
    CMD_STORE_SWITCH,           /* 切换媒体库 */
} COMMAND_AUDIO;

typedef enum {
    CMD_DB_MD5 = 0,
    CMD_SYNC_PULL,
    CMD_REMOVE,                 /* 删除媒体文件 */
    CMD_SYNC_STORE,             /* 整库同步，慎用 */
} COMMAND_STORAGE;

typedef enum {
    SEQ_RESERVE = 0,
    SEQ_SERVER_CLOSED,
    SEQ_CONNECTION_LOST,
    SEQ_PLAY_INFO,              /* 查询当前播放信息（文件，艺术家等），音源切歌时可主动推送 */
    SEQ_PLAY_STEP,              /* 音源正常播放中 */
    SEQ_SYNC_REQ = 101,         /* libpocket 请求了热情期待返回的包 */
    SEQ_USER_START = 0x401,
} SYS_CALLBACK_SEQ;

#pragma pack(1)
typedef struct {
    uint8_t sof;
    uint8_t idiot;              /* idiot [0x65 ~ 0xFF] means IdiotPacket */
} IdiotPacket;

typedef struct {
    uint8_t  sof;
    uint8_t  idiot;             /* idiot [0x0 ~ 0x64] means CommandPacket, as version useage */
    uint32_t length;
    uint8_t  frame_type;
    uint16_t seqnum;
    uint16_t preamble_crc;

    uint16_t command;
    uint8_t data[1];
} MessagePacket;
#pragma pack()

size_t packetPINGFill(uint8_t *buf, size_t buflen);
size_t packetPONGFill(uint8_t *buf, size_t buflen);
size_t packetIdiotFill(uint8_t *buf, IDIOT_INDICATOR idiot);

/*
 * message packet 3 steps:
 * 1. init
 */
MessagePacket* packetMessageInit(uint8_t *buf, size_t buflen);

/*
 * 2. content fill
 */
size_t packetBroadcastFill(MessagePacket *packet,
                           const char *cpuid, uint16_t port_contrl, uint16_t port_binary);
size_t packetConnectFill(MessagePacket *packet, const char *clientid);
size_t packetBFileFill(MessagePacket *packet, const char *filename, uint64_t size);
size_t packetACKFill(MessagePacket *packet, uint16_t seqnum, uint16_t command,
                     bool success, const char *errmsg);
size_t packetResponseFill(MessagePacket *packet, uint16_t seqnum, uint16_t command,
                          bool success, const char *errmsg, MDF *datanode);
size_t packetDataFill(MessagePacket *packet, FRAME_TYPE type, uint16_t cmd, MDF *datanode); /* -_-! */
size_t packetNODataFill(MessagePacket *packet, FRAME_TYPE type, uint16_t command);

/*
 * 3. CRC fill
 */
bool packetCRCFill(MessagePacket *packet);

IdiotPacket* packetIdiotGot(uint8_t *buf, size_t len);
MessagePacket* packetMessageGot(uint8_t *buf, ssize_t len);

#endif  /* __PACKET_H__ */
