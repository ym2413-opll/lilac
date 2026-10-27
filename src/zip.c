/*
 * zip.c - minimal in-memory ZIP writer (deflate or store), enough for DOCX.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "zip.h"

typedef struct ZipEntry {
    char    *name;
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} ZipEntry;

struct ZipWriter {
    Buf       data;
    ZipEntry *ent;
    int       n, cap;
    uint16_t  dosTime, dosDate;
};

static void put16(Buf *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; buf_put(b, x, 2); }
static void put32(Buf *b, uint32_t v) { uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; buf_put(b, x, 4); }

ZipWriter *zip_new(void)
{
    ZipWriter *z = (ZipWriter *)calloc(1, sizeof *z);
    SYSTEMTIME st;
    GetLocalTime(&st);
    z->dosTime = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    z->dosDate = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    return z;
}

int zip_add(ZipWriter *z, const char *name, const void *data, size_t len, int compress)
{
    ZipEntry *e;
    uint8_t *comp = NULL;
    size_t clen = 0;
    size_t nlen = strlen(name);
    if (z->n == z->cap) {
        z->cap = z->cap ? z->cap * 2 : 16;
        z->ent = (ZipEntry *)realloc(z->ent, z->cap * sizeof(ZipEntry));
    }
    e = &z->ent[z->n++];
    memset(e, 0, sizeof *e);
    e->name = _strdup(name);
    e->crc = crc32_calc(data, len);
    e->usize = (uint32_t)len;
    e->offset = (uint32_t)z->data.n;
    e->method = 0;
    if (compress && len > 64) {
        clen = deflate_raw(data, len, &comp);
        if (comp && clen > 0 && clen < len) e->method = 8;
    }
    if (e->method == 8) e->csize = (uint32_t)clen;
    else e->csize = (uint32_t)len;

    put32(&z->data, 0x04034b50);
    put16(&z->data, 20);              /* version needed */
    put16(&z->data, 0x0800);          /* flags: UTF-8 names */
    put16(&z->data, e->method);
    put16(&z->data, z->dosTime);
    put16(&z->data, z->dosDate);
    put32(&z->data, e->crc);
    put32(&z->data, e->csize);
    put32(&z->data, e->usize);
    put16(&z->data, (uint16_t)nlen);
    put16(&z->data, 0);
    buf_put(&z->data, name, nlen);
    if (e->method == 8) buf_put(&z->data, comp, clen);
    else if (len) buf_put(&z->data, data, len);
    free(comp);
    return 0;
}

int zip_close(ZipWriter *z, const wchar_t *path)
{
    uint32_t cdStart = (uint32_t)z->data.n, cdSize;
    int i, rc;
    for (i = 0; i < z->n; i++) {
        ZipEntry *e = &z->ent[i];
        size_t nlen = strlen(e->name);
        put32(&z->data, 0x02014b50);
        put16(&z->data, 20);          /* version made by */
        put16(&z->data, 20);          /* version needed */
        put16(&z->data, 0x0800);
        put16(&z->data, e->method);
        put16(&z->data, z->dosTime);
        put16(&z->data, z->dosDate);
        put32(&z->data, e->crc);
        put32(&z->data, e->csize);
        put32(&z->data, e->usize);
        put16(&z->data, (uint16_t)nlen);
        put16(&z->data, 0);           /* extra */
        put16(&z->data, 0);           /* comment */
        put16(&z->data, 0);           /* disk */
        put16(&z->data, 0);           /* internal attrs */
        put32(&z->data, 0);           /* external attrs */
        put32(&z->data, e->offset);
        buf_put(&z->data, e->name, nlen);
    }
    cdSize = (uint32_t)z->data.n - cdStart;
    put32(&z->data, 0x06054b50);
    put16(&z->data, 0);
    put16(&z->data, 0);
    put16(&z->data, (uint16_t)z->n);
    put16(&z->data, (uint16_t)z->n);
    put32(&z->data, cdSize);
    put32(&z->data, cdStart);
    put16(&z->data, 0);
    rc = file_write_all(path, z->data.p, z->data.n);
    zip_discard(z);
    return rc;
}

void zip_discard(ZipWriter *z)
{
    int i;
    if (!z) return;
    for (i = 0; i < z->n; i++) free(z->ent[i].name);
    free(z->ent);
    buf_free(&z->data);
    free(z);
}
