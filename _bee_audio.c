#include <poll.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <iconv.h>
#include <alsa/asoundlib.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ALLOW_MONO_STEREO_TRANSITION
#include "minimp3_ex.h"

#define LEN_DOMMEID 11
#define LEN_ID3_STRING 128

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

    uint8_t  sn;
    size_t   filesize;
    uint32_t duration;            /* in seconds */

    DommeAlbum  *disk;
    DommeArtist *artist;

    bool touched;
} DommeFile;

typedef struct {
    char *name;
    char *basedir;
    bool moren;                 /* 默认媒体库 */

    MLIST *dirs;
    MHASH *mfiles;
    MLIST *artists;

    uint32_t count_album;
    uint32_t count_track;
    uint32_t count_touched;
    uint32_t pos;               /* 当前播放曲目 */
} DommeStore;

typedef struct {
    char *id;                   /* 当前正在播放的曲目 */

    mp3dec_t mp3d;
    mp3dec_map_info_t file;     /* buffer, size */
    mp3dec_file_info_t info;    /* samples, channels, hz, layer, bitrate_kbps */
    uint64_t samples_eat;
    float percent;              /* 当前播放进度，或拖拽百分比 */
    size_t offset;

    mp3d_sample_t buffer[MINIMP3_MAX_SAMPLES_PER_FRAME];

    uint32_t length;            /* track length in seconds */
    uint32_t position;          /* playing position (in seconds) */
} AudioTrack;

/*
 * 总的来说，用户动作有
 * 1. 设置播放范围
 * 2. 设置后续动作（顺序播放、随机播放、循环模式、几首后关闭）
 * 3. 播放、暂停、上一首、下一首
 * 4. 拖动播放
 */
typedef struct {
    BeeEntry base;

    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;

    pthread_t indexer;

    snd_pcm_t *pcm;
    snd_mixer_elem_t *mixer;
    bool pcm_param_seted;

    MLIST *plans;               /* 所有媒体库列表 */
    DommeStore *plan;           /* 当前使用的媒体库 */

    char *trackid;              /* 设置播放曲目 */
    char *album;                /* 设置播放专辑 */
    char *artist;               /* 设置播放艺术家 */

    PLAY_ACTION act;

    bool shuffle;
    bool loopon;
    int remain;
    uint32_t dragto;

    MLIST *playlist;            /* 播放列表 */

    AudioTrack *track;
} AudioEntry;

#include "_mp3.c"
#include "_audio_init.c"
#include "_audio_indexer.c"

static char* _action_string(PLAY_ACTION act)
{
    switch (act) {
    case ACT_NONE: return "无操作";
    case ACT_PLAY: return "播放";
    case ACT_PAUSE: return "暂停";
    case ACT_RESUME: return "恢复";
    case ACT_NEXT: return "下一首";
    case ACT_PREV: return "上一首";
    case ACT_DRAG: return "拖动";
    case ACT_STOP: return "停止播放";
    default: return "瞎搞";
    }
}

uint32_t albumFreeReset(DommeAlbum *disk)
{
    if (!disk) return 0;

    DommeFile *mfile;
    MLIST_ITERATE(disk->tracks, mfile) {
        mfile->touched = false;
    }
    disk->pos = 0;

    return mlist_length(disk->tracks);
}

uint32_t artistFreeReset(DommeArtist *artist)
{
    if (!artist) return 0;

    DommeAlbum *disk;
    MLIST_ITERATE(artist->albums, disk) {
        albumFreeReset(disk);
    }
    artist->pos = 0;

    return artist->count_track;
}

uint32_t dommeFreeReset(DommeStore *plan)
{
    if (!plan) return 0;

    DommeArtist *artist;
    MLIST_ITERATE(plan->artists, artist) {
        artistFreeReset(artist);
    }
    plan->count_touched = 0;
    plan->pos = 0;

    return plan->count_track;
}

DommeFile* dommeGetFile(DommeStore *plan, char *id)
{
    if (!plan || !id) return NULL;

    return mhash_lookup(plan->mfiles, (void*)id);
}

DommeFile* dommeGetPos(DommeStore *plan, uint32_t pos)
{
    if (!plan) return NULL;

    char *key;
    DommeFile *mfile;
    MHASH_ITERATE(plan->mfiles, key, mfile) {
        if (pos == 0) return mfile;
        else pos--;
    }

    return NULL;
}

uint32_t albumFreeCount(DommeAlbum *disk)
{
    if (!disk) return 0;

    uint32_t freecount = 0;

    DommeFile *mfile;
    MLIST_ITERATE(disk->tracks, mfile) {
        if (!mfile->touched) freecount++;
    }

    return freecount;
}

uint32_t artistFreeCount(DommeArtist *artist)
{
    if (!artist) return 0;

    uint32_t freecount = 0;

    DommeAlbum *disk;
    MLIST_ITERATE(artist->albums, disk) {
        freecount += albumFreeCount(disk);
    }

    return freecount;
}

DommeStore* dommeStoreDefault(MLIST *plans)
{
    if (!plans) return NULL;

    DommeStore *plan;
    MLIST_ITERATE(plans, plan) {
        if (plan->moren) return plan;
    }

    return plan;
}

DommeStore* dommeStoreFind(MLIST *plans, char *name)
{
    if (!plans || !name) return NULL;

    DommeStore *plan;
    MLIST_ITERATE(plans, plan) {
        if (!strcmp(plan->name, name)) return plan;
    }

    return NULL;
}

/* 用户指明了播放范围 */
char* _told_todo(AudioEntry *me)
{
    if (!me || !me->plan) return NULL;

    if (me->trackid) return me->trackid;
    else if (me->album && me->artist) {
        /* 指明专辑 */
        DommeArtist *artist = artistFind(me->plan->artists, me->artist);
        DommeAlbum *disk = albumFind(artist->albums, me->album);
        if (disk) {
            disk->pos = 0;
            if (me->shuffle) disk->pos = mos_rand(mlist_length(disk->tracks));

            DommeFile *mfile = mlist_getx(disk->tracks, disk->pos);
            if (mfile) {
                mfile->touched = true;
                return mfile->id;
            }
        }

        mtc_mt_warn("no track to play %s %s", me->artist, me->album);
        me->album = me->artist = NULL;
        return NULL;
    } else if (me->artist) {
        /* 指明艺术家 */
        DommeArtist *artist = artistFind(me->plan->artists, me->artist);
        if (artist) {
            artist->pos = 0;
            if (me->shuffle) artist->pos = mos_rand(mlist_length(artist->albums));

            DommeAlbum *disk = mlist_getx(artist->albums, artist->pos);
            if (disk) {
                disk->pos = me->shuffle ? mos_rand(mlist_length(disk->tracks)) : 0;
                DommeFile *mfile = mlist_getx(disk->tracks, disk->pos);
                if (mfile) {
                    mfile->touched = true;
                    return mfile->id;
                }
            }
        }

        mtc_mt_warn("no track to play %s", me->artist);
        me->artist = NULL;
        return NULL;
    } else {
        /* 指明媒体库 */
        dommeFreeReset(me->plan);
        me->plan->pos = 0;
        if (me->shuffle) me->plan->pos = mos_rand(me->plan->count_track);

        DommeFile *mfile = dommeGetPos(me->plan, me->plan->pos);
        if (mfile) {
            mfile->touched = true;
            me->plan->count_touched = 1;
            return mfile->id;
        }

        mtc_mt_warn("no track to play %s", me->plan->name);
        return NULL;
    }
}

/* 当前范围内轮到下一首 */
char* _next_todo(AudioEntry *me)
{
    if (!me || !me->plan) return NULL;

    DommeFile *mfile;
    DommeAlbum *disk;
    DommeArtist *artist;
    DommeStore *plan = me->plan;

    if (me->trackid) return me->trackid;
    else if (me->album && me->artist) {
        /* 专辑内下一首 */
        artist = artistFind(plan->artists, me->artist);
        disk = albumFind(artist->albums, me->album);
        if (disk) {
            if (me->shuffle) {
                uint32_t freecount = albumFreeCount(disk);
                if (freecount == 0) {
                    if (me->loopon) freecount = albumFreeReset(disk);
                    else goto album_done;
                }
                uint32_t pos = mos_rand(freecount);
                MLIST_ITERATE(disk->tracks, mfile) {
                    if (!mfile->touched) {
                        if (pos == 0) {
                            mfile->touched = true;
                            return mfile->id;
                        }
                        pos--;
                    }
                }
            } else {
                if (disk->pos >= mlist_length(disk->tracks) - 1) {
                    if (me->loopon) {
                        albumFreeReset(disk);
                        disk->pos = 0;
                        mfile = mlist_getx(disk->tracks, disk->pos);
                        if (mfile) {
                            mfile->touched = true;
                            return mfile->id;
                        }
                    } else goto album_done;
                } else {
                    disk->pos += 1;
                    mfile = mlist_getx(disk->tracks, disk->pos);
                    if (mfile) {
                        mfile->touched = true;
                        return mfile->id;
                    }
                }
            }
        }

    album_done:
        mtc_mt_warn("no track to play %s %s", me->artist, me->album);
        me->album = me->artist = NULL;
        return NULL;
    } else if (me->artist) {
        /* 艺术家内下一首 */
        artist = artistFind(plan->artists, me->artist);
        if (artist) {
            if (me->shuffle) {
                /* 随机播放艺术家 */
                uint32_t freecount = artistFreeCount(artist);
                if (freecount == 0) {
                    if (me->loopon) freecount = artistFreeReset(artist);
                    else goto artist_done;
                }
                uint32_t pos = mos_rand(freecount);
                MLIST_ITERATE(artist->albums, disk) {
                    MLIST_ITERATEB(disk->tracks, mfile) {
                        if (!mfile->touched) {
                            if (pos == 0) {
                                mfile->touched = true;
                                return mfile->id;
                            }
                            pos--;
                        }
                    }
                }
            } else {
                /* 顺序播放艺术家 */
                disk = mlist_getx(artist->albums, artist->pos);
                if (disk) {
                    if (disk->pos >= mlist_length(disk->tracks) -1) {
                        if (artist->pos >= mlist_length(artist->albums) - 1) {
                            /* 弹尽粮绝 */
                            if (me->loopon) {
                                artistFreeReset(artist);
                                //artist->pos = 0; TODO
                                disk = mlist_getx(artist->albums, artist->pos);
                                mfile = mlist_getx(disk->tracks, disk->pos);
                                if (mfile) {
                                    mfile->touched = true;
                                    return mfile->id;
                                }
                            } else goto artist_done;
                        } else {
                            /* 播放下一张专辑 */
                            artist->pos++;
                            disk = mlist_getx(artist->albums, artist->pos);
                            disk->pos = 0;
                            mfile = mlist_getx(disk->tracks, disk->pos);
                            if (mfile) {
                                mfile->touched = true;
                                return mfile->id;
                            }
                        }
                    } else {
                        /* 播放下一首曲目 */
                        disk->pos++;
                        mfile = mlist_getx(disk->tracks, disk->pos);
                        if (mfile) {
                            mfile->touched = true;
                            return mfile->id;
                        }
                    }
                }
            }
        }

    artist_done:
        mtc_mt_warn("no track to play %s", me->artist);
        me->artist = NULL;
        return NULL;
    } else {
        /* 媒体库内下一首 */
        if (plan->count_touched >= plan->count_track) {
            if (me->loopon) dommeFreeReset(plan);
            else return NULL;
        }

        if (me->shuffle) {
            char *key;
            int32_t pos = mos_rand(plan->count_track - plan->count_touched);
            MHASH_ITERATE(plan->mfiles, key, mfile) {
                if (!mfile->touched) {
                    if (pos == 0) {
                        mfile->touched = true;
                        plan->count_touched++;
                        return mfile->id;
                    }
                    pos--;
                }
            }
        } else {
            /* 顺序播放 */
            plan->pos++;
            mfile = dommeGetPos(plan, plan->pos);
            if (mfile) {
                mfile->touched = true;
                plan->count_touched++;
                return mfile->id;
            }
        }

        mtc_mt_warn("no track to play %s", plan->name);
        return NULL;
    }
}

static int _iterate_callback(void *user_data, const uint8_t *frame, int frame_size,
                             int free_format_bytes, size_t buf_size, uint64_t offset,
                             mp3dec_frame_info_t *info)
{
    static bool playing = false;
    AudioEntry *me = (AudioEntry*)user_data;
    AudioTrack *track = me->track;

    if (playing && me->act != ACT_NONE) {
        mtc_mt_dbg("%s while playing", _action_string(me->act));
        playing = false;
        return 1;
    }

    if (!playing) playing = true;

    int rv;

    if (!frame) {
        /* 播放正常完成 */
        mtc_mt_dbg("play done");
        track->percent = 0;
        playing = false;
        return 1;
    }

    if (!me->pcm_param_seted) {
        if ((rv = snd_pcm_set_params(me->pcm,
                                     SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                     info->channels, info->hz, 1, 100000)) < 0) {
            mtc_mt_err("can't set parameter. %s", snd_strerror(rv));
            track->percent = 0;
            playing = false;
            return 1;
        }
        me->pcm_param_seted = true;
    }

    memset(track->buffer, 0x0, sizeof(track->buffer));
    int samples = mp3dec_decode_frame(&track->mp3d, frame, frame_size, track->buffer, info);
    if (samples > 0) {
        rv = snd_pcm_writei(me->pcm, track->buffer, samples);
        if (rv == -EPIPE) {
            mtc_mt_warn("XRUN");
            snd_pcm_prepare(me->pcm);
        }

        track->samples_eat += rv;
        track->percent = (float)track->samples_eat / track->info.samples;
        track->position = track->length * track->percent;

        return 0;
    } else {
        /* never reach here */
        mtc_mt_dbg("play done");
        track->percent = 0;
        playing = false;
        return 1;
    }
}

static bool _play_raw(AudioEntry *me, char *filename)
{
    if (!me || !me->pcm || !filename || !me->track) return false;

    AudioTrack *track = me->track;

    mp3dec_close_file(&track->file);
    mp3dec_init(&track->mp3d);
    memset(&track->info, 0x0, sizeof(mp3dec_file_info_t));
    track->length = 0;
    track->position = 0;

    mtc_mt_dbg("playing %s", filename);

    int ret = mp3dec_open_file(filename, &track->file);
    if (ret != 0) {
        mtc_mt_err("open %s failure", filename);
        return false;
    }

    ret = mp3dec_load_buf(&track->mp3d, track->file.buffer, track->file.size,
                          &track->info, NULL, NULL);
    if (ret != 0) {
        mtc_mt_err("load buffer info %s failure", filename);
        return false;
    }

    track->length = (uint32_t)track->info.samples / track->info.hz;

    if (track->percent != 0) {
        track->offset = track->file.size * track->percent;
        track->samples_eat = track->info.samples * track->percent;
    } else {
        track->offset = 0;
        track->samples_eat = 0;
    }

    mtc_mt_dbg("file info: layer %d, %zu samples, %d HZ, %d kbps, %u length",
               track->info.layer, track->info.samples, track->info.hz,
               track->info.avg_bitrate_kbps, track->length);

    me->act = ACT_NONE;

    if (mp3dec_iterate_buf(track->file.buffer + track->offset,
                           track->file.size - track->offset, _iterate_callback, me) != 0) {
        mtc_mt_err("play buffer error failure");
        return false;
    }

    return true;
}

static bool _play(AudioEntry *me)
{
    if (!me || !me->pcm || !me->plan || !me->track || !me->track->id) return false;

    AudioTrack *track = me->track;
    DommeStore *plan = me->plan;

    DommeFile *mfile = dommeGetFile(plan, track->id);
    if (!mfile) {
        mtc_mt_warn("domme get %s failure", track->id);
        return false;
    }

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s%s%s", plan->basedir, mfile->dir, mfile->name);

    return _play_raw(me, filename);
}

static void* _player(void *arg)
{
    AudioEntry *me = (AudioEntry*)arg;
    AudioTrack *track = me->track;
    int rv;

    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.worker", "debug"));
    mtc_mt_initf("player", loglevel, g_log_tostdout ? "-"  :"%slog/%s.log", g_location, "player");

    mtc_mt_dbg("I am audio player");

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%sconnect.mp3", g_location);
    _play_raw(me, filename);

    while (me->running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;

        pthread_mutex_lock(&me->lock);
        while (me->act == ACT_NONE &&
               (rv = pthread_cond_timedwait(&me->cond, &me->lock, &timeout)) == ETIMEDOUT) {
            timeout.tv_sec += 1;
            if (!me->running) break;
        }

        if (!me->running) {
            pthread_mutex_unlock(&me->lock);
            break;
        }

        if (me->act == ACT_NONE && rv != 0) {
            mtc_mt_err("timedwait error %s", strerror(errno));
            pthread_mutex_unlock(&me->lock);
            continue;
        }

        /* rv == 0 */
        pthread_mutex_unlock(&me->lock);

        /*
         * 对于正在播放中的曲目，在 me->act 不为 ACT_NONE 时，会自动退出播放，线程执行才能到达此地。
         * 对于播放完成的曲目，会根据播放范围和循环状态，继续播放下一首，或者等待播放信号
         */
        switch (me->act) {
        case ACT_PLAY:
            /* 播放可以是指定曲目播放、专辑内播放、艺术家内播放、媒体库内播放 */
            track->id = _told_todo(me);
            if (track->id) {
                _play(me);
                mlist_append(me->playlist, strdup(track->id));
            } else mtc_mt_warn("nothing to play");

            if (!me->loopon) me->trackid = NULL;

            break;
        case ACT_NEXT:
            /* 自动切换下一首 */
            break;
        case ACT_PREV:
            track->id = (char*)mlist_getx(me->playlist, -1);
            if (track->id) _play(me);
            else mtc_mt_warn("nothing to play");

            break;
        case ACT_DRAG:
            if (track->id) {
                track->percent = (float)me->dragto / track->length;
                _play(me);
            } else mtc_mt_warn("nothing to drag");

            break;
        case ACT_PAUSE:
            if (track->id) {
                me->act = ACT_NONE;
                continue;
            } else mtc_mt_warn("nothing to pause");

            break;
        case ACT_RESUME:
            if (track->id) {
                _play(me);
            } else mtc_mt_warn("nothing to resume");
            break;
        case ACT_STOP:
            track->id = NULL;
            me->trackid = NULL;
            me->album = NULL;
            me->artist = NULL;
            me->act = ACT_NONE;
            continue;           /* 静待下一次动作信号 */
        default:
            break;
        }
        me->act = ACT_NONE;

        while (me->act == ACT_NONE && (track->id = _next_todo(me)) != NULL) {
            _play(me);
            mlist_append(me->playlist, strdup(track->id));
        }
    }

    return NULL;
}

/*
 * 可接受的指令：
 * 1. 切换媒体库, 重读媒体库索引
 * 2. 设置播放内容（曲目、专辑、艺术家、媒体库）
 * 3. 设置下一首模式（顺序、随机）
 * 4. 设置循环方式（不循环、循环）
 * 5. 设置几首后关闭（0 播放当前后关闭，-1永不关闭）
 *
 * 6. 获取音量
 * 7. 获取当前播放曲目信息（ID、时长、当前播放位置）
 * 8. 上一首
 * 9. 下一首
 * 10. 暂停播放
 * 11. 继续播放
 *
 * 12. 设置音量
 * 13. 设置播放位置
 */
bool audio_process(BeeEntry *be, QueueEntry *qe)
{
    AudioEntry *me = (AudioEntry*)be;
    AudioTrack *track = me->track;

    mtc_mt_dbg("process command %d", qe->command);

    switch (qe->command) {
    case CMD_STORE_SWITCH:
        me->act = ACT_STOP;
        /* TODO wait _play() ? */
        char *name = mdf_get_value(qe->nodein, "name", NULL);
        if (name) me->plan = dommeStoreFind(me->plans, name);
        break;
    case CMD_WHERE_AM_I:
        if (track->id) {
            mdf_set_value(qe->nodeout, "trackid", track->id);
            mdf_set_int_value(qe->nodeout ,"length", track->length);
            mdf_set_int_value(qe->nodeout, "progress", track->position);
        } else mdf_set_int_value(qe->nodeout, "progress", 0);

        MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetDataFill(packet, FRAME_RESPONSE, qe->command, qe->nodeout);
        packetCRCFill(packet);

        packet->seqnum = qe->seqnum;

        SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);

        break;
    default:
        break;
    }

    return true;
}

void audio_stop(BeeEntry *be)
{
    AudioEntry *me = (AudioEntry*)be;

    mtc_mt_dbg("stop worker %s", be->name);

    me->running = false;
    pthread_cancel(me->worker);
    pthread_join(me->worker, NULL);

    pthread_cancel(me->indexer);
    pthread_join(me->indexer, NULL);

    mos_free(me->track);
    mlist_destroy(&me->plans);
    mlist_destroy(&me->playlist);
}

BeeEntry* _start_audio()
{
    MERR *err;

    AudioEntry *me = mos_calloc(1, sizeof(AudioEntry));
    me->base.process = audio_process;
    me->base.stop = audio_stop;

    mlist_init(&me->plans, dommeStoreFree);

    err = dommeStoresLoad(me->plans);
    RETURN_V_NOK(err, NULL);

    me->trackid = NULL;
    me->album = NULL;
    me->artist = NULL;
    me->plan = dommeStoreDefault(me->plans);

    me->shuffle = false;
    me->loopon = false;
    me->remain = -1;
    me->dragto = 0;
    mlist_init(&me->playlist, free);

    me->track = mos_calloc(1, sizeof(AudioTrack));
    me->track->id = NULL;
    mp3dec_init(&me->track->mp3d);
    memset(&me->track->file, 0x0, sizeof(mp3dec_map_info_t));
    memset(&me->track->info, 0x0, sizeof(mp3dec_file_info_t));
    me->track->samples_eat = 0;
    me->track->percent = 0;
    me->track->offset = 0;
    me->track->length = 0;
    me->track->position = 0;

    int rv = snd_pcm_open(&me->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rv < 0) {
        mtc_mt_err("Can't open default PCM device. %s", snd_strerror(rv));
        return NULL;
    }

    snd_mixer_t *mixer_handle;
    snd_mixer_open(&mixer_handle, 0);
    snd_mixer_attach(mixer_handle, "default");
    snd_mixer_selem_register(mixer_handle, NULL, NULL);
    snd_mixer_load(mixer_handle);

    //snd_mixer_selem_id_t *sid;
    //snd_mixer_selem_id_alloca(&sid);
    //snd_mixer_selem_id_set_index(sid, 0);
    //snd_mixer_selem_id_set_name(sid, "Master");
    //me->mixer = snd_mixer_find_selem(mixer_handle, sid);
    me->mixer = snd_mixer_first_elem(mixer_handle);
    if (!me->mixer) {
        mtc_mt_err("Can't find mixer.");
        return NULL;
    }

    me->running = true;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&me->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&me->cond, NULL);

    pthread_create(&me->worker, NULL, _player, me);

    pthread_create(&me->indexer, NULL, dommeIndexerStart, me);

    return (BeeEntry*)me;
}

BeeDriver audio_driver = {
    .id = FRAME_AUDIO,
    .name = "audio",
    .init_driver = _start_audio
};
