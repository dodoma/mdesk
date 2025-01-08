#ifndef __ASSET_H__
#define __ASSET_H__

typedef enum {
    ASSET_UNKNOWN = 0,
    ASSET_AUDIO,
    ASSET_IMAGE,
    ASSET_CUE
} ASSET_TYPE;

ASSET_TYPE assetType(const char *filename);

void assetClose();

#endif  /* __ASSET_H__ */
