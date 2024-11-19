#include <dirent.h>
#include <sys/statvfs.h>

typedef struct {
    BeeEntry base;
} HardwareEntry;

struct diskinfo {
    uint64_t capacity;
    uint64_t remain;
    float percent;         /* 使用占比 0.64 */
};

struct ustick {
    char name[32];

    struct ustick *next;
};

static char* _size_2_string(uint64_t len)
{
    static char res[10];

    uint32_t oneM = 1024 * 1024;
    uint32_t oneG = 1024 * 1024 * 1024;

    memset(res, 0x0, sizeof(res));

    if (len > oneG) sprintf(res, "%.1fG", (double)len / oneG);
    else sprintf(res, "%.1fM", (double)len / oneM);

    return res;
}

static struct diskinfo _get_disk_space(const char *path)
{
    struct diskinfo fsinfo = {.capacity = 0, .remain = 0, .percent = 0.0};

    if (!path) return fsinfo;

    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        mtc_mt_warn("statvfs error %s", strerror(errno));
        return fsinfo;
    }

    fsinfo.capacity = (uint64_t)st.f_blocks * st.f_frsize;
    fsinfo.remain = (uint64_t)st.f_bavail * st.f_frsize;
    if (fsinfo.capacity != 0)
        fsinfo.percent = (double)(fsinfo.capacity - fsinfo.remain) / fsinfo.capacity;
    //fsinfo.percent = roundf(fsinfo.percent * 100) / 100;

    return fsinfo;
}

static bool _dir_is_empty(const char *path)
{
    struct dirent *d;

    if (!path) return true;

    DIR *dir = opendir(path);
    if (!dir) return true;

    int n = 0;
    while ((d = readdir(dir)) != NULL) {
        if (++n > 2) break;
    }
    closedir(dir);

    if (n <= 2) return true;
    else return false;
}

static bool _dir_mounted_separary(const char *path, const char *ppath)
{
    if (!path || !ppath) return false;

    struct stat st, pst;

    if (stat(path, &st) != 0) {
        mtc_mt_warn("stat %s failure %s", path, strerror(errno));
        return false;
    }

    if (stat(ppath, &pst) != 0) {
        mtc_mt_warn("stat %s failure %s", ppath, strerror(errno));
        return false;
    }

    if (st.st_dev == pst.st_dev) return false;
    else return true;
}

/* 获取目录下所有块设备列表 */
static struct ustick* _usb_stick_get(const char *path)
{
    if (!path) return NULL;

    DIR *pwd = opendir(path);
    if (!pwd) {
        mtc_mt_warn("open directory %s failure %s", path, strerror(errno));
        return NULL;
    }

    struct ustick *stick = NULL;

    struct dirent *dirent;
    while ((dirent = readdir(pwd)) != NULL) {
        if (!strcmp(dirent->d_name, "..") || !strcmp(dirent->d_name, ".") ||
            !strncmp(dirent->d_name, "mmcblk0", 7)) continue;

        if (dirent->d_type == DT_DIR) {
            char dirname[PATH_MAX];
            snprintf(dirname, sizeof(dirname), "%s/%s", path, dirent->d_name);

            if (_dir_mounted_separary(dirname, path)) {
                struct ustick *newstick = mos_calloc(1, sizeof(struct ustick));
                int slen = strlen(dirent->d_name);
                if (slen > sizeof(newstick->name)) slen = sizeof(newstick->name);
                strncpy(newstick->name, dirent->d_name, slen);
                newstick->next = stick;
                stick = newstick;
            }
        }
    }

    closedir(pwd);

    return stick;
}

static void _usb_stick_free(struct ustick *stick)
{
    struct ustick *node = stick, *next;
    while (node) {
        next = node->next;
        free(node);

        node = next;
    }
}

bool hdw_process(BeeEntry *be, QueueEntry *qe)
{
    //HardwareEntry *me = (HardwareEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);

    switch (qe->command) {
    case CMD_WIFI_SET:
        mtc_mt_dbg("set wifi ... %s", mdf_get_value(qe->nodein, "name", "unknownName"));
        /* TODO business logic */
        MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);
        packetCRCFill(packet);

        SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);

        break;
    case CMD_HOME_INFO:
    {
        char *libroot = mdf_get_value(g_config, "libraryRoot", "/");
        struct diskinfo fsinfo = _get_disk_space(libroot);

        mdf_set_value(qe->nodeout, "deviceID", g_cpuid);
        mdf_set_value(qe->nodeout, "deviceName", mdf_get_value(g_runtime, "deviceName", ""));
        mdf_set_bool_value(qe->nodeout, "autoPlay", mdf_get_bool_value(g_runtime, "autoPlay", false));
        mdf_set_value(qe->nodeout, "shareLocation", "//TODO");

        mdf_set_value(qe->nodeout, "capacity", _size_2_string(fsinfo.capacity));
        mdf_set_value(qe->nodeout, "remain", _size_2_string(fsinfo.remain));
        mdf_set_double_value(qe->nodeout, "percent", fsinfo.percent);

        struct ustick *stick = _usb_stick_get("/media");
        struct ustick *tnode = stick;

        MDF *snode = mdf_get_or_create_node(qe->nodeout, "usbStick");
        while (tnode) {
            MDF *cnode = mdf_insert_node(snode, NULL, -1);
            mdf_set_value(cnode, NULL, tnode->name);

            tnode = tnode->next;
        }
        mdf_object_2_array(snode, NULL);

        _usb_stick_free(stick);

        MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetResponseFill(packet, qe->seqnum, qe->command, true, NULL, qe->nodeout);
        packetCRCFill(packet);

        SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);
    }
    break;
    default:
        break;
    }

    return true;
}

void hdw_stop(BeeEntry *be)
{
    mtc_mt_dbg("stop worker %s", be->name);
}

BeeEntry* _start_hardware()
{
    HardwareEntry *me = mos_calloc(1, sizeof(HardwareEntry));

    me->base.process = hdw_process;
    me->base.stop = hdw_stop;

    return (BeeEntry*)me;
}

BeeDriver hardware_driver = {
    .id = FRAME_HARDWARE,
    .name = "hardware",
    .init_driver = _start_hardware
};
