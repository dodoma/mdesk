struct indexer_arg {
    DommeStore *plan;
    MLIST *files;
};

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

    mtc_mt_dbg("scan directory %s", plan->basedir);

    if (!*subdir) mlist_append(plan->dirs, "./");
    else if (!mlist_search(plan->dirs, &subdir, _strcompare)) mlist_append(plan->dirs, strdup(subdir));

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
        snprintf(dirname, sizeof(dirname), "%s%s", subdir, deps[i]->d_name);

        _scan_for_files(plan, dirname, filesok, filesnew);

        free(deps[i]);
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
    snprintf(dir, sizeof(dir), "%.*s", q - p + 1, p);
    char *addr = mlist_search(plan->dirs, &ptr, _strcompare);
    if (!addr) return false;

    *path = *(char**)addr;

    return true;
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

    char stitle[LEN_ID3_STRING], sartist[LEN_ID3_STRING], salbum[LEN_ID3_STRING];
    char syear[LEN_ID3_STRING], strack[LEN_ID3_STRING];
    while (filename) {
        mfile = NULL;
        memset(stitle,  0x0, sizeof(stitle));
        memset(sartist, 0x0, sizeof(sartist));
        memset(salbum,  0x0, sizeof(salbum));
        memset(syear,   0x0, sizeof(syear));
        memset(strack,  0x0, sizeof(strack));

        if (!_extract_filename(filename, plan, &fpath, &fname)) {
            mtc_mt_dbg("extract %s failure", filename);
            goto nextone;
        }

        if (mp3dec_detect(filename) == 0) {
            /* title, sn, artist, album */
            if (mp3_id3_get(filename, stitle, sartist, salbum, syear, strack)) {
                mfile = mos_calloc(1, sizeof(DommeFile));
                mp3_md5_get(filename, mfile->id, &mfile->filesize, &mfile->duration);
                mfile->dir = fpath;
                mfile->name = strdup(fname);
                mfile->title = strdup(stitle);
                mfile->sn = atoi(strack);
                mfile->touched = false;

                mtc_mt_noise("%s %s %s %s %s", mfile->id, sartist, stitle, salbum, strack);
            } else mtc_mt_dbg("%s not valid mp3 file", filename);
        //} else if (apedec_detect(filename) == 0) {
        } else {
            mtc_mt_warn("%s not music", filename);
        }

    nextone:
        pthread_mutex_lock(&m_indexer_lock);
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

            indexcount++;
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
bool indexerScan(const char *basedir, DommeStore *plan, bool fresh)
{
    char filename[PATH_MAX];
    MLIST *filesa, *filesb;
    bool ret = false;

    mlist_init(&filesa, free);
    mlist_init(&filesb, free);

    /*
     * 1. 已索引文件名保存至列表 filesa
     */
    if (!fresh) {
        char *key;
        DommeFile *mfile;
        MHASH_ITERATE(plan->mfiles, key, mfile) {
            snprintf(filename, sizeof(filename), "%s%s%s", basedir, mfile->dir, mfile->name);
            mlist_append(filesa, strdup(filename));
        }
    }

    /*
     * 2. 找出所有待检测文件保存至列表 filesb, 同时把所有目录更新至 plan->dirs，方便后续使用
     */
    _scan_for_files(plan, "", filesa, filesb);

    if (mlist_length(filesb) > 0) {
        mtc_mt_dbg("got %d files to index", mlist_length(filesb));
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

        for (int i = 0; i < pnum; i++) {
            pthread_join(*threads[i], NULL);
            mos_free(threads[i]);
        }
        mos_free(threads);

        ret = true;
    } else if (fresh) {
        /* 对于新建索引，写入空的 music.db 并返回 true */
        ret = true;
    }

    mlist_destroy(&filesa);
    mlist_destroy(&filesb);

    return ret;
}

/*
 * 实时监控媒体库目录，更新至已有索引，保持持续同步
 */
void indexerWatch(const char *basedir, DommeStore *plan, AudioEntry *me)
{
}

void* dommeIndexerStart(void *arg)
{
    AudioEntry *me = (AudioEntry*)arg;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf("indexer", loglevel, g_log_tostdout ? "-"  :"%slog/%s.log", g_location, "indexer");

    mtc_mt_dbg("I am audio indexer");

    mlist_clear(me->planb);

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

            DommeStore *plan = dommeStoreCreate();
            plan->name = strdup(name);
            plan->basedir = strdup(basedir);
            plan->moren = mdf_get_bool_value(cnode, "default", false);

            err = dommeLoadFromFile(filename, plan);
            if (err == MERR_OK) {
                mlist_append(me->planb, plan);

                /* 保持首次同步 (TODO 删除文件) */
                if (indexerScan(basedir, plan, false)) {
                    dommeStoreDumpFilef(plan, "%smusic.db", basedir);
                    dommeStoreReload(me, basedir, name);
                }

                indexerWatch(basedir, plan, me);
            } else {
                if (indexerScan(basedir, plan, true)) {
                    mlist_append(me->planb, plan);

                    dommeStoreDumpFilef(plan, "%smusic.db", basedir);
                    dommeStoreReload(me, basedir, name);

                    indexerWatch(basedir, plan, me);
                } else mtc_mt_err("scan %s with directory %s failure", name, basedir);
            }
        }

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&config);

    /* me->planb 已是最新的索引文件，并都已吐出至music.db，epoll 和 ionotify 监控目录变化 */
    mtc_mt_dbg("planb DONE. monitor file system...");

    return MERR_OK;
}
