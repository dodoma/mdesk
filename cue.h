#ifndef __CUE_H__
#define __CUE_H__

typedef struct {
    char md5[33];
    char title[256];

    int index0;            /* 曲目开始前留白的位置（精确到毫秒） */
    int index1;            /* 曲目开始位置（精确到毫秒） */
    int sn;                /* 曲目编号 */
} CueTrack;

typedef struct {
    char md5[33];
    char artist[256];
    char album[256];
    char date[64];
    char genre[128];
    char filename[1024];        /* FILE content in cue sheet */
    char fullname[PATH_MAX];    /* filename with dirname */
    int length;            /* 整张专辑的长度（精确到秒） */

    MLIST *tracks;

    uint8_t *_trackbuf;    /* 用来生成曲目MD5（内部使用） */
    size_t _buflen;
} CueSheet;

CueSheet* cueOpen(const char *filename);
void cueFree(CueSheet *centry);
void cueDump(CueSheet *centry);

#endif  /* __CUE_H__ */
