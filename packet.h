#ifndef __PACKET_H__
#define __PACKET_H__

#define PACKET_SOF 0xAA         /* start of frame */
#define LEN_IDIOT 2
#define LEN_PREAMBLE 7

typedef enum {
    IDIOT_PING = 101,
    IDIOT_PONG,
} IDIOT_INDICATOR;

typedef enum {
    FRAME_TYPE_CMD = 0,
    FRAME_TYPE_ACK,
    FRAME_TYPE_MSG
} FRAME_TYPE;

typedef enum {
    CMDSET_GENERAL = 0,
    CMDSET_CONTROL,
    CMDSET_FILE
} COMMAND_SET;

typedef enum {
    CMD_BROADCAST = 0,
    CMD_HEARTBEAT,
} COMMAND_ID;

#pragma pack(1)
typedef struct {
    uint8_t sof;
    uint8_t idiot;              /* idiot [0x65 ~ 0xFF] means IdiotPacket */
} IdiotPacket;

typedef struct {
    uint8_t  sof;
    uint8_t  idiot;             /* idiot [0x0 ~ 0x64] means CommandPacket, as version useage */
    uint16_t length;
    uint8_t  frame_type;
    uint16_t seqnum;
    uint16_t preamble_crc;

    uint8_t cmd_set;
    uint8_t cmd_id;
    uint8_t data[1];
} CommandPacket;
#pragma pack()

size_t packetPINGFill(uint8_t *buf, size_t buflen);
size_t packetPONGFill(uint8_t *buf, size_t buflen);

CommandPacket* packetCommandFill(uint8_t *buf, size_t buflen);
size_t packetBroadcastFill(CommandPacket *packet,
                           const char *cpuid, uint16_t port_contrl, uint16_t port_binary);
bool packetCRCFill(CommandPacket *packet);

IdiotPacket* packetIdiotGot(uint8_t *buf, size_t len);
CommandPacket* packetCommandGot(uint8_t *buf, ssize_t len);

#endif  /* __PACKET_H__ */
