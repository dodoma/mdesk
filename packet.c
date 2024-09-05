#include <reef.h>

#include "packet.h"
#include "global.h"

CommandPacket* packetCommandFill(uint8_t *buf, size_t buflen)
{
    static uint16_t seqnum = 0xEA;

    if (!buf || buflen < sizeof(CommandPacket)) return NULL;

    memset(buf, 0x0, buflen);
    if (seqnum > 0xFFFA) seqnum = 0;

    CommandPacket *packet = (CommandPacket*)buf;
    packet->sof = 0xAB;
    packet->version = 1;
    packet->seqnum = seqnum & 0xFFFF;

    seqnum++;

    return packet;
}

size_t packetBroadcastFill(CommandPacket *packet)
{
    if (!packet) return 0;

    int idlen = strlen(g_cpuid);
    uint8_t *bufhead = (uint8_t*)packet;

    packet->frame_type = FRAME_TYPE_MSG;
    packet->cmd_set = CMDSET_GENERAL;
    packet->cmd_id = CMD_BROADCAST;

    uint8_t *buf = packet->data;
    memcpy(buf, g_cpuid, idlen);
    buf += idlen;
    *buf = 0x0; buf++;
    /* TODO ip, model */

    size_t packetlen = buf - bufhead + 4;
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
