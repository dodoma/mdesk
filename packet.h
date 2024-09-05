#ifndef __PACKET_H__
#define __PACKET_H__

#define LEN_PREAMBLE 7

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
    uint8_t  sof;
    uint8_t  version;
    uint16_t length;
    uint8_t  frame_type;
    uint16_t seqnum;
    uint16_t preamble_crc;

    uint8_t cmd_set;
    uint8_t cmd_id;
    uint8_t data[1];
} CommandPacket;
#pragma pack()

CommandPacket* packetCommandFill(uint8_t *buf, size_t buflen);
size_t packetBroadcastFill(CommandPacket *packet);
bool packetCRCFill(CommandPacket *packet);

#endif  /* __PACKET_H__ */
