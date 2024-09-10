#include <reef.h>

#include <sys/epoll.h>

#include "global.h"
#include "net.h"
#include "client.h"
#include "packet.h"

static uint8_t *m_recvbuf = NULL;

static bool _parse_recv(NetClientNode *client, uint8_t *rcvbuf, size_t rcvlen)
{
    uint8_t sendbuf[256] = {0};
    size_t sendlen = packetPONGFill(sendbuf, sizeof(sendbuf));

    if (rcvlen < LEN_IDIOT) {
        client->complete = false;
        return true;
    }

    IdiotPacket *ipacket = packetIdiotGot(rcvbuf, rcvlen);
    if (ipacket) {
        switch (ipacket->idiot) {
        case IDIOT_PING:
            mtc_mt_dbg("ping received");
            netHornPing();
            send(client->base.fd, sendbuf, sendlen, MSG_NOSIGNAL);
            break;
        case IDIOT_PONG:
            break;
        }
    } else {
        CommandPacket *packet = packetCommandGot(rcvbuf, rcvlen);
        if (packet) {
            ;
        } else {
            ;
            /* ERROR */
        }
    }

    return true;
}

void clientInit()
{
    if (!m_recvbuf) m_recvbuf = mos_calloc(1, CONTRL_PACKET_MAX_LEN);
}

void clientRecv(int sfd, NetClientNode *client)
{
    int rv;

    if (client->buf == NULL) {
        memset(m_recvbuf, 0x0, CONTRL_PACKET_MAX_LEN);

        while (true) {
            rv = recv(sfd, m_recvbuf, CONTRL_PACKET_MAX_LEN, 0);

            MSG_DUMP_MT("RECV: ", m_recvbuf, rv);

            if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else if (rv <= 0) {
                clientDrop(client);
                return;
            }

            if (!_parse_recv(client, m_recvbuf, rv)) {
                /* TODO drop client */
                mtc_mt_warn("packet error, return");
                return;
            }
        }
    } else {
        ;
    }
}

void clientDrop(NetClientNode *client)
{
    if (!client) return;

    mtc_mt_dbg("drop client %d", client->base.fd);

    client->dropped = true;

    epoll_ctl(g_efd, EPOLL_CTL_DEL, client->base.fd, NULL);
    shutdown(client->base.fd, SHUT_RDWR);
    close(client->base.fd);
    client->base.fd = -1;

    /* TODO free */
}
