/*
 * image.c - Image loading (WIC), data: URL parsing, remote downloading, PNG encoder.
 * All internal strings are UTF-8; UTF-16 is used strictly at Win32 API boundaries.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "util.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

static IWICImagingFactory *g_wicFactory = NULL;

static IWICImagingFactory *get_wic_factory(void)
{
    if (!g_wicFactory) {
        HRESULT hrInit = CoInitialize(NULL);
        if (SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE) {
            if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                        &IID_IWICImagingFactory, (void **)&g_wicFactory))) {
                g_wicFactory = NULL;
            }
        }
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

    if (!factory || !data || !size || size > 0x10000000u) { img->failed = 1; return -1; }
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
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || (uint64_t)w * (uint64_t)h > 0x04000000u) {
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
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    uint8_t *out;
    size_t o = 0, i;
    uint32_t val = 0;
    int bits = -8;

    if (!src || len == 0 || !outlen) return NULL;
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

static int load_data_url(Image *img, const char *url)
{
    const char *comma;
    size_t dlen = 0;
    uint8_t *data = NULL;
    if (!url) { img->failed = 1; return -1; }
    comma = strchr(url, ',');
    if (!comma) { img->failed = 1; return -1; }
    if (strstr(url, ";base64,")) {
        data = base64_decode(comma + 1, strlen(comma + 1), &dlen);
    }
    if (!data) { img->failed = 1; return -1; }
    img->data = data;
    img->size = dlen;
    return image_decode(img, data, dlen);
}

static int is_public_ipv4(uint32_t ipHostOrder)
{
    uint8_t a = (uint8_t)(ipHostOrder >> 24);
    uint8_t b = (uint8_t)((ipHostOrder >> 16) & 0xFF);
    if (a == 0 || a == 10 || a == 127) return 0;
    if (a == 100 && b >= 64 && b <= 127) return 0; /* CGNAT */
    if (a == 169 && b == 254) return 0;            /* Link-local / cloud metadata */
    if (a == 172 && b >= 16 && b <= 31) return 0;  /* RFC1918 */
    if (a == 192 && b == 168) return 0;            /* RFC1918 */
    if (a == 192 && b == 0) return 0;              /* IETF protocol / TEST-NET-1 */
    if (a == 198 && (b == 18 || b == 19)) return 0;/* Benchmarking */
    if (a >= 224) return 0;                        /* Multicast & reserved */
    return 1;
}

static int is_public_sockaddr(const struct sockaddr *sa)
{
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        return is_public_ipv4(ntohl(s4->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        const uint8_t *b = s6->sin6_addr.s6_addr;
        int i, allZero = 1;
        for (i = 0; i < 15; i++) if (b[i]) { allZero = 0; break; }
        if (allZero && (b[15] == 0 || b[15] == 1)) return 0; /* :: or ::1 */
        /* ::ffff:0:0/96 IPv4-mapped */
        if (allZero == 0) {
            int v4mapped = 1;
            for (i = 0; i < 10; i++) if (b[i]) { v4mapped = 0; break; }
            if (v4mapped && b[10] == 0xFF && b[11] == 0xFF) {
                uint32_t ip4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                               ((uint32_t)b[14] << 8) | (uint32_t)b[15];
                return is_public_ipv4(ip4);
            }
        }
        if ((b[0] & 0xFE) == 0xFC) return 0;               /* fc00::/7 ULA */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return 0; /* fe80::/10 link-local */
        if (b[0] == 0xFF) return 0;                        /* ff00::/8 multicast */
        return 1;
    }
    return 0;
}

static int host_resolves_public_only(const wchar_t *host, INTERNET_PORT port)
{
    static int wsaInit = 0;
    WSADATA wsa;
    ADDRINFOW hints = {0}, *res = NULL, *cur;
    wchar_t portStr[16];
    int ok = 0;

    if (!host || !host[0]) return 0;
    if (_wcsicmp(host, L"localhost") == 0) return 0;
    if (!wsaInit) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        wsaInit = 1;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    _snwprintf_s(portStr, 16, _TRUNCATE, L"%u", (unsigned)port);
    if (GetAddrInfoW(host, portStr, &hints, &res) != 0 || !res) return 0;
    for (cur = res; cur; cur = cur->ai_next) {
        if (!is_public_sockaddr(cur->ai_addr)) {
            ok = 0;
            break;
        }
        ok = 1;
    }
    FreeAddrInfoW(res);
    return ok;
}

int image_fetch_remote(Image *img)
{
    wchar_t *wurl;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    Buf body = {0};
    int hops;
    const size_t maxBytes = 32u * 1024u * 1024u;

    if (!img || !img->key) return -1;
    wurl = utf8_to_utf16(img->key, -1, NULL);
    if (!wurl) { img->pending = 0; img->failed = 1; return -1; }

    hSession = WinHttpOpen(L"Lilac/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        free(wurl);
        img->pending = 0;
        img->failed = 1;
        return -1;
    }

    for (hops = 0; hops < 5; hops++) {
        URL_COMPONENTSW uc = {0};
        wchar_t host[256] = {0};
        wchar_t path[2048] = {0};
        DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE;
        DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD status = 0, stSz = sizeof(status);

        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wurl, 0, 0, &uc) ||
            (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) ||
            !host[0] || !host_resolves_public_only(host, uc.nPort)) {
            break;
        }

        if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;
        hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConnect) break;
        hRequest = WinHttpOpenRequest(hConnect, L"GET", path[0] ? path : L"/", NULL,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) { WinHttpCloseHandle(hConnect); hConnect = NULL; break; }

        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, NULL) ||
            !WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &stSz, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(hRequest); hRequest = NULL;
            WinHttpCloseHandle(hConnect); hConnect = NULL;
            break;
        }

        if (status >= 300 && status < 400) {
            DWORD locLen = 0;
            wchar_t *loc;
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                NULL, &locLen, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locLen > 0 && locLen < 16384) {
                loc = (wchar_t *)malloc(locLen);
                if (loc && WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                               loc, &locLen, WINHTTP_NO_HEADER_INDEX)) {
                    free(wurl);
                    wurl = loc;
                    WinHttpCloseHandle(hRequest); hRequest = NULL;
                    WinHttpCloseHandle(hConnect); hConnect = NULL;
                    continue;
                }
                free(loc);
            }
            WinHttpCloseHandle(hRequest); hRequest = NULL;
            WinHttpCloseHandle(hConnect); hConnect = NULL;
            break;
        }

        if (status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                DWORD got = 0;
                char *chunk;
                if (body.n + avail > maxBytes) { avail = 0; buf_free(&body); break; }
                chunk = (char *)malloc(avail);
                if (!chunk) { buf_free(&body); break; }
                if (!WinHttpReadData(hRequest, chunk, avail, &got) || got == 0) { free(chunk); break; }
                buf_put(&body, chunk, got);
                free(chunk);
            }
        }
        WinHttpCloseHandle(hRequest); hRequest = NULL;
        WinHttpCloseHandle(hConnect); hConnect = NULL;
        break;
    }

    WinHttpCloseHandle(hSession);
    free(wurl);

    if (body.p && body.n > 0) {
        img->data = (uint8_t *)body.p;
        img->size = body.n;
        img->pending = 0;
        return image_decode(img, img->data, img->size);
    }
    buf_free(&body);
    img->pending = 0;
    img->failed = 1;
    return -1;
}

Image *imagecache_get(ImageCache *c, const char *key, int allowRemote)
{
    Image *img;
    if (!key || !key[0]) return NULL;

    for (img = c->head; img; img = img->next) {
        if (!strcmp(img->key, key)) {
            if (img->pending && allowRemote) {
                if (!ascii_nicmp(img->key, "http://", 7) || !ascii_nicmp(img->key, "https://", 8)) {
                    image_fetch_remote(img);
                } else {
                    img->pending = 0;
                    img->failed = 1;
                }
            }
            return img;
        }
    }

    img = (Image *)calloc(1, sizeof(Image));
    if (!img) return NULL;
    img->key = _strdup(key);
    img->next = c->head;
    c->head = img;

    if (!ascii_nicmp(key, "data:", 5)) {
        load_data_url(img, key);
    } else if (!ascii_nicmp(key, "http://", 7) || !ascii_nicmp(key, "https://", 8)) {
        if (allowRemote) {
            image_fetch_remote(img);
        } else {
            img->pending = 1;
        }
    } else if (path_is_remote(key)) {
        /* Remote UNC or network device path: block local file I/O to prevent NTLM credential leak */
        if (!allowRemote) {
            img->pending = 1;
        } else {
            /* Block UNC SMB access even during export to prevent credential theft */
            img->failed = 1;
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
    if (len && data) crc = crc32_update(crc, data, len);
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
