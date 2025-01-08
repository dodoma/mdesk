#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ALLOW_MONO_STEREO_TRANSITION
#include "minimp3_ex.h"
#include "mp3.h"

#include "_mp3.c"

typedef struct {
    MediaNode base;
    mp3dec_t mp3d;
    mp3dec_map_info_t file;     /* buffer, size */
} MediaNodeMp3;

typedef struct {
    MediaEntry base;
    mp3d_sample_t psamples[MINIMP3_MAX_SAMPLES_PER_FRAME];
} MediaEntryMp3;

struct bird_egg {
    AudioEntry *audio;
    MediaNodeMp3 *mnode;
    MediaEntryMp3 *mentry;
};

static int _iterate_info(void *user_data, const uint8_t *frame, int frame_size,
                         int free_format_bytes, size_t buf_size, uint64_t offset,
                         mp3dec_frame_info_t *info)
{
    if (!frame) return 1;

    TechInfo *outinfo = (TechInfo*)user_data;
    //d->samples += mp3dec_decode_frame(d->mp3d, frame, frame_size, NULL, info);
    outinfo->samples += hdr_frame_samples(frame);
    outinfo->channels = info->channels;
    outinfo->hz = info->hz;

    if (outinfo->kbps < info->bitrate_kbps) outinfo->kbps = info->bitrate_kbps;

    return 0;
}

static int _iterate_callback(void *user_data, const uint8_t *frame, int frame_size,
                             int free_format_bytes, size_t buf_size, uint64_t offset,
                             mp3dec_frame_info_t *info)
{
    static int channels = 0, hz = 0;
    int rv;

    struct bird_egg *egg = (struct bird_egg*)user_data;
    AudioEntry *me = egg->audio;
    MediaNodeMp3 *mp3node = egg->mnode;
    MediaEntryMp3 *mp3entry = egg->mentry;
    struct audioTrack *track = me->track;

    if (track->playing && me->act != ACT_NONE) {
        mtc_mt_dbg("%s while playing", _action_string(me->act));
        track->playing = false;
        return 1;
    }

    if (!track->playing) {
        if (track->media_switch || info->channels != channels || info->hz != hz) {
            /* 设置播放参数 */
            mtc_mt_dbg("set pcm params %d %dHZ", info->channels, info->hz);

            switch (snd_pcm_state(me->pcm)) {
            case SND_PCM_STATE_RUNNING:
                snd_pcm_drop(me->pcm);
                break;
            case SND_PCM_STATE_XRUN:
                snd_pcm_prepare(me->pcm);
                break;
            default:
                break;
            }
            rv = snd_pcm_set_params(me->pcm,
                                    SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                    info->channels, info->hz, 1, 100000);
            if (rv < 0) {
                mtc_mt_err("can't set parameter. %s", snd_strerror(rv));
                track->percent = 0;
                track->playing = false;
                return 1;
            }
            channels = info->channels;
            hz = info->hz;
        }

        track->playing = true;
    }

    if (!frame) {
        /* 播放正常完成 */
        mtc_mt_dbg("play done");
        track->percent = 0;
        track->playing = false;
        return 1;
    }

    memset(mp3entry->psamples, 0x0, sizeof(mp3entry->psamples));
    int samples = mp3dec_decode_frame(&mp3node->mp3d, frame, frame_size, mp3entry->psamples, info);
    if (samples > 0) {
        rv = snd_pcm_writei(me->pcm, mp3entry->psamples, samples);
        if (rv == -EPIPE) {
            mtc_mt_warn("XRUN");
            snd_pcm_prepare(me->pcm);
        }

        track->samples_eat += rv;
        track->percent = (float)track->samples_eat / track->tinfo.samples;
    } else mtc_mt_warn("decode frame failure");

    return 0;
}

static bool _mp3_verify(const char *filename)
{
    mp3dec_map_info_t map_info;
    if (!filename) return false;

    if (mp3dec_open_file(filename, &map_info) == 0) {
        if (mp3dec_detect_buf(map_info.buffer, map_info.size) == 0) {
            mp3dec_close_file(&map_info);
            return true;
        }

        mp3dec_close_file(&map_info);
    }

    return false;
}

static MediaNode* _mp3_open(const char *filename)
{
    if (!filename) return NULL;


    MediaNodeMp3 *mnode = mos_calloc(1, sizeof(MediaNodeMp3));
    memset(mnode, 0x0, sizeof(MediaNodeMp3));
    mp3dec_init(&mnode->mp3d);
    mnode->file.buffer = NULL;
    mnode->file.size = 0;

    if (mp3dec_open_file(filename, &mnode->file) == 0) {
        if (mp3dec_detect_buf(mnode->file.buffer, mnode->file.size) == 0) {
            mp3_md5_buf(mnode->file.buffer, mnode->file.size, mnode->base.md5);
            return (MediaNode*)mnode;
        }

        mp3dec_close_file(&mnode->file);
    }

    return NULL;
}

static TechInfo* _mp3_get_tinfo(MediaNode *mnode)
{
    if (!mnode) return NULL;

    MediaNodeMp3 *mp3node = (MediaNodeMp3*)mnode;

    mp3dec_iterate_buf(mp3node->file.buffer, mp3node->file.size, _iterate_info, &mnode->tinfo);
    mnode->tinfo.length = (int)(mnode->tinfo.samples / mnode->tinfo.hz) + 1;
    mnode->ainfo.length = mnode->tinfo.length;

    return &mnode->tinfo;
}

static ArtInfo* _mp3_get_ainfo(MediaNode *mnode)
{
    if (!mnode) return NULL;

    MediaNodeMp3 *mp3node = (MediaNodeMp3*)mnode;

    if (mp3_id3_get_buf(mp3node->file.buffer, mp3node->file.size,
                        mnode->ainfo.title, mnode->ainfo.artist, mnode->ainfo.album,
                        mnode->ainfo.year, mnode->ainfo.track) != true) {
        return NULL;
    }

    return &mnode->ainfo;
}

static uint8_t* _mp3_get_cover(MediaNode *mnode, size_t *imagelen)
{
    if (!mnode) return NULL;

    MediaNodeMp3 *mp3node = (MediaNodeMp3*)mnode;
    uint8_t *imagebuf = NULL;

    if (_seek_cover(mp3node->file.buffer, mp3node->file.size, NULL, &imagebuf, imagelen))
        return imagebuf;
    else return NULL;
}

static bool _mp3_play(MediaNode *mnode, AudioEntry *audio)
{
    MediaNodeMp3 *mp3node = (MediaNodeMp3*)mnode;
    if (!mp3node || mp3node->file.buffer == NULL) return false;

    MediaEntryMp3 *mp3entry = (MediaEntryMp3*)mp3node->base.driver;

    if (!audio) return false;
    struct audioTrack *track = audio->track;

    size_t offset = track->percent == 0.0 ? 0 : mp3node->file.size * track->percent;

    struct bird_egg egg = {.audio = audio, .mnode = mp3node, .mentry = mp3entry};
    if (mp3dec_iterate_buf(mp3node->file.buffer + offset,
                           mp3node->file.size - offset, _iterate_callback, &egg) != 0) {
        mtc_mt_err("play buffer error failure");
        return false;
    }

    return true;
}

static void _mp3_close(MediaNode *mnode)
{
    if (!mnode) return;

    MediaNodeMp3 *mp3node = (MediaNodeMp3*)mnode;

    mp3dec_close_file(&mp3node->file);
    mos_free(mp3node);
}

MediaEntryMp3 media_mp3 = {
    .base = {
        .type = MEDIA_MP3,
        .name = "MP3",
        .check = _mp3_verify,
        .open          = _mp3_open,
        .tech_info_get = _mp3_get_tinfo,
        .art_info_get  = _mp3_get_ainfo,
        .cover_get     = _mp3_get_cover,
        .play          = _mp3_play,
        .close         = _mp3_close
    },

    .psamples = {0}
};
