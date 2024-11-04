#include <reef.h>

#include <sys/epoll.h>

#include "global.h"
#include "net.h"
#include "client.h"
#include "packet.h"
#include "bee.h"

static MLIST *m_clients = NULL;
static uint8_t *m_recvbuf = NULL;

static bool _parse_packet(NetClientNode *client, MessagePacket *packet)
{
    BeeEntry *be;
    QueueEntry *qe;

    mtc_mt_dbg("parse packet %d %d", packet->frame_type, packet->command);

    MDF *datanode;
    mdf_init(&datanode);
    if (packet->frame_type > FRAME_RESPONSE && packet->length > LEN_HEADER + 4) {
        if (mdf_mpack_deserialize(datanode, packet->data, packet->length - LEN_HEADER - 4) <= 0) {
            mtc_mt_warn("message pack deserialize failure");
            mdf_destroy(&datanode);
            return false;
        }
    }

    switch (packet->frame_type) {
    case FRAME_AUDIO:
        be = beeFind(FRAME_AUDIO);
        if (!be) {
            mtc_mt_err("lookup backend %d failure", FRAME_AUDIO);
            mdf_destroy(&datanode);
            return false;
        }

        qe = queueEntryCreate(packet->seqnum, packet->command, client, datanode);
        pthread_mutex_lock(&be->op_queue->lock);
        queueEntryPush(be->op_queue, qe);
        pthread_cond_signal(&be->op_queue->cond);
        pthread_mutex_unlock(&be->op_queue->lock);

        break;
    case FRAME_STORAGE:
        be = beeFind(FRAME_STORAGE);
        if (!be) {
            mtc_mt_err("lookup backend %d failure", FRAME_STORAGE);
            mdf_destroy(&datanode);
            return false;
        }

        qe = queueEntryCreate(packet->seqnum, packet->command, client, datanode);
        pthread_mutex_lock(&be->op_queue->lock);
        queueEntryPush(be->op_queue, qe);
        pthread_cond_signal(&be->op_queue->cond);
        pthread_mutex_unlock(&be->op_queue->lock);

        break;
    case FRAME_HARDWARE:
        be = beeFind(FRAME_HARDWARE);
        if (!be) {
            mtc_mt_err("lookup backend %d failure", FRAME_HARDWARE);
            mdf_destroy(&datanode);
            return false;
        }

        qe = queueEntryCreate(packet->seqnum, packet->command, client, datanode);
        if (!qe) {
            mtc_mt_warn("queue entry create failure");
            mdf_destroy(&datanode);
            return false;
        }

        pthread_mutex_lock(&be->op_queue->lock);
        queueEntryPush(be->op_queue, qe);
        pthread_cond_signal(&be->op_queue->cond);
        pthread_mutex_unlock(&be->op_queue->lock);

        break;
    case FRAME_CMD:
        if (packet->command == CMD_STORE_LIST) {
            uint8_t bufsend[LEN_PACKET_NORMAL];

            MDF *dnode;
            mdf_init(&dnode);
            char *libroot = mdf_get_value(g_config, "libraryRoot", "");
            mdf_json_import_filef(dnode, "%sconfig.json", libroot);

            MessagePacket *packetr = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
            size_t sendlen = packetResponseFill(packetr,
                                                packet->seqnum, packet->command, true, NULL, dnode);
            packetCRCFill(packet);
            mdf_destroy(&dnode);

            SSEND(client->base.fd, bufsend, sendlen);
        }
        break;
    default:
        mtc_mt_warn("unsupport frame %d", packet->frame_type);
        mdf_destroy(&datanode);
        return false;
    }

    return true;
}

static bool _parse_recv(NetClientNode *client, uint8_t *recvbuf, size_t recvlen)
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

    uint8_t sendbuf[256] = {0};
    size_t sendlen = 0;
    MessagePacket *outpacket = NULL;
    IdiotPacket *ipacket = packetIdiotGot(recvbuf, recvlen);
    if (ipacket) {
        switch (ipacket->idiot) {
        case IDIOT_PING:
            //mtc_mt_dbg("ping received");
            sendlen = packetPONGFill(sendbuf, sizeof(sendbuf));
            //SSEND(client->base.fd, sendbuf, sendlen);
            send(client->base.fd, sendbuf, sendlen, MSG_NOSIGNAL);
            break;
        case IDIOT_PONG:
            break;
        case IDIOT_CONNECT:
            outpacket = packetMessageInit(sendbuf, sizeof(sendbuf));
            sendlen = packetConnectFill(outpacket, client->id);
            packetCRCFill(outpacket);

            //SSEND((NetNode*)client, sendbuf, sendlen);
            send(client->base.fd, sendbuf, sendlen, MSG_NOSIGNAL);
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
        }
    } else {
        /* command packet ? */
        if (recvlen < LEN_HEADER + 4) PARTLY_PACKET;

        MessagePacket *packet = (MessagePacket*)recvbuf;
        if (packet && packet->sof == PACKET_SOF && packet->idiot == 1) {
            if (recvlen < packet->length) {
                if (packet->length > CONTRL_PACKET_MAX_LEN) {
                    /* 玩不起 */
                    clientDrop(client);
                    return false;
                }

                PARTLY_PACKET;
            } else {
                _parse_packet(client, packet);

                if (recvlen > packet->length) {
                    size_t exceed = recvlen - packet->length;
                    memmove(recvbuf, recvbuf + packet->length, exceed);
                    return _parse_recv(client, recvbuf, exceed);
                }
            }
        } else {
            /* not my bussiness */
            clientDrop(client);
            return false;
        }
    }

    mos_free(client->buf);      /* TODO 是不是有这个必要？ */
    client->recvlen = 0;

    return true;

#undef PARTLY_PACKET
}

void clientInit()
{
    if (!m_recvbuf) m_recvbuf = mos_calloc(1, CONTRL_PACKET_MAX_LEN);
    if (!m_clients) mlist_init(&m_clients, NULL);
}

bool clientRecv(int sfd, NetClientNode *client)
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
                    clientDrop(client);
                    return false;
                }
            } else if (rv == 0) {
                /* peer performded orderly shutdown */

                /* 如果用户发完包马上 close 掉自己，是处理不到他发包内容的 */
                mtc_mt_dbg("%d closed", sfd);
                clientDrop(client);
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
            clientDrop(client);
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
                    clientDrop(client);
                    return false;
                }
            } else if (rv == 0) {
                /* peer performded orderly shutdown */
                mtc_mt_dbg("%d closed", sfd);
                clientDrop(client);
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
void clientDrop(NetClientNode *client)
{
    if (!client) return;

    mtc_mt_dbg("drop client %s %p %d", client->id, client, client->base.fd);

    client->base.dropped = true;

    epoll_ctl(g_efd, EPOLL_CTL_DEL, client->base.fd, NULL);
    shutdown(client->base.fd, SHUT_RDWR);
    close(client->base.fd);
    client->base.fd = -1;

    if (client->binary) client->binary->contrl = NULL;

    NetClientNode *lclient;
    MLIST_ITERATE(m_clients, lclient) {
        if (!strcmp(lclient->id, client->id)) {
            mlist_delete(m_clients, _moon_i);
            break;
        }
    }

    if (mlist_length(client->bees) == 0) {
        mlist_destroy(&client->channels);
        mlist_destroy(&client->bees);
        mos_free(client->buf);
        mos_free(client);
    }
}

void clientAdd(NetClientNode *client)
{
    if (m_clients && client) mlist_append(m_clients, client);
}

NetClientNode* clientMatch(char *clientid)
{
    NetClientNode *client;

    if (!clientid) return NULL;

    MLIST_ITERATE(m_clients, client) {
        if (!strcmp(client->id, clientid)) return client;
    }

    return NULL;
}

bool clientOn()
{
    NetClientNode *client;
    MLIST_ITERATE(m_clients, client) {
        if (!client->base.dropped) return true;
    }

    return false;
}
