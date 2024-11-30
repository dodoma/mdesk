#include <reef.h>

#include <sys/epoll.h>

#include "global.h"
#include "net.h"
#include "client.h"
#include "bee.h"
#include "binary.h"
#include "packet.h"

static uint8_t *m_recvbuf = NULL;

static bool _parse_packet(NetBinaryNode *client, MessagePacket *packet)
{
    mtc_mt_dbg("parse packet %d %d", packet->frame_type, packet->command);

    switch (packet->frame_type) {
    case FRAME_CMD:
        if (packet->command == CMD_CONNECT) {
            /* 绑定 contrl socket */
            char clientid[LEN_CLIENTID] = {0};
            uint8_t *buf = packet->data;

            int idlen = strlen((char*)buf);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
            strncpy(clientid, (char*)buf, idlen > LEN_CLIENTID ? LEN_CLIENTID : idlen);
#pragma GCC diagnostic pop
            clientid[LEN_CLIENTID-1] = 0;
            buf += idlen;
            buf++;              /* '\0' */

            NetClientNode *contrl = clientMatch(clientid);
            if (contrl) {
                mtc_mt_dbg("%d matched contrl socket %d by %s",
                           client->base.fd, contrl->base.fd, clientid);
                contrl->binary = client;
                client->contrl = contrl;
            }
        }
        break;
    default:
        mtc_mt_warn("unsupport frame %d", packet->frame_type);
        return false;
    }

    return true;
}

static bool _parse_recv(NetBinaryNode *client, uint8_t *recvbuf, size_t recvlen)
{
#define PARTLY_PACKET                                           \
    do {                                                        \
        if (!client->buf) {                                     \
            client->buf = mos_calloc(1, CONTRL_PACKET_MAX_LEN); \
            memcpy(client->buf, recvbuf, recvlen);              \
        }                                                       \
        client->recvlen = recvlen;                              \
        return true;                                            \
    } while (0)

    /* idiot packet ? */
    if (recvlen < LEN_IDIOT) PARTLY_PACKET;

    BeeEntry *be;
    IdiotPacket *ipacket = packetIdiotGot(recvbuf, recvlen);
    if (ipacket) {
        switch (ipacket->idiot) {
        case IDIOT_PING:
            /* 此时回 PONG 包可能会破坏 binary client 在 storage 中的回包顺序，造成客户端报 packet error */
            be = beeFind(FRAME_STORAGE);
            if (be) binaryPush(be, SYNC_PONG, client);
            break;
        case IDIOT_PONG:
        case IDIOT_CONNECT:
            break;
        default:
            mtc_mt_warn("unsupport idot packet %d", ipacket->idiot);
            break;
        }

        if (recvlen > LEN_IDIOT) {
            memmove(recvbuf, recvbuf + LEN_IDIOT, recvlen - LEN_IDIOT);
            /*
             * _parse_recv() 里面没把 client->recvlen 用来做条件判断
             * 且，在缺包时会根据 (recvlen - packet->length) 更新其值
             * 所以，此处不用处理 client->recvlen
             * 要处理的话，也要根据 client->buf 将其赋为 0 或 recvlen - packet->length
             */
            return _parse_recv(client, recvbuf, recvlen - LEN_IDIOT);
        } else mos_free(client->buf);
    } else {
        /* command packet ? */
        if (recvlen < LEN_HEADER + 4) PARTLY_PACKET;

        MessagePacket *packet = (MessagePacket*)recvbuf;
        if (packet && packet->sof == PACKET_SOF && packet->idiot == 1) {
            if (recvlen < packet->length) {
                if (packet->length > CONTRL_PACKET_MAX_LEN) {
                    /* 玩不起 */
                    binaryDrop(client);
                    return false;
                }

                PARTLY_PACKET;
            } else {
                _parse_packet(client, packet);

                if (recvlen > packet->length) {
                    size_t exceed = recvlen - packet->length;
                    memmove(recvbuf, recvbuf + packet->length, exceed);
                    return _parse_recv(client, recvbuf, exceed);
                } else mos_free(client->buf);
            }
        } else {
            /* not my bussiness */
            binaryDrop(client);
            return false;
        }
    }

    mos_free(client->buf);      /* TODO 是不是有这个必要？ */
    client->recvlen = 0;

    return true;

#undef PARTLY_PACKET
}

void binaryInit()
{
    if (!m_recvbuf) m_recvbuf = mos_calloc(1, CONTRL_PACKET_MAX_LEN);
}

bool binaryRecv(int sfd, NetBinaryNode *client)
{
    int rv;

    if (client->buf == NULL) {
        /* 新发来的包 */
        int recvlen = 0;
        memset(m_recvbuf, 0x0, CONTRL_PACKET_MAX_LEN);
        while (true) {
            rv = recv(sfd, m_recvbuf + recvlen, CONTRL_PACKET_MAX_LEN - recvlen, 0);
            MSG_DUMP_MT(g_dumprecv, "RECV: ", m_recvbuf, rv);

            if (rv == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 包收完了， 或者被唤醒了又没事干 */
                    break;
                } else {
                    mtc_mt_err("%d error occurred %s", sfd, strerror(errno));
                    binaryDrop(client);
                    return false;
                }
            } else if (rv == 0) {
                /* peer performded orderly shutdown */

                /* 如果用户发完包马上 close 掉自己，是处理不到他发包内容的 */
                mtc_mt_dbg("%d closed", sfd);
                binaryDrop(client);
                return false;
            } else recvlen += rv;
        }

        if (recvlen > 0 && !_parse_recv(client, m_recvbuf, recvlen)) {
            /* client dropped in _parse_recv on failure */
            mtc_mt_warn("packet error");
            return false;
        }
    } else {
        /* 已收到部分老包 */
        if (client->recvlen > CONTRL_PACKET_MAX_LEN) {
            mtc_mt_err("unbeleiveable, packet too biiiiig");
            binaryDrop(client);
            return false;
        }

        ssize_t prev_length = client->recvlen;
        while (true) {
            rv = recv(sfd, client->buf + client->recvlen, CONTRL_PACKET_MAX_LEN - client->recvlen, 0);
            MSG_DUMP_MT(g_dumprecv, "CRECV: ", client->buf + client->recvlen, rv);

            if (rv == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 包收完了， 或者被唤醒了又没事干 */
                    break;
                } else {
                    mtc_mt_err("%d error occurred %s", sfd, strerror(errno));
                    binaryDrop(client);
                    return false;
                }
            } else if (rv == 0) {
                /* peer performded orderly shutdown */
                mtc_mt_dbg("%d closed", sfd);
                binaryDrop(client);
                return false;
            } else client->recvlen += rv;
        }

        if (client->recvlen > prev_length && !_parse_recv(client, client->buf, client->recvlen)) {
            /* client dropped in _parse_recv on failure */
            mtc_mt_warn("packet error");
            return false;
        }
    }

    return true;
}

/*
 * 处理异常客户端链接：
 *
 * 1. 已送往业务逻辑的客户端，由业务线程空闲时释放内存。
 * 2. 未送往业务逻辑的客户端，就地正法。
 */
void binaryDrop(NetBinaryNode *client)
{
    if (!client) return;

    mtc_mt_dbg("drop client %p %d", client, client->base.fd);

    client->base.dropped = true;

    epoll_ctl(g_efd, EPOLL_CTL_DEL, client->base.fd, NULL);
    shutdown(client->base.fd, SHUT_RDWR);
    close(client->base.fd);
    client->base.fd = -1;

    if (client->contrl) client->contrl->binary = NULL;

    if (!client->in_business) {
        mos_free(client->buf);
        mos_free(client);
    }
}
