typedef enum {
    SYNC_RAWFILE = 0,
    SYNC_TRACK_COVER,
} SYNC_TYPE;

typedef struct {
    BeeEntry base;

    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;

    char *libroot;

    MDF *storeconfig;

    DommeStore *plan;
    char *storename;
    char *storepath;

    MLIST *synclist;
} StorageEntry;

struct reqitem {
    char *name;
    SYNC_TYPE type;
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
    mos_free(item->name);
    mos_free(item);
}

bool _push_raw(StorageEntry *me, struct reqitem *item)
{
    mtc_mt_dbg("push %s to %d", item->name, item->client->base.fd);

    NetBinaryNode *client = item->client;

    if (client->base.fd <= 0 || !me->storepath) return false;

    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%s%s%s", me->libroot, me->storepath, item->name);

    /*
     * CMD_SYNC
     */
    struct stat fs;
    if (stat(filename, &fs) == 0) {
        char nameWithPath[PATH_MAX];
        snprintf(nameWithPath, sizeof(nameWithPath), "%s%s", me->storepath, item->name);

        uint8_t bufsend[LEN_PACKET_NORMAL];
        MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetBFileFill(packet, nameWithPath, fs.st_size);
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

bool _push_cover(StorageEntry *me, struct reqitem *item)
{
    mtc_mt_dbg("push %s to %d", item->name, item->client->base.fd);

    NetBinaryNode *client = item->client;

    if (client->base.fd <= 0 || !me->storepath) return false;

    DommeFile *mfile = dommeGetFile(me->plan, item->name);
    if (mfile) {
        uint8_t *imgbuf;
        size_t coversize;
        char filename[PATH_MAX] = {0};
        snprintf(filename, sizeof(filename), "%s%s%s%s",
                 me->libroot, me->storepath, mfile->dir, mfile->name);

        mp3dec_map_info_t *mapinfo = mp3_cover_open(filename, &imgbuf, &coversize);
        if (!mapinfo) return false;

        /* CMD_SYNC */
        char nameWithPath[PATH_MAX];
        snprintf(nameWithPath, sizeof(nameWithPath), "assets/cover/%s.jpg", item->name);

        uint8_t bufsend[LEN_PACKET_NORMAL];
        MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetBFileFill(packet, nameWithPath, coversize);
        packetCRCFill(packet);

        SSEND(client->base.fd, bufsend, sendlen);

        /* file contents */
        SSEND(client->base.fd, imgbuf, coversize);

        mp3_cover_close(mapinfo);
    } else mtc_mt_warn("%s not exist", item->name);

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

        switch (item->type) {
        case SYNC_RAWFILE:
            _push_raw(me, item);
            break;
        case SYNC_TRACK_COVER:
            _push_cover(me, item);
            break;
        default:
            break;
        }

        reqitem_free(item);
    }

    return NULL;
}

void _push(StorageEntry *me, const char *name, SYNC_TYPE stype, NetBinaryNode *client)
{
    if (!me || !name) return;

    if (!client) {
        mtc_mt_warn("%s client null", name);
        return;
    }

    struct reqitem *item = (struct reqitem*)mos_calloc(1, sizeof(struct reqitem));
    item->name = strdup(name);
    item->client = client;
    item->type = stype;

    pthread_mutex_lock(&me->lock);
    mlist_append(me->synclist, item);
    pthread_cond_signal(&me->cond);
    pthread_mutex_unlock(&me->lock);
}

bool storage_process(BeeEntry *be, QueueEntry *qe)
{
    MERR *err;
    StorageEntry *me = (StorageEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);
    MDF_TRACE_MT(qe->nodein);

    switch (qe->command) {
    /* 为减少网络传输，CMD_DB_MD5为同步前必传，用以指定后续同步文件之媒体库 */
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

        if (me->plan) dommeStoreFree(me->plan);
        me->plan = dommeStoreCreate();
        me->plan->name = strdup(me->storename);
        me->plan->basedir = strdup(me->storepath);
        err = dommeLoadFromFilef(me->plan, "%s%smusic.db", me->libroot, me->storepath);
        if (err) {
            TRACE_NOK_MT(err);
            break;
        }

        char filename[PATH_MAX] = {0};
        snprintf(filename, sizeof(filename), "%s%smusic.db", me->libroot, me->storepath);
        char ownsum[33] = {0};
        ssize_t ownsize = mhash_md5_file_s(filename, ownsum);
        if (ownsize >= 0) {
            int64_t insize = mdf_get_int64_value(qe->nodein, "size", 0);
            char *insum = mdf_get_value(qe->nodein, "checksum", NULL);
            if (insize != ownsize || !insum || strcmp(ownsum, insum)) {
                mtc_mt_dbg("dbsync, push %s music.db", name);

                MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
                size_t sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "文件已更新");
                packetCRCFill(packet);

                SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);

                _push(me, "music.db", SYNC_RAWFILE, qe->client->binary);
            } else {
                /* 文件没更新 */
                MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
                size_t sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);
                packetCRCFill(packet);

                SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);
            }
        } else mtc_mt_warn("%s db not exist", me->storepath);
    }
    break;
    case CMD_SYNC_PULL:
    {
        char *name = mdf_get_value(qe->nodein, "name", NULL);
        SYNC_TYPE type = mdf_get_int_value(qe->nodein, "type", SYNC_RAWFILE);

        if (name) _push(me, name, type, qe->client->binary);
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

    if (me->plan) dommeStoreFree(me->plan);
    mdf_destroy(&me->storeconfig);
    mlist_destroy(&me->synclist);
}

BeeEntry* _start_storage()
{
    MERR *err;
    StorageEntry *me = mos_calloc(1, sizeof(StorageEntry));

    me->base.process = storage_process;
    me->base.stop = storage_stop;

    me->libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!me->libroot) {
        mtc_mt_err("library root path not found");
        return NULL;
    }
    mdf_init(&me->storeconfig);

    err = mdf_json_import_filef(me->storeconfig, "%sconfig.json", me->libroot);
    RETURN_V_NOK(err, NULL);

    me->plan = NULL;
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
