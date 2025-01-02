struct indexer_arg {
    DommeStore *plan;
    MLIST *files;
};

struct meta_arg {
    char *smd5;
    char *title;
    char *artist;
    char *album;
    char *year;
    char *track;
};

static void _on_meta(void *data, drflac_metadata *meta)
{
    struct meta_arg *arg = (struct meta_arg*)data;

    if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO) {
        mstr_bin2hexstr(meta->data.streaminfo.md5, 16, arg->smd5);
        mstr_tolower(arg->smd5);
    } else if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        const char *comment = NULL;
        drflac_uint32 comment_len = 0;
        drflac_vorbis_comment_iterator piter;
        drflac_init_vorbis_comment_iterator(&piter,
                                            meta->data.vorbis_comment.commentCount,
                                            meta->data.vorbis_comment.pComments);

        while ((comment = drflac_next_vorbis_comment(&piter, &comment_len)) != NULL) {
            if (comment_len > LEN_ID3_STRING) comment_len = LEN_ID3_STRING;

            if (!strncasecmp(comment, "TITLE", 5)) {
                comment += 6;
                memcpy(arg->title, comment, comment_len - 6);
            } else if (!strncasecmp(comment, "ARTIST", 6)) {
                comment += 7;
                memcpy(arg->artist, comment, comment_len - 7);
            } else if (!strncasecmp(comment, "ALBUM", 5)) {
                comment += 6;
                memcpy(arg->album, comment, comment_len - 6);
            } else if (!strncasecmp(comment, "TRACKNUMBER", 11)) {
                comment += 12;
                memcpy(arg->track, comment, comment_len - 12);
            } else if (!strncasecmp(comment, "YEAR", 4)) {
                comment += 5;
                memcpy(arg->year, comment, comment_len - 5);
            }
        }
    }
}

static int _scan_directory(const struct dirent *ent)
{
    if ((ent->d_name[0] == '.' && ent->d_name[1] == 0) ||
        (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == 0)) return 0;

    if (ent->d_type == DT_DIR) return 1;
    else return 0;
}

static int _scan_music(const struct dirent *ent)
{
    if (ent->d_type == DT_REG && strcmp(ent->d_name, "music.db")) return 1;
    else return 0;
}

static void _scan_for_files(DommeStore *plan, const char *subdir, MLIST *filesok, MLIST *filesnew)
{
    struct dirent **deps = NULL, **feps = NULL;

    mtc_mt_dbg("scan directory %s", subdir);

    if (!mlist_search(plan->dirs, &subdir, _strcompare)) mlist_append(plan->dirs, strdup(subdir));

    char fullpath[PATH_MAX] = {0};
    snprintf(fullpath, sizeof(fullpath), "%s%s", plan->basedir, subdir);

    int n = scandir(fullpath, &feps, _scan_music, alphasort);
    for (int i = 0; i < n; i++) {
        char filename[PATH_MAX], *ptr = filename;
        snprintf(filename, sizeof(filename), "%s%s%s", plan->basedir, subdir, feps[i]->d_name);
        if (!mlist_search(filesok, &ptr, _strcompare)) mlist_append(filesnew, strdup(filename));

        free(feps[i]);
    }

    n = scandir(fullpath, &deps, _scan_directory, alphasort);
    for (int i = 0; i < n; i++) {
        char dirname[PATH_MAX];
        snprintf(dirname, sizeof(dirname), "%s%s/", subdir, deps[i]->d_name);

        _scan_for_files(plan, dirname, filesok, filesnew);

        free(deps[i]);
    }
}

static struct watcher* _add_watch(DommeStore *plan, char *subdir, int efd, struct watcher *seed)
{
    struct dirent *dirent;
    char fullpath[PATH_MAX] = {0};
    snprintf(fullpath, sizeof(fullpath), "%s%s", plan->basedir, subdir);

    int wd = inotify_add_watch(efd, fullpath, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd == -1) {
        mtc_mt_warn("can't watch %s %s", fullpath, strerror(errno));
        return seed;
    }

    mtc_mt_dbg("add %s to watch", fullpath);

    struct watcher *arg = mos_calloc(1, sizeof(struct watcher));
    arg->wd = wd;
    arg->on_dirty = 0;
    arg->plan = plan;
    arg->path = strdup(subdir);
    arg->next = seed;

    DIR *pwd = opendir(fullpath);
    if (!pwd) {
        mtc_mt_warn("open directory %s failure %s", fullpath, strerror(errno));
        return arg;
    }

    while ((dirent = readdir(pwd)) != NULL) {
        if (!strcmp(dirent->d_name, "..") || !strcmp(dirent->d_name, ".")) continue;

        if (dirent->d_type == DT_DIR) {
            char dirname[PATH_MAX];
            snprintf(dirname, sizeof(dirname), "%s%s/", subdir, dirent->d_name);

            arg = _add_watch(plan, dirname, efd, arg);
        }
    }

    closedir(pwd);

    return arg;
}

static void _remove_watch(AudioEntry *me, DommeStore *plan)
{
    if (!plan || !me->seeds) return;

    mtc_mt_dbg("remove plan %s", plan->name);

    struct watcher *prev = NULL, *next = NULL;
    struct watcher *node = me->seeds;
    while (node) {
        next = node->next;

        if (node->plan == plan) {
            if (node == me->seeds) me->seeds = next;

            inotify_rm_watch(me->efd, node->wd);
            mos_free(node->path);
            mos_free(node);

            if (prev) prev->next = next;
        } else prev = node;

        node = next;
    }
}

/*
 * /home/pi/music/Steve Ray Vaughan/Track01.mp3
 */
static bool _extract_filename(char *filename, DommeStore *plan, char **path, char **name)
{
    if (!filename || !plan || !path || !name) return false;

    size_t len = strlen(plan->basedir);

    if (strncmp(filename, plan->basedir, len)) return false;

    char *p = filename + len;
    char *q = filename + strlen(filename);

    while (*q != '/' && q > filename) q--;

    if (q <= filename) return false;

    *name = q + 1;

    if (q < p) {
        *path = "";
        return true;
    }

    char dir[PATH_MAX], *ptr = dir;
    snprintf(dir, sizeof(dir), "%.*s", (int)(q - p + 1), p);
    char *addr = mlist_search(plan->dirs, &ptr, _strcompare);
    if (!addr) return false;

    *path = *(char**)addr;

    return true;
}

/* 告诉所有在线用户哥在忙着索引文件 */
static void _onStoreIndexing(AudioEntry *me)
{
    if (!me || mlist_length(me->base.users) == 0) return;

    uint8_t bufsend[LEN_IDIOT];
    packetIdiotFill(bufsend, IDIOT_BUSY_INDEXING);

    NetClientNode *client;
    MLIST_ITERATE(me->base.users, client) {
        if (!client->base.dropped) {
            SSEND(client->base.fd, bufsend, LEN_IDIOT);
        }
    }
}

/* 告诉所有在线用户索引弄完了 */
static void _onStoreIndexDone(AudioEntry *me)
{
    if (!me || mlist_length(me->base.users) == 0) return;

    uint8_t bufsend[LEN_IDIOT];
    packetIdiotFill(bufsend, IDIOT_FREE);

    NetClientNode *client;
    MLIST_ITERATE(me->base.users, client) {
        if (!client->base.dropped) {
            SSEND(client->base.fd, bufsend, LEN_IDIOT);
        }
    }
}

/* 给所有的在线用户推送music.db */
static void _onStoreChange(AudioEntry *me, DommeStore *plan)
{
    if (!me || mlist_length(me->base.users) == 0 || !plan || !plan->basedir) return;

    char *libroot = mdf_get_value(g_config, "libraryRoot", "");
    int rlen = strlen(libroot), blen = strlen(plan->basedir);
    if (rlen >= blen) return;

    char *storepath = plan->basedir + rlen;

    struct stat fs;
    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%smusic.db", plan->basedir);
    if (stat(filename, &fs) != 0) {
        mtc_mt_warn("stat %s failure %s", filename, strerror(errno));
        return;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        mtc_mt_warn("open %s failure %s", filename, strerror(errno));
        return;
    }

    NetClientNode *client;
    MLIST_ITERATE(me->base.users, client) {
        if (!client->base.dropped && client->binary) {
            NetBinaryNode *bnode = client->binary;

            mtc_mt_dbg("push %smusic.db to %d", plan->basedir, bnode->base.fd);

            /*
             * CMD_SYNC
             */
            char nameWithPath[PATH_MAX];
            snprintf(nameWithPath, sizeof(nameWithPath), "%smusic.db", storepath);

            uint8_t bufsend[LEN_PACKET_NORMAL];
            MessagePacket *packet = packetMessageInit(bufsend, LEN_PACKET_NORMAL);
            size_t sendlen = packetBFileFill(packet, nameWithPath, fs.st_size);
            packetCRCFill(packet);

            SSEND(bnode->base.fd, bufsend, sendlen);

            /*
             * file contents
             */
            fseek(fp, 0, SEEK_SET);
            uint8_t buf[4096] = {0};
            size_t len = 0;
            while ((len = fread(buf, 1, sizeof(buf), fp)) > 0) {
                SSEND(bnode->base.fd, buf, len);
            }

        }
    }

    fclose(fp);
}

static void* _index_music(void *arg)
{
    struct indexer_arg *arga = arg;
    DommeStore *plan = arga->plan;
    char *filename = NULL;
    char *fpath, *fname;

    DommeFile *mfile;
    DommeAlbum *disk;
    DommeArtist *artist;

    int indexcount = 0;
    static int threadsn = 0;
    char threadname[24];
    snprintf(threadname, sizeof(threadname), "indexer%02d", threadsn++);
    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf(threadname, loglevel, g_log_tostdout ? "-" : "%slog/%s.log", g_location, threadname);

    mtc_mt_dbg("I am %s", threadname);

    pthread_mutex_lock(&m_indexer_lock);
    filename = mlist_popx(arga->files);
    pthread_mutex_unlock(&m_indexer_lock);

    char smd5[33], stitle[LEN_ID3_STRING], sartist[LEN_ID3_STRING], salbum[LEN_ID3_STRING];
    char syear[LEN_ID3_STRING], strack[LEN_ID3_STRING];
    drflac *pflac = NULL;
    struct meta_arg metaarg = {
        .smd5 = smd5, .title = stitle, .artist = sartist, .album = salbum,
        .year = syear, .track = strack
    };
    mp3dec_map_info_t map_info;
    mp3dec_file_info_t fileinfo;
    while (filename) {
        mfile = NULL;
        pflac = NULL;
        memset(smd5,    0x0, sizeof(smd5));
        memset(stitle,  0x0, sizeof(stitle));
        memset(sartist, 0x0, sizeof(sartist));
        memset(salbum,  0x0, sizeof(salbum));
        memset(syear,   0x0, sizeof(syear));
        memset(strack,  0x0, sizeof(strack));
        memset(&fileinfo, 0x0, sizeof(mp3dec_file_info_t));

        if (!_extract_filename(filename, plan, &fpath, &fname)) {
            mtc_mt_dbg("extract %s failure", filename);
            goto nextone;
        }

        if ((pflac = drflac_open_file_with_metadata(filename, _on_meta, &metaarg, NULL)) != NULL) {
            //mhash_file_md5_s(filename, md5sum);
            mfile = mos_calloc(1, sizeof(DommeFile));
            memcpy(mfile->id, smd5, LEN_DOMMEID);
            mfile->id[LEN_DOMMEID-1] = '\0';
            mfile->length = (uint32_t)pflac->totalPCMFrameCount / pflac->sampleRate + 1;

            mfile->dir = fpath;
            mfile->name = strdup(fname);
            mfile->title = strdup(stitle);
            mfile->sn = atoi(strack);
            mfile->touched = false;

            mtc_mt_dbg("%s %s %s %s %s %s", filename, mfile->id, sartist, stitle, salbum, strack);
            //mtc_mt_dbg("%s %s", filename, mfile->id);

            drflac_close(pflac);
            pflac = NULL;
        } else if (mp3dec_open_file(filename, &map_info) == 0) {
            if (mp3dec_detect_buf(map_info.buffer, map_info.size) == 0) {
                /* title, sn, artist, album */
                if (mp3_id3_get_buf(map_info.buffer, map_info.size,
                                    stitle, sartist, salbum, syear, strack)) {
                    mfile = mos_calloc(1, sizeof(DommeFile));
                    mp3_md5_get_buf(map_info.buffer, map_info.size, mfile->id);
                    mp3dec_iterate_buf(map_info.buffer, map_info.size, _iterate_info, &fileinfo);
                    mfile->dir = fpath;
                    mfile->name = strdup(fname);
                    mfile->title = strdup(stitle);
                    mfile->sn = atoi(strack);
                    mfile->length = (uint32_t)fileinfo.samples / fileinfo.hz + 1;
                    mfile->touched = false;

                    //mtc_mt_noise("%s %s %s %s %s", mfile->id, sartist, stitle, salbum, strack);
                    mtc_mt_dbg("%s %s", filename, mfile->id);
                } else {
                    mtc_mt_dbg("%s not valid mp3 file, REMOVE", filename);
                    remove(filename);
                }
            } else mtc_mt_warn("%s not music", filename);

            mp3dec_close_file(&map_info);
        }

    nextone:
        pthread_mutex_lock(&m_indexer_lock);
        if (mfile) {
            if (mhash_lookup(plan->mfiles, mfile->id)) {
                /* TODO file length verify, and id crash process */
                mtc_mt_dbg("%s already exist", filename);

                remove(filename);
                dommeFileFree(mfile->id, mfile);
            } else {
                artist = artistFind(plan->artists, sartist);
                if (!artist) {
                    artist = artistCreate(sartist);
                    mlist_append(plan->artists, artist);
                }
                disk = albumFind(artist->albums, salbum);
                if (!disk) {
                    disk = albumCreate(salbum);
                    disk->year = strdup(syear);
                    mlist_append(artist->albums, disk);
                    plan->count_album++;
                }

                mfile->artist = artist;
                mfile->disk = disk;

                mlist_append(disk->tracks, mfile);
                artist->count_track++;

                mhash_insert(plan->mfiles, mfile->id, mfile);
                plan->count_track++;

                indexcount++;
            }
        }
        filename = mlist_popx(arga->files);
        pthread_mutex_unlock(&m_indexer_lock);
    }

    mtc_mt_dbg("%s done with %d tracks indexed.", threadname, indexcount);

    return NULL;
}

/*
 * 遍历媒体库目录，重新建立/首次更新 索引
 * 有更新，返回true, 否则返回false
 */
bool indexerScan(DommeStore *plan, bool fresh, AudioEntry *me)
{
    char filename[PATH_MAX];
    MLIST *filesa, *filesb, *filesc;
    DommeFile *mfile;
    char *key;
    bool ret = fresh; /* 对于新建索引，返回 true */

    mtc_mt_dbg("scan library %s...", plan->basedir);

    mlist_init(&filesa, free);  /* 保存数据库中原有文件列表 */
    mlist_init(&filesb, free);  /* 保存数据库中没有，媒体库中有的文件列表 （新增） */
    mlist_init(&filesc, free);  /* 保存数据库中有，媒体库中没有的文件列表 （删除） */

    /*
     * 1. 已索引文件名保存至列表 filesa
     */
    if (!fresh) {
        MHASH_ITERATE(plan->mfiles, key, mfile) {
            snprintf(filename, sizeof(filename), "%s%s%s", plan->basedir, mfile->dir, mfile->name);
            mlist_append(filesa, strdup(filename));

            if (access(filename, F_OK) != 0) mlist_append(filesc, strdup(filename));
        }
    }

    /*
     * 2. 找出所有待检测文件保存至列表 filesb, 同时把所有目录更新至 plan->dirs，方便后续使用
     */
    _scan_for_files(plan, "", filesa, filesb);

    if (mlist_length(filesb) > 0) {
        mtc_mt_dbg("got %d files to index", mlist_length(filesb));

        _onStoreIndexing(me);

        /*
         * 3. 探测文件
         */
        struct indexer_arg arga = {plan, filesb};

        int pnum = sysconf(_SC_NPROCESSORS_ONLN);
        if (pnum <= 0) pnum = 2;
        pthread_t **threads = mos_calloc(pnum, sizeof(pthread_t*));
        for (int i = 0; i < pnum; i++) {
            threads[i] = mos_calloc(1, sizeof(pthread_t));
            pthread_create(threads[i], NULL, _index_music, &arga);
        }

        int joinnum = 0;
        while (joinnum < pnum) {
            for (int i = 0; i < pnum; i++) {
                if (threads[i]) {
                    struct timespec timeout;
                    clock_gettime(CLOCK_REALTIME, &timeout);
                    timeout.tv_sec += 3;
                    int rv = pthread_timedjoin_np(*threads[i], NULL ,&timeout);
                    if (rv == 0) {
                        free(threads[i]);
                        threads[i] = NULL;

                        joinnum++;
                    } else _onStoreIndexing(me);
                }
            }
        }
        mos_free(threads);

        ret = true;

        _onStoreIndexDone(me);
    }

    /*
     * 处理已删除的媒体文件
     */
    if (mlist_length(filesc) > 0) {
        char *delfile, *fpath, *fname;
        MLIST_ITERATE(filesc, delfile) {
            if (!_extract_filename(delfile, plan, &fpath, &fname)) {
                mtc_mt_warn("%s not valid file", delfile);
                continue;
            }

            MHASH_ITERATE(plan->mfiles, key, mfile) {
                if (!strcmp(fpath, mfile->dir) && !strcmp(fname, mfile->name)) {
                    mhash_remove(plan->mfiles, key);
                    break;
                }
            }
        }

        ret = true;
    }

    mlist_destroy(&filesa);
    mlist_destroy(&filesb);
    mlist_destroy(&filesc);

    return ret;
}

/*
 * 遍历媒体库子目录，将子目录下所有媒体文件加入媒体库 [删除子目录下媒体数据库]
 * 常用于监控新剪切过来的媒体目录
 */
void indexerScanSubdirectory(DommeStore *plan, const char *subpath)
{
    MLIST *files;

    mtc_mt_dbg("scan library %s with subdirectory %s...", plan->name, subpath);

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s%smusic.db", plan->basedir, subpath);
    if (remove(filename) != 0) mtc_mt_warn("remove %s failure %s", filename, strerror(errno));

    mlist_init(&files, free);

    _scan_for_files(plan, subpath, NULL, files);

    if (mlist_length(files) > 0) {
        mtc_mt_dbg("got %d files to index", mlist_length(files));

        struct indexer_arg arga = {plan, files};

        int pnum = sysconf(_SC_NPROCESSORS_ONLN);
        if (pnum <= 0) pnum = 2;
        pthread_t **threads = mos_calloc(pnum, sizeof(pthread_t*));
        for (int i = 0; i < pnum; i++) {
            threads[i] = mos_calloc(1, sizeof(pthread_t));
            pthread_create(threads[i], NULL, _index_music, &arga);
        }

        for (int i = 0; i < pnum; i++) {
            pthread_join(*threads[i], NULL);
            mos_free(threads[i]);
        }
        mos_free(threads);
    }

    mlist_destroy(&files);
}

/*
 * 实时监控媒体库目录，更新至已有索引，保持持续同步
 */
struct watcher* indexerWatch(int efd, struct watcher *seeds, AudioEntry *me)
{
    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    char *ptr;

    DommeFile *mfile;
    DommeAlbum *disk;
    DommeArtist *artist;
    DommeStore *plan;

    /* Loop while events can be read from inotify file descriptor. */
    for (;;) {
        /* Read some events. */
        ssize_t len = read(efd, buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            perror("read");
            return seeds;
        }

        /* If the nonblocking read() found no events to read, then
           it returns -1 with errno set to EAGAIN. In that case,
           we exit the loop. */
        if (len <= 0) break;

        /* Loop over all events in the buffer */
        for (ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *) ptr;

            struct watcher *arg = seeds;
            while (arg) {
                if (arg->wd == event->wd) break;
                arg = arg->next;
            }

            if (!arg || !arg->plan) {
                mtc_mt_warn("监控外事件");
                continue;
            }

            plan = arg->plan;

            if (event->mask & IN_ISDIR) {
                /* 目录动作 */
                char pathname[PATH_MAX] = {0}, *key = pathname;
                char fullname[PATH_MAX] = {0};
                snprintf(pathname, PATH_MAX, "%s%s/", arg->path, event->name);
                snprintf(fullname, PATH_MAX, "%s%s%s/", plan->basedir, arg->path, event->name);

                if (event->mask & IN_CREATE) {
                    mtc_mt_dbg("%s%s CREATE directory %s", plan->basedir, arg->path, event->name);

                    if (!mlist_search(plan->dirs, &key, _strcompare)) {
                        mtc_mt_dbg("append dir %s", pathname);
                        mlist_append(plan->dirs, strdup(pathname));
                    }

                    int wd = inotify_add_watch(efd, fullname,
                                               IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_TO);
                    if (wd == -1) {
                        mtc_mt_warn("can't watch %s %s", fullname, strerror(errno));
                    } else {
                        struct watcher *item = mos_calloc(1, sizeof(struct watcher));
                        item->wd = wd;
                        item->on_dirty = 0;
                        item->plan = plan;
                        item->path = strdup(pathname);
                        item->next = seeds;
                        seeds = item;
                    }
                } else if (event->mask & IN_DELETE) {
                    mtc_mt_dbg("%s%s DELETE directory %s", plan->basedir, arg->path, event->name);
                    arg = seeds;
                    struct watcher *prev = NULL, *next = NULL;
                    while (arg) {
                        next = arg->next;

                        if (!strcmp(arg->path, pathname)) {
                            //mtc_mt_dbg("del watcher %s", arg->path);
                            if (arg == seeds) seeds = next;

                            inotify_rm_watch(efd, arg->wd);
                            mos_free(arg->path);
                            mos_free(arg);
                            if (prev) prev->next = next;

                            break;
                        } else prev = arg;

                        arg = next;
                    }
                } else if (event->mask & IN_MOVED_TO) {
                    mtc_mt_dbg("%s%s MOVED IN directory %s", plan->basedir, arg->path, event->name);

                    if (!mlist_search(plan->dirs, &key, _strcompare)) {
                        mtc_mt_dbg("append dir %s", pathname);
                        mlist_append(plan->dirs, strdup(pathname));
                    }

                    int wd = inotify_add_watch(efd, fullname,
                                               IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_TO);
                    if (wd == -1) {
                        mtc_mt_warn("can't watch %s %s", fullname, strerror(errno));
                    } else {
                        struct watcher *item = mos_calloc(1, sizeof(struct watcher));
                        item->wd = wd;
                        item->on_dirty = 0;
                        item->plan = plan;
                        item->path = strdup(pathname);
                        item->next = seeds;
                        seeds = item;
                    }

                    indexerScanSubdirectory(plan, pathname);

                    arg->on_dirty = g_ctime;
                } else mtc_mt_warn("%s unknown event %s %d", arg->path, event->name, event->mask);
            } else if (event->len > 0) {
                /* 文件动作 */
                char filename[PATH_MAX];
                char *fpath, *fname;
                snprintf(filename, PATH_MAX, "%s%s%s", plan->basedir, arg->path, event->name);

                if (event->mask & IN_CLOSE_WRITE || event->mask & IN_MOVED_TO) {
                    if (event->name[0] == '.' || !strcmp(event->name, "music.db")) continue;

                    mtc_mt_dbg("%s%s CREATE file %s", plan->basedir, arg->path, event->name);

                    drflac *pflac = NULL;
                    mfile = NULL;
                    char smd5[33];
                    char stitle[LEN_ID3_STRING], sartist[LEN_ID3_STRING], salbum[LEN_ID3_STRING];
                    char syear[LEN_ID3_STRING], strack[LEN_ID3_STRING];
                    struct meta_arg metaarg = {
                        .smd5 = smd5, .title = stitle, .artist = sartist, .album = salbum,
                        .year = syear, .track = strack
                    };
                    mp3dec_map_info_t map_info;
                    mp3dec_file_info_t fileinfo;
                    memset(smd5,    0x0, sizeof(smd5));
                    memset(stitle,  0x0, sizeof(stitle));
                    memset(sartist, 0x0, sizeof(sartist));
                    memset(salbum,  0x0, sizeof(salbum));
                    memset(syear,   0x0, sizeof(syear));
                    memset(strack,  0x0, sizeof(strack));
                    memset(&fileinfo, 0x0, sizeof(mp3dec_file_info_t));

                    if (!_extract_filename(filename, plan, &fpath, &fname)) {
                        mtc_mt_warn("extract %s failure", filename);
                        continue;
                    }

                    if ((pflac = drflac_open_file_with_metadata(filename, _on_meta,
                                                                &metaarg, NULL)) != NULL) {
                        mfile = mos_calloc(1, sizeof(DommeFile));
                        memcpy(mfile->id, smd5, LEN_DOMMEID);
                        mfile->id[LEN_DOMMEID-1] = '\0';
                        mfile->length = (uint32_t)pflac->totalPCMFrameCount / pflac->sampleRate + 1;

                        mfile->dir = fpath;
                        mfile->name = strdup(event->name);
                        mfile->title = strdup(stitle);
                        mfile->sn = atoi(strack);
                        mfile->touched = false;

                        drflac_close(pflac);
                    } else if (mp3dec_open_file(filename, &map_info) == 0) {
                        if (mp3dec_detect_buf(map_info.buffer, map_info.size) == 0) {
                            if (mp3_id3_get_buf(map_info.buffer, map_info.size,
                                                stitle, sartist, salbum, syear, strack)) {
                                mfile = mos_calloc(1, sizeof(DommeFile));
                                mp3_md5_get_buf(map_info.buffer, map_info.size, mfile->id);
                                mp3dec_iterate_buf(map_info.buffer, map_info.size,
                                                   _iterate_info, &fileinfo);
                                mfile->dir = fpath;
                                mfile->name = strdup(event->name);
                                mfile->title = strdup(stitle);
                                mfile->sn = atoi(strack);
                                mfile->length = (uint32_t)fileinfo.samples / fileinfo.hz + 1;
                                mfile->touched = false;

                            } else mtc_mt_warn("%s not valid mp3 file", filename);
                        } else mtc_mt_warn("%s not music", filename);

                        mp3dec_close_file(&map_info);
                    }

                    if (mfile) {
                        artist = artistFind(plan->artists, sartist);
                        if (!artist) {
                            artist = artistCreate(sartist);
                            mlist_append(plan->artists, artist);
                        }
                        disk = albumFind(artist->albums, salbum);
                        if (!disk) {
                            disk = albumCreate(salbum);
                            disk->year = strdup(syear);
                            mlist_append(artist->albums, disk);
                            plan->count_album++;
                        }

                        mfile->artist = artist;
                        mfile->disk = disk;

                        mlist_append(disk->tracks, mfile);
                        artist->count_track++;

                        mhash_insert(plan->mfiles, mfile->id, mfile);
                        plan->count_track++;

                        if (arg->on_dirty == 0) {
                            /* 通知UI */
                            _onStoreIndexing(me);
                        }

                        arg->on_dirty = g_ctime;
                    }
                } else if (event->mask & IN_DELETE) {
                    mtc_mt_dbg("%s%s DELETE file %s", plan->basedir, arg->path, event->name);

                    if (!_extract_filename(filename, plan, &fpath, &fname)) {
                        mtc_mt_warn("extract %s failure", filename);
                        continue;
                    }

                    char *key;
                    MHASH_ITERATE(plan->mfiles, key, mfile) {
                        if (!strcmp(fpath, mfile->dir) && !strcmp(fname, mfile->name)) {
                            mhash_remove(plan->mfiles, key);
                            break;
                        }
                    }

                    arg->on_dirty = g_ctime;
                }
            } else mtc_mt_warn("file event %d with name NLL", event->mask);
        }
    }

    return seeds;
}

void watcherFree(struct watcher *arg)
{
    struct watcher *next = NULL;

    while (arg) {
        next = arg->next;

        mos_free(arg->path);
        mos_free(arg);

        arg = next;
    }
}

void* dommeIndexerStart(void *arg)
{
    AudioEntry *me = (AudioEntry*)arg;
    DommeStore *plan;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf("indexer", loglevel, g_log_tostdout ? "-"  :"%slog/%s.log", g_location, "indexer");

    mtc_mt_dbg("I am audio indexer");

    char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!libroot) {
        mtc_mt_err("library root path not found");
        return NULL;
    }

    MDF *config;
    mdf_init(&config);
    MERR *err = mdf_json_import_filef(config, "%sconfig.json", libroot);
    if (err) {
        TRACE_NOK_MT(err);
        return NULL;
    }

    MDF *cnode = mdf_node_child(config);
    while (cnode) {
        mdf_makesure_endwithc(cnode, "path", '/');
        char *name = mdf_get_value(cnode, "name", NULL);
        char *path = mdf_get_value(cnode, "path", NULL);
        if (name && path) {
            char basedir[PATH_MAX], filename[PATH_MAX];
            snprintf(basedir, sizeof(basedir), "%s%s", libroot, path);
            snprintf(filename, sizeof(filename), "%s%smusic.db", libroot, path);

            plan = dommeStoreCreate();
            plan->name = strdup(name);
            plan->basedir = strdup(basedir);
            plan->moren = mdf_get_bool_value(cnode, "default", false);

            err = dommeLoadFromFile(filename, plan);
            if (err == MERR_OK) {
                /* 保持首次同步 */
                if (indexerScan(plan, false, me)) {
                    dommeStoreDumpFilef(plan, "%smusic.db", basedir);
                    dommeStoreReplace(me, plan);
                } else dommeStoreFree(plan);
            } else {
                if (indexerScan(plan, true, me)) {
                    dommeStoreDumpFilef(plan, "%smusic.db", basedir);
                    dommeStoreReplace(me, plan);
                } else {
                    mtc_mt_err("scan %s with directory %s failure", name, basedir);
                    dommeStoreFree(plan);
                }
            }
        }

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&config);

    if (mdf_get_bool_value(g_runtime, "autoplay", false)) me->act = ACT_PLAY;

    /* me->plans 已是最新的索引文件，并都已吐出至music.db，poll 和 ionotify 监控目录变化 */
    mtc_mt_dbg("plans DONE. monitor file system...");

    me->efd = inotify_init1(IN_NONBLOCK);
    if (me->efd == -1) {
        mtc_mt_err("inotify init failure %s", strerror(errno));
        return NULL;
    }

    me->seeds = NULL;

    MLIST_ITERATE(me->plans, plan) {
        me->seeds = _add_watch(plan, "", me->efd, me->seeds);
    }

    if (me->seeds) {
        struct pollfd fds[1];
        fds[0].fd = me->efd;
        fds[0].events = POLLIN;
        int nfd = 1;

        while (me->running) {
            int poll_num = poll(fds, nfd, 1000);
            if (poll_num == -1 && errno != EINTR) {
                mtc_mt_err("poll error %s", strerror(errno));
                return NULL;
            }

            if (poll_num > 0) {
                if (fds[0].revents & POLLIN) {
                    pthread_mutex_lock(&me->index_lock);
                    me->seeds = indexerWatch(me->efd, me->seeds, me);
                    pthread_mutex_unlock(&me->index_lock);
                }
            } else if (poll_num == 0) {
                /* timeout */
                pthread_mutex_lock(&me->index_lock);
                struct watcher *item = me->seeds;
                while (item) {
                    if (item->on_dirty && g_ctime > item->on_dirty && g_ctime - item->on_dirty > 19) {
                        dommeStoreDumpFilef(item->plan, "%smusic.db", item->plan->basedir);

                        /* 通知所有已连接客户端，更新媒体数据库 */
                        _onStoreChange(me, item->plan);

                        _onStoreIndexDone(me);
                        item->on_dirty = 0;

                        /* 避免重复 dump */
                        struct watcher *next = item->next;
                        while (next) {
                            if (next->plan == item->plan) next->on_dirty = 0;

                            next = next->next;
                        }
                    }

                    item = item->next;
                }
                pthread_mutex_unlock(&me->index_lock);
            }
        }
    } else mtc_mt_err("no directory need to watch");

    watcherFree(me->seeds);

    return MERR_OK;
}
