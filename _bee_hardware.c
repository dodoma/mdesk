#include <dirent.h>
#include <sys/statvfs.h>

typedef struct {
    BeeEntry base;
} HardwareEntry;

struct diskinfo {
    uint64_t capacity;
    uint64_t occupy;
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
    struct diskinfo fsinfo = {.capacity = 0, .occupy = 0, .percent = 0.0};

    if (!path) return fsinfo;

    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        mtc_mt_warn("statvfs error %s", strerror(errno));
        return fsinfo;
    }

    fsinfo.capacity = (uint64_t)st.f_blocks * st.f_frsize;
    fsinfo.remain = (uint64_t)st.f_bavail * st.f_frsize;
    fsinfo.occupy = fsinfo.capacity > fsinfo.remain ? fsinfo.capacity - fsinfo.remain : 0;
    if (fsinfo.capacity != 0) {
        fsinfo.percent = (double)(fsinfo.occupy) / fsinfo.capacity;
        //fsinfo.percent = roundf(fsinfo.percent * 100) / 100;
    }

    return fsinfo;
}

static uint64_t _disk_useage(const char *path)
{
    char filename[PATH_MAX];

    if (!path) return 0;

    DIR *dir = opendir(path);
    if (!dir) return 0;

    uint64_t filesize = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        struct stat info;
        snprintf(filename, sizeof(filename), "%s%s", path, entry->d_name);
        if (stat(filename, &info) == -1) {
            mtc_mt_warn("stat %s failure %s", filename, strerror(errno));
            continue;
        }

        if (S_ISDIR(info.st_mode)) {
            snprintf(filename, sizeof(filename), "%s%s/", path, entry->d_name);
            filesize += _disk_useage(filename);
        } else if (S_ISREG(info.st_mode)) filesize += info.st_size;
    }

    closedir(dir);

    return filesize;
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

static char* _interface_ipv4(char *iface)
{
    static char ip[INET_ADDRSTRLEN];
    struct ifreq ifr;

    if (!iface) return "";

    memset(ip, 0x0, INET_ADDRSTRLEN);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    strncpy(ifr.ifr_name, iface, IFNAMSIZ-1);
    ifr.ifr_addr.sa_family = AF_INET;

    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);

    inet_ntop(AF_INET, (void*)&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, ip, INET_ADDRSTRLEN);

    return ip;
}

bool hdw_process(BeeEntry *be, QueueEntry *qe)
{
    char filename[PATH_MAX];
    //HardwareEntry *me = (HardwareEntry*)be;
    MessagePacket *packet = NULL;
    size_t sendlen = 0;
    MERR *err;

    mtc_mt_dbg("process command %d", qe->command);

    memset(filename, 0x0, PATH_MAX);

    switch (qe->command) {
    case CMD_WIFI_SET:
        mtc_mt_dbg("set wifi ... %s", mdf_get_value(qe->nodein, "name", "unknownName"));
        /* TODO business logic */
        packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);

        break;
    case CMD_HOME_INFO:
    {
        char *libroot = mdf_get_value(g_config, "libraryRoot", "/");
        struct diskinfo fsinfo = _get_disk_space(libroot);

        mdf_set_value(qe->nodeout, "deviceID", g_cpuid);
        mdf_set_value(qe->nodeout, "deviceName", mdf_get_value(g_runtime, "deviceName", ""));
        mdf_set_bool_value(qe->nodeout, "autoPlay", mdf_get_bool_value(g_runtime, "autoPlay", false));
        mdf_set_value(qe->nodeout, "shareLocation", _interface_ipv4("wlan0"));

        mdf_set_value(qe->nodeout, "capacity", _size_2_string(fsinfo.capacity));
        mdf_set_value(qe->nodeout, "remain", _size_2_string(fsinfo.remain));
        mdf_set_value(qe->nodeout, "useage", _size_2_string(fsinfo.occupy));
        mdf_set_double_value(qe->nodeout, "percent", fsinfo.percent);

        /* usbStick */
        struct ustick *stick = _usb_stick_get("/media");
        struct ustick *tnode = stick;

        MDF *snode = mdf_get_or_create_node(qe->nodeout, "usbStick");
        while (tnode) {
            MDF *cnode = mdf_insert_node(snode, NULL, -1);
            mdf_set_value(cnode, NULL, tnode->name);

            tnode = tnode->next;
        }
        mdf_object_2_array(snode, NULL);
        if (mdf_child_count(snode, NULL) == 0) mdf_set_bool_value(qe->nodeout, "usbON", false);
        else mdf_set_bool_value(qe->nodeout, "usbON", true);

        _usb_stick_free(stick);

        /* libraries */
        snode = mdf_get_or_create_node(qe->nodeout, "libraries");
        MLIST *plans = mediaStoreList();
        DommeStore *plan;
        MLIST_ITERATE(plans, plan) {
            uint64_t filesize = _disk_useage(plan->basedir);

            MDF *cnode = mdf_insert_node(snode, NULL, -1);
            mdf_set_value(cnode, "name", plan->name);
            mdf_set_int_value(cnode, "countTrack", plan->count_track);
            mdf_set_value(cnode, "space", _size_2_string(filesize));
            if (plan->moren) mdf_set_bool_value(cnode, "dft", true);
            else mdf_set_bool_value(cnode, "dft", false);
        }
        mdf_object_2_array(snode, NULL);

        packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        sendlen = packetResponseFill(packet, qe->seqnum, qe->command, true, NULL, qe->nodeout);
    }
    break;
    case CMD_STORE_CREATE:
    {
        /*
         * 创建媒体库需要：
         * 1. 创建媒体库磁盘目录
         * 2. 修改 libraryRoot/config.json 写入媒体库信息
         * 3. 生成 samba 配置文件，并重启 samba
         * 4. 通知 bee_audio 管理该媒体库
         */
        packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);

        char *storename = mdf_get_value(qe->nodein, "name", NULL);
        if (!storename) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "缺少name参数");
            break;
        }

        char *libroot = mdf_get_value(g_config, "libraryRoot", NULL);
        if (!libroot) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "配置文件损坏");
            break;
        }

        if (storeExist(storename)) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "媒体库已存在");
            break;
        }

        /* 创建媒体库目录 */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char storepath[20] = {0}; /* 2024-11-20 11:05:12 */
        struct tm *tm = localtime(&tv.tv_sec);
        strftime(storepath, 20, "%Y-%m-%d %H:%M:%S", tm);
        storepath[24] = 0;

        snprintf(filename, sizeof(filename), "%s%s", libroot, storepath);
        if (!mos_mkdir(filename, 0755)) {
            mtc_mt_warn("mkdir %s failure %s", filename, strerror(errno));
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "创建目录失败");
            break;
        }

        /* 修改媒体库配置文件 */
        snprintf(filename, sizeof(filename), "%sconfig.json", libroot);

        MDF *libconfig;
        mdf_init(&libconfig);
        err = mdf_json_import_file(libconfig, filename);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "读取库文件失败");

            mdf_destroy(&libconfig);
            break;
        }

        MDF *snode = mdf_insert_node(libconfig, NULL, -1);
        mdf_set_value(snode, "name", storename);
        mdf_set_valuef(snode, "path=%s/", storepath);

        err = mdf_json_export_file(libconfig, filename);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "写入库文件失败");

            mdf_destroy(&libconfig);
            break;
        }

        /* 重启samba服务 */
        MCS *tpl;
        snprintf(filename, sizeof(filename), "%stemplate/smb.conf", g_location);
        err = mcs_parse_file(filename, NULL, NULL, &tpl);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "读取模板文件失败");
            mdf_destroy(&libconfig);
            break;
        }

        MDF *datanode;
        mdf_init(&datanode);
        mdf_set_value(datanode, "libroot", libroot);
        mdf_copy(datanode, "stores", libconfig, true);

        //err = mcs_rend(tpl, datanode, "/tmp/smb.conf");
        err = mcs_rend(tpl, datanode, "/etc/samba/smb.conf");
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "写入模板文件失败");
            mcs_destroy(&tpl);
            mdf_destroy(&libconfig);
            mdf_destroy(&datanode);
            break;
        }

        mcs_destroy(&tpl);
        mdf_destroy(&datanode);
        mdf_destroy(&libconfig);

        if (system("systemctl restart smbd") != 0) {
            mtc_mt_warn("restart smbd failure");
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "重启服务失败");
            break;
        }

        /* 通知 audio */
        storeCreated(storename);

        sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);
    }
    break;
    case CMD_STORE_RENAME:
    {
        packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);

        char *storea = mdf_get_value(qe->nodein, "from", NULL);
        char *storeb = mdf_get_value(qe->nodein, "to", NULL);
        if (!storea || !storeb) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "参数有误");
            break;
        }

        if (!storeExist(storea)) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "媒体库不存在");
            break;
        }

        if (storeExist(storeb)) {
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "媒体库已存在");
            break;
        }

        /* 修改媒体库配置文件 */
        char *libroot = mdf_get_value(g_config, "libraryRoot", "");
        snprintf(filename, sizeof(filename), "%sconfig.json", libroot);
        MDF *libconfig;
        mdf_init(&libconfig);
        err = mdf_json_import_file(libconfig, filename);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "读取库文件失败");

            mdf_destroy(&libconfig);
            break;
        }

        MDF *cnode = mdf_node_child(libconfig);
        while (cnode) {
            char *name = mdf_get_value(cnode, "name", NULL);
            char *path = mdf_get_value(cnode, "path", NULL);

            if (name && path && !strcmp(name, storea)) {
                mdf_set_value(cnode, "name", storeb);
                break;
            }

            cnode = mdf_node_next(cnode);
        }

        err = mdf_json_export_file(libconfig, filename);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "写入库文件失败");

            mdf_destroy(&libconfig);
            break;
        }

        /* 重启samba服务 */
        MCS *tpl;
        snprintf(filename, sizeof(filename), "%stemplate/smb.conf", g_location);
        err = mcs_parse_file(filename, NULL, NULL, &tpl);
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "读取模板文件失败");
            mdf_destroy(&libconfig);
            break;
        }

        MDF *datanode;
        mdf_init(&datanode);
        mdf_set_value(datanode, "libroot", libroot);
        mdf_copy(datanode, "stores", libconfig, true);

        err = mcs_rend(tpl, datanode, "/etc/samba/smb.conf");
        if (err != MERR_OK) {
            TRACE_NOK_MT(err);
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "写入模板文件失败");
            mcs_destroy(&tpl);
            mdf_destroy(&libconfig);
            mdf_destroy(&datanode);
            break;
        }

        mcs_destroy(&tpl);
        mdf_destroy(&datanode);
        mdf_destroy(&libconfig);

        if (system("systemctl restart smbd") != 0) {
            mtc_mt_warn("restart smbd failure");
            sendlen = packetACKFill(packet, qe->seqnum, qe->command, false, "重启服务失败");
            break;
        }

        /* 通知 audio */
        storeRename(storea, storeb);

        sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);
    }
    break;
    default:
        break;
    }

    if (packet && sendlen > 0) {
        packetCRCFill(packet);
        SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);
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
