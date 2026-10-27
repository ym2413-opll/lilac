/*
 * zip.h - minimal ZIP archive writer (used for DOCX).
 */
#ifndef MV_ZIP_H
#define MV_ZIP_H

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZipWriter ZipWriter;

/* Archive is built in memory and written on zip_close(). */
ZipWriter *zip_new(void);
/* Add a file. name is UTF-8 with '/' separators. compress: 1 = deflate, 0 = store. */
int        zip_add(ZipWriter *z, const char *name, const void *data, size_t len, int compress);
/* Write to path and free the writer. Returns 0 on success. */
int        zip_close(ZipWriter *z, const wchar_t *path);
void       zip_discard(ZipWriter *z);

#ifdef __cplusplus
}
#endif
#endif
