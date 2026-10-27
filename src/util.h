/*
 * util.h - small shared helpers: arena allocator, growable byte buffer,
 * UTF-8/UTF-16 conversion, file I/O and zlib compression.
 */
#ifndef MV_UTIL_H
#define MV_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Arena allocator (everything freed at once) ---------- */
typedef struct ArenaBlock ArenaBlock;
typedef struct Arena {
    ArenaBlock *head;
} Arena;

void    *arena_alloc(Arena *a, size_t size);          /* zero-initialised, 16-byte aligned */
void     arena_free(Arena *a);
wchar_t *arena_wcsndup(Arena *a, const wchar_t *s, int len); /* len < 0 => wcslen */
char    *arena_strndup(Arena *a, const char *s, int len);

/* ---------- Growable byte buffer ---------- */
typedef struct Buf {
    char  *p;
    size_t n, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_put(Buf *b, const void *data, size_t len);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
/* Append UTF-16 text converted to UTF-8 (len < 0 => wcslen). */
void buf_put_w(Buf *b, const wchar_t *s, int len);
/* Append UTF-16 text converted to UTF-8 and XML-escaped (&, <, >, ", '). Control chars dropped. */
void buf_put_xml_w(Buf *b, const wchar_t *s, int len);
/* Append UTF-8 text XML-escaped. */
void buf_put_xml(Buf *b, const char *s, int len);
void buf_free(Buf *b);

/* ---------- Growable wide-char buffer ---------- */
typedef struct WBuf {
    wchar_t *p;
    int      n, cap;
} WBuf;

void wbuf_put(WBuf *b, const wchar_t *s, int len);
void wbuf_putc(WBuf *b, wchar_t c);
void wbuf_free(WBuf *b);

/* ---------- Encoding ---------- */
/* Returned strings are malloc()ed and zero-terminated; *outlen (optional) excludes the terminator. */
char    *utf16_to_utf8(const wchar_t *s, int len, int *outlen);
wchar_t *utf8_to_utf16(const char *s, int len, int *outlen);
/* Decode one code point from UTF-16 at s[*i] (advances *i). */
uint32_t utf16_next(const wchar_t *s, int len, int *i);

/* ---------- Files ---------- */
int  file_read_all(const wchar_t *path, uint8_t **data, size_t *len); /* 0 = ok; data malloc()ed (+1 NUL byte) */
int  file_write_all(const wchar_t *path, const void *data, size_t len); /* 0 = ok */
/* Directory part of a path (malloc()ed, no trailing slash). */
wchar_t *path_dir(const wchar_t *path);
/* File name part (pointer into path). */
const wchar_t *path_name(const wchar_t *path);
/* Join dir + relative (handles '/' separators, strips "./"); malloc()ed. */
wchar_t *path_join(const wchar_t *dir, const wchar_t *rel);
/* Replace extension (ext includes dot, e.g. L".pdf"); malloc()ed. */
wchar_t *path_with_ext(const wchar_t *path, const wchar_t *ext);

/* ---------- Compression ---------- */
/* zlib-wrapped deflate (RFC 1950) - for PDF FlateDecode. Returns length, *out malloc()ed. */
size_t zlib_compress(const void *data, size_t len, uint8_t **out);
/* raw deflate (RFC 1951) - for ZIP. Returns length, *out malloc()ed. */
size_t deflate_raw(const void *data, size_t len, uint8_t **out);
uint32_t crc32_calc(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
