static int _iterate_info(void *user_data, const uint8_t *frame, int frame_size,
                            int free_format_bytes, size_t buf_size, uint64_t offset,
                            mp3dec_frame_info_t *info)
{
    if (!frame) return 1;

    mp3dec_file_info_t *outinfo = (mp3dec_file_info_t*)user_data;
    outinfo->samples += hdr_frame_samples(frame);
    outinfo->hz = info->hz;

    return 0;
}

static int _iterate_track(void *user_data, const uint8_t *frame, int frame_size,
                          int free_format_bytes, size_t buf_size, uint64_t offset,
                          mp3dec_frame_info_t *info)
{
    if (!frame) return 1;

    AudioTrack *outinfo = (AudioTrack*)user_data;

    //d->samples += mp3dec_decode_frame(d->mp3d, frame, frame_size, NULL, info);
    outinfo->samples += hdr_frame_samples(frame);
    outinfo->channels = info->channels;
    outinfo->hz = info->hz;

    if (outinfo->kbps < info->bitrate_kbps) outinfo->kbps = info->bitrate_kbps;

    return 0;
}

static int _dir_compare(const void *a, void *key)
{
    MDF *node = (MDF*)a;

    char *val = mdf_get_value(node, "dir", NULL);
    if (val && !strcmp(val, (char*)key)) return 0;

    return 1;
}

static int _album_compare(const void *a, void *key)
{
    MDF *node = (MDF*)a;
    DommeFile *mfile = (DommeFile*)key;

    char *sa = mdf_get_value(node, "a", NULL);
    char *sb = mdf_get_value(node, "b", NULL);

    if (sa && sb) {
        /* 专辑名首先要一样 */
        if (mfile->disk && !strcmp(sb, mfile->disk->title)) {
            /* 作者一样，或者有包含关系，都算同一张专辑 */
            if (mfile->artist && !strcmp(sa, mfile->artist->name)) return 0;
            else if (mfile->artist &&
                     (strstr(sa, mfile->artist->name) || strstr(mfile->artist->name, sa))) return 0;
        }
    }

    return 1;
}

static int _plan_compare(const void *a, const void *b)
{
    DommeStore *pa, *pb;

    pa = *(DommeStore**)a;
    pb = *(DommeStore**)b;

    return strcmp(pa->name, pb->name);
}

static int _strcompare(const void *a, const void *b)
{
    char *sa, *sb;

    sa = *(char **)a;
    sb = *(char **)b;

    return strcmp(sa, sb);
}

static int _track_compare(const void *a, const void *b)
{
    DommeFile *pa, *pb;

    pa = *(DommeFile**)a;
    pb = *(DommeFile**)b;

    return pa->sn - pb->sn;
}

DommeArtist* artistFind(MLIST *artists, char *name)
{
    DommeArtist *artist;
    MLIST_ITERATE(artists, artist) {
        /* TODO 艺术家可能有包含关系，srv, srv & double trouble... */
        if (!strcmp(artist->name, name)) return artist;
    }

    return NULL;
}

DommeAlbum* albumFind(MLIST *albums, char *title)
{
    DommeAlbum *disk;
    MLIST_ITERATE(albums, disk) {
        if (!strcmp(disk->title, title)) return disk;
    }

    return NULL;
}

void dommeFileFree(void *key, void *val)
{
    DommeFile *mfile = (DommeFile*)val;

    /* dir free in StoreFree */
    mos_free(mfile->name);
    mos_free(mfile->title);
    mos_free(mfile);
}

DommeAlbum* albumCreate(char *title)
{
    if (!title) return NULL;

    DommeAlbum *disk = mos_calloc(1, sizeof(DommeAlbum));
    disk->title = strdup(title);
    disk->year = NULL;
    disk->pos = 0;
    mlist_init(&disk->tracks, NULL);

    return disk;
}

void albumFree(void *p)
{
    DommeAlbum *disk = (DommeAlbum*)p;

    mos_free(disk->title);
    mos_free(disk->year);
    mlist_destroy(&disk->tracks);
    mos_free(disk);
}

DommeArtist* artistCreate(char *name)
{
    if (!name) return NULL;

    DommeArtist *artist = mos_calloc(1, sizeof(DommeArtist));
    artist->name = strdup(name);
    artist->pos = 0;
    artist->count_track = 0;
    mlist_init(&artist->albums, albumFree);

    return artist;
}

void artistFree(void *p)
{
    DommeArtist *artist = (DommeArtist*)p;

    mos_free(artist->name);
    mlist_destroy(&artist->albums);
    mos_free(artist);
}

DommeStore* dommeStoreCreate()
{
    DommeStore *plan = mos_calloc(1, sizeof(DommeStore));
    plan->name = NULL;
    plan->basedir = NULL;
    plan->moren = false;
    plan->count_album = 0;
    plan->count_track = 0;
    plan->count_touched = 0;

    mlist_init(&plan->dirs, free);
    mhash_init(&plan->mfiles, mhash_str_hash, mhash_str_comp, dommeFileFree);
    mlist_init(&plan->artists, artistFree);

    return plan;
}

void dommeStoreFree(void *p)
{
    DommeStore *plan = (DommeStore*)p;

    mos_free(plan->name);
    mos_free(plan->basedir);

    mlist_destroy(&plan->dirs);
    mhash_destroy(&plan->mfiles);
    mlist_destroy(&plan->artists);

    mos_free(plan);
}

MERR* dommeLoadFromFile(char *filename, DommeStore *plan)
{
    DommeFile *mfile;
    DommeAlbum *disk;
    DommeArtist *artist;
    MERR *err;

    MERR_NOT_NULLB(filename, plan);

    MDF *dbnode;
    mdf_init(&dbnode);

    err = mdf_mpack_import_file(dbnode, filename);
    if (err) {
        mdf_destroy(&dbnode);
        return merr_pass(err);
    }
    //MDF_TRACE_MT(dbnode);

    MDF *cnode = mdf_node_child(dbnode);
    while (cnode) {
        char *dir = mdf_get_value_copy(cnode, "dir", NULL);
        if (!dir) goto nextdir;

        char **tmps = mlist_search(plan->dirs, &dir, _strcompare);
        if (tmps) {
            mos_free(dir);
            dir = *tmps;
        } else mlist_append(plan->dirs, dir);

        MDF *artnode = mdf_get_child(cnode, "art");
        while (artnode) {
            char *sartist = mdf_get_value(artnode, "a", NULL);
            char *salbum  = mdf_get_value(artnode, "b", NULL);
            char *syear   = mdf_get_value(artnode, "c", "");

            if (!sartist || !salbum) goto nextdisk;

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

            MDF *song = mdf_get_child(artnode, "d");
            while (song) {
                if (mdf_child_count(song, NULL) != 5) goto nextsong;

                char *id = mdf_get_value(song, "[0]", NULL);
                char *name = mdf_get_value(song, "[1]", NULL);
                char *title = mdf_get_value(song, "[2]", NULL);

                mtc_mt_noise("restore music %s%s with id %s", dir, name, id);

                mfile = mos_calloc(1, sizeof(DommeFile));
                memcpy(mfile->id, id, strlen(id) > LEN_DOMMEID ? LEN_DOMMEID : strlen(id));
                mfile->id[LEN_DOMMEID-1] = 0;
                mfile->dir = dir;
                mfile->name = strdup(name);
                mfile->title = strdup(title);

                mfile->sn = mdf_get_int_value(song, "[3]", 0);
                mfile->length = mdf_get_int_value(song, "[4]", 0);
                mfile->touched = false;

                mfile->artist = artist;
                mfile->disk = disk;

                mhash_insert(plan->mfiles, mfile->id, mfile);
                plan->count_track++;

                mlist_append(disk->tracks, mfile);
                artist->count_track++;

            nextsong:
                song = mdf_node_next(song);
            }

        nextdisk:
            artnode = mdf_node_next(artnode);
        }

    nextdir:
        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&dbnode);

    /*
     * post dbload scan
     */
    MLIST_ITERATE(plan->artists, artist) {
        MLIST_ITERATEB(artist->albums, disk) {
            mlist_sort(disk->tracks, _track_compare);
        }
    }

    return MERR_OK;
}

MERR* dommeLoadFromFilef(DommeStore *plan, char *fmt, ...)
{
    char filename[PATH_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(filename, sizeof(filename), fmt, ap);
    va_end(ap);

    return dommeLoadFromFile(filename, plan);
}

MERR* dommeStoresLoad(MLIST *plans)
{
    MERR *err;

    mlist_clear(plans);

    MDF *config;
    mdf_init(&config);

    char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!libroot) return merr_raise(MERR_ASSERT, "library root path not found");

    err = mdf_json_import_filef(config, "%sconfig.json", libroot);
    if (err) return merr_pass(err);

    MDF *cnode = mdf_node_child(config);
    while (cnode) {
        mdf_makesure_endwithc(cnode, "path", '/');
        char *name = mdf_get_value(cnode, "name", NULL);
        char *path = mdf_get_value(cnode, "path", NULL);
        if (name && path) {
            DommeStore *plan = dommeStoreCreate();

            char fullpath[PATH_MAX-64] = {0};
            snprintf(fullpath, sizeof(fullpath), "%s%s", libroot, path);
            plan->name = strdup(name);
            plan->basedir = strdup(fullpath);
            plan->moren = mdf_get_bool_value(cnode, "default", false);

            err = dommeLoadFromFilef(plan, "%smusic.db", plan->basedir);
            if (err == MERR_OK) {
                mlist_append(plans, plan);
            } else {
                dommeStoreFree(plan);
                TRACE_NOK_MT(err);
            }

        }

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&config);

    return MERR_OK;
}

bool dommeStoreDumpFile(DommeStore *plan, char *filename)
{
    if (!plan || !plan->mfiles || !filename) return false;

    mtc_mt_dbg("STORE DUMP %s", filename);

    MDF *datanode;
    mdf_init(&datanode);

    char *key;
    DommeFile *mfile;
    MHASH_ITERATE(plan->mfiles, key, mfile) {
        MDF *cnode = mdf_search(datanode, mfile->dir, _dir_compare);
        if (!cnode) {
            cnode = mdf_insert_node(datanode, NULL, -1);
            mdf_set_value(cnode, "dir", mfile->dir);
        }

        MDF *fnode = mdf_get_or_create_node(cnode, "art");
        MDF *tnode = mdf_search(fnode, mfile, _album_compare);
        if (!tnode) {
            tnode = mdf_insert_node(fnode, NULL, -1);

            mdf_set_value(tnode, "a", mfile->artist->name);
            mdf_set_value(tnode, "b", mfile->disk->title);
            mdf_set_value(tnode, "c", mfile->disk->year);
        }

        MDF *mnode = mdf_insert_node(tnode, "d", -1);
        mdf_set_value(mnode, "0", mfile->id);
        mdf_set_value(mnode, "1", mfile->name);
        mdf_set_value(mnode, "2", mfile->title);
        mdf_set_int_value(mnode, "3", mfile->sn);
        mdf_set_int_value(mnode, "4", mfile->length);

        mdf_object_2_array(mnode, NULL);
        mdf_object_2_array(tnode, "d");
        mdf_object_2_array(cnode, "art");
    }

    mdf_object_2_array(datanode, NULL);

    mdf_mpack_export_file(datanode, filename);

    mdf_destroy(&datanode);
    return true;
}

bool dommeStoreDumpFilef(DommeStore *plan, char *fmt, ...)
{
    char filename[PATH_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(filename, sizeof(filename), fmt, ap);
    va_end(ap);

    return dommeStoreDumpFile(plan, filename);
}

MERR* dommeStoreReload(AudioEntry *me, char *basedir, char *name)
{
    MERR_NOT_NULLC(me, name, basedir);

    mtc_mt_dbg("reload store %s with dir %s", name, basedir);

    me->act = ACT_STOP;

    DommeStore *plan = dommeStoreCreate();
    plan->name = strdup(name);
    plan->basedir = strdup(basedir);
    plan->moren = false;

    MERR *err = dommeLoadFromFilef(plan, "%smusic.db", basedir);
    if (err) return merr_pass(err);

    if (me->plan && !strcmp(me->plan->name, name)) me->plan = plan;

    DommeStore sample = {.name = name}, *key = &sample;
    int index = mlist_index(me->plans, &key, _plan_compare);
    if (index >= 0) {
        DommeStore *olan = mlist_getx(me->plans, index);
        plan->moren = olan->moren;

        mlist_set(me->plans, index, plan);
        dommeStoreFree(olan);
    } else mlist_append(me->plans, plan);

    return MERR_OK;
}

bool dommeStoreReplace(AudioEntry *me, DommeStore *plan)
{
    if (!me || !plan || !plan->name) return false;

    mtc_mt_dbg("replace store %s", plan->name);

    if (plan->moren) {
        me->act = ACT_STOP;
        me->plan = plan;
    }

    DommeStore sample = {.name = plan->name}, *key = &sample;
    int index = mlist_index(me->plans, &key, _plan_compare);
    if (index >= 0) {
        DommeStore *olan = mlist_getx(me->plans, index);
        plan->moren = olan->moren;

        mlist_set(me->plans, index, plan);
        dommeStoreFree(olan);
    } else mlist_append(me->plans, plan);

    return true;
}
