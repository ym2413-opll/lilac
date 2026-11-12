/*
 * font.h - TrueType font access through GDI (GetFontData).
 *
 * Fonts are installed Windows fonts. We parse the TrueType tables ourselves
 * (cmap, hmtx, head, hhea, OS/2, name) so that layout metrics are exact and
 * identical for the screen renderer (GDI, glyph indices) and the PDF writer
 * (embedded subset fonts).
 *
 * All face names and fallback chains are UTF-8 internally.
 */
#ifndef MV_FONT_H
#define MV_FONT_H

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Font {
    int      id;            /* index in the global font table (0..n-1) */
    char     face[64];      /* GDI face name in UTF-8, e.g. "Segoe UI" */
    int      bold, italic;  /* requested style */
    int      synthBold;     /* requested bold but the font file is not bold (must emulate) */
    int      synthItalic;   /* requested italic but the font file is not italic (must emulate) */
    int      isTrueType;    /* has 'glyf' outlines (embeddable as CIDFontType2) */

    int      upem;          /* head.unitsPerEm */
    int      ascent;        /* OS/2 usWinAscent (font units, positive) */
    int      descent;       /* OS/2 usWinDescent (font units, positive) */
    int      capHeight;     /* OS/2 sCapHeight or estimate */
    int      xMin, yMin, xMax, yMax; /* head bbox */
    int      italicAngle;   /* post.italicAngle (integer degrees) */
    int      fixedPitch;    /* post.isFixedPitch */
    int      weightClass;   /* OS/2 usWeightClass */
    int      underlinePos;  /* post.underlinePosition (font units, negative = below baseline) */
    int      underlineThickness;
    char     psName[64];    /* PostScript name (name ID 6), ASCII, no spaces */

    int       numGlyphs;
    uint16_t *adv;          /* advance width per glyph, numGlyphs entries (font units) */

    /* cmap (internal) */
    uint8_t  *cmapData;
    uint32_t  cmapLen;
    uint32_t  cmapSub;      /* offset of the selected subtable */
    int       cmapFormat;   /* 4 or 12 */

    void     *hfonts;       /* internal GDI HFONT cache */
} Font;

/* Font families used by the layout engine. */
typedef enum FontFamily {
    FAM_BODY = 0,   /* Segoe UI -> Malgun Gothic -> ... */
    FAM_MONO,       /* Consolas -> D2Coding/Malgun Gothic -> ... */
    FAM_MATH,       /* Cambria Math -> Cambria -> Times New Roman -> Segoe UI Symbol -> ... */
    FAM_COUNT
} FontFamily;

void  font_init(void);
void  font_shutdown(void);

/* Get (cached) font by UTF-8 face/style, NULL if not installed or unusable. */
Font *font_get(const char *face, int bold, int italic);
/* Number of fonts in the global table and access by id (for the PDF writer). */
int   font_count(void);
Font *font_by_id(int id);

/* Glyph index for a Unicode code point, 0 if not present. */
int   font_glyph(Font *f, uint32_t cp);
/* Advance width of a glyph in font units. */
static __inline int font_adv(const Font *f, int gid) {
    return (gid >= 0 && gid < f->numGlyphs) ? f->adv[gid] : (f->numGlyphs ? f->adv[0] : 0);
}

/* Pick a font of the family that contains cp (fallback chain); *gid receives the glyph.
 * Never returns NULL: if no font has the glyph, returns the primary font with gid 0 (.notdef). */
Font *font_resolve(int family, int bold, int italic, uint32_t cp, int *gid);
/* Primary font for a family/style (used for line metrics). */
Font *font_primary(int family, int bold, int italic);

/* Set/get preferred UTF-8 font face for a family (FAM_BODY, FAM_MONO, FAM_MATH). */
void        font_set_family_face(int family, const char *face);
const char *font_get_family_face(int family);

/* Set/get prioritized fallback UTF-8 font chain for a family (comma/semicolon-separated). */
void        font_set_family_chain(int family, const char *chain);
const char *font_get_family_chain(int family);

/* Read a raw table from the font file (tag e.g. 'glyf' as 0x676C7966 big-endian order
 * written as MV_TAG('g','l','y','f')). Returns malloc()ed buffer or NULL. */
uint8_t *font_table(const Font *f, uint32_t tag, uint32_t *len);
#define MV_TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* GDI font handle (HFONT) of the given pixel em-size, cached. */
void *font_hfont(Font *f, int pixelSize);

#ifdef __cplusplus
}
#endif
#endif
