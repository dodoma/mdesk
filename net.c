#include <reef.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "net.h"
#include "global.h"
#include "packet.h"

#define MAXEVENTS 512
#define HEARBEAT_TIMEOUT 10

static bool dad_call_me_back = false;

static void _sig_exit(int sig)
{
    mtc_mt_dbg("dad call me back, exit!");

    dad_call_me_back = true;
}

static bool _broadcast_me(void *data)
{
    NetHornNode *nitem = (NetHornNode*)data;

    mtc_mt_dbg("on broadcast timeout");

    if (g_ctime > nitem->pong && g_ctime - nitem->pong > HEARBEAT_TIMEOUT) {
        /* 长时间没收到客户端心跳了，广播自己 */
        struct sockaddr_in dest;
        int destlen = sizeof(struct sockaddr_in);
        uint8_t sendbuf[256] = {0};
        size_t sendlen = 0;

        dest.sin_family = AF_INET;
        dest.sin_port = htons(mdf_get_int_value(g_config, "server.broadcast_dst", 3102));
        dest.sin_addr.s_addr = INADDR_BROADCAST;

        CommandPacket *packet = packetCommandFill(sendbuf, sizeof(sendbuf));
        sendlen = packetBroadcastFill(packet);
        packetCRCFill(packet);

        MSG_DUMP_MT("SEND: ", sendbuf, sendlen);

        int rv = sendto(nitem->base.fd, sendbuf, sendlen, 0, (struct sockaddr*)&dest, destlen);
        if (rv != sendlen) mtc_mt_err("send failue %d %d", sendlen, rv);
    }

    return true;
}

static void _timer_handler(int fd)
{
    uint64_t value;
    read(fd, &value, sizeof(uint64_t));

    time_t now = time(NULL);
    if (now <= g_ctime) return;

    g_ctime = now;
    g_elapsed = now - g_starton;

    TimerEntry *t = g_timers, *p, *n;
    p = NULL;
    while (t && t->timeout > 0) {
        n = t->next;

        if (g_elapsed % t->timeout == 0 && !t->pause) {
            if (!t->callback(t->data)) {
                if (p) p->next = n;
                if (t == g_timers) g_timers = n;

                mos_free(t);
            } else p = t;
        } else p = t;

        t = n;
    }
}

MERR* netExposeME()
{
    int fd, rv;
    struct epoll_event ev;

    signal(SIGTERM, _sig_exit);
    signal(SIGINT,  _sig_exit);

    g_efd = epoll_create1(0);
    if (g_efd < 0) return merr_raise(MERR_ASSERT, "epoll create failure");

    /* timer fd */
    fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd == -1) return merr_raise(MERR_ASSERT, "timer fd create failure");

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1) return merr_raise(MERR_ASSERT, "get time");
    struct itimerspec new_value;
    new_value.it_value.tv_sec = now.tv_sec + 1;
    new_value.it_value.tv_nsec = now.tv_nsec;
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = 100000000ul;
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL) == -1)
        return merr_raise(MERR_ASSERT, "timer set time");

    NetNode *nitem = mos_calloc(1, sizeof(NetNode));
    nitem->fd = fd;
    nitem->type = NET_TIMER;
    ev.events = EPOLLIN;
    ev.data.ptr = nitem;
    rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nitem->fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    /* fd_horn */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create horn socket failure");

    int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct sockaddr_in srvsa;
    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.broadcast_src", 3101));
    srvsa.sin_addr.s_addr = INADDR_ANY;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind broadcast failure");

    NetHornNode *nodehorn = mos_calloc(1, sizeof(NetHornNode));
    nodehorn->base.fd = fd;
    nodehorn->base.type = NET_HORN;
    nodehorn->pong = g_ctime;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nodehorn;
    rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nodehorn->base.fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    g_timers = timerAdd(g_timers, 3, nodehorn, _broadcast_me);

    /* fd */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create socket failure");

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.port", 3001));
    srvsa.sin_addr.s_addr = INADDR_ANY;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind failure");

    rv = listen(fd, 1024);
    if (rv < 0) return merr_raise(MERR_ASSERT, "listen failure");

    nitem = mos_calloc(1, sizeof(NetNode));
    nitem->fd = fd;
    nitem->type = NET_STREAM;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nitem;
    rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nitem->fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    /* fd_ssl */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create ssl socket failure");

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.port_ssl", 3002));
    srvsa.sin_addr.s_addr = INADDR_ANY;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind ssl failure");

    rv = listen(fd, 1024);
    if (rv < 0) return merr_raise(MERR_ASSERT, "listen ssl failure");

    nitem = mos_calloc(1, sizeof(NetNode));
    nitem->fd = fd;
    nitem->type = NET_STREAM_SSL;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nitem;
    rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nitem->fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    /* epoll */
    struct epoll_event *events = mos_calloc(MAXEVENTS, sizeof(struct epoll_event));

    dad_call_me_back = false;
    while (!dad_call_me_back) {
        int nfd = epoll_wait(g_efd, events, MAXEVENTS, 2000);
        if (nfd == -1 && errno != EINTR) {
            mtc_mt_err("epoll wait error %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nfd; i++) {
            nitem = events[i].data.ptr;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (nitem->type == NET_CLIENT || nitem->type == NET_CLIENT_SSL) {
                    mtc_mt_warn("client error %d", nitem->fd);

                    netNodeFree(nitem);
                    continue;
                } else {
                    mtc_mt_err("system socket error! %d", nitem->fd);

                    netNodeFree(nitem);

                    return merr_raise(MERR_ASSERT, "system socket error %d", nitem->fd);
                }
            }

            switch (nitem->type) {
            case NET_TIMER:
                _timer_handler(nitem->fd);
                break;
            default:
                break;
            }
        }
    }

    /* TODO nitem memory leak */

    mos_free(events);
    close(g_efd);

    return MERR_OK;
}

void netNodeFree(NetNode *node)
{
    if (!node) return;

    switch (node->type) {
    default:
        break;
    }

    epoll_ctl(g_efd, EPOLL_CTL_DEL, node->fd, NULL);
    close(node->fd);

    mos_free(node);
}
