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
    if (err) return merr_pass(err);

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
                if (mdf_child_count(song, NULL) != 6) continue;

                char *id = mdf_get_value(song, "[0]", NULL);
                char *name = mdf_get_value_copy(song, "[1]", NULL);
                char *title = mdf_get_value_copy(song, "[2]", NULL);

                mtc_mt_noise("restore music %s%s with id %s", dir, name, id);

                mfile = mos_calloc(1, sizeof(DommeFile));
                snprintf(mfile->id, sizeof(mfile->id), id);
                mfile->dir = dir;
                mfile->name = name;
                mfile->title = title;

                mfile->sn = mdf_get_int_value(song, "[3]", 0);
                mfile->filesize = mdf_get_uint32_value(song, "[4]", 0);
                mfile->duration = mdf_get_uint32_value(song, "[5]", 0);
                mfile->touched = false;

                mhash_insert(plan->mfiles, mfile->id, mfile);
                plan->count_track++;

                mlist_append(disk->tracks, mfile);
                artist->count_track++;

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
    //size_t slen = strlen(libroot);
    //if (slen == 0 || libroot[slen-1] != '/') mdf_append_string_value(g_config, "libraryRoot", "/");

    err = mdf_json_import_filef(config, "%s/config.json", libroot);
    if (err) return merr_pass(err);

    MDF *cnode = mdf_node_child(config);
    while (cnode) {
        char *name = mdf_get_value_copy(cnode, "name", NULL);
        char *path = mdf_get_value_copy(cnode, "path", NULL);
        if (name && path) {
            DommeStore *plan = dommeStoreCreate();

            char fullpath[PATH_MAX-64] = {0};
            snprintf(fullpath, sizeof(fullpath), "%s/%s/", libroot, path);
            plan->name = strdup(name);
            plan->basedir = strdup(fullpath);
            plan->moren = mdf_get_bool_value(cnode, "default", false);

            err = dommeLoadFromFilef(plan, "%smp3.db", plan->basedir);
            TRACE_NOK_MT(err);
        }

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&config);

    return MERR_OK;
}

void* _indexer(void *arg)
{
    MERR *err;

    char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!libroot) return merr_raise(MERR_ASSERT, "library root path not found");

    MDF *config;
    mdf_init(&config);
    err = mdf_json_import_filef(config, "%s/config.json", libroot);
    if (err) return merr_pass(err);

    MDF *cnode = mdf_node_child(config);
    while (cnode) {

        cnode = mdf_node_next(cnode);
    }

    mdf_destroy(&config);
}
