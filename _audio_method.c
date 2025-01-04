void onUstickMount(char *name)
{
    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return;
    }

    uint8_t bufsend[LEN_IDIOT];
    packetIdiotFill(bufsend, IDIOT_USTICK_MOUNT);

    NetClientNode *client;
    MLIST_ITERATE(be->users, client) {
        if (!client->base.dropped) SSEND(client->base.fd, bufsend, LEN_IDIOT);
    }
}

MLIST* mediaStoreList()
{
    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return NULL;
    }

    AudioEntry *me = (AudioEntry*)be;

    return me->plans;
}

static int _store_compare(const void *anode, void *key)
{
    return strcmp(mdf_get_value((MDF*)anode, "name", ""), (char*)key);
}

DommeStore* storeExist(char *storename)
{
    if (!storename) return NULL;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) return NULL;

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, storename)) return plan;
    }

    return NULL;
}

bool storeIsDefault(char *storename)
{
    if (!storename) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) return false;

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, storename)) return plan->moren;
    }

    return false;
}

/*
 * 创建了媒体库，需要加入媒体库列表，生成空的媒体数据库，并监控该媒体库下所有新增和删除事件
 */
MERR* storeCreated(char *storename)
{
    MERR *err;
    MERR_NOT_NULLA(storename);

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) return merr_raise(MERR_ASSERT, "can't find audio backend");

    AudioEntry *me = (AudioEntry*)be;
    char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
    if (!libroot) return merr_raise(MERR_ASSERT, "libraryRoot not found");

    MDF *config;
    mdf_init(&config);

#define RETURN(ret)                             \
    do {                                        \
        mdf_destroy(&config);                   \
        return (ret);                           \
    } while(0)

    err = mdf_json_import_filef(config, "%sconfig.json", libroot);
    if (err) RETURN(merr_pass(err));

    MDF *snode = mdf_search(config, storename, _store_compare);
    if (!snode) RETURN(merr_raise(MERR_ASSERT, "can't find store %s", storename));

    char *path = mdf_get_value(snode, "path", NULL);
    if (!path) RETURN(merr_raise(MERR_ASSERT, "path not found"));

    DommeStore *plan = dommeStoreCreate();
    char fullpath[PATH_MAX-64] = {0};
    snprintf(fullpath, sizeof(fullpath), "%s%s", libroot, path);
    plan->name = strdup(storename);
    plan->basedir = strdup(fullpath);
    plan->moren = mdf_get_bool_value(snode, "default", false);

    if (indexerScan(plan, true, me)) {
        dommeStoreDumpFilef(plan, "%smusic.db", plan->basedir);
        dommeStoreReplace(me, plan);
    } else {
        dommeStoreFree(plan);
        RETURN(merr_raise(MERR_ASSERT, "scan %s failure", storename));
    }

    /* 一家人使用，_add_watch 就不改名字了 */
    pthread_mutex_lock(&me->index_lock);
    me->seeds = _add_watch(plan, "", me->efd, me->seeds);
    pthread_mutex_unlock(&me->index_lock);

#undef RETURN

    mdf_destroy(&config);

    return MERR_OK;
}

bool storeRename(char *namesrc, char *namedst)
{
    if (!namesrc || !namedst) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, namesrc)) {
            free(plan->name);
            plan->name = strdup(namedst);
            return true;
        }
    }

    return false;
}

bool storeSetDefault(char *storename)
{
    if (!storename) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, storename)) {
            me->act = ACT_STOP;
            me->plan = plan;
            plan->moren = true;
        } else plan->moren = false;
    }

    return true;
}

/*
 * 删除媒体库，force 为 true 时删除非空媒体库
 */
bool storeDelete(char *storename, bool force)
{
    if (!storename) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    DommeStore *plandft = NULL;
    MLIST_ITERATE(me->plans, plan) {
        if (plan->moren) {
            /* 默认媒体库不能删除 */
            if (!strcmp(plan->name, storename)) {
                mtc_mt_warn("can'd delete default store");
                return false;
            } else {
                plandft = plan;
                break;
            }
        }
    }

    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, storename)) {
            /* 删除当前媒体库时的切换 */
            if (me->plan == plan) {
                me->act = ACT_STOP;
                me->plan = plandft;
            }

            if (plan->count_track == 0) {
                /* 删除空的媒体库 */
                pthread_mutex_lock(&me->index_lock);
                _remove_watch(me, plan);
                pthread_mutex_unlock(&me->index_lock);
                mos_rmrf(plan->basedir);
            } else {
                if (!force) {
                    mtc_mt_dbg("don't delete nonempty store");
                    return false;
                } else {
                    /* 删除非空媒体库 */
                    pthread_mutex_lock(&me->index_lock);
                    _remove_watch(me, plan);
                    pthread_mutex_unlock(&me->index_lock);
                    mos_rmrf(plan->basedir);
                }
            }

            mlist_delete(me->plans, _moon_i);
            return true;
        }
    }

    return false;
}

/*
 * 将媒体库 src 中的媒体文件合并（移动）至媒体库 dest 中，并删除媒体库 src
 */
bool storeMerge(char *src, char *dest)
{
    if (!src || !dest || !strcmp(src, dest)) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    /* 源媒体库中 媒体文件 移动至目标媒体库 */
    DommeStore *plansrc = NULL, *plandest = NULL, *plandft = NULL, *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (plan->moren) plandft = plan;

        if (!strcmp(plan->name, src)) plansrc = plan;
        else if (!strcmp(plan->name, dest)) plandest = plan;
    }

    if (!plansrc || !plandest) {
        mtc_mt_warn("can't find store %s %s", src, dest);
        return false;
    }

    /* 默认媒体库不能合并 */
    if (plansrc->moren) {
        mtc_mt_warn("can't merge default store %s %s", me->plan->name, src);
        return false;
    }

    /* 合并当前媒体库时的切换 */
    if (me->plan == plansrc) {
        me->act = ACT_STOP;
        me->plan = plandft;
    }

    if (plansrc->count_track > 0) {
        /* /home/pi/music/2024-11-20 16:09:40[/] */
        char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
        if (!libroot) return false;

        size_t len = strlen(libroot);
        if (strlen(plansrc->basedir) <= len) return false;
        if (strncmp(plansrc->basedir, libroot, len)) return false;
        char *srcpath = plansrc->basedir + len;

        char pathname[PATH_MAX];
        snprintf(pathname, sizeof(pathname), "%s%s", plandest->basedir, srcpath);
        mtc_mt_dbg("rename %s ===> %s", plansrc->basedir, pathname);
        if (rename(plansrc->basedir, pathname) != 0) {
            mtc_mt_warn("rename %s to %s failure %s", plansrc->basedir, pathname, strerror(errno));
            return false;
        }
    }

    return storeDelete(src, true);
}

/*
 * make sure pathfrom and pathto both end with '/'
 */
int storeMediaCopy(DommeStore *plan, char *pathfrom, char *pathto, bool recursive)
{
    char srcfile[PATH_MAX], destfile[PATH_MAX];
    char mediaID[LEN_DOMMEID];

    if (!pathfrom || !pathto) return 0;

    DIR *dir = opendir(pathfrom);
    if (!dir) return 0;

    if (!mos_mkdir(pathto, 0755)) return 0;

    int trackcount = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        if (entry->d_type == DT_REG) {
            snprintf(srcfile, sizeof(srcfile), "%s%s", pathfrom, entry->d_name);

            memset(mediaID, 0x0, LEN_DOMMEID);

            MediaNode *mnode = mediaOpen(srcfile);
            if (mnode) {
                memcpy(mediaID, mnode->md5, LEN_DOMMEID);
                mediaID[LEN_DOMMEID-1] = '\0';

                mnode->driver->close(mnode);
            } else continue;

            if (dommeGetFile(plan, mediaID)) {
                mtc_mt_dbg("file %s exist, skip", srcfile);
                continue;
            } else mtc_mt_dbg("COPY %s %s", srcfile, mediaID);

            /* 拷贝媒体文件 */
            snprintf(destfile, sizeof(destfile), "%s%s", pathto, entry->d_name);
            if (mos_copyfile(srcfile, destfile, 0644)) trackcount++;
            else mtc_mt_warn("copy %s => %s failure %s", srcfile, destfile, strerror(errno));
        } else if (entry->d_type == DT_DIR && recursive) {
            char fullfrom[PATH_MAX], fullto[PATH_MAX];
            snprintf(fullfrom, sizeof(fullfrom), "%s%s/", pathfrom, entry->d_name);
            snprintf(fullto, sizeof(fullto), "%s%s/", pathto, entry->d_name);

            trackcount += storeMediaCopy(plan, fullfrom, fullto, recursive);
        }
    }

    closedir(dir);

    return trackcount;
}
