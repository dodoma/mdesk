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

CommandPacket* packetCommandFill(uint8_t *buf, size_t buflen)
{
    static uint16_t seqnum = 0xEA;

    if (!buf || buflen < sizeof(CommandPacket)) return NULL;

    memset(buf, 0x0, buflen);
    if (seqnum > 0xFFFA) seqnum = 0;

    CommandPacket *packet = (CommandPacket*)buf;
    packet->sof = PACKET_SOF;
    packet->idiot = 1;
    packet->length = buflen;    /* 临时存放，真正填包时用作长度判断 */
    packet->seqnum = seqnum & 0xFFFF;

    seqnum++;

    return packet;
}

size_t packetBroadcastFill(CommandPacket *packet,
                           const char *cpuid, uint16_t port_contrl, uint16_t port_binary)
{
    if (!packet || !cpuid) return 0;

    uint8_t *bufhead = (uint8_t*)packet;

    packet->frame_type = FRAME_MSG;
    packet->command = CMD_BROADCAST;

    uint8_t *buf = packet->data;
    int idlen = strlen(cpuid);
    memcpy(buf, cpuid, idlen);
    buf += idlen;
    *buf = 0x0; buf++;

    *(uint16_t*)buf = port_contrl; buf += 2;
    *(uint16_t*)buf = port_binary; buf += 2;

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

size_t packetACKFill(CommandPacket *packet, uint16_t seqnum, uint16_t command,
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
        *buf = 0x0; buf++;
    }

    size_t packetlen = buf - bufhead + 4;
    packet->length = packetlen;

    return packetlen;
}

size_t packetDataFill(CommandPacket *packet, FRAME_TYPE type, uint16_t command, MDF *datanode)
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

bool packetCRCFill(CommandPacket *packet)
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

CommandPacket* packetCommandGot(uint8_t *buf, ssize_t len)
{
    if (!buf || len < sizeof(CommandPacket)) return NULL;

    CommandPacket *packet = (CommandPacket*)buf;

    if (packet->sof != PACKET_SOF) return NULL;
    if (packet->idiot != 1) return NULL;
    if (packet->length != len) return NULL;
    /* TODO crc valid */

    return packet;
}
