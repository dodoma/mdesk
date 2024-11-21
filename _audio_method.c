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

bool storeExist(char *storename)
{
    if (!storename) return false;

    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) return false;

    AudioEntry *me = (AudioEntry*)be;

    DommeStore *plan;
    MLIST_ITERATE(me->plans, plan) {
        if (!strcmp(plan->name, storename)) return true;
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

    if (indexerScan(plan->basedir, plan, true)) {
        dommeStoreDumpFilef(plan, "%smusic.db", plan->basedir);
        dommeStoreReplace(me, plan);
    } else {
        dommeStoreFree(plan);
        RETURN(merr_raise(MERR_ASSERT, "scan %s %s failure", storename, plan->basedir));
    }

    /* 对现存seeds内容无修改操作，暂不加锁，另外，一家人使用，_add_watch 就不改名字了 */
    me->seeds = _add_watch(plan, "", me->efd, me->seeds);

#undef RETURN

    mdf_destroy(&config);

    return MERR_OK;
}

/*
 * 删除媒体库，force 为 true 时删除非空媒体库
 */
bool storeDelete(char *storename, bool force)
{
    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    return true;
}

/*
 * 将媒体库 src 中的媒体文件合并（移动）至媒体库 dest 中，并删除媒体库 src
 */
bool storeMerge(char *src, char *dest)
{
    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return false;
    }

    AudioEntry *me = (AudioEntry*)be;

    return true;
}
