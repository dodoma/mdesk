#ifndef __BEE_AUDIO_H__
#define __BEE_AUDIO_H__

#include <alsa/asoundlib.h>

#define LEN_DOMMEID 11
#define LEN_MEDIA_TOKEN 128

typedef enum {
    ACT_NONE = 0,
    ACT_PLAY,
    ACT_PAUSE,
    ACT_RESUME,
    ACT_NEXT,
    ACT_PREV,
    ACT_DRAG,
    ACT_STOP,
} PLAY_ACTION;

typedef enum {
    MEDIA_UNKNOWN = 0,
    MEDIA_WAV,
    MEDIA_FLAC,
    MEDIA_MP3,
} MEDIA_TYPE;

/*
 * ================ DOMME ================
 */
typedef struct {
    char *title;
    char *year;
    MLIST *tracks;
    uint32_t pos;               /* 当前播放曲目 */
} DommeAlbum;

typedef struct {
    char *name;
    MLIST *albums;
    uint32_t count_track;
    uint32_t pos;               /* 当前播放专辑 */
} DommeArtist;

typedef struct {
    char id[LEN_DOMMEID];

    char *dir;                  /* directory part of filename */
    char *name;                 /* name part of filename */

    char *title;

    uint8_t  sn;                /* 曲目编号 */
    int index;                  /* 精确到毫秒的开始位置 */
    int length;                 /* 精确到秒的媒体文件时长 */

    DommeAlbum  *disk;
    DommeArtist *artist;

    bool touched;
} DommeFile;

typedef struct {
    char *name;
    char *basedir;              /* libroot + config.json中的path + [/] */
    bool moren;                 /* 默认媒体库 */

    MLIST *dirs;
    MHASH *mfiles;
    MLIST *artists;

    uint32_t count_album;
    uint32_t count_track;
    uint32_t count_touched;
    uint32_t pos;               /* 当前播放艺术家 */
} DommeStore;

/*
 * ================ AUDIO ================
 */
typedef struct {
    uint64_t samples;
    int channels;
    int hz;                     /* sample rate */
    int kbps;                   /* bit rate */
    uint32_t length;            /* track length in seconds */
} TechInfo;

typedef struct {
    char title[LEN_MEDIA_TOKEN];
    char artist[LEN_MEDIA_TOKEN];
    char album[LEN_MEDIA_TOKEN];
    char year[LEN_MEDIA_TOKEN];
    char track[LEN_MEDIA_TOKEN];
    uint32_t length;            /* track length in seconds */
} ArtInfo;

struct audioTrack {
    char *id;                   /* 当前正在播放的曲目 */
    bool playing;

    TechInfo tinfo;

    MEDIA_TYPE media_type;
    const char *media_name;
    bool media_switch;          /* 是否切换了媒体文件类型 */

    float percent;              /* 当前播放进度，或拖拽百分比 */
    uint64_t samples_eat;
};

struct watcher {
    int wd;
    time_t on_dirty;
    DommeStore *plan;
    char *path;

    struct watcher *next;
};

/*
 * 总的来说，用户动作有
 * 1. 设置播放范围
 * 2. 设置后续动作（顺序播放、随机播放、循环模式、几首后关闭）
 * 3. 播放、暂停、上一首、下一首
 * 4. 拖动播放
 */
typedef struct {
    BeeEntry base;

    pthread_t worker;           /* player */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;

    pthread_t indexer;          /* indexer */
    struct watcher *seeds;
    int efd;
    pthread_mutex_t index_lock;

    snd_pcm_t *pcm;
    snd_mixer_elem_t *mixer;

    MLIST *plans;               /* 所有媒体库列表 */
    DommeStore *plan;           /* 当前使用的媒体库 */

    char *trackid;              /* 设置播放曲目 */
    char *album;                /* 设置播放专辑 */
    char *artist;               /* 设置播放艺术家 */

    PLAY_ACTION act;

    bool shuffle;
    bool loopon;
    int remain;
    float dragto;

    MLIST *playlist;            /* 播放列表 */

    struct audioTrack *track;
} AudioEntry;

/*
 * ================ MEDIA ================
 */
typedef struct {
    char filename[PATH_MAX];
    char md5[33];
    TechInfo tinfo;
    ArtInfo ainfo;
    struct _media_entry *driver;
} MediaNode;

typedef struct _media_entry {
    MEDIA_TYPE type;
    const char *name;

    bool (*check)(const char *filename);

    MediaNode* (*open)(const char *filename);
    TechInfo*  (*tech_info_get)(MediaNode *mnode);
    ArtInfo*   (*art_info_get)(MediaNode *mnode);
    uint8_t*   (*cover_get)(MediaNode *mnode, size_t *imagelen);
    bool       (*play)(MediaNode *mnode, AudioEntry *audio);
    void       (*close)(MediaNode *mnode);
} MediaEntry;

MEDIA_TYPE mediaType(const char *filename);
MediaNode* mediaOpen(const char *filename);

/*
 * ================ method ================
 */
DommeStore* dommeStoreCreate();
void dommeStoreFree(void *p);
MERR* dommeLoadFromFilef(DommeStore *plan, char *fmt, ...);
DommeFile* dommeGetFile(DommeStore *plan, char *id);

DommeArtist* artistFind(MLIST *artists, char *name);
DommeAlbum* albumFind(MLIST *albums, char *title);

void onUstickMount(char *name);

/* 注意，请严格控制，对返回结果只读不写 */
MLIST* mediaStoreList();

DommeStore* storeExist(char *storename);
bool storeIsDefault(char *storename);
MERR* storeCreated(char *storename);
bool storeRename(char *namesrc, char *namedst);
bool storeSetDefault(char *storename);
bool storeDelete(char *storename, bool force);
bool storeMerge(char *src, char *dest);
int storeMediaCopy(DommeStore *plan, char *pathfrom, char *pathto, bool recursive);

#endif  /* __BEE_AUDIO_H__ */
