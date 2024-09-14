#include <reef.h>

#include "global.h"
#include "packet.h"
#include "net.h"
#include "bee.h"

#include "_bee_hardware.c"

MLIST *g_bees = NULL;

static void _user_destroy(void *p)
{
    if (!p) return;

    NetClientNode *client = (NetClientNode*)p;

    mtc_mt_dbg("free client %p", client);
    mos_free(client->buf);
    mos_free(client);
}

static void _bee_stop(void *p)
{
    if (!p) return;

    BeeEntry *be = p;
    be->running = false;
    be->stop(be);

    pthread_join(*(be->op_thread), NULL);
    mos_free(be->op_thread);
    queueFree(be->op_queue);

    mlist_destroy(&be->users);

    mos_free(be);
}

static void* _worker(void *arg)
{
    uint8_t idle_count = 0;
    int rv;

    BeeEntry *be = (BeeEntry*)arg;
    QueueManager *queue = be->op_queue;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf(be->name, loglevel, g_log_tostdout ? "-"  :"%s/log/%s.log", g_location, be->name);

    mtc_mt_dbg("I am your business %s worker No.%d", be->name, be->id);

    while (be->running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;

        pthread_mutex_lock(&queue->lock);

        while ((rv = pthread_cond_timedwait(&queue->cond, &queue->lock, &timeout)) == ETIMEDOUT) {
            //mtc_mt_dbg("condwait timeout");
            timeout.tv_sec += 1;

            if (++idle_count >= 3) {
                mtc_mt_noise("check my users in freetime");

                idle_count = 0;

                NetClientNode *client;
                MLIST_ITERATE(be->users, client) {
                    if (client->dropped) {
                        mlist_delete(be->users, _moon_i);
                        _moon_i--;
                    }
                }
            }

            if (!be->running) break;
        }

        if (!be->running) {
            pthread_mutex_unlock(&queue->lock);
            break;
        }

        if (rv != 0) {
            mtc_mt_err("timedwait error %s", strerror(errno));
            pthread_mutex_unlock(&queue->lock);
            continue;
        }

        /* rv == 0 */
        QueueEntry *qentry = queueEntryGet(queue);
        pthread_mutex_unlock(&queue->lock);

        if (!qentry) {
            mtc_mt_warn("timedwait success, but got nothing!");
            continue;
        }

        idle_count = 0;

        if (!qentry->client->in_business) {
            qentry->client->in_business = true;
            mlist_append(be->users, qentry->client);
        }

        be->process(be, qentry);

        queueEntryFree(qentry);
    }

    return NULL;
}

static BeeEntry* _start_driver(BeeDriver *driver)
{
    BeeEntry *be = driver->init_driver();
    if (!be) return NULL;

    be->id = driver->id;
    be->name = driver->name;
    be->running = true;

    mlist_init(&be->users, _user_destroy);
    be->op_queue = queueCreate();
    be->op_thread = mos_calloc(1, sizeof(pthread_t));
    pthread_create(be->op_thread, NULL, _worker, (void*)be);

    return be;
}

static int _bee_compare(const void *a, const void *b)
{
    BeeEntry *pa, *pb;

    pa = *(BeeEntry**)a;
    pb = *(BeeEntry**)b;

    return pa->id - pb->id;
}

MERR* beeStart()
{
    if (!g_bees) mlist_init(&g_bees, _bee_stop);

    BeeEntry *be = _start_driver(&hardware_driver);
    if (!be) return merr_raise(MERR_ASSERT, "硬件设置启动失败");

    mlist_append(g_bees, be);

    mlist_sort(g_bees, _bee_compare);

    return MERR_OK;
}

void beeStop()
{
    if (g_bees) mlist_destroy(&g_bees);
}

BeeEntry* beeFind(uint8_t id)
{
    if (!g_bees) return NULL;

    BeeEntry dummy = {.id = id}, *key = &dummy;

    BeeEntry **be = (BeeEntry**)mlist_search(g_bees, &key, _bee_compare);
    if (be) return *be;
    else return NULL;
}

QueueManager* queueCreate()
{
    QueueManager *queue = mos_calloc(1, sizeof(QueueManager));
    queue->size = 0;
    queue->top = NULL;
    queue->bottom = NULL;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&(queue->lock), &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&(queue->cond), NULL);

    return queue;
}

void queueFree(QueueManager *queue)
{
    QueueEntry *entry = queueEntryGet(queue);
    while (entry) {
        queueEntryFree(entry);

        entry = queueEntryGet(queue);
    }

    pthread_mutex_destroy(&queue->lock);

    mos_free(queue);
}

QueueEntry* queueEntryCreate(uint16_t seqnum, uint16_t command, NetClientNode *client, MDF *datanode)
{
    if (!client) return NULL;

    QueueEntry *entry = mos_calloc(1, sizeof(QueueEntry));
    entry->seqnum = seqnum;
    entry->command = command;
    entry->client = client;
    entry->nodein = datanode;

    entry->next = NULL;

    return entry;
}

void queueEntryFree(void *p)
{
    if (!p) return;

    QueueEntry *entry = (QueueEntry*)p;

    mdf_destroy(&entry->nodein);
    mos_free(entry);
}

/*
 * 因为QueueEntry是单向链表，对于O(1)操作只能要么取top、要么取bottom，此处用Get笼统表示
 * 配合先入先出原则，此处取 bottom
 */
QueueEntry* queueEntryGet(QueueManager *queue)
{
    if (!queue || !queue->bottom || queue->size < 1) return NULL;

    QueueEntry *entry = queue->bottom;

    queue->bottom = entry->next;

    if (queue->size == 1) {
        queue->top = NULL;      /* redundancy */
        queue->bottom = NULL;
    }

    queue->size -= 1;

    return entry;
}

void queueEntryPush(QueueManager *queue, QueueEntry *entry)
{
    if (!queue || !entry) return;

    //entry->next = queue->top; /* 方便后入先出 */
    if (queue->top) queue->top->next = entry; /* 方便先入先出 */

    queue->top = entry;

    if (queue->size == 0) queue->bottom = entry;

    queue->size += 1;

    return;
}
