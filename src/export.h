/*
 * export.h - document exporters.
 */
#ifndef MV_EXPORT_H
#define MV_EXPORT_H

#include "doc.h"
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write a paged Layout as PDF (embedded subset TrueType fonts, images, links, outline).
 * Returns 0 on success; on failure fills err (may be NULL). */
int pdf_write(const Layout *L, const char *path, char *err, int errcap);

/* Write a Doc as DOCX (Office Open XML). Images must be resolved (doc_resolve_images).
 * Returns 0 on success; on failure fills err (may be NULL). */
int docx_write(Doc *doc, const char *path, char *err, int errcap);

/* Convenience: lay out doc on A4 pages and write PDF. */
int export_pdf(Doc *doc, const char *path, char *err, int errcap);

typedef struct PngExportOptions {
    int width;    /* Output width in pixels (<= 0 uses default 1050; clamped to 320..4096). */
    int paged;    /* 0 = continuous single vertical image, 1 = split by A4-proportioned pages. */
    int dark;     /* 0 = light theme, 1 = dark theme. */
    int maxPages; /* Max pages allowed in paged mode (<= 0 uses default 1000). */
} PngExportOptions;

/* Compute how many pages will be produced for the given PNG options, or -1 on error. */
int export_png_page_count(Doc *doc, const PngExportOptions *opt);

/* Returns 1 if any paged output file (<stem>_1.png .. _<pageCount>.png) already exists.
 * If firstConflict != NULL, copies the first conflicting path into it. */
int export_png_check_overwrite(const char *path, int pageCount, char *firstConflict, size_t cap);

/* Render Doc as PNG (continuous or paged) and write to path (paged uses _1.png, _2.png, ...).
 * Returns 0 on success; on failure fills err (may be NULL). */
int export_png(Doc *doc, const char *path, const PngExportOptions *opt,
               int *outPagesWritten, char *err, int errcap);

#ifdef __cplusplus
}
#endif
#endif
