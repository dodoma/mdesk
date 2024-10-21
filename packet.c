#include <reef.h>

#include "packet.h"
#include "net.h"
#include "global.h"

size_t packetPINGFill(uint8_t *buf, size_t buflen)
{
    if (!buf || buflen < LEN_IDIOT) return 0;

    memset(buf, 0x0, buflen);

    *buf = PACKET_SOF; buf++;
    *buf = IDIOT_PING; buf++;

    return LEN_IDIOT;
}

size_t packetPONGFill(uint8_t *buf, size_t buflen)
{
    if (!buf || buflen < LEN_IDIOT) return 0;

    memset(buf, 0x0, buflen);

    *buf = PACKET_SOF; buf++;
    *buf = IDIOT_PONG; buf++;

    return LEN_IDIOT;
}

size_t packetIdiotFill(uint8_t *buf, IDIOT_INDICATOR idiot)
{
    if (!buf) return 0;

    memset(buf, 0x0, LEN_IDIOT);

    *buf = PACKET_SOF; buf++;
    *buf = idiot; buf++;

    return LEN_IDIOT;
}

MessagePacket* packetMessageInit(uint8_t *buf, size_t buflen)
{
    static uint16_t seqnum = SEQ_USER_START; /* 0 ~ 1024 的 seqnum 被系统固定回调占用 */

    if (!buf || buflen < sizeof(MessagePacket)) return NULL;

    memset(buf, 0x0, buflen);
    if (seqnum > 0xFFFA) seqnum = SEQ_USER_START;

    MessagePacket *packet = (MessagePacket*)buf;
    packet->sof = PACKET_SOF;
    packet->idiot = 1;
    packet->length = buflen;    /* 临时存放，真正填包时用作长度判断 */
    packet->seqnum = seqnum & 0xFFFF;

    seqnum++;

    return packet;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * n     cpuid     n
 * |      \0       |
 * |     Port      |
 * |       contrl  |
 * |     Port      |
 * |       binary  |
 * \---------------/
 */
size_t packetBroadcastFill(MessagePacket *packet,
                           const char *cpuid, uint16_t port_contrl, uint16_t port_binary)
{
    if (!packet || !cpuid) return 0;

    uint8_t *bufhead = (uint8_t*)packet;

    packet->frame_type = FRAME_CMD;
    packet->command = CMD_BROADCAST;

    uint8_t *buf = packet->data;
    int slen = strlen(cpuid);
    memcpy(buf, cpuid, slen);
    buf += slen;
    *buf = 0x0; buf++;

    *(uint16_t*)buf = port_contrl; buf += 2;
    *(uint16_t*)buf = port_binary; buf += 2;

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * n   clientid    n
 * |      \0       |
 * \---------------/
 */
size_t packetConnectFill(MessagePacket *packet, const char *clientid)
{
    if (!packet || !clientid) return 0;

    uint8_t *bufhead = (uint8_t*)packet;

    packet->frame_type = FRAME_CMD;
    packet->command = CMD_CONNECT;

    uint8_t *buf = packet->data;
    int slen = strlen(clientid);
    memcpy(buf, clientid, slen);
    buf += slen;
    *buf = 0x0; buf++;

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * n   filename    n
 * |      \0       |
 * 8   filesize    8
 * \---------------/
 */
size_t packetBFileFill(MessagePacket *packet, const char *filename, uint64_t size)
{
    if (!packet || !filename) return 0;

    uint8_t *bufhead = (uint8_t*)packet;

    packet->frame_type = FRAME_CMD;
    packet->command = CMD_SYNC;

    uint8_t *buf = packet->data;
    int slen = strlen(filename);
    memcpy(buf, filename, slen);
    buf += slen;
    *buf = 0x0; buf++;

    *(uint64_t*)buf = size;
    buf += 8;

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * |   success     |
 * ...  errmsg   ...
 * |      \0       |
 * \---------------/
 */
size_t packetACKFill(MessagePacket *packet, uint16_t seqnum, uint16_t command,
                     bool success, const char *errmsg)
{
    if (!packet || (errmsg && strlen(errmsg) > LEN_PACKET_NORMAL - LEN_HEADER - 4)) return 0;

    uint8_t *bufhead = (uint8_t*)packet;

    packet->seqnum = seqnum & 0xFFFF;
    packet->frame_type = FRAME_ACK;
    packet->command = command;

    uint8_t *buf = packet->data;
    *buf = success; buf++;

    if (errmsg) {
        int msglen = strlen(errmsg);
        memcpy(buf, errmsg, msglen);
        buf += msglen;
    }
    *buf = 0x0; buf++;

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * |   success     |
 * ...  errmsg   ...
 * |      \0       |
 * ... message pack ...
 * \---------------/
 */
size_t packetResponseFill(MessagePacket *packet, uint16_t seqnum, uint16_t command,
                          bool success, const char *errmsg, MDF *datanode)
{
    if (!packet) return 0;

    packet->seqnum = seqnum & 0xFFFF;
    packet->frame_type = FRAME_RESPONSE;
    packet->command = command;

    uint8_t *buf = packet->data;
    *buf = success; buf++;

    int msglen = 0;
    if (errmsg) {
        msglen = strlen(errmsg);
        memcpy(buf, errmsg, msglen);
        buf += msglen;
    }
    *buf = 0x0; buf++;

    size_t mpacklen = 0;
    if (datanode) {
        mpacklen = mdf_mpack_serialize(datanode, buf, packet->length - msglen - LEN_HEADER - 2);
        if (mpacklen == 0) return 0;
    }

    size_t packetlen = 1 + msglen + 1 + mpacklen + LEN_HEADER + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * ...message pack...
 * \---------------/
 */
size_t packetDataFill(MessagePacket *packet, FRAME_TYPE type, uint16_t command, MDF *datanode)
{
    if (!packet || !datanode) return 0;

    packet->frame_type = type;
    packet->command = command;

    size_t mpacklen = mdf_mpack_serialize(datanode, packet->data, packet->length - LEN_HEADER);
    if (mpacklen == 0) return 0;

    size_t packetlen = mpacklen + LEN_HEADER + 4;
    packet->length = packetlen;

    return packetlen;
}

/*
 * 0 1 2 3 4 5 6 7 8
 * /---------------\
 * \---------------/
 */
size_t packetNODataFill(MessagePacket *packet, FRAME_TYPE type, uint16_t command)
{
    if (!packet) return 0;

    packet->frame_type = type;
    packet->command = command;

    packet->length = LEN_HEADER + 4;

    return packet->length;
}

bool packetCRCFill(MessagePacket *packet)
{
    if (!packet || packet->length < 4) return false;

    uint8_t *bufhead = (uint8_t*)packet;
    packet->preamble_crc = mcrc16(bufhead, LEN_PREAMBLE);

    uint8_t *buf = bufhead + packet->length - 4;
    uint32_t crc = mcrc32(bufhead, buf - bufhead);
    *buf = crc & 0xFF; buf++;
    *buf = (crc >> 8) & 0xFF; buf++;
    *buf = (crc >> 16) & 0xFF; buf++;
    *buf = (crc >> 24) & 0xFF; buf++;

    return true;
}

IdiotPacket* packetIdiotGot(uint8_t *buf, size_t len)
{
    if (!buf || len < LEN_IDIOT) return NULL;

    IdiotPacket *packet = (IdiotPacket*)buf;

    if (packet->sof != PACKET_SOF) return NULL;
    if (packet->idiot < IDIOT_PING) return NULL;

    return packet;
}

MessagePacket* packetMessageGot(uint8_t *buf, ssize_t len)
{
    if (!buf || len < sizeof(MessagePacket)) return NULL;

    MessagePacket *packet = (MessagePacket*)buf;

    if (packet->sof != PACKET_SOF) return NULL;
    if (packet->idiot != 1) return NULL;
    //if (packet->length != len) return NULL;
    /* TODO crc valid */

    return packet;
}
