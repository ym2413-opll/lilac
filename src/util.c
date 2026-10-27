/*
 * util.c - shared helpers (see util.h).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "util.h"

#define SDEFL_IMPLEMENTATION
#include "../third_party/sdefl/sdefl.h"

/* ---------------- Arena ---------------- */

struct ArenaBlock {
    ArenaBlock *next;
    size_t      used, cap;
    /* data follows */
};

#define ARENA_BLOCK (64 * 1024)

void *arena_alloc(Arena *a, size_t size)
{
    ArenaBlock *b = a->head;
    size = (size + 15) & ~(size_t)15;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > ARENA_BLOCK ? size : ARENA_BLOCK;
        size_t hdr = (sizeof(ArenaBlock) + 15) & ~(size_t)15;
        ArenaBlock *nb = (ArenaBlock *)malloc(hdr + cap);
        if (!nb) { fprintf(stderr, "out of memory\n"); abort(); }
        nb->used = 0; nb->cap = cap;
        if (b && size > ARENA_BLOCK) {
            /* large allocation: insert behind current block so the current keeps filling */
            nb->next = b->next; b->next = nb;
            nb->used = size;
            memset((char *)nb + hdr, 0, size);
            return (char *)nb + hdr;
        }
        nb->next = b; a->head = nb; b = nb;
    }
    {
        size_t hdr = (sizeof(ArenaBlock) + 15) & ~(size_t)15;
        char *p = (char *)b + hdr + b->used;
        b->used += size;
        memset(p, 0, size);
        return p;
    }
}

void arena_free(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    a->head = NULL;
}

wchar_t *arena_wcsndup(Arena *a, const wchar_t *s, int len)
{
    wchar_t *r;
    if (!s) return NULL;
    if (len < 0) len = (int)wcslen(s);
    r = (wchar_t *)arena_alloc(a, (len + 1) * sizeof(wchar_t));
    memcpy(r, s, len * sizeof(wchar_t));
    r[len] = 0;
    return r;
}

char *arena_strndup(Arena *a, const char *s, int len)
{
    char *r;
    if (!s) return NULL;
    if (len < 0) len = (int)strlen(s);
    r = (char *)arena_alloc(a, len + 1);
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}

/* ---------------- Buf ---------------- */

void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + extra + 1) nc *= 2;
        b->p = (char *)realloc(b->p, nc);
        if (!b->p) { fprintf(stderr, "out of memory\n"); abort(); }
        b->cap = nc;
    }
}

void buf_put(Buf *b, const void *data, size_t len)
{
    buf_reserve(b, len);
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
}

void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }
void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    char tmp[512];
    va_start(ap, fmt);
    n = _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n >= 0) { buf_put(b, tmp, (size_t)n); return; }
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_reserve(b, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

void buf_put_w(Buf *b, const wchar_t *s, int len)
{
    int n;
    if (!s) return;
    if (len < 0) len = (int)wcslen(s);
    if (!len) return;
    n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    buf_reserve(b, (size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, len, b->p + b->n, n, NULL, NULL);
    b->n += (size_t)n;
    b->p[b->n] = 0;
}

void buf_put_xml_w(Buf *b, const wchar_t *s, int len)
{
    int i, st = 0;
    if (!s) return;
    if (len < 0) len = (int)wcslen(s);
    for (i = 0; i < len; i++) {
        wchar_t c = s[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default:
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0xFFFE || c == 0xFFFF) rep = "";
        }
        if (rep) {
            buf_put_w(b, s + st, i - st);
            buf_puts(b, rep);
            st = i + 1;
        }
    }
    buf_put_w(b, s + st, len - st);
}

void buf_put_xml(Buf *b, const char *s, int len)
{
    int i, st = 0;
    if (!s) return;
    if (len < 0) len = (int)strlen(s);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default:
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') rep = "";
        }
        if (rep) {
            buf_put(b, s + st, (size_t)(i - st));
            buf_puts(b, rep);
            st = i + 1;
        }
    }
    buf_put(b, s + st, (size_t)(len - st));
}

void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---------------- WBuf ---------------- */

void wbuf_put(WBuf *b, const wchar_t *s, int len)
{
    if (len < 0) len = (int)wcslen(s);
    if (b->n + len + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 128;
        while (nc < b->n + len + 1) nc *= 2;
        b->p = (wchar_t *)realloc(b->p, nc * sizeof(wchar_t));
        if (!b->p) abort();
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, len * sizeof(wchar_t));
    b->n += len;
    b->p[b->n] = 0;
}

void wbuf_putc(WBuf *b, wchar_t c) { wbuf_put(b, &c, 1); }
void wbuf_free(WBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---------------- Encoding ---------------- */

char *utf16_to_utf8(const wchar_t *s, int len, int *outlen)
{
    int n;
    char *r;
    if (!s) s = L"";
    if (len < 0) len = (int)wcslen(s);
    n = len ? WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL) : 0;
    r = (char *)malloc((size_t)n + 1);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s, len, r, n, NULL, NULL);
    r[n] = 0;
    if (outlen) *outlen = n;
    return r;
}

wchar_t *utf8_to_utf16(const char *s, int len, int *outlen)
{
    int n;
    wchar_t *r;
    if (!s) s = "";
    if (len < 0) len = (int)strlen(s);
    n = len ? MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0) : 0;
    r = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, len, r, n);
    r[n] = 0;
    if (outlen) *outlen = n;
    return r;
}

uint32_t utf16_next(const wchar_t *s, int len, int *i)
{
    uint32_t c = s[*i];
    (*i)++;
    if (c >= 0xD800 && c <= 0xDBFF && *i < len) {
        uint32_t d = s[*i];
        if (d >= 0xDC00 && d <= 0xDFFF) {
            (*i)++;
            return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        }
    }
    return c;
}

/* ---------------- Files ---------------- */

int file_read_all(const wchar_t *path, uint8_t **data, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    uint8_t *p;
    DWORD got = 0;
    size_t total = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFFF) { CloseHandle(h); return -1; }
    p = (uint8_t *)malloc((size_t)sz.QuadPart + 1);
    if (!p) { CloseHandle(h); return -1; }
    while (total < (size_t)sz.QuadPart) {
        if (!ReadFile(h, p + total, (DWORD)((size_t)sz.QuadPart - total), &got, NULL) || !got) break;
        total += got;
    }
    CloseHandle(h);
    p[total] = 0;
    *data = p;
    *len = total;
    return 0;
}

int file_write_all(const wchar_t *path, const void *data, size_t len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    size_t total = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x10000000 ? 0x10000000 : (len - total));
        if (!WriteFile(h, (const char *)data + total, chunk, &wr, NULL) || !wr) { CloseHandle(h); return -1; }
        total += wr;
    }
    CloseHandle(h);
    return 0;
}

wchar_t *path_dir(const wchar_t *path)
{
    const wchar_t *name = path_name(path);
    int n = (int)(name - path);
    wchar_t *r;
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    if (n == 2 && path[1] == ':') { n = 3; } /* keep "C:\" */
    r = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    memcpy(r, path, n * sizeof(wchar_t));
    r[n] = 0;
    if (n == 3 && r[2] != '\\') r[2] = '\\';
    return r;
}

const wchar_t *path_name(const wchar_t *path)
{
    const wchar_t *p = path, *last = path;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') last = p + 1;
    return last;
}

wchar_t *path_join(const wchar_t *dir, const wchar_t *rel)
{
    WBuf b = {0};
    wchar_t full[MAX_PATH * 4];
    DWORD n;
    int i;
    if (!rel) rel = L"";
    /* absolute? */
    if ((rel[0] && rel[1] == ':') || (rel[0] == '\\' && rel[1] == '\\') || !dir || !dir[0]) {
        wbuf_put(&b, rel, -1);
    } else {
        while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
        wbuf_put(&b, dir, -1);
        if (b.n && b.p[b.n - 1] != '\\' && b.p[b.n - 1] != '/') wbuf_putc(&b, '\\');
        if (rel[0] == '/' || rel[0] == '\\') rel++;
        wbuf_put(&b, rel, -1);
    }
    for (i = 0; i < b.n; i++) if (b.p[i] == '/') b.p[i] = '\\';
    n = GetFullPathNameW(b.p, (DWORD)(sizeof full / sizeof full[0]), full, NULL);
    if (n > 0 && n < sizeof full / sizeof full[0]) {
        wchar_t *r = _wcsdup(full);
        wbuf_free(&b);
        return r;
    }
    return b.p;
}

wchar_t *path_with_ext(const wchar_t *path, const wchar_t *ext)
{
    const wchar_t *name = path_name(path);
    const wchar_t *dot = wcsrchr(name, '.');
    int base = dot ? (int)(dot - path) : (int)wcslen(path);
    int el = (int)wcslen(ext);
    wchar_t *r = (wchar_t *)malloc((base + el + 1) * sizeof(wchar_t));
    memcpy(r, path, base * sizeof(wchar_t));
    memcpy(r + base, ext, (el + 1) * sizeof(wchar_t));
    return r;
}

/* ---------------- Compression ---------------- */

static size_t do_deflate(const void *data, size_t len, uint8_t **out, int zlib)
{
    struct sdefl *s;
    int bound, n;
    uint8_t *o;
    if (len > 0x7FFFFFF0) { *out = NULL; return 0; }
    s = (struct sdefl *)calloc(1, sizeof(struct sdefl));
    bound = sdefl_bound((int)len) + 16;
    o = (uint8_t *)malloc((size_t)bound);
    if (!s || !o) { free(s); free(o); *out = NULL; return 0; }
    n = zlib ? zsdeflate(s, o, data, (int)len, 6) : sdeflate(s, o, data, (int)len, 6);
    free(s);
    *out = o;
    return (size_t)n;
}

size_t zlib_compress(const void *data, size_t len, uint8_t **out) { return do_deflate(data, len, out, 1); }
size_t deflate_raw(const void *data, size_t len, uint8_t **out) { return do_deflate(data, len, out, 0); }

uint32_t crc32_calc(const void *data, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!init) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
        init = 1;
    }
    for (i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
