#ifndef __MP3_H__
#define __MP3_H__

DommeStore* dommeStoreCreate();
void dommeStoreFree(void *p);
MERR* dommeLoadFromFilef(DommeStore *plan, char *fmt, ...);
DommeFile* dommeGetFile(DommeStore *plan, char *id);

bool mp3_id3_get(const char *filename,
                 char *title, char *artist, char *album, char *year, char *track);
bool mp3_md5_get(const char *filename, char id[LEN_DOMMEID]);

/* 确保二者成对调用 */
mp3dec_map_info_t* mp3_cover_open(const char *filename, uint8_t **imgbuf, size_t *imgsize);
void mp3_cover_close(mp3dec_map_info_t *mapinfo);

#endif  /* __MP3_H__ */
