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

#ifdef __cplusplus
}
#endif
#endif
