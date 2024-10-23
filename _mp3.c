typedef enum {
    ID3_TITLE = 0,
    ID3_ALBUM,
    ID3_ARTIST,
    ID3_TRACK_NUM,
    ID3_YEAR,
    ID3_LENGTH,
    ID3_PIC,
    ID3_ARTIST2,
    ID3_UNKNOWN
} TAGNAME;

struct callback_data {
    mp3dec_t *mp3d;
    int hz;
    uint64_t samples;
};

static pthread_mutex_t m_indexer_lock = PTHREAD_MUTEX_INITIALIZER;

static int doconv(char* inbuf, size_t inbytes, char* encode, char* outbuf, size_t outbytes)
{
    iconv_t cd;

    if ((cd = iconv_open("UTF-8", encode)) == (iconv_t) -1) {
        perror("Create Iconv: ");
        return -1;
    }

    if ((iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes)) != -1) {
        iconv_close(cd);
        return 1;
    } else {
        iconv_close(cd);
        //mtc_mt_noise("iconv: %s", strerror(errno));
        memcpy(outbuf, inbuf, inbytes);
        return -1;
    }
}

static size_t _read_id3v2(const uint8_t *buf, size_t len, TAGNAME tname, char *out, size_t outlen)
{
	const char id3v2_3[] = "TIT2TALBTPE1TRCKTYERTLENAPICTPE2UNKN";  //id3, v2, spec 3 uses 4 byte ID
	const char id3v2_2[] = "TT2 TAL TP1 TRK TYE TLE PIC TP2 UNK ";  //id3, v2, spec 2 uses 3 byte ID

    int taglen = (buf[9]) | (buf[8] << 7) | (buf[7] << 14) | (buf[6] << 28);

    if (taglen > len) return 0;

    bool spec3 = (buf[3] == 0x03) || (buf[3] == 0x04);

    const char *tagname = spec3 ? (id3v2_3+(tname*4)) : (id3v2_2+(tname*4));
    int header_size = spec3 ? 10 : 6;
    int tagname_size = spec3 ? 4 : 3;
    unsigned long frame_size = 0;
    const uint8_t *pos = buf + 10;

    while (taglen > 0) {
        if (spec3) frame_size = (pos[7]) | (pos[6] << 8) | (pos[5] << 16) | ((pos[4] << 16) << 16);
        else frame_size = (pos[5]) | (pos[4] << 8) | (pos[3] << 16);

        //mtc_mt_dbg("%.*s length %lu", tagname_size, pos, frame_size);

        if (!strncmp((char*)pos, tagname, tagname_size)) {
            pos += header_size;

            uint32_t needtoread = MINIMP3_MIN(frame_size, outlen) - 1;
            uint8_t encoding = *pos;
            pos++;

            //if (tname == ID3_TITLE)
            //    mtc_mt_dbg("%d %s", encoding, tagname);

            if (tname != ID3_PIC) {
                if (encoding == 0x00) doconv((char*)pos, needtoread, "GB18030", out, outlen);
                else if (encoding == 0x01 || encoding == 0x02)
                    doconv((char*)pos, needtoread, "UTF16", out, outlen);
                else memcpy(out, pos, needtoread);
            } else {
                uint32_t infolen = 0;

                /* mime */
                while (infolen < 30 && *pos != '\0') {
                    pos++;
                    infolen++;
                }
                pos++;
                infolen++;

                /* picture type */
                pos++;
                infolen++;

                /* description */
                if (encoding == 0x01 || encoding == 0x02) {
                    /* skip UTF-16 description */
                    while (infolen < needtoread && *(uint16_t*)pos != 0) {
                        pos += 2;
                        infolen += 2;
                    }
                    /* skip end */
                    pos += 2;
                    infolen += 2;
                } else {
                    /* skip UTF-8 or Latin-1 description */
                    while (infolen < needtoread && *pos != 0) {
                        pos += 1;
                        infolen += 1;
                    }
                    /* skip end */
                    pos += 1;
                    infolen += 1;
                }

                if (infolen < needtoread) memcpy(out, pos, needtoread - infolen);

                //while (needtoread > 0 && *pos != 0xFF && *(pos+1) != 0xD8) {
                //    pos++;
                //    needtoread--;
                //}
                //if (needtoread > 0) memcpy(out, pos, needtoread);
            }
            return needtoread;
        } else {
            pos += (frame_size + header_size);
            taglen -= (frame_size + header_size);
        }
    }

    return 0;
}

static bool _info_get_id3v1(const uint8_t *buf, size_t len,
                            char *title, char *artist, char *album, char *year, char *track)
{
    if (len >= 128) {
        char *pos = (char*)buf + len - 128;
        if (!memcmp(pos, "TAG", 3)) {
            pos += 3;

            doconv(pos, 30, "GB18030", title, LEN_ID3_STRING);
            pos += 30;

            doconv(pos, 30, "GB18030", artist, LEN_ID3_STRING);
            pos += 30;

            doconv(pos, 30, "GB18030", album, LEN_ID3_STRING);
            pos += 30;

            strncpy(year, pos, 4);

            return true;
        }
    }

    return false;
}

static bool _info_get_id3v2(const uint8_t *buf, size_t len,
                            char *title, char *artist, char *album, char *year, char *track)
{
    if (len >= 10 && !memcmp(buf, "ID3", 3) && buf[3] <= 0x04 &&
        !((buf[5] & 15) || (buf[6] & 0x80) || (buf[7] & 0x80) || (buf[8] & 0x80) || (buf[9] & 0x80))) {

        _read_id3v2(buf, len, ID3_TITLE, title, LEN_ID3_STRING);
        if (_read_id3v2(buf, len, ID3_ARTIST2, artist, LEN_ID3_STRING) == 0)
            _read_id3v2(buf, len, ID3_ARTIST, artist, LEN_ID3_STRING);
        _read_id3v2(buf, len, ID3_ALBUM, album,  LEN_ID3_STRING);
        _read_id3v2(buf, len, ID3_YEAR, year, LEN_ID3_STRING);
        _read_id3v2(buf, len, ID3_TRACK_NUM, track, LEN_ID3_STRING);

        return true;
    }

    return false;
}

bool mp3_id3_get(const char *filename,
                 char *title, char *artist, char *album, char *year, char *track)
{
    bool ret = false;

    mp3dec_map_info_t map_info;
    if (mp3dec_open_file(filename, &map_info) != 0) return false;

    if (_info_get_id3v2(map_info.buffer, map_info.size, title, artist, album, year, track))
        ret = true;
    else ret = _info_get_id3v1(map_info.buffer, map_info.size, title, artist, album, year, track);

    mp3dec_close_file(&map_info);

    return ret;
}

bool mp3_md5_get(const char *filename, char id[LEN_DOMMEID])
{
    mp3dec_map_info_t map_info;

    memset(id, 0x0, LEN_DOMMEID);

    if (mp3dec_open_file(filename, &map_info) != 0) return false;

    const uint8_t *buf = map_info.buffer;
    size_t size = map_info.size;

    /* md5 string */
    mp3dec_skip_id3(&buf, &size);
    unsigned char sum[16];
    mhash_md5_buf((unsigned char*)buf, size, sum);

    /* TODO 只取16个字节中的前几个，出现了冲突再做处理 */
    mstr_bin2hexstr(sum, (int)(LEN_DOMMEID-1)/2, id);
    mstr_tolower(id);

    mp3dec_close_file(&map_info);

    return true;
}

static bool _seek_cover(const uint8_t *buf, size_t len, char **mime, uint8_t **outmem, size_t *outlen)
{
	const char id3v2_3[] = "TIT2TALBTPE1TRCKTYERTLENAPICTPE2UNKN";  //id3, v2, spec 3 uses 4 byte ID
	const char id3v2_2[] = "TT2 TAL TP1 TRK TYE TLE PIC TP2 UNK ";  //id3, v2, spec 2 uses 3 byte ID

    int taglen = (buf[9]) | (buf[8] << 7) | (buf[7] << 14) | (buf[6] << 28);

    if (taglen > len) return false;

    bool spec3 = (buf[3] == 0x03) || (buf[3] == 0x04);

    const char *tagname = spec3 ? (id3v2_3+(ID3_PIC*4)) : (id3v2_2+(ID3_PIC*4));
    int header_size = spec3 ? 10 : 6;
    int tagname_size = spec3 ? 4 : 3;
    unsigned long frame_size = 0;
    const uint8_t *pos = buf + 10;

    while (taglen > 0) {
        if (spec3) frame_size = (pos[7]) | (pos[6] << 8) | (pos[5] << 16) | ((pos[4] << 16) << 16);
        else frame_size = (pos[5]) | (pos[4] << 8) | (pos[3] << 16);

        //mtc_mt_dbg("%.*s length %lu", tagname_size, pos, frame_size);

        if (!strncmp((char*)pos, tagname, tagname_size)) {
            pos += header_size;

            uint32_t needtoread = frame_size - 1;
            uint8_t encoding = *pos;
            pos++;

            uint32_t infolen = 0;

            /* mime */
            if (mime) *mime = (char*)pos;
            while (infolen < 30 && *pos != '\0') {
                pos++;
                infolen++;
            }
            pos++;
            infolen++;

            /* picture type */
            pos++;
            infolen++;

            /* description */
            if (encoding == 0x01 || encoding == 0x02) {
                /* skip UTF-16 description */
                while (infolen < needtoread && *(uint16_t*)pos != 0) {
                    pos += 2;
                    infolen += 2;
                }
                /* skip end */
                pos += 2;
                infolen += 2;
            } else {
                /* skip UTF-8 or Latin-1 description */
                while (infolen < needtoread && *pos != 0) {
                    pos += 1;
                    infolen += 1;
                }
                /* skip end */
                pos += 1;
                infolen += 1;
            }

            if (infolen < needtoread) {
                *outmem = (uint8_t*)pos;
                *outlen = needtoread - infolen;
                if (*outlen < len) return true;
                else return false;
            } else return false;
        } else {
            pos += (frame_size + header_size);
            taglen -= (frame_size + header_size);
        }
    }

    return false;
}

mp3dec_map_info_t* mp3_cover_open(char *filename, char **mime, uint8_t **imgbuf, size_t *imgsize)
{
    if (!filename || !imgsize || !imgbuf) return NULL;

    mp3dec_map_info_t *mapinfo = mos_calloc(1, sizeof(mp3dec_map_info_t));
    if (mp3dec_open_file(filename, mapinfo) != 0) {
        mtc_mt_warn("open %s failure %s", filename, strerror(errno));
        mos_free(mapinfo);
        return NULL;
    }

    if (!_seek_cover(mapinfo->buffer, mapinfo->size, mime, imgbuf, imgsize)) {
        mtc_mt_warn("%s cover empty", filename);
        mp3dec_close_file(mapinfo);
        mos_free(mapinfo);
        return NULL;
    }

    return mapinfo;
}

void mp3_cover_close(mp3dec_map_info_t *mapinfo)
{
    if (!mapinfo) return;

    mp3dec_close_file(mapinfo);
    mos_free(mapinfo);
}
