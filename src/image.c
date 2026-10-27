/*
 * image.c - Image loading (WIC), data: URL parsing, remote downloading, PNG encoder.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <urlmon.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "util.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "ole32.lib")

static IWICImagingFactory *g_wicFactory = NULL;

static IWICImagingFactory *get_wic_factory(void)
{
    if (!g_wicFactory) {
        CoInitialize(NULL);
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IWICImagingFactory, (void **)&g_wicFactory);
    }
    return g_wicFactory;
}

ImageCache *imagecache_new(void)
{
    return (ImageCache *)calloc(1, sizeof(ImageCache));
}

void imagecache_free(ImageCache *c)
{
    Image *cur;
    if (!c) return;
    cur = c->head;
    while (cur) {
        Image *next = cur->next;
        free(cur->key);
        free(cur->pixels);
        free(cur->data);
        if (cur->hbm) DeleteObject((HBITMAP)cur->hbm);
        free(cur);
        cur = next;
    }
    free(c);
}

static int detect_format(const uint8_t *data, size_t size)
{
    if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return IMG_PNG;
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return IMG_JPEG;
    if (size >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) return IMG_GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return IMG_BMP;
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42))) return IMG_TIFF;
    return IMG_OTHER;
}

int image_decode(Image *img, const uint8_t *data, size_t size)
{
    IWICImagingFactory *factory = get_wic_factory();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    HRESULT hr;
    UINT w = 0, h = 0;
    uint32_t *px = NULL;
    int hasAlpha = 0;
    size_t i, total;

    if (!factory || !data || !size) { img->failed = 1; return -1; }
    img->format = detect_format(data, size);

    hr = factory->lpVtbl->CreateStream(factory, &stream);
    if (FAILED(hr)) { img->failed = 1; return -1; }
    hr = stream->lpVtbl->InitializeFromMemory(stream, (WICInProcPointer)data, (DWORD)size);
    if (FAILED(hr)) { stream->lpVtbl->Release(stream); img->failed = 1; return -1; }

    hr = factory->lpVtbl->CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                  WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { stream->lpVtbl->Release(stream); img->failed = 1; return -1; }

    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) {
        decoder->lpVtbl->Release(decoder);
        stream->lpVtbl->Release(stream);
        img->failed = 1;
        return -1;
    }

    hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) {
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        stream->lpVtbl->Release(stream);
        img->failed = 1;
        return -1;
    }

    hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame,
                                       &GUID_WICPixelFormat32bppBGRA,
                                       WICBitmapDitherTypeNone, NULL, 0.0,
                                       WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        stream->lpVtbl->Release(stream);
        img->failed = 1;
        return -1;
    }

    converter->lpVtbl->GetSize(converter, &w, &h);
    if (w == 0 || h == 0 || (size_t)w * h > 0x10000000) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        stream->lpVtbl->Release(stream);
        img->failed = 1;
        return -1;
    }

    px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        stream->lpVtbl->Release(stream);
        img->failed = 1;
        return -1;
    }

    hr = converter->lpVtbl->CopyPixels(converter, NULL, w * 4, w * h * 4, (BYTE *)px);
    converter->lpVtbl->Release(converter);
    frame->lpVtbl->Release(frame);
    decoder->lpVtbl->Release(decoder);
    stream->lpVtbl->Release(stream);

    if (FAILED(hr)) {
        free(px);
        img->failed = 1;
        return -1;
    }

    total = (size_t)w * h;
    for (i = 0; i < total; i++) {
        uint32_t a = (px[i] >> 24) & 0xFF;
        if (a < 255) { hasAlpha = 1; break; }
    }

    img->w = (int)w;
    img->h = (int)h;
    img->pixels = px;
    img->hasAlpha = hasAlpha;
    img->failed = 0;
    return 0;
}

static uint8_t *base64_decode(const char *src, size_t len, size_t *outlen)
{
    static const int8_t tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    uint8_t *out;
    size_t o = 0, i;
    uint32_t val = 0;
    int bits = -8;

    out = (uint8_t *)malloc(len);
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        int8_t d = tbl[c];
        if (d >= 0) {
            val = (val << 6) | (uint32_t)d;
            bits += 6;
            if (bits >= 0) {
                out[o++] = (uint8_t)((val >> bits) & 0xFF);
                bits -= 8;
            }
        } else if (c == '=') {
            break;
        }
    }
    *outlen = o;
    return out;
}

static int load_data_url(Image *img, const wchar_t *url)
{
    char *utf8 = utf16_to_utf8(url, -1, NULL);
    const char *comma;
    size_t dlen = 0;
    uint8_t *data = NULL;
    if (!utf8) { img->failed = 1; return -1; }
    comma = strchr(utf8, ',');
    if (!comma) { free(utf8); img->failed = 1; return -1; }
    if (strstr(utf8, ";base64,")) {
        data = base64_decode(comma + 1, strlen(comma + 1), &dlen);
    }
    free(utf8);
    if (!data) { img->failed = 1; return -1; }
    img->data = data;
    img->size = dlen;
    return image_decode(img, data, dlen);
}

int image_fetch_remote(Image *img)
{
    wchar_t cacheFile[MAX_PATH];
    HRESULT hr;
    if (!img->key) return -1;
    hr = URLDownloadToCacheFileW(NULL, img->key, cacheFile, MAX_PATH, 0, NULL);
    if (SUCCEEDED(hr)) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (!file_read_all(cacheFile, &data, &len)) {
            img->data = data;
            img->size = len;
            img->pending = 0;
            return image_decode(img, data, len);
        }
    }
    img->pending = 0;
    img->failed = 1;
    return -1;
}

Image *imagecache_get(ImageCache *c, const wchar_t *key, int allowRemote)
{
    Image *img;
    if (!key || !key[0]) return NULL;

    for (img = c->head; img; img = img->next) {
        if (!wcscmp(img->key, key)) {
            if (img->pending && allowRemote) image_fetch_remote(img);
            return img;
        }
    }

    img = (Image *)calloc(1, sizeof(Image));
    if (!img) return NULL;
    img->key = _wcsdup(key);
    img->next = c->head;
    c->head = img;

    if (!_wcsnicmp(key, L"data:", 5)) {
        load_data_url(img, key);
    } else if (!_wcsnicmp(key, L"http://", 7) || !_wcsnicmp(key, L"https://", 8)) {
        if (allowRemote) {
            image_fetch_remote(img);
        } else {
            img->pending = 1;
        }
    } else {
        uint8_t *data = NULL;
        size_t len = 0;
        if (!file_read_all(key, &data, &len)) {
            img->data = data;
            img->size = len;
            image_decode(img, data, len);
        } else {
            img->failed = 1;
        }
    }
    return img;
}

/* PNG encoder */
static void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void append_chunk(Buf *b, const char *type, const void *data, size_t len)
{
    uint8_t len_bytes[4];
    uint8_t crc_bytes[4];
    uint32_t crc;
    put_u32_be(len_bytes, (uint32_t)len);
    buf_put(b, len_bytes, 4);
    buf_put(b, type, 4);
    if (len && data) buf_put(b, data, len);

    crc = crc32_calc(type, 4);
    if (len && data) {
        /* accumulate crc with data */
        size_t i;
        const uint8_t *p = (const uint8_t *)data;
        for (i = 0; i < len; i++) {
            /* standard table CRC step: util.c crc32_calc calculates over entire buffer */
        }
    }
    /* Let's compute crc of (type + data) directly */
    {
        uint8_t *tmp = (uint8_t *)malloc(4 + len);
        if (tmp) {
            memcpy(tmp, type, 4);
            if (len && data) memcpy(tmp + 4, data, len);
            crc = crc32_calc(tmp, 4 + len);
            free(tmp);
        }
    }
    put_u32_be(crc_bytes, crc);
    buf_put(b, crc_bytes, 4);
}

int image_encode_png(const uint32_t *bgra, int w, int h, uint8_t **out, size_t *outlen)
{
    Buf b = {0};
    uint8_t ihdr[13];
    size_t raw_stride = 1 + (size_t)w * 4;
    size_t raw_len = raw_stride * (size_t)h;
    uint8_t *raw;
    uint8_t *comp = NULL;
    size_t clen = 0;
    int y, x;

    if (!bgra || w <= 0 || h <= 0) return -1;
    raw = (uint8_t *)malloc(raw_len);
    if (!raw) return -1;

    for (y = 0; y < h; y++) {
        uint8_t *line = raw + (size_t)y * raw_stride;
        const uint32_t *src_row = bgra + (size_t)y * w;
        *line++ = 0; /* filter type 0: None */
        for (x = 0; x < w; x++) {
            uint32_t c = src_row[x];
            *line++ = (uint8_t)(c >> 16); /* R */
            *line++ = (uint8_t)(c >> 8);  /* G */
            *line++ = (uint8_t)c;         /* B */
            *line++ = (uint8_t)(c >> 24); /* A */
        }
    }

    clen = zlib_compress(raw, raw_len, &comp);
    free(raw);
    if (!comp || clen == 0) return -1;

    /* PNG signature */
    buf_put(&b, "\x89PNG\r\n\x1a\n", 8);

    /* IHDR */
    put_u32_be(ihdr, (uint32_t)w);
    put_u32_be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; /* 8-bit */
    ihdr[9] = 6; /* RGBA */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    append_chunk(&b, "IHDR", ihdr, 13);

    /* IDAT */
    append_chunk(&b, "IDAT", comp, clen);
    free(comp);

    /* IEND */
    append_chunk(&b, "IEND", NULL, 0);

    *out = (uint8_t *)b.p;
    *outlen = b.n;
    return 0;
}
