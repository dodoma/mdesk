typedef struct {
    BeeEntry base;

    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;

    char *libroot;

    DommeStore *plan;
    char *storename;
    char *storepath;

    MLIST *synclist;            /* list of struct reqitem* */
    MLIST *clients;             /* list of NetBinaryNode* */
} StorageEntry;

struct reqitem {
    SYNC_TYPE type;
    char *name;
    char *id;
    char *artist;
    char *album;

    NetBinaryNode *client;
};

static void _client_destroy(void *p)
{
    if (!p) return;

    NetBinaryNode *client = (NetBinaryNode*)p;

    mtc_mt_dbg("user %p left", client);

    mos_free(client->buf);
    mos_free(client);
}

/* 为避免同步通知，每次从磁盘上读取媒体库配置 */
bool _store_node(StorageEntry *me, char *name, char **storename, char **storepath)
{
    if (!me || !me->libroot || !name || !storename || !storepath) return false;

    MDF *storeconfig;
    mdf_init(&storeconfig);

    MERR *err = mdf_json_import_filef(storeconfig, "%sconfig.json", me->libroot);
    RETURN_V_NOK(err, false);

    MDF *cnode = mdf_node_child(storeconfig);
    while (cnode) {
        char *lname = mdf_get_value(cnode, "name", NULL);
        char *path = mdf_get_value(cnode, "path", NULL);

        if (lname && !strcmp(lname, name) && path) {
            *storename = strdup(name);
            *storepath = strdup(path);

            mdf_destroy(&storeconfig);
            return true;
        }

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&storeconfig);
    return false;
}

void reqitem_free(void *arg)
{
    if (!arg) return;

    struct reqitem *item = (struct reqitem*)arg;
    mos_free(item->name);
    mos_free(item->id);
    mos_free(item->artist);
    mos_free(item->album);
    mos_free(item);
}

bool _push_puppet(NetBinaryNode *client, const char *filename, const char *pupname)
{
    if (client->base.fd <= 0 || !filename || !pupname) return false;

    mtc_mt_dbg("push %s to %d", filename, client->base.fd);

    /*
     * CMD_SYNC
     */
    struct stat fs;
    if (stat(filename, &fs) == 0) {
        uint8_t bufsend[LEN_PACKET_NORMAL];
        MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetBFileFill(packet, pupname, fs.st_size);
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

bool _push_raw(StorageEntry *me, struct reqitem *item)
{
    NetBinaryNode *client = item->client;
    if (client->base.fd <= 0 || !item->name || !me->storepath) return false;

    mtc_mt_dbg("push %s to %d", item->name, item->client->base.fd);

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

bool _push_track_cover(StorageEntry *me, struct reqitem *item)
{
    NetBinaryNode *client = item->client;
    if (client->base.fd <= 0 || !item->id || !me->storepath) return false;

    mtc_mt_dbg("push %s to %d", item->id, item->client->base.fd);

    char nameWithPath[PATH_MAX] = {0}, filename[PATH_MAX] = {0};
    snprintf(nameWithPath, sizeof(nameWithPath), "assets/cover/%s", item->id);

    DommeFile *mfile = dommeGetFile(me->plan, item->id);
    if (mfile) {
        uint8_t *imgbuf;
        char *mime;
        size_t coversize;
        snprintf(filename, sizeof(filename), "%s%s%s%s",
                 me->libroot, me->storepath, mfile->dir, mfile->name);

        mp3dec_map_info_t *mapinfo = mp3_cover_open(filename, &mime, &imgbuf, &coversize);
        if (!mapinfo) {
            snprintf(filename, sizeof(filename), "%s.avm/track_cover.jpg", me->libroot);
            return _push_puppet(item->client, filename, nameWithPath);
        }

        /* CMD_SYNC */
        uint8_t bufsend[LEN_PACKET_NORMAL];
        MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetBFileFill(packet, nameWithPath, coversize);
        packetCRCFill(packet);

        SSEND(client->base.fd, bufsend, sendlen);

        /* file contents */
        SSEND(client->base.fd, imgbuf, coversize);

        mp3_cover_close(mapinfo);

        return true;
    } else mtc_mt_warn("%s not exist", item->id);

    return false;
}

bool _push_artist_cover(StorageEntry *me, struct reqitem *item)
{
    NetBinaryNode *client = item->client;
    if (client->base.fd <= 0 || !item->artist || !me->storepath) return false;

    mtc_mt_dbg("push %s to %d", item->artist, item->client->base.fd);

    char nameWithPath[PATH_MAX] = {0}, filename[PATH_MAX] = {0};
    snprintf(nameWithPath, sizeof(nameWithPath), "assets/cover/%s", item->artist);

    DommeArtist *artist = artistFind(me->plan->artists, item->artist);
    DommeAlbum *disk;
    DommeFile *mfile;
    if (artist) {
        MLIST_ITERATE(artist->albums, disk) {
            MLIST_ITERATEB(disk->tracks, mfile) {
                uint8_t *imgbuf;
                char *mime;
                size_t coversize;
                snprintf(filename, sizeof(filename), "%s%s%s%s",
                         me->libroot, me->storepath, mfile->dir, mfile->name);

                mp3dec_map_info_t *mapinfo = mp3_cover_open(filename, &mime, &imgbuf, &coversize);
                if (!mapinfo) continue;

                /* CMD_SYNC */
                uint8_t bufsend[LEN_PACKET_NORMAL];
                MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
                size_t sendlen = packetBFileFill(packet, nameWithPath, coversize);
                packetCRCFill(packet);

                SSEND(client->base.fd, bufsend, sendlen);

                /* file contents */
                SSEND(client->base.fd, imgbuf, coversize);

                mp3_cover_close(mapinfo);

                return true;
            }
        }
    } else mtc_mt_warn("can't find %s", item->artist);

    mtc_mt_warn("%s don't have cover", item->artist);

    snprintf(filename, sizeof(filename), "%s.avm/artist_cover.jpg", me->libroot);
    return _push_puppet(item->client, filename, nameWithPath);
}

bool _push_album_cover(StorageEntry *me, struct reqitem *item)
{
    NetBinaryNode *client = item->client;
    if (client->base.fd <= 0 || !item->artist || !item->album || !me->storepath) return false;

    mtc_mt_dbg("push %s %s to %d", item->artist, item->album, item->client->base.fd);

    char nameWithPath[PATH_MAX] = {0}, filename[PATH_MAX] = {0};
    snprintf(nameWithPath, sizeof(nameWithPath), "assets/cover/%s_%s", item->artist, item->album);

    DommeArtist *artist = artistFind(me->plan->artists, item->artist);
    DommeFile *mfile;
    if (artist) {
        DommeAlbum *disk = albumFind(artist->albums, item->album);
        if (disk) {
            MLIST_ITERATE(disk->tracks, mfile) {
                uint8_t *imgbuf;
                char *mime;
                size_t coversize;
                snprintf(filename, sizeof(filename), "%s%s%s%s",
                         me->libroot, me->storepath, mfile->dir, mfile->name);

                mp3dec_map_info_t *mapinfo = mp3_cover_open(filename, &mime, &imgbuf, &coversize);
                if (!mapinfo) continue;

                /* CMD_SYNC */
                uint8_t bufsend[LEN_PACKET_NORMAL];
                MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
                size_t sendlen = packetBFileFill(packet, nameWithPath, coversize);
                packetCRCFill(packet);

                SSEND(client->base.fd, bufsend, sendlen);

                /* file contents */
                SSEND(client->base.fd, imgbuf, coversize);

                mp3_cover_close(mapinfo);

                return true;
            }
        } else mtc_mt_warn("can't find album %s", item->album);
    } else mtc_mt_warn("can't find artist %s", item->artist);

    mtc_mt_warn("%s don't have cover", item->artist);

    snprintf(filename, sizeof(filename), "%s.avm/disk_cover.jpg", me->libroot);
    return _push_puppet(item->client, filename, nameWithPath);
}

bool _push_pong(StorageEntry *me, struct reqitem *item)
{
    NetBinaryNode *client = item->client;
    if (client->base.fd <= 0) return false;

    mtc_mt_dbg("push PONG to %d", item->client->base.fd);

    uint8_t sendbuf[LEN_IDIOT];
    packetPONGFill(sendbuf, LEN_IDIOT);
    SSEND(client->base.fd, sendbuf, LEN_IDIOT);

    return true;
}

void* _pusher(void *arg)
{
    StorageEntry *me = (StorageEntry*)arg;
    uint8_t idle_count = 0;
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

            if (++idle_count >= 3) {
                mtc_mt_noise("check my users in freetime");

                idle_count = 0;

                NetBinaryNode *client;
                MLIST_ITERATE(me->clients, client) {
                    if (client->base.dropped) {
                        mlist_delete(me->clients, _moon_i);
                        _moon_i--;
                    }
                }
            }

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

        idle_count = 0;

        switch (item->type) {
        case SYNC_RAWFILE:
            _push_raw(me, item);
            break;
        case SYNC_TRACK_COVER:
            _push_track_cover(me, item);
            break;
        case SYNC_ARTIST_COVER:
            _push_artist_cover(me, item);
            break;
        case SYNC_ALBUM_COVER:
            _push_album_cover(me, item);
            break;
        case SYNC_PONG:
            _push_pong(me, item);
            break;
        default:
            break;
        }

        reqitem_free(item);
    }

    return NULL;
}

void _push(StorageEntry *me, char *name, char *id, char *artist, char *album,
           SYNC_TYPE stype, NetBinaryNode *client)
{
    if (!me) return;

    if (!client) {
        mtc_mt_warn("%s client null", name);
        return;
    }

    struct reqitem *item = (struct reqitem*)mos_calloc(1, sizeof(struct reqitem));
    if (name)   item->name   = strdup(name);
    if (id)     item->id     = strdup(id);
    if (artist) item->artist = strdup(artist);
    if (album)  item->album  = strdup(album);
    item->client = client;
    item->type = stype;

    pthread_mutex_lock(&me->lock);
    if (!client->in_business) {
        client->in_business = true;
        mlist_append(me->clients, client);
    }
    mlist_append(me->synclist, item);
    pthread_cond_signal(&me->cond);
    pthread_mutex_unlock(&me->lock);
}

void binaryPush(BeeEntry *be, SYNC_TYPE stype, NetBinaryNode *client)
{
    if (!be || !client) return;

    StorageEntry *me = (StorageEntry*)be;

    _push(me, NULL, NULL, NULL, NULL, stype, client);
}

bool storage_process(BeeEntry *be, QueueEntry *qe)
{
    char filename[PATH_MAX] = {0};
    MERR *err;
    StorageEntry *me = (StorageEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);
    //MDF_TRACE_MT(qe->nodein);

    switch (qe->command) {
    /* 为减少网络传输，CMD_DB_MD5为同步前必传，用以指定后续同步文件之媒体库 */
    case CMD_DB_MD5:
    {
        char *name = mdf_get_value(qe->nodein, "name", NULL);
        mos_free(me->storename);
        mos_free(me->storepath);
        if (!_store_node(me, name, &me->storename, &me->storepath)) {
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

                _push(me, "music.db", NULL, NULL, NULL, SYNC_RAWFILE, qe->client->binary);
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
        char *name   = mdf_get_value(qe->nodein, "name", NULL);
        char *id     = mdf_get_value(qe->nodein, "id", NULL);
        char *artist = mdf_get_value(qe->nodein, "artist", NULL);
        char *album  = mdf_get_value(qe->nodein, "album", NULL);

        SYNC_TYPE type = mdf_get_int_value(qe->nodein, "type", SYNC_RAWFILE);

        _push(me, name, id, artist, album, type, qe->client->binary);
    }
    break;
    case CMD_REMOVE:
    {
        char *id = mdf_get_value(qe->nodein, "id", NULL);
        if (id) {
            DommeFile *mfile = dommeGetFile(me->plan, id);
            if (mfile) {
                snprintf(filename, sizeof(filename), "%s%s%s%s", me->libroot, me->storepath,
                         mfile->dir, mfile->name);
                remove(filename);

                /*
                 * 因为独特(SB)的设计，删除文件时，能及时更新plan，新增时则不能
                 * 再次强调，CMD_DB_MD5为同步前必传，以更新plan
                 */
                mhash_remove(me->plan->mfiles, id);
            }
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

    pthread_mutex_destroy(&me->lock);

    if (me->plan) dommeStoreFree(me->plan);
    mlist_destroy(&me->synclist);
    mlist_destroy(&me->clients);
}

BeeEntry* _start_storage()
{
    StorageEntry *me = mos_calloc(1, sizeof(StorageEntry));

    me->base.process = storage_process;
    me->base.stop = storage_stop;

    me->libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!me->libroot) {
        mtc_mt_err("library root path not found");
        return NULL;
    }

    me->plan = NULL;
    me->storename = NULL;
    me->storepath = NULL;
    mlist_init(&me->synclist, reqitem_free);
    mlist_init(&me->clients, _client_destroy);

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
