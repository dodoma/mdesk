#include <reef.h>
#include <magic.h>

#include "cue.h"
#include "asset.h"

static magic_t cookie = NULL;
static pthread_mutex_t m_lock = PTHREAD_MUTEX_INITIALIZER;

ASSET_TYPE assetType(const char *filename)
{
    if (!filename) return ASSET_UNKNOWN;

#define RETURN(ret)                             \
    do {                                        \
        pthread_mutex_unlock(&m_lock);          \
        return (ret);                           \
    } while (0)

    pthread_mutex_lock(&m_lock);

    if (!cookie) {
        cookie = magic_open(MAGIC_MIME_TYPE);
        if (!cookie) {
            mtc_mt_err("magic_open failure");
            RETURN(ASSET_UNKNOWN);
        }

        if (magic_load(cookie, NULL) != 0) {
            mtc_mt_err("load magic database failure %s", magic_error(cookie));
            magic_close(cookie);
            RETURN(ASSET_UNKNOWN);
        }
    }

    const char *mime = magic_file(cookie, filename);
    if (mime) {
        if (!memcmp(mime, "audio/", 6)) RETURN(ASSET_AUDIO);
        else if (!memcmp(mime, "image/", 6)) RETURN(ASSET_IMAGE);
        else if (!memcmp(mime, "text/", 5) && mstr_endwith(filename, ".cue", true)) {
            CueSheet *centry = cueOpen(filename);
            if (centry) {
                cueFree(centry);
                RETURN(ASSET_CUE);
            }
        }
    }

#undef RETURN

    pthread_mutex_unlock(&m_lock);
    return ASSET_UNKNOWN;
}

void assetClose()
{
    if (cookie) magic_close(cookie);
}
