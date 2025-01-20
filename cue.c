#include <reef.h>
#include <libgen.h>
#include <iconv.h>
#include <uchardet/uchardet.h>

#include "net.h"
#include "bee.h"
#include "cue.h"

enum TOKEN_TYPE {
    TOK_REM = 0,
    TOK_TITLE,
    TOK_PERFORMER,
    TOK_FILE,
    TOK_TRACK,
    TOK_INDEX00,
    TOK_INDEX01
};

/*
 * 01 AUDIO ===> 01 AUDIO
 * "Telegraph Road" ===> Telegraph Road
 * "DS - Telegraph Road.flac" WAVE ===> DS - Telegraph Road.flac
 */
#define TAKEOFF_WRAP(buf, tail)                                         \
    do {                                                                \
        while (isspace(*buf) || *buf == '\"') buf++;                    \
        int _lena = strlen(buf);                                        \
        int _lenb = strlen(tail);                                       \
        if (_lenb > 0 && _lena > _lenb) {                               \
            if (!strncasecmp(buf+(_lena-_lenb), tail, _lenb)) {         \
                _lena -= _lenb;                                         \
            }                                                           \
        }                                                               \
        _lena -= 1;                                                     \
        while (_lena > 0 && (isspace(buf[_lena]) || buf[_lena] == '\"')) buf[_lena--] = '\0'; \
    } while (0)

struct metaToken {
    char *name;
    int namelen;
    enum TOKEN_TYPE type;
    void (*process)(char *content, CueSheet *centry);
};

static int _doconv(char *inbuf, size_t inlen, const char *encode, char *outbuf, size_t outlen)
{
    iconv_t cd;

    if ((cd = iconv_open("UTF-8", encode)) == (iconv_t)-1) {
        mtc_mt_err("iconv_open failure %s", strerror(errno));
        return -1;
    }

    if (iconv(cd, &inbuf, &inlen, &outbuf, &outlen) != -1) {
        iconv_close(cd);
        return 0;
    } else {
        mtc_mt_warn("iconv failure %s", strerror(errno));

        memcpy(outbuf, inbuf, outlen > inlen ? inlen : outlen);
        iconv_close(cd);
        return -1;
    }
}

static void _on_rem(char *content, CueSheet *centry)
{
    char *buf = content;
    if (!strncasecmp(buf, "DATE", 4)) {
        buf += 4;
        while (isspace(*buf)) buf++;

        strncpy(centry->date, buf, sizeof(centry->date) - 1);
    } else if (!strncasecmp(buf, "GENRE", 5)) {
        buf += 5;
        while (isspace(*buf)) buf++;

        strncpy(centry->genre, buf, sizeof(centry->genre) - 1);
    }
}

static void _on_title(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "");

    CueTrack *track = mlist_getx(centry->tracks, -1);

    if (track) {
        if (strcmp(centry->charset, "ASCII") && strcmp(centry->charset, "UTF-8"))
            _doconv(content, strlen(content), centry->charset,
                    track->title, sizeof(track->title) - 1);
        else strncpy(track->title, content, sizeof(track->title) - 1);
    } else {
        if (strcmp(centry->charset, "ASCII") && strcmp(centry->charset, "UTF-8"))
            _doconv(content, strlen(content), centry->charset,
                    centry->album, sizeof(centry->album) - 1);
        else strncpy(centry->album, content, sizeof(centry->album) - 1);
    }
}

static void _on_performer(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "");

    if (strcmp(centry->charset, "ASCII") && strcmp(centry->charset, "UTF-8"))
        _doconv(content, strlen(content), centry->charset,
                centry->artist, sizeof(centry->artist) - 1);
    else strncpy(centry->artist, content, sizeof(centry->artist) - 1);
}

static void _on_file(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "WAVE");

    if (centry->filename[0]) {
        mtc_mt_warn("DON'T support FILE command under TRACK");
        return;
    }

    int dirlen = strlen(centry->fullname);
    if (centry->fullname[dirlen-1] != '/') {
        centry->fullname[dirlen] = '/';
        dirlen++;
    }

    if (strcmp(centry->charset, "ASCII") && strcmp(centry->charset, "UTF-8")) {
        _doconv(content, strlen(content), centry->charset,
                centry->filename, sizeof(centry->filename) - 1);
        strncpy(centry->fullname + dirlen, centry->filename, sizeof(centry->fullname) - dirlen - 1);
    } else {
        strncpy(centry->filename, content, sizeof(centry->filename) - 1);
        strncpy(centry->fullname + dirlen, content, sizeof(centry->fullname) - dirlen - 1);
    }
}

static void _on_track(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "AUDIO");

    int tracksn = atoi(content);

    CueTrack *track = mos_calloc(1, sizeof(CueTrack));
    memset(track, 0x0, sizeof(CueTrack));
    track->sn = tracksn;

    uint8_t checksum[16] = {0};
    mhash_md5_buf(centry->_trackbuf, centry->_buflen, checksum);
    mstr_bin2hexstr(checksum, 16, track->md5);
    mstr_tolower(track->md5);

    mlist_append(centry->tracks, track);
}

static void _on_index0(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "");

    CueTrack *track = mlist_getx(centry->tracks, -1);
    if (track) {
        char *buf = content;
        int min = atoi(buf); buf += 3;
        int sec = atoi(buf); buf += 3;
        int frm = atoi(buf);

        track->index0 = (int) ((min * 60 + sec + (float)frm/75) * 1000);
    }
}

static void _on_index1(char *content, CueSheet *centry)
{
    TAKEOFF_WRAP(content, "");

    CueTrack *track = mlist_getx(centry->tracks, -1);
    if (track) {
        char *buf = content;
        int min = atoi(buf); buf += 3;
        int sec = atoi(buf); buf += 3;
        int frm = atoi(buf);

        track->index1 = (int) ((min * 60 + sec + (float)frm/75) * 1000);
    }
}

static struct metaToken meta_token[] = {
    {"REM",       3, TOK_REM,       _on_rem},
    {"TITLE",     5, TOK_TITLE,     _on_title},
    {"PERFORMER", 9, TOK_PERFORMER, _on_performer},
    {"FILE",      4, TOK_FILE,      _on_file},
    {"TRACK",     5, TOK_TRACK,     _on_track},
    {"INDEX 00",  8, TOK_INDEX00,   _on_index0},
    {"INDEX 01",  8, TOK_INDEX01,   _on_index1},
    {NULL,        0, TOK_REM,       NULL}
};

/*
 * REM GENRE Rock
 * REM DATE 2005
 * REM DISCID 83123E0B
 * REM COMMENT "ExactAudioCopy v1.0b3"
 * PERFORMER "Dire Straits & Mark Knopfler"
 * TITLE "Private Investigations - The Best Of (CD1)"
 * FILE "Dire Straits & Mark Knopfler - Private Investigations - The Best Of (CD1).flac" WAVE
 *   TRACK 01 AUDIO
 *     TITLE "Telegraph Road"
 *     PERFORMER "Dire Straits"
 *     INDEX 01 00:00:00
 *   TRACK 02 AUDIO
 *     TITLE "Sultans Of Swing"
 *     PERFORMER "Dire Straits"
 *     INDEX 01 14:20:25
 */
CueSheet* cueOpen(const char *filename)
{
    if (!filename) return NULL;

#define SKIP_HEADER(buf, head, len)             \
    do {                                        \
        int __len = 0;                          \
        while (__len < (len) && *buf) {         \
            buf++;                              \
            __len++;                            \
        }                                       \
        if (__len < len) continue;              \
        while (isspace(*buf)) buf++;            \
    } while (0)

    struct stat fs;
    if (stat(filename, &fs) != 0) return NULL;

    char buffer[fs.st_size], *line;

    FILE *fp = fopen(filename, "r");
    if (fp) {
        CueSheet *centry = mos_calloc(1, sizeof(CueSheet));
        memset(centry, 0x0, sizeof(CueSheet));
        mlist_init(&centry->tracks, free);
        strncpy(centry->fullname, filename, sizeof(centry->fullname) - 1);
        dirname(centry->fullname);
        centry->charset = "ASCII";

        uchardet_t ud = uchardet_new();
        if (ud) {
            size_t len = fread(buffer, 1, fs.st_size, fp);
            if (len == fs.st_size) {
                uchardet_handle_data(ud, buffer, len);
                uchardet_data_end(ud);
                centry->charset = uchardet_get_charset(ud);
            }
        }

        fseek(fp, 0, SEEK_SET);
        memset(buffer, 0x0, fs.st_size);

        centry->_trackbuf = mos_calloc(1, strlen(filename) + fs.st_size + 6);
        memcpy(centry->_trackbuf, filename, strlen(filename));
        centry->_buflen = strlen(filename);

        while (fgets(buffer, fs.st_size - 1, fp)) {
            line = mstr_strip_space(buffer);
            if (!line) break;
            else {
                memcpy(centry->_trackbuf + centry->_buflen, line, strlen(line));
                centry->_buflen += strlen(line);
            }

            struct metaToken *tok = meta_token;
            while (tok->name) {
                if (!strncasecmp(line, tok->name, tok->namelen)) {
                    SKIP_HEADER(line, tok->name, tok->namelen);
                    tok->process(line, centry);
                    break;
                }

                tok++;
            }
        }

        fclose(fp);
        if (ud) uchardet_delete(ud);
        ud = NULL;

        /* 给本大王打上标记，不要重复骚扰我 */
        uint8_t checksum[16] = {0};
        memcpy(centry->_trackbuf + centry->_buflen, "TOKEND", 6);
        centry->_buflen += 6;
        mhash_md5_buf(centry->_trackbuf, centry->_buflen, checksum);
        mstr_bin2hexstr(checksum, 16, centry->md5);
        mstr_tolower(centry->md5);

        /* cue 文件中，如果没有媒体文件、或者音轨，则舍弃该文件 */
        if (!centry->filename[0] || mlist_length(centry->tracks) <= 0) {
            mtc_mt_dbg("%s not cueSheet", filename);

            cueFree(centry);
            return NULL;
        } else {
            /* 临时去掉文件名后缀 */
            int pointindex = strlen(centry->filename) - 1;
            while (pointindex > 0 && centry->filename[pointindex] != '.') pointindex--;
            if (pointindex > 0) centry->filename[pointindex] = '\0';

            if (!centry->artist[0]) {
                /* 从文件名中取艺术家名 */
                char *pe = strstr(centry->filename, " - ");
                if (!pe) pe = strchr(centry->filename, ' ');
                if (!pe) memcpy(centry->artist, centry->filename, sizeof(centry->artist) - 1);
                else {
                    int toklen = sizeof(centry->artist) - 1;
                    if (toklen > pe - centry->filename) toklen = pe - centry->filename;
                    memcpy(centry->artist, centry->filename, toklen);
                }
            }

            if (!centry->album[0]) {
                /* 从文件名中取专辑名 */
                char *pe = strstr(centry->filename, " - ");
                if (pe) pe += 3;
                else {
                    pe = strchr(centry->filename, ' ');
                    if (pe) pe += 1;
                }

                if (pe && *pe) {
                    int toklen = sizeof(centry->album) - 1;
                    if (toklen > strlen(pe)) toklen = strlen(pe);
                    strncpy(centry->album, pe, toklen);
                } else memcpy(centry->album, centry->filename, sizeof(centry->album) - 1);
            }

            if (pointindex > 0) centry->filename[pointindex] = '.';

            MediaNode *mnode = mediaOpen(centry->fullname);
            if (mnode) {
                TechInfo *tinfo = mnode->driver->tech_info_get(mnode);
                centry->length = tinfo->length;
                mnode->driver->close(mnode);

                return centry;
            } else {
                mtc_mt_warn("%s not media file", centry->fullname);

                cueFree(centry);
                return NULL;
            }
        }
    }

#undef SKIP_HEADER

    return NULL;
}

void cueDump(CueSheet *centry)
{
    if (!centry) return;

    mtc_mt_dbg("CueSheet %s\n\t.10s%s\t%s\t%s\t%s\t%s with %d tracks in %d seconds",
               centry->fullname,
               centry->md5, centry->artist, centry->album, centry->date, centry->genre,
               mlist_length(centry->tracks), centry->length);

    CueTrack *track;
    MLIST_ITERATE(centry->tracks, track) {
        mtc_mt_dbg("Track %d %.10s: %s %d %d",
                   track->sn, track->md5, track->title,
                   track->index0, track->index1);
    }
}

void cueFree(CueSheet *centry)
{
    if (!centry) return;

    mlist_destroy(&centry->tracks);
    mos_free(centry->_trackbuf);
    free(centry);
}
