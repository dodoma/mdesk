#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define WAV_DECODE_SAMPLE 1024
#define WAV_DECODE_BUFLEN 32768 /* 8 channels, 1024 samples, 4 bytes/sample */

typedef struct {
    MediaNode base;
    drwav wav;
    uint8_t *imagebuf;
    size_t imagelen;
} MediaNodeWav;

typedef struct {
    MediaEntry base;
    drwav_int32 psamples[WAV_DECODE_BUFLEN];
} MediaEntryWav;

static bool _wav_verify(const char *filename)
{
    if (!filename) return false;

    drwav wav;
    bool ret = drwav_init_file(&wav, filename, NULL) ? true : false;
    drwav_uninit(&wav);

    return ret;
}

static MediaNode* _wav_open(const char *filename)
{
    if (!filename) return NULL;

    MediaNodeWav *mnode = mos_calloc(1, sizeof(MediaNodeWav));
    memset(mnode, 0x0, sizeof(MediaNodeWav));
    mnode->imagebuf = NULL;
    mnode->imagelen = 0;

    if (drwav_init_file(&mnode->wav, filename, NULL)) {
        uint8_t bps = mnode->wav.bitsPerSample;
        mnode->base.tinfo.samples = mnode->wav.totalPCMFrameCount;
        mnode->base.tinfo.channels = mnode->wav.channels;
        mnode->base.tinfo.hz = mnode->wav.sampleRate;
        mnode->base.tinfo.kbps = bps * mnode->wav.channels * mnode->wav.sampleRate / 1000;
        mnode->base.tinfo.length = (int)(mnode->wav.totalPCMFrameCount / mnode->wav.sampleRate) + 1;
        mnode->base.ainfo.length = mnode->base.tinfo.length;

        return (MediaNode*)mnode;
    } else {
        mos_free(mnode);
        return NULL;
    }
}

static TechInfo* _wav_get_tinfo(MediaNode *mnode)
{
    if (!mnode) return NULL;

    return &mnode->tinfo;
}

static ArtInfo* _wav_get_ainfo(MediaNode *mnode)
{
    /* TODO wav art info */
    return NULL;
}

static uint8_t* _wav_get_cover(MediaNode *mnode, size_t *imagelen)
{
    MediaNodeWav *wavnode = (MediaNodeWav*)mnode;

    if (!wavnode) return NULL;

    if (!wavnode->imagebuf) {
        /* TODO wav 文件里没有封面，尝试在当前目录及 Cover、Covers、Artwork 目录下读取 */
        char *dumpname = strdup(mnode->filename);
        char *pathname = dirname(dumpname);
        int dirlen = strlen(pathname);
        if (pathname[dirlen-1] != '/') {
            pathname[dirlen] = '/';
            dirlen++;
            pathname[dirlen] = '\0';
        }

        wavnode->imagebuf = _scan_cover(pathname, &wavnode->imagelen);

        free(dumpname);
    }

    if (imagelen) *imagelen = wavnode->imagelen;

    return wavnode->imagebuf;
}

static bool _wav_play(MediaNode *mnode, AudioEntry *audio)
{
    static int channels = 0, hz = 0;
    int rv;

    MediaNodeWav *wavnode = (MediaNodeWav*)mnode;
    if (!wavnode) return false;

    MediaEntryWav *waventry = (MediaEntryWav*)wavnode->base.driver;

    if (!audio) return false;
    struct audioTrack *track = audio->track;

    if (track->percent != 0) {
        drwav_uint64 index = track->tinfo.samples * track->percent;
        drwav_seek_to_pcm_frame(&wavnode->wav, index);
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
    while ((samples = drwav_read_pcm_frames_s32(&wavnode->wav, WAV_DECODE_SAMPLE,
                                                waventry->psamples)) > 0) {
        if (audio->act != ACT_NONE) {
            mtc_mt_dbg("%s while playing", _action_string(audio->act));
            track->playing = false;
            return false;
        }

        rv = snd_pcm_writei(audio->pcm, waventry->psamples, samples);
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

static void _wav_close(MediaNode *mnode)
{
    if (!mnode) return;

    MediaNodeWav *wavnode = (MediaNodeWav*)mnode;

    mos_free(wavnode->imagebuf);
    drwav_uninit(&wavnode->wav);
    mos_free(wavnode);
}

MediaEntryWav media_wav = {
    .base = {
        .type = MEDIA_WAV,
        .name = "WAV",
        .check = _wav_verify,
        .open          = _wav_open,
        .tech_info_get = _wav_get_tinfo,
        .art_info_get  = _wav_get_ainfo,
        .cover_get     = _wav_get_cover,
        .play          = _wav_play,
        .close         = _wav_close,
    },

    .psamples = {0}
};
