#include <reef.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "net.h"
#include "client.h"
#include "binary.h"
#include "global.h"
#include "packet.h"

#define MAXEVENTS 512
#define BROADCAST_PERIOD 1

static pthread_t m_timer;
static bool dad_call_me_back = false;

static void _sig_exit(int sig)
{
    mtc_mt_dbg("dad call me back, exit!");

    dad_call_me_back = true;
}

static bool _fill_serversa(const char *host, int port, struct sockaddr_in *srvsa)
{
    struct in_addr ia;
    int rv = inet_pton(AF_INET, host, &ia);
    if (rv <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            mtc_mt_err("get host by name %s failure", host);
            return false;
        }
        ia.s_addr = *( (in_addr_t *) (he->h_addr_list[0]) );
    }

    srvsa->sin_family = AF_INET;
    srvsa->sin_port = htons(port);
    srvsa->sin_addr.s_addr = ia.s_addr;

    return true;
}

static bool _try_connect(struct sockaddr_in *srvsa)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) return false;

    int rv = connect(fd, (struct sockaddr*)srvsa, sizeof(struct sockaddr_in));
    if (rv < 0) {
        if (errno == EINPROGRESS) {
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(fd, &fdset);

            struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
            if (select(fd + 1, NULL, &fdset, NULL, &tv) > 0) {
                int valopt;
                socklen_t slen = sizeof(int);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&valopt, &slen);
                if (valopt != 0) {
                    mtc_mt_warn("connected failure");
                    return false;
                }
            } else {
                mtc_mt_warn("connect timeout");
                return false;
            }
        } else {
            mtc_mt_warn("connect failure");
            return false;
        }
    }

    close(fd);

    return true;
}

static bool _broadcast_me(void *data)
{
    NetHornNode *nitem = (NetHornNode*)data;

    if (!clientOn()) {
        mtc_mt_noise("broadcast me");

        static bool online = false;
        static struct sockaddr_in srvsa = {.sin_family = AF_INET, .sin_port = 0};

        char *host = mdf_get_value(g_config, "mocserver.host", "mbox.net.cn");
        int port = mdf_get_int_value(g_config, "mocserver.port", 4001);

        if (srvsa.sin_port == 0) _fill_serversa(host, port, &srvsa);

        if (!online && _try_connect(&srvsa)) online = true;

        char cpuid[14] = {0};
        memcpy(cpuid, g_cpuid, sizeof(cpuid));
        if (!online) {
            /* 连不上网的音源发 'b' 类广播包 */
            cpuid[0] = 'b';
        }

        /* 长时间没收到客户端心跳了，广播自己 */
        struct sockaddr_in dest;
        int destlen = sizeof(struct sockaddr_in);
        uint8_t sendbuf[256] = {0};
        size_t sendlen = 0;

        dest.sin_family = AF_INET;
        dest.sin_port = htons(mdf_get_int_value(g_config, "server.broadcast_dst", 4102));
        dest.sin_addr.s_addr = INADDR_BROADCAST;

        MessagePacket *packet = packetMessageInit(sendbuf, sizeof(sendbuf));
        sendlen = packetBroadcastFill(packet, cpuid,
                                      mdf_get_int_value(g_config, "server.port_contrl", 4001),
                                      mdf_get_int_value(g_config, "server.port_binary", 4002));
        packetCRCFill(packet);

        MSG_DUMP_MT(g_dumpsend, "SEND: ", sendbuf, sendlen);

        int rv = sendto(nitem->base.fd, sendbuf, sendlen, 0, (struct sockaddr*)&dest, destlen);
        if (rv != sendlen) mtc_mt_err("send failue %d %d %s", nitem->base.fd, rv, strerror(errno));
    }

    return true;
}

static void _timer_handler(int fd)
{
    uint64_t value;
    read(fd, &value, sizeof(uint64_t));

    time_t now = time(NULL);
    if (now <= g_ctime) return;

    //mtc_mt_foo("tick tock");

    g_ctime = now;
    g_elapsed = now - g_starton;

    TimerEntry *t = g_timers, *p, *n;
    p = NULL;
    while (t && t->timeout > 0) {
        n = t->next;

        if (t->right_now || (g_elapsed % t->timeout == 0 && !t->pause)) {
            t->right_now = false;
            if (!t->callback(t->data)) {
                if (p) p->next = n;
                if (t == g_timers) g_timers = n;

                mos_free(t);
            } else p = t;
        } else p = t;

        t = n;
    }
}

static void* el_timer(void *arg)
{
    int timerfd = *(int*)arg;

    fd_set readset;
    int maxfd = 0, rv;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.main", "debug"));
    mtc_mt_initf("timer", loglevel, g_log_tostdout ? "-" : "%s/log/timer.log", g_location);

    mtc_mt_dbg("I am timer routine with timerfd %d", timerfd);

    while (!dad_call_me_back) {
        FD_ZERO(&readset);

        if (timerfd > 0) {
            FD_SET(timerfd, &readset);
            maxfd = timerfd;
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        rv = select(maxfd + 1, &readset, NULL, NULL, &tv);

        if (rv == -1 && errno != EINTR) {
            mtc_mt_err("select error %s", strerror(errno));
            break;
        } else if (rv == 0) continue;

        if (FD_ISSET(timerfd, &readset)) _timer_handler(timerfd);
    }

    mtc_mt_dbg("timer done");

    return NULL;
}

static int _new_connection(int efd, int sfd)
{
    struct sockaddr_in clisa;
    socklen_t clilen = sizeof(struct sockaddr_in);

    NetClientNode *nitem = mos_calloc(1, sizeof(NetClientNode));
    nitem->base.type = NET_CLIENT_CONTRL;
    nitem->base.fd = accept(sfd, (struct sockaddr*)&clisa, &clilen);
    if(nitem->base.fd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            mos_free(nitem);
            return 0;
        } else return -1;
    }

    if (fcntl(nitem->base.fd, F_SETFL, fcntl(nitem->base.fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        close(nitem->base.fd);
        mos_free(nitem);
        return 0;
    }

    mtc_mt_dbg("new contrl connection on %d ==> %d", sfd, nitem->base.fd);

    memset(nitem->id, 0x0, LEN_CLIENTID);
    mstr_rand_word_fixlen(nitem->id, LEN_CLIENTID-1);
    nitem->binary = NULL;

    mlist_init(&nitem->bees, NULL);
    mlist_init(&nitem->channels, NULL);
    nitem->buf = NULL;
    nitem->recvlen = 0;
    pthread_mutex_init(&nitem->lock, NULL);
    nitem->base.dropped = false;

    struct epoll_event ev = {.data.ptr = nitem, .events = EPOLLIN | EPOLLET};
    if (epoll_ctl(efd, EPOLL_CTL_ADD, nitem->base.fd, &ev) == -1)
        mtc_mt_err("epoll add failure %s", strerror(errno));

    clientAdd(nitem);

    return 1;
}

static int _new_connection_binary(int efd, int sfd)
{
    struct sockaddr_in clisa;
    socklen_t clilen = sizeof(struct sockaddr_in);

    NetBinaryNode *nitem = mos_calloc(1, sizeof(NetBinaryNode));
    nitem->base.type = NET_CLIENT_BINARY;
    nitem->base.fd = accept(sfd, (struct sockaddr*)&clisa, &clilen);
    if(nitem->base.fd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            mos_free(nitem);
            return 0;
        } else return -1;
    }

    if (fcntl(nitem->base.fd, F_SETFL, fcntl(nitem->base.fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        close(nitem->base.fd);
        mos_free(nitem);
        return 0;
    }

    mtc_mt_dbg("new binary connection on %d ==> %d", sfd, nitem->base.fd);

    nitem->contrl = NULL;
    nitem->buf = NULL;
    nitem->recvlen = 0;
    nitem->in_business = false;
    nitem->base.dropped = false;

    struct epoll_event ev = {.data.ptr = nitem, .events = EPOLLIN | EPOLLET};
    if (epoll_ctl(efd, EPOLL_CTL_ADD, nitem->base.fd, &ev) == -1)
        mtc_mt_err("epoll add failure %s", strerror(errno));

    return 1;
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
    static int timerfd;
    timerfd = timerfd_create(CLOCK_REALTIME, 0);
    if (timerfd == -1) return merr_raise(MERR_ASSERT, "timer fd create failure");

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1) return merr_raise(MERR_ASSERT, "get time");
    struct itimerspec new_value;
    new_value.it_value.tv_sec = now.tv_sec + 1;
    new_value.it_value.tv_nsec = now.tv_nsec;
    new_value.it_interval.tv_sec = 1;
    //new_value.it_interval.tv_nsec = 100000000ul;
    new_value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &new_value, NULL) == -1)
        return merr_raise(MERR_ASSERT, "timer set time");

    pthread_create(&m_timer, NULL, el_timer, &timerfd);

    /* 优选有线网络 */
    int tmpfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tmpfd < 0) return merr_raise(MERR_ASSERT, "create detect socket failure");

    struct ifreq ifr;
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ-1);
    ifr.ifr_addr.sa_family = AF_INET;
    rv = ioctl(tmpfd, SIOCGIFADDR, &ifr);
    if (rv != 0) {
        /* 尝试无线网络 */
        strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
        ifr.ifr_addr.sa_family = AF_INET;
        rv = ioctl(tmpfd, SIOCGIFADDR, &ifr);
        if (rv != 0) {
            close(tmpfd);
            return merr_raise(MERR_ASSERT, "网卡初始化错误");
        }
    }

    mtc_mt_dbg("bind to %s", ifr.ifr_name);
    close(tmpfd);

    /* fd_horn */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create horn socket failure");

    int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct sockaddr_in srvsa;
    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.broadcast_src", 4101));
    //srvsa.sin_addr.s_addr = INADDR_ANY;
    srvsa.sin_addr.s_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind broadcast failure");

    NetHornNode *nodehorn = mos_calloc(1, sizeof(NetHornNode));
    nodehorn->base.fd = fd;
    nodehorn->base.type = NET_HORN;
    //ev.events = EPOLLIN | EPOLLET;
    //ev.data.ptr = nodehorn;
    //rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nodehorn->base.fd, &ev);
    //if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    g_timers = timerAdd(g_timers, BROADCAST_PERIOD, true, nodehorn, _broadcast_me);

    /* fd contrl */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create contrl socket failure");

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.port_contrl", 4001));
    //srvsa.sin_addr.s_addr = INADDR_ANY;
    srvsa.sin_addr.s_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind contrl failure");

    rv = listen(fd, 1024);
    if (rv < 0) return merr_raise(MERR_ASSERT, "listen contrl failure");

    NetNode *nitem = mos_calloc(1, sizeof(NetNode));
    nitem->fd = fd;
    nitem->type = NET_CONTRL;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nitem;
    rv = epoll_ctl(g_efd, EPOLL_CTL_ADD, nitem->fd, &ev);
    if(rv == -1) return merr_raise(MERR_ASSERT, "add fd failure");

    /* fd binary */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return merr_raise(MERR_ASSERT, "create binary socket failure");

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    srvsa.sin_family = AF_INET;
    srvsa.sin_port = htons(mdf_get_int_value(g_config, "server.port_binary", 4002));
    //srvsa.sin_addr.s_addr = INADDR_ANY;
    srvsa.sin_addr.s_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    rv = bind(fd, (const struct sockaddr*)&srvsa, sizeof(srvsa));
    if (rv != 0) return merr_raise(MERR_ASSERT, "bind binary failure");

    rv = listen(fd, 1024);
    if (rv < 0) return merr_raise(MERR_ASSERT, "listen binary failure");

    nitem = mos_calloc(1, sizeof(NetNode));
    nitem->fd = fd;
    nitem->type = NET_BINARY;
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
                if (nitem->type == NET_CLIENT_CONTRL || nitem->type == NET_CLIENT_BINARY) {
                    mtc_mt_warn("client error %d", nitem->fd);

                    netNodeFree(nitem);
                    continue;
                } else {
                    mtc_mt_err("system socket error! %d", nitem->fd);

                    netNodeFree(nitem);

                    /* TODO memory leak */
                    return merr_raise(MERR_ASSERT, "system socket error %d", nitem->fd);
                }
            }

            switch (nitem->type) {
            case NET_CONTRL:
                while (true) {
                    rv = _new_connection(g_efd, nitem->fd);
                    if (rv < 0) {
                        return merr_raise(MERR_ASSERT, "new connection error %s", strerror(errno));
                    } else if (rv == 0) break;
                }
                break;
            case NET_BINARY:
                while (true) {
                    rv = _new_connection_binary(g_efd, nitem->fd);
                    if (rv < 0) {
                        return merr_raise(MERR_ASSERT, "new connection error %s", strerror(errno));
                    } else if (rv == 0) break;
                }
                break;
            case NET_HORN:
                //mtc_mt_dbg("receive broadcast response");
                //((NetHornNode*)nitem)->ping = g_ctime;
                break;
            case NET_CLIENT_CONTRL:
                clientRecv(nitem->fd, (NetClientNode*)nitem);
                break;
            case NET_CLIENT_BINARY:
                binaryRecv(nitem->fd, (NetBinaryNode*)nitem);
                break;
            default:
                break;
            }
        }
    }

    /* TODO nitem memory leak */

    mos_free(events);
    close(g_efd);
    pthread_cancel(m_timer);
    pthread_join(m_timer, NULL);

    return MERR_OK;
}

void netNodeFree(NetNode *node)
{
    if (!node) return;

    switch (node->type) {
    case NET_CLIENT_CONTRL:
        return clientDrop((NetClientNode*)node);
    case NET_CLIENT_BINARY:
        return binaryDrop((NetBinaryNode*)node);
    default:
        break;
    }

    epoll_ctl(g_efd, EPOLL_CTL_DEL, node->fd, NULL);
    close(node->fd);

    mos_free(node);
}

bool SSEND(int fd, uint8_t *buf, size_t len)
{
    ssize_t count = 0;
    int rv;

    if (fd <= 0 || !buf || len <= 0) return false;

    while (count < len) {
        rv = send(fd, buf + count, len - count, MSG_NOSIGNAL);
        MSG_DUMP_MT(g_dumpsend, "SEND: ", buf + count, rv);

        if (rv == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            } else {
                mtc_mt_err("send failure %s", strerror(errno));
                return false;
            }
        } else if (rv == 0) {
            return false;
        } else {
            count += rv;
        }
    }

    return true;
}
