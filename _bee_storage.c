typedef struct {
    BeeEntry base;
    char *storename;
    char *storepath;
} StorageEntry;

bool storage_process(BeeEntry *be, QueueEntry *qe)
{
    //StorageEntry *me = (StorageEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);

    switch (qe->command) {
    case CMD_DB_MD5:
    {
        char *checksum = mdf_get_value(qe->nodein, "checksum", NULL);
        if (!checksum) {
            char *name = mdf_get_value(qe->nodein, "name", NULL);
            mtc_mt_dbg("dbsync, push %s music.db", name);

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
    mtc_mt_dbg("stop worker %s", be->name);
}

BeeEntry* _start_storage()
{
    StorageEntry *me = mos_calloc(1, sizeof(StorageEntry));

    me->base.process = storage_process;
    me->base.stop = storage_stop;

    me->storename = NULL;
    me->storepath = NULL;

    return (BeeEntry*)me;
}

BeeDriver storage_driver = {
    .id = FRAME_STORAGE,
    .name = "storage",
    .init_driver = _start_storage
};
