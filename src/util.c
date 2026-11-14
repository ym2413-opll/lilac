/*
 * util.c - shared helpers (see util.h).
 * All internal strings and paths are UTF-8 (char *); UTF-16 (wchar_t *) is used
 * strictly at Win32 API boundaries.
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

char *arena_strndup(Arena *a, const char *s, int len)
{
    char *r;
    if (!s) return NULL;
    if (len < 0) len = (int)strlen(s);
    r = (char *)arena_alloc(a, (size_t)len + 1);
    memcpy(r, s, (size_t)len);
    r[len] = 0;
    return r;
}

/* ---------------- Buf ---------------- */

void buf_reserve(Buf *b, size_t extra)
{
    size_t needed;
    if (extra > SIZE_MAX - b->n - 1) { fprintf(stderr, "buffer overflow\n"); abort(); }
    needed = b->n + extra + 1;
    if (needed > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char *np;
        while (nc < needed) {
            if (nc > SIZE_MAX / 2) { nc = needed; break; }
            nc *= 2;
        }
        np = (char *)realloc(b->p, nc);
        if (!np) { fprintf(stderr, "out of memory\n"); abort(); }
        b->p = np;
        b->cap = nc;
    }
}

void buf_put(Buf *b, const void *data, size_t len)
{
    buf_reserve(b, len);
    if (len) memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
}

void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }
void buf_puts(Buf *b, const char *s) { if (s) buf_put(b, s, strlen(s)); }

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

void buf_put_cp(Buf *b, uint32_t cp)
{
    char u8[4];
    int n = utf8_encode(cp, u8);
    buf_put(b, u8, (size_t)n);
}

void buf_put_xml(Buf *b, const char *s, int len)
{
    int i, st = 0;
    if (!s) return;
    if (len < 0) len = (int)strlen(s);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = NULL;
        int skip = 1;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        case 0xEF:
            /* Filter U+FFFE (EF BF BE) and U+FFFF (EF BF BF) which are invalid in XML 1.0 */
            if (i + 2 < len && (unsigned char)s[i + 1] == 0xBF &&
                ((unsigned char)s[i + 2] == 0xBE || (unsigned char)s[i + 2] == 0xBF)) {
                rep = "";
                skip = 3;
            }
            break;
        default:
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') rep = "";
        }
        if (rep) {
            buf_put(b, s + st, (size_t)(i - st));
            buf_puts(b, rep);
            st = i + skip;
            i += skip - 1;
        }
    }
    buf_put(b, s + st, (size_t)(len - st));
}

void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---------------- UTF-8 & Win32 UTF-16 Boundary Encoding ---------------- */

uint32_t utf8_next(const char *s, int len, int *i)
{
    int pos = *i;
    uint32_t c;
    if (pos >= len) return 0;
    c = (unsigned char)s[pos++];
    if (c < 0x80) {
        *i = pos;
        return c;
    } else if ((c & 0xE0) == 0xC0 && pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)s[pos] & 0x3F);
        *i = pos + 1;
        return cp >= 0x80 ? cp : 0xFFFD;
    } else if ((c & 0xF0) == 0xE0 && pos + 1 < len &&
               ((unsigned char)s[pos] & 0xC0) == 0x80 &&
               ((unsigned char)s[pos + 1] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0F) << 12) |
                      (((unsigned char)s[pos] & 0x3F) << 6) |
                      ((unsigned char)s[pos + 1] & 0x3F);
        *i = pos + 2;
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
        return cp;
    } else if ((c & 0xF8) == 0xF0 && pos + 2 < len &&
               ((unsigned char)s[pos] & 0xC0) == 0x80 &&
               ((unsigned char)s[pos + 1] & 0xC0) == 0x80 &&
               ((unsigned char)s[pos + 2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07) << 18) |
                      (((unsigned char)s[pos] & 0x3F) << 12) |
                      (((unsigned char)s[pos + 1] & 0x3F) << 6) |
                      ((unsigned char)s[pos + 2] & 0x3F);
        *i = pos + 3;
        if (cp < 0x10000 || cp > 0x10FFFF) return 0xFFFD;
        return cp;
    }
    *i = pos;
    return 0xFFFD;
}

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = (char)(signed char)-17; out[1] = (char)(signed char)-65; out[2] = (char)(signed char)-67; /* U+FFFD EF BF BD */
    return 3;
}

int ascii_icmp(const char *a, const char *b)
{
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return _stricmp(a, b);
}

int ascii_nicmp(const char *a, const char *b, size_t n)
{
    if (!n) return 0;
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return _strnicmp(a, b, n);
}

char *utf16_to_utf8(const wchar_t *s, int len, int *outlen)
{
    int n;
    char *r;
    if (!s) s = L"";
    if (len < 0) len = (int)wcslen(s);
    n = len ? WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL) : 0;
    r = (char *)malloc((size_t)n + 1);
    if (!r) return NULL;
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
    if (!r) return NULL;
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, len, r, n);
    r[n] = 0;
    if (outlen) *outlen = n;
    return r;
}

/* ---------------- Files & Paths (UTF-8 internally) ---------------- */

int file_read_all(const char *path, uint8_t **data, size_t *len)
{
    HANDLE h;
    LARGE_INTEGER sz;
    uint8_t *p;
    DWORD got = 0;
    size_t total = 0;
    wchar_t *wpath;
    if (!path || !path[0]) return -1;
    wpath = utf8_to_utf16(path, -1, NULL);
    if (!wpath) return -1;
    h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 0x10000000) { CloseHandle(h); return -1; }
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

int file_write_all(const char *path, const void *data, size_t len)
{
    wchar_t *wpath;
    HANDLE h;
    DWORD wr = 0;
    size_t total = 0;
    if (!path || !path[0]) return -1;
    wpath = utf8_to_utf16(path, -1, NULL);
    if (!wpath) return -1;
    h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return -1;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x10000000 ? 0x10000000 : (len - total));
        if (!WriteFile(h, (const char *)data + total, chunk, &wr, NULL) || !wr) { CloseHandle(h); return -1; }
        total += wr;
    }
    CloseHandle(h);
    return 0;
}

int file_exists(const char *path)
{
    wchar_t *wpath;
    DWORD attr;
    if (!path || !path[0]) return 0;
    wpath = utf8_to_utf16(path, -1, NULL);
    if (!wpath) return 0;
    attr = GetFileAttributesW(wpath);
    free(wpath);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

char *path_full(const char *path)
{
    wchar_t *wpath;
    DWORD n;
    if (!path) return utf16_to_utf8(L"", 0, NULL);
    wpath = utf8_to_utf16(path, -1, NULL);
    if (!wpath) return _strdup(path);
    n = GetFullPathNameW(wpath, 0, NULL, NULL);
    if (n > 0) {
        wchar_t *wfull = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (wfull) {
            DWORD actual = GetFullPathNameW(wpath, n, wfull, NULL);
            if (actual > 0 && actual < n) {
                char *res = utf16_to_utf8(wfull, (int)actual, NULL);
                free(wfull);
                free(wpath);
                return res ? res : _strdup(path);
            }
            free(wfull);
        }
    }
    free(wpath);
    return _strdup(path);
}

char *path_dir(const char *path)
{
    const char *name;
    int n;
    char *r;
    if (!path) return _strdup("");
    name = path_name(path);
    n = (int)(name - path);
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    if (n == 2 && path[1] == ':') { n = 3; } /* keep "C:\" */
    r = (char *)malloc((size_t)n + 1);
    if (!r) return NULL;
    memcpy(r, path, (size_t)n);
    r[n] = 0;
    if (n == 3 && r[2] != '\\') r[2] = '\\';
    return r;
}

const char *path_name(const char *path)
{
    const char *p = path, *last = path;
    if (!path) return "";
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') last = p + 1;
    return last;
}

char *path_join(const char *dir, const char *rel)
{
    Buf b = {0};
    char *full;
    size_t i;
    if (!rel) rel = "";
    /* absolute? */
    if ((rel[0] && rel[1] == ':') || (rel[0] == '\\' && rel[1] == '\\') || !dir || !dir[0]) {
        buf_puts(&b, rel);
    } else {
        while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
        buf_puts(&b, dir);
        if (b.n && b.p[b.n - 1] != '\\' && b.p[b.n - 1] != '/') buf_putc(&b, '\\');
        if (rel[0] == '/' || rel[0] == '\\') rel++;
        buf_puts(&b, rel);
    }
    for (i = 0; i < b.n; i++) if (b.p[i] == '/') b.p[i] = '\\';
    full = path_full(b.p ? b.p : "");
    buf_free(&b);
    return full;
}

char *path_with_ext(const char *path, const char *ext)
{
    const char *name;
    const char *dot;
    size_t base;
    size_t el;
    char *r;
    if (!path || !ext) return NULL;
    name = path_name(path);
    dot = strrchr(name, '.');
    base = dot ? (size_t)(dot - path) : strlen(path);
    el = strlen(ext);
    r = (char *)malloc(base + el + 1);
    if (!r) return NULL;
    memcpy(r, path, base);
    memcpy(r + base, ext, el + 1);
    return r;
}

int path_is_within(const char *child, const char *dir)
{
    char *cf, *df;
    size_t dlen, clen;
    int ok = 0;
    if (!child || !child[0] || !dir || !dir[0]) return 0;
    if (path_is_remote(child)) return 0;
    cf = path_full(child);
    df = path_full(dir);
    if (cf && df && cf[0] && df[0]) {
        dlen = strlen(df);
        clen = strlen(cf);
        while (dlen > 0 && (df[dlen - 1] == '\\' || df[dlen - 1] == '/')) {
            if (dlen == 3 && df[1] == ':') break; /* preserve "C:\" */
            df[--dlen] = 0;
        }
        /* Reject if path canonicalization failed (e.g. contains relative "..") */
        if (clen >= dlen && ascii_nicmp(cf, df, dlen) == 0) {
            if (clen == dlen) ok = 1;
            else if (dlen == 3 && df[1] == ':' && df[2] == '\\') ok = 1;
            else if (cf[dlen] == '\\' || cf[dlen] == '/') ok = 1;
        }
    }
    free(cf);
    free(df);
    return ok;
}

int path_is_remote(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (!*s) return 0;

    /* HTTP or HTTPS */
    if (!ascii_nicmp(s, "http://", 7) || !ascii_nicmp(s, "https://", 8))
        return 1;

    /* UNC or network path: \\ or // or \/ or /\ */
    if ((s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/'))
        return 1;

    /* Device paths: \\?\ or \\.\ or //?/ or //./ */
    if ((s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/') &&
        (s[2] == '?' || s[2] == '.') && (s[3] == '\\' || s[3] == '/'))
        return 1;

    /* file:// with host or UNC (e.g. file://host/..., file:////...) */
    if (!ascii_nicmp(s, "file:", 5)) {
        const char *p = s + 5;
        if (!ascii_nicmp(p, "///", 3)) {
            p += 3;
            if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'))
                return 1;
            return 0;
        } else if (!ascii_nicmp(p, "//localhost/", 12)) {
            p += 12;
            if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'))
                return 1;
            return 0;
        } else if (!ascii_nicmp(p, "//", 2)) {
            return 1;
        } else if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
            return 1;
        }
    }

    return 0;
}

int link_is_safe_external(const char *s)
{
    const char *p;
    int isMailto = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (ascii_nicmp(s, "http://", 7) == 0) p = s + 7;
    else if (ascii_nicmp(s, "https://", 8) == 0) p = s + 8;
    else if (ascii_nicmp(s, "mailto:", 7) == 0) { p = s + 7; isMailto = 1; }
    else return 0;
    if (!*p) return 0;
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) return 0;
        if (isMailto && (c == '"' || c == '<' || c == '>' || c == '`' || c == '\\')) return 0;
        if (isMailto && c == '%' && p[1] == '0' && p[2] == '0') return 0;
    }
    return 1;
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

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
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
    if (p) {
        for (i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32_calc(const void *data, size_t len)
{
    return crc32_update(0u, data, len);
}
