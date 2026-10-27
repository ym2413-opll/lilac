/*
 * image.h - image loading (via Windows Imaging Component, part of Windows).
 */
#ifndef MV_IMAGE_H
#define MV_IMAGE_H

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ImageFormat {
    IMG_UNKNOWN = 0, IMG_PNG, IMG_JPEG, IMG_GIF, IMG_BMP, IMG_TIFF, IMG_OTHER
} ImageFormat;

typedef struct Image {
    wchar_t  *key;      /* resolved absolute path or URL */
    int       w, h;     /* pixel size (0 if not loaded) */
    uint32_t *pixels;   /* w*h BGRA, straight (non-premultiplied) alpha; NULL if failed/pending */
    int       hasAlpha; /* any pixel with alpha < 255 */
    uint8_t  *data;     /* original encoded bytes (may be NULL if pixels were synthesised) */
    size_t    size;
    int       format;   /* ImageFormat of data */
    int       failed;   /* could not be loaded / decoded */
    int       pending;  /* remote image not downloaded yet */
    void     *hbm;      /* cached HBITMAP (premultiplied 32bpp DIB) for GDI drawing */
    struct Image *next;
} Image;

typedef struct ImageCache {
    Image *head;
} ImageCache;

ImageCache *imagecache_new(void);
void        imagecache_free(ImageCache *c);
/* Get or create an entry for a key (absolute path or URL). Loads local files immediately.
 * Remote URLs are downloaded synchronously only if allowRemote, otherwise left pending. */
Image      *imagecache_get(ImageCache *c, const wchar_t *key, int allowRemote);
/* Download/decode a pending remote image now (blocking). Returns 0 on success. */
int         image_fetch_remote(Image *img);

/* Decode encoded bytes into img (sets w,h,pixels,hasAlpha,format). 0 = ok. */
int image_decode(Image *img, const uint8_t *data, size_t size);
/* Encode BGRA pixels as PNG. *out malloc()ed. 0 = ok. */
int image_encode_png(const uint32_t *bgra, int w, int h, uint8_t **out, size_t *outlen);

#ifdef __cplusplus
}
#endif
#endif
