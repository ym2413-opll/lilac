/*
 * layout.h - layout engine: Doc -> positioned display list.
 *
 * Units are DIPs (1/96 inch). y grows downwards.
 * Continuous mode (pageHeight == 0): one tall page (page 0) used by the viewer.
 * Paged mode (pageHeight > 0): items carry a page index and page-local
 * coordinates (margins included); used by the PDF writer and printing.
 */
#ifndef MV_LAYOUT_H
#define MV_LAYOUT_H

#include "util.h"
#include "doc.h"
#include "font.h"
#include "image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ItemType {
    IT_GLYPHS = 0,  /* text run: font, size, n glyphs. (x,y) = baseline origin, w = advance width */
    IT_FILL,        /* filled rectangle (x,y,w,h), optional corner radius */
    IT_STROKE,      /* stroked rectangle outline (x,y,w,h), line width lw, optional radius */
    IT_LINE,        /* line from (x,y) to (x+w,y+h), width lw */
    IT_IMAGE,       /* image scaled into (x,y,w,h) */
    IT_DISC,        /* filled ellipse in box (x,y,w,h) */
    IT_CIRCLE,      /* stroked ellipse in box (x,y,w,h), width lw */
    IT_CHECK        /* check mark polyline inside box (x,y,w,h), width lw */
} ItemType;

#define IF_SYNTH_BOLD   0x01   /* emulate bold (font file is not bold) */
#define IF_SYNTH_ITALIC 0x02   /* emulate italic (skew) */

typedef struct Item {
    uint8_t   type;     /* ItemType */
    uint8_t   layer;    /* 0 = background (drawn first), 1 = foreground */
    uint16_t  flags;    /* IF_* */
    int       page;
    float     x, y, w, h;
    float     ty;       /* internal (pagination anchor) */
    uint32_t  color;    /* 0xRRGGBB */
    float     lw;       /* line width for STROKE/LINE/CIRCLE/CHECK */
    float     radius;   /* corner radius for FILL/STROKE */

    /* IT_GLYPHS */
    Font     *font;
    float     size;     /* em size in DIP */
    int       n;        /* glyph count */
    uint16_t *gids;     /* glyph indices */
    float    *dx;       /* x offset of each glyph relative to x */
    int      *toff;     /* offset in Layout.text of each glyph's source text */
    int       tend;     /* end offset in Layout.text of this run */
    int       link;     /* Doc link index or -1 */

    /* IT_IMAGE */
    Image    *img;
} Item;

typedef struct LinkArea {
    int   page;
    float x, y, w, h;
    int   link;         /* index into Doc.links */
} LinkArea;

typedef struct Anchor {
    wchar_t *name;      /* slug, e.g. "introduction" or "fn-1" */
    wchar_t *title;     /* heading text (NULL for non-heading anchors) */
    int      level;     /* heading level 1..6, 0 = non-heading anchor */
    int      page;
    float    y;         /* page-local y of the top of the target */
} Anchor;

typedef struct LayoutOptions {
    float width;            /* page / viewport width (DIP) */
    float pageHeight;       /* 0 = continuous */
    float marginL, marginR, marginT, marginB;
    float maxContentWidth;  /* 0 = unlimited; content is centered if narrower than available */
    float baseSize;         /* body font size (DIP), e.g. 16 */
    int   dark;             /* dark colour theme (viewer only) */
} LayoutOptions;

typedef struct Layout {
    LayoutOptions opt;
    Doc      *doc;
    Item     *items;
    int       nitems, capitems;
    LinkArea *links;
    int       nlinks, caplinks;
    Anchor   *anchors;
    int       nanchors, capanchors;
    int       npages;       /* >= 1 */
    float     pageWidth;    /* = opt.width */
    float     pageHeight;   /* paged: opt.pageHeight; continuous: total document height */
    wchar_t  *text;         /* plain text of the document, for selection / search / ToUnicode */
    int       textlen, textcap;
    uint32_t  background;   /* page background colour */
    Arena     arena;
} Layout;

Layout *layout_build(Doc *doc, const LayoutOptions *opt);
void    layout_free(Layout *L);

/* Hit testing (continuous or paged). Returns text offset nearest to point, or -1. */
int     layout_hit_text(const Layout *L, int page, float x, float y);
/* Link index at point, or -1. */
int     layout_hit_link(const Layout *L, int page, float x, float y);
/* Find the anchor by name (case-insensitive), NULL if none. */
const Anchor *layout_find_anchor(const Layout *L, const wchar_t *name);
/* Position (page, y) of the glyph containing text offset; returns 0 if found. */
int     layout_text_pos(const Layout *L, int off, int *page, float *x, float *y);

#ifdef __cplusplus
}
#endif
#endif
