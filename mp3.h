#ifndef __MP3_H__
#define __MP3_H__

bool mp3_id3_get(const char *filename,
                 char *title, char *artist, char *album, char *year, char *track);
bool mp3_id3_get_buf(const uint8_t *buffer, size_t buf_size,
                     char *title, char *artist, char *album, char *year, char *track);
bool mp3_md5_get(const char *filename, char id[LEN_DOMMEID]);
bool mp3_md5_get_buf(const uint8_t *buffer, size_t buf_size, char id[LEN_DOMMEID]);
bool mp3_md5_buf(const uint8_t *buffer, size_t buf_size, char md5[33]);


/* 确保二者成对调用 */
mp3dec_map_info_t* mp3_cover_open(char *filename, char **mime, uint8_t **imgbuf, size_t *imgsize);
void mp3_cover_close(mp3dec_map_info_t *mapinfo);

#endif  /* __MP3_H__ */
