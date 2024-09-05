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
    NetNode *node = (NetNode*)data;

    mtc_mt_dbg("on broadcast timeout");

    if (g_ctime > node->pong && g_ctime - node->pong > HEARBEAT_TIMEOUT) {
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

        int rv = sendto(node->fd_horn, sendbuf, sendlen, 0, (struct sockaddr*)&dest, destlen);
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
    signal(SIGTERM, _sig_exit);
    signal(SIGINT,  _sig_exit);

    NetNode *node = mos_calloc(1, sizeof(NetNode));
    node->pong = g_ctime;

    /* timer fd */
    int fd = timerfd_create(CLOCK_REALTIME, 0);
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

    node->fd_timer = fd;

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
    int rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind broadcast failure");

    node->fd_horn = fd;

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

    node->fd = fd;

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

    node->fd_ssl = fd;

    /* timer */
    g_timers = timerAdd(g_timers, 3, node, _broadcast_me);

    /* epoll */
    int efd = epoll_create1(0);
    if (efd < 0) return merr_raise(MERR_ASSERT, "epoll create failure");

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;

    ev.data.fd = node->fd;
    rv = epoll_ctl(efd, EPOLL_CTL_ADD, node->fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    ev.data.fd = node->fd_ssl;
    rv = epoll_ctl(efd, EPOLL_CTL_ADD, node->fd_ssl, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd ssl failure");

    ev.data.fd = node->fd_horn;
    rv = epoll_ctl(efd, EPOLL_CTL_ADD, node->fd_horn, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd horn failure");

    ev.events = EPOLLIN;
    ev.data.fd = node->fd_timer;
    rv = epoll_ctl(efd, EPOLL_CTL_ADD, node->fd_timer, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd timer failure");

    struct epoll_event *events = mos_calloc(MAXEVENTS, sizeof(struct epoll_event));

    dad_call_me_back = false;
    while (!dad_call_me_back) {
        int nfd = epoll_wait(efd, events, MAXEVENTS, 2000);
        if (nfd == -1 && errno != EINTR) {
            mtc_mt_err("epoll wait error %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nfd; i++) {
            fd = events[i].data.fd;
            if (fd == node->fd) {
                ;
            } else if (fd == node->fd_ssl) {
                ;
            } else if (fd == node->fd_horn) {
                ;
            } else if (fd == node->fd_timer) {
                _timer_handler(node->fd_timer);
            } else {
                ;
            }
        }
    }

    epoll_ctl(efd, EPOLL_CTL_DEL, node->fd, NULL);
    epoll_ctl(efd, EPOLL_CTL_DEL, node->fd_ssl, NULL);
    epoll_ctl(efd, EPOLL_CTL_DEL, node->fd_horn, NULL);
    epoll_ctl(efd, EPOLL_CTL_DEL, node->fd_timer, NULL);

    shutdown(node->fd, SHUT_RDWR);
    shutdown(node->fd_ssl, SHUT_RDWR);
    shutdown(node->fd_horn, SHUT_RDWR);
    shutdown(node->fd_timer, SHUT_RDWR);

    close(node->fd);
    close(node->fd_ssl);
    close(node->fd_horn);
    close(node->fd_timer);

    mos_free(events);
    close(efd);

    return MERR_OK;
}
