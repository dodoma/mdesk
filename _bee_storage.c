typedef struct {
    BeeEntry base;

    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;

    char *basedir;

    MDF *storeconfig;
    char *storename;
    char *storepath;

    MLIST *synclist;
} StorageEntry;

struct reqitem {
    char *filename;             /* except basedir */
    NetBinaryNode *client;
};

MDF* _store_node(StorageEntry *me, char *name)
{
    if (!me->storeconfig || !name) return NULL;

    MDF *cnode = mdf_node_child(me->storeconfig);
    while (cnode) {
        char *lname = mdf_get_value(cnode, "name", NULL);

        if (lname && !strcmp(lname, name)) return cnode;

        cnode = mdf_node_next(cnode);
    }

    return NULL;
}

void reqitem_free(void *arg)
{
    if (!arg) return;

    struct reqitem *item = (struct reqitem*)arg;
    mos_free(item->filename);
    mos_free(item);
}

bool _do_push(StorageEntry *me, struct reqitem *item)
{
    mtc_mt_dbg("push %s to %d", item->filename, item->client->base.fd);

    NetBinaryNode *client = item->client;

    if (client->base.fd <= 0) return false;

    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%s%s", me->basedir, item->filename);

    /*
     * CMD_SYNC
     */
    struct stat fs;
    if (stat(filename, &fs) == 0) {
        uint8_t bufsend[LEN_PACKET_NORMAL];
        MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetBFileFill(packet, item->filename, fs.st_size);
        packetCRCFill(packet);

        SSEND(client->base.fd, bufsend, sendlen);
    }

    /*
     * file contents
     */
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        mtc_mt_warn("open %s failure %s", filename, strerror(errno));
        return false;
    }

    uint8_t buf[4096] = {0};
    size_t len = 0;
    while ((len = fread(buf, 1, sizeof(buf), fp)) > 0) {
        SSEND(client->base.fd, buf, len);
    }

    fclose(fp);

    return true;
}

void* _pusher(void *arg)
{
    StorageEntry *me = (StorageEntry*)arg;
    int rv = 0;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf("pusher", loglevel, g_log_tostdout ? "-"  :"%slog/%s.log", g_location, "pusher");

    mtc_mt_dbg("I am binary pusher");

    while (me->running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;

        pthread_mutex_lock(&me->lock);

        while (mlist_length(me->synclist) == 0 &&
               (rv = pthread_cond_timedwait(&me->cond, &me->lock, &timeout)) == ETIMEDOUT) {
            timeout.tv_sec += 1;

            if (!me->running) break;
        }

        if (!me->running) {
            pthread_mutex_unlock(&me->lock);
            break;
        }

        if (mlist_length(me->synclist) == 0 && rv != 0) {
            mtc_mt_err("timedwait error %s", strerror(errno));
            pthread_mutex_unlock(&me->lock);
            continue;
        }

        struct reqitem *item = mlist_popx(me->synclist);
        pthread_mutex_unlock(&me->lock);

        if (!item) continue;

        _do_push(me, item);

        reqitem_free(item);
    }

    return NULL;
}

void _push(StorageEntry *me, const char *filename, NetBinaryNode *client)
{
    if (!me || !filename) return;

    if (!client) {
        mtc_mt_warn("%s client null", filename);
        return;
    }

    struct reqitem *item = (struct reqitem*)mos_calloc(1, sizeof(struct reqitem));
    item->filename = strdup(filename);
    item->client = client;

    pthread_mutex_lock(&me->lock);
    mlist_append(me->synclist, item);
    pthread_cond_signal(&me->cond);
    pthread_mutex_unlock(&me->lock);
}

bool storage_process(BeeEntry *be, QueueEntry *qe)
{
    StorageEntry *me = (StorageEntry*)be;
    char filename[PATH_MAX] = {0};

    mtc_mt_dbg("process command %d", qe->command);

    switch (qe->command) {
    case CMD_DB_MD5:
    {
        char *name = mdf_get_value(qe->nodein, "name", NULL);
        MDF *snode = _store_node(me, name);
        me->storename = mdf_get_value(snode, "name", NULL);
        me->storepath = mdf_get_value(snode, "path", NULL);
        if (!me->storename || !me->storepath) {
            mtc_mt_warn("can't find library %s", name);
            break;
        }

        char *checksum = mdf_get_value(qe->nodein, "checksum", NULL);
        if (!checksum) {
            mtc_mt_dbg("dbsync, push %s music.db", name);

            snprintf(filename, sizeof(filename), "%smusic.db", me->storepath);
            _push(me, filename, qe->client->binary);
        }
    }
    break;
    default:
        break;
    }

    return true;
}

void storage_stop(BeeEntry *be)
{
    StorageEntry *me = (StorageEntry*)be;

    mtc_mt_dbg("stop worker %s", be->name);

    me->running = false;
    pthread_cancel(me->worker);
    pthread_join(me->worker, NULL);

    mdf_destroy(&me->storeconfig);
    mlist_destroy(&me->synclist);
}

BeeEntry* _start_storage()
{
    MERR *err;
    StorageEntry *me = mos_calloc(1, sizeof(StorageEntry));

    me->base.process = storage_process;
    me->base.stop = storage_stop;

    me->basedir = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!me->basedir) {
        mtc_mt_err("library root path not found");
        return NULL;
    }
    mdf_init(&me->storeconfig);

    err = mdf_json_import_filef(me->storeconfig, "%sconfig.json", me->basedir);
    RETURN_V_NOK(err, NULL);

    me->storename = NULL;
    me->storepath = NULL;
    mlist_init(&me->synclist, reqitem_free);

    me->running = true;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&me->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&me->cond, NULL);

    pthread_create(&me->worker, NULL, _pusher, me);

    return (BeeEntry*)me;
}

BeeDriver storage_driver = {
    .id = FRAME_STORAGE,
    .name = "storage",
    .init_driver = _start_storage
};
