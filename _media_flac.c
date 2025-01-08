#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define FLAC_DECODE_SAMPLE 1024
#define FLAC_DECODE_BUFLEN 32768 /* 8 channels, 1024 samples, 4 bytes/sample */

typedef struct {
    MediaNode base;
    drflac *pflac;
    uint8_t *imagebuf;
    size_t imagelen;
} MediaNodeFlac;

typedef struct {
    MediaEntry base;
    drflac_int32 psamples[FLAC_DECODE_BUFLEN];
} MediaEntryFlac;

static void _on_meta(void *data, drflac_metadata *meta)
{
    MediaNodeFlac *mnode = (MediaNodeFlac*)data;

    if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO) {
        mstr_bin2hexstr(meta->data.streaminfo.md5, 16, mnode->base.md5);
        mstr_tolower(mnode->base.md5);
    } else if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE &&
               meta->data.picture.pictureDataSize > 0) {
        mnode->imagebuf = mos_calloc(1, meta->data.picture.pictureDataSize);
        mnode->imagelen = meta->data.picture.pictureDataSize;
        memcpy(mnode->imagebuf, meta->data.picture.pPictureData, mnode->imagelen);
    } else if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        const char *comment = NULL;
        drflac_uint32 comment_len = 0;
        drflac_vorbis_comment_iterator piter;
        drflac_init_vorbis_comment_iterator(&piter,
                                            meta->data.vorbis_comment.commentCount,
                                            meta->data.vorbis_comment.pComments);

        while ((comment = drflac_next_vorbis_comment(&piter, &comment_len)) != NULL) {
            if (comment_len > LEN_MEDIA_TOKEN) comment_len = LEN_MEDIA_TOKEN;

            if (!strncasecmp(comment, "TITLE", 5)) {
                comment += 6;
                memcpy(mnode->base.ainfo.title, comment, comment_len - 6);
            } else if (!strncasecmp(comment, "ARTIST", 6)) {
                comment += 7;
                memcpy(mnode->base.ainfo.artist, comment, comment_len - 7);
            } else if (!strncasecmp(comment, "ALBUM", 5)) {
                comment += 6;
                memcpy(mnode->base.ainfo.album, comment, comment_len - 6);
            } else if (!strncasecmp(comment, "TRACKNUMBER", 11)) {
                comment += 12;
                memcpy(mnode->base.ainfo.track, comment, comment_len - 12);
            } else if (!strncasecmp(comment, "DATE", 4)) {
                comment += 5;
                memcpy(mnode->base.ainfo.year, comment, comment_len - 5);
            }
        }
    }
}

static bool _flac_verify(const char *filename)
{
    if (!filename) return false;

    drflac *pflac = drflac_open_file(filename, NULL);
    if (pflac) {
        drflac_close(pflac);
        return true;
    } else return false;
}

static MediaNode* _flac_open(const char *filename)
{
    if (!filename) return NULL;

    MediaNodeFlac *mnode = mos_calloc(1, sizeof(MediaNodeFlac));
    memset(mnode, 0x0, sizeof(MediaNodeFlac));
    mnode->pflac = NULL;
    mnode->imagebuf = NULL;
    mnode->imagelen = 0;

    drflac *pflac = drflac_open_file_with_metadata(filename, _on_meta, mnode, NULL);
    if (pflac) {
        mnode->pflac = pflac;

        uint8_t bps = pflac->bitsPerSample;
        mnode->base.tinfo.samples = pflac->totalPCMFrameCount;
        mnode->base.tinfo.channels = pflac->channels;
        mnode->base.tinfo.hz = pflac->sampleRate;
        mnode->base.tinfo.kbps = bps * pflac->channels * pflac->sampleRate / 1000;
        mnode->base.tinfo.length = (int)(pflac->totalPCMFrameCount / pflac->sampleRate) + 1;
        mnode->base.ainfo.length = mnode->base.tinfo.length;

        return (MediaNode*)mnode;
    } else {
        mos_free(mnode);
        return NULL;
    }
}

static TechInfo* _flac_get_tinfo(MediaNode *mnode)
{
    if (!mnode) return NULL;

    return &mnode->tinfo;
}

static ArtInfo* _flac_get_ainfo(MediaNode *mnode)
{
    if (!mnode) return NULL;

    if (mnode->ainfo.title[0] == 0 ||
        mnode->ainfo.artist[0] == 0 ||
        mnode->ainfo.album[0] == 0) return NULL;
    else return &mnode->ainfo;
}

static uint8_t* _flac_get_cover(MediaNode *mnode, size_t *imagelen)
{
    MediaNodeFlac *flacnode = (MediaNodeFlac*)mnode;

    if (!flacnode) return NULL;

    if (imagelen) *imagelen = flacnode->imagelen;

    return flacnode->imagebuf;
}

static bool _flac_play(MediaNode *mnode, AudioEntry *audio)
{
    static int channels = 0, hz = 0;
    int rv;

    MediaNodeFlac *flacnode = (MediaNodeFlac*)mnode;
    if (!flacnode || !flacnode->pflac) return false;

    MediaEntryFlac *flacentry = (MediaEntryFlac*)flacnode->base.driver;

    if (!audio) return false;
    struct audioTrack *track = audio->track;

    if (track->percent != 0) {
        drflac_uint64 index = track->tinfo.samples * track->percent;
        drflac_seek_to_pcm_frame(flacnode->pflac, index);
    }

    if (track->media_switch || track->tinfo.channels != channels || track->tinfo.hz != hz) {
        /* 设置播放参数 */
        mtc_mt_dbg("set pcm params %d %dHZ", track->tinfo.channels, track->tinfo.hz);

        switch (snd_pcm_state(audio->pcm)) {
        case SND_PCM_STATE_RUNNING:
            snd_pcm_drop(audio->pcm);
            break;
        case SND_PCM_STATE_XRUN:
            snd_pcm_prepare(audio->pcm);
            break;
        default:
            break;
        }
        rv = snd_pcm_set_params(audio->pcm,
                                SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                track->tinfo.channels, track->tinfo.hz, 1, 100000);
        if (rv < 0) {
            mtc_mt_err("can't set parameter. %s", snd_strerror(rv));
            return false;
        }
        channels = track->tinfo.channels;
        hz = track->tinfo.hz;
    }

    track->playing = true;

    int samples = 0;
    while ((samples = drflac_read_pcm_frames_s32(flacnode->pflac, FLAC_DECODE_SAMPLE,
                                                 flacentry->psamples)) > 0) {
        if (audio->act != ACT_NONE) {
            mtc_mt_dbg("%s while playing", _action_string(audio->act));
            track->playing = false;
            return false;
        }

        rv = snd_pcm_writei(audio->pcm, flacentry->psamples, samples);
        if (rv == -EPIPE) {
            mtc_mt_warn("XRUN");
            snd_pcm_prepare(audio->pcm);
        }

        track->samples_eat += rv;
        track->percent = (float)track->samples_eat / track->tinfo.samples;
    }

    /* 播放正常完成 */
    track->percent = 0;
    track->playing = false;

    return true;
}

static void _flac_close(MediaNode *mnode)
{
    if (!mnode) return;

    MediaNodeFlac *flacnode = (MediaNodeFlac*)mnode;

    mos_free(flacnode->imagebuf);
    drflac_close(flacnode->pflac);
    mos_free(flacnode);
}

MediaEntryFlac media_flac = {
    .base = {
        .type = MEDIA_FLAC,
        .name = "FLAC",
        .check = _flac_verify,
        .open          = _flac_open,
        .tech_info_get = _flac_get_tinfo,
        .art_info_get  = _flac_get_ainfo,
        .cover_get     = _flac_get_cover,
        .play          = _flac_play,
        .close         = _flac_close
    },

    .psamples = {0}
};
