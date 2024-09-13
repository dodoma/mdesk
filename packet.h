#ifndef __PACKET_H__
#define __PACKET_H__

#define PACKET_SOF 0xAA         /* start of frame */
#define LEN_IDIOT 2
#define LEN_PREAMBLE 9
#define LEN_HEADER 13

typedef enum {
    IDIOT_PING = 101,
    IDIOT_PONG,
} IDIOT_INDICATOR;

typedef enum {
    FRAME_MSG = 0,         /* 硬解消息 */
    FRAME_CMD,             /* 硬解命令 */
    FRAME_ACK,             /* 简短回包（command, success, errmsg?） */
    FRAME_HARDWARE,        /* 音源控制相关 */
} FRAME_TYPE;

typedef enum {
    CMD_BROADCAST = 0,
} COMMAND_MSG;

typedef enum {
    CMD_WIFI_SET = 0,
} COMMAND_HDARDWAR;

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
} CommandPacket;
#pragma pack()

size_t packetPINGFill(uint8_t *buf, size_t buflen);
size_t packetPONGFill(uint8_t *buf, size_t buflen);

CommandPacket* packetCommandFill(uint8_t *buf, size_t buflen);
size_t packetBroadcastFill(CommandPacket *packet,
                           const char *cpuid, uint16_t port_contrl, uint16_t port_binary);
size_t packetDataFill(CommandPacket *packet, FRAME_TYPE type, uint16_t cmd, MDF *datanode); /* -_-! */
size_t packetACKFill(CommandPacket *packet, uint16_t seqnum, uint16_t command,
                     bool success, const char *errmsg);
bool packetCRCFill(CommandPacket *packet);

IdiotPacket* packetIdiotGot(uint8_t *buf, size_t len);
CommandPacket* packetCommandGot(uint8_t *buf, ssize_t len);

#endif  /* __PACKET_H__ */
