/*
 * util.h - small shared helpers: arena allocator, growable byte buffer,
 * UTF-8 encoding (with UTF-16 conversion strictly for Win32 API boundaries),
 * file I/O and zlib compression.
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

void *arena_alloc(Arena *a, size_t size);                /* zero-initialised, 16-byte aligned */
void  arena_free(Arena *a);
char *arena_strndup(Arena *a, const char *s, int len);   /* len < 0 => strlen */

/* ---------- Growable UTF-8 / byte buffer ---------- */
typedef struct Buf {
    char  *p;
    size_t n, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_put(Buf *b, const void *data, size_t len);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
/* Append one Unicode code point encoded as UTF-8. */
void buf_put_cp(Buf *b, uint32_t cp);
/* Append UTF-8 text XML-escaped (&, <, >, ", '). Control chars dropped. len < 0 => strlen. */
void buf_put_xml(Buf *b, const char *s, int len);
void buf_free(Buf *b);

/* ---------- UTF-8 & Win32 UTF-16 Boundary Encoding ---------- */
/* Decode one Unicode code point from UTF-8 at s[*i] (advances *i). */
uint32_t utf8_next(const char *s, int len, int *i);
/* Encode one Unicode code point as 1..4 UTF-8 bytes into out. Returns byte count. */
int      utf8_encode(uint32_t cp, char out[4]);
/* Case-insensitive ASCII/UTF-8 comparison helpers. */
int      ascii_icmp(const char *a, const char *b);
int      ascii_nicmp(const char *a, const char *b, size_t n);

/* Win32 API boundary conversion helpers.
 * Returned strings are malloc()ed and zero-terminated; *outlen (optional) excludes terminator. */
char    *utf16_to_utf8(const wchar_t *s, int len, int *outlen);
wchar_t *utf8_to_utf16(const char *s, int len, int *outlen);

/* ---------- Files & Paths (all paths are UTF-8 internally) ---------- */
int   file_read_all(const char *path, uint8_t **data, size_t *len); /* 0 = ok; data malloc()ed (+1 NUL byte) */
int   file_write_all(const char *path, const void *data, size_t len); /* 0 = ok */
int   file_exists(const char *path); /* 1 if file/directory exists, 0 otherwise */
/* Full canonical path via Win32 GetFullPathNameW (malloc()ed UTF-8). */
char *path_full(const char *path);
/* Directory part of a UTF-8 path (malloc()ed, no trailing slash). */
char *path_dir(const char *path);
/* File name part (pointer into UTF-8 path). */
const char *path_name(const char *path);
/* Join dir + relative (handles '/' separators, strips "./"); malloc()ed UTF-8. */
char *path_join(const char *dir, const char *rel);
/* Replace extension (ext includes dot, e.g. ".pdf"); malloc()ed UTF-8. */
char *path_with_ext(const char *path, const char *ext);
/* Returns 1 if child resides inside (or equals) directory dir on Windows, 0 otherwise. */
int   path_is_within(const char *child, const char *dir);
/* Returns 1 if path is a remote URL (http/https), UNC path (\\ or //), device path, or file:// with host. */
int   path_is_remote(const char *path);
/* Returns 1 if link target is a safe external URI scheme (http://, https://, mailto:) with no control chars. */
int   link_is_safe_external(const char *s);

/* ---------- Compression ---------- */
/* zlib-wrapped deflate (RFC 1950) - for PDF FlateDecode. Returns length, *out malloc()ed. */
size_t   zlib_compress(const void *data, size_t len, uint8_t **out);
/* raw deflate (RFC 1951) - for ZIP. Returns length, *out malloc()ed. */
size_t   deflate_raw(const void *data, size_t len, uint8_t **out);
uint32_t crc32_calc(const void *data, size_t len);
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
