/*
 * pdf.c - Self-contained PDF 1.7 writer with subset TrueType fonts & Korean support.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "export.h"
#include "layout.h"
#include "font.h"
#include "image.h"
#include "util.h"

#define MAX_PDF_OBJS 4096

typedef struct PdfWriter {
    Buf      buf;
    uint32_t xref[MAX_PDF_OBJS];
    int      nextObj;
    int      pageObjs[256];
    int      pageCount;
} PdfWriter;

static int pdf_new_obj(PdfWriter *pw)
{
    int id = ++pw->nextObj;
    if (id < MAX_PDF_OBJS) {
        pw->xref[id] = (uint32_t)pw->buf.n;
    }
    buf_printf(&pw->buf, "%d 0 obj\n", id);
    return id;
}

static void pdf_end_obj(PdfWriter *pw)
{
    buf_puts(&pw->buf, "endobj\n\n");
}

static void pdf_put_stream(PdfWriter *pw, const void *data, size_t len, int compress)
{
    if (compress && len > 64) {
        uint8_t *comp = NULL;
        size_t clen = zlib_compress(data, len, &comp);
        if (comp && clen > 0 && clen < len) {
            buf_printf(&pw->buf, "<< /Filter /FlateDecode /Length %u >>\nstream\n", (unsigned)clen);
            buf_put(&pw->buf, comp, clen);
            buf_puts(&pw->buf, "\nendstream\n");
            free(comp);
            return;
        }
        free(comp);
    }
    buf_printf(&pw->buf, "<< /Length %u >>\nstream\n", (unsigned)len);
    if (len && data) buf_put(&pw->buf, data, len);
    buf_puts(&pw->buf, "\nendstream\n");
}

/* ---------------- TrueType Font Subsetting ---------------- */

typedef struct UsedGlyphs {
    uint8_t *flags; /* bitmask for up to numGlyphs */
    int      maxGlyph;
} UsedGlyphs;

static void mark_glyph(UsedGlyphs *ug, int gid)
{
    if (gid >= 0 && gid < ug->maxGlyph) {
        ug->flags[gid >> 3] |= (1 << (gid & 7));
    }
}

static int is_glyph_marked(const UsedGlyphs *ug, int gid)
{
    if (gid >= 0 && gid < ug->maxGlyph) {
        return (ug->flags[gid >> 3] & (1 << (gid & 7))) != 0;
    }
    return 0;
}

/* Parse composite glyph references recursively to mark component glyphs */
static void scan_composite_glyphs(UsedGlyphs *ug, const uint8_t *glyf, uint32_t glyfLen,
                                  const uint32_t *loca, int numGlyphs)
{
    int g;
    int added = 1;

    while (added) {
        added = 0;
        for (g = 0; g < numGlyphs; g++) {
            if (is_glyph_marked(ug, g)) {
                uint32_t off = loca[g];
                uint32_t len = loca[g + 1] - off;
                if (len >= 10 && off + len <= glyfLen) {
                    int16_t numContours = (int16_t)((glyf[off] << 8) | glyf[off + 1]);
                    if (numContours < 0) { /* Composite glyph */
                        uint32_t p = off + 10;
                        uint16_t flags, compGid;
                        do {
                            if (p + 4 > off + len) break;
                            flags = (uint16_t)((glyf[p] << 8) | glyf[p + 1]);
                            compGid = (uint16_t)((glyf[p + 2] << 8) | glyf[p + 3]);
                            p += 4;
                            if (compGid < (uint16_t)numGlyphs && !is_glyph_marked(ug, compGid)) {
                                mark_glyph(ug, compGid);
                                added = 1;
                            }
                            if (flags & 0x0001) p += 4; /* ARG_1_AND_2_ARE_WORDS */
                            else p += 2;
                            if (flags & 0x0008) p += 2; /* WE_HAVE_A_SCALE */
                            else if (flags & 0x0040) p += 4; /* WE_HAVE_AN_X_AND_Y_SCALE */
                            else if (flags & 0x0080) p += 8; /* WE_HAVE_A_TWO_BY_TWO */
                        } while (flags & 0x0020); /* MORE_COMPONENTS */
                    }
                }
            }
        }
    }
}

static uint32_t calc_table_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t n = (len + 3) / 4;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t v = 0;
        uint32_t off = i * 4;
        if (off < len) v |= (uint32_t)data[off] << 24;
        if (off + 1 < len) v |= (uint32_t)data[off + 1] << 16;
        if (off + 2 < len) v |= (uint32_t)data[off + 2] << 8;
        if (off + 3 < len) v |= (uint32_t)data[off + 3];
        sum += v;
    }
    return sum;
}

static uint8_t *build_subset_font(Font *f, const UsedGlyphs *ug, size_t *outLen)
{
    uint8_t *head = NULL, *hhea = NULL, *maxp = NULL, *os2 = NULL, *hmtx = NULL, *loca = NULL, *glyf = NULL;
    uint32_t headLen = 0, hheaLen = 0, maxpLen = 0, os2Len = 0, hmtxLen = 0, locaLen = 0, glyfLen = 0;
    uint32_t *oldLoca = NULL;
    uint32_t *newLoca = NULL;
    uint8_t *newGlyf = NULL;
    size_t newGlyfSize = 0, newGlyfCap = 65536;
    int isLocaLong = 1;
    int g;
    Buf sfnt = {0};

    head = font_table(f, MV_TAG('h','e','a','d'), &headLen);
    hhea = font_table(f, MV_TAG('h','h','e','a'), &hheaLen);
    maxp = font_table(f, MV_TAG('m','a','x','p'), &maxpLen);
    os2  = font_table(f, MV_TAG('O','S','/','2'), &os2Len);
    hmtx = font_table(f, MV_TAG('h','m','t','x'), &hmtxLen);
    loca = font_table(f, MV_TAG('l','o','c','a'), &locaLen);
    glyf = font_table(f, MV_TAG('g','l','y','f'), &glyfLen);

    if (!head || !hhea || !maxp || !hmtx || !loca || !glyf) {
        free(head); free(hhea); free(maxp); free(os2); free(hmtx); free(loca); free(glyf);
        return NULL;
    }

    isLocaLong = (head[51] != 0);
    oldLoca = (uint32_t *)malloc((f->numGlyphs + 1) * sizeof(uint32_t));
    newLoca = (uint32_t *)malloc((f->numGlyphs + 1) * sizeof(uint32_t));
    newGlyf = (uint8_t *)malloc(newGlyfCap);

    for (g = 0; g <= f->numGlyphs; g++) {
        if (isLocaLong) {
            oldLoca[g] = ((uint32_t)loca[g * 4] << 24) | ((uint32_t)loca[g * 4 + 1] << 16) |
                         ((uint32_t)loca[g * 4 + 2] << 8) | (uint32_t)loca[g * 4 + 3];
        } else {
            oldLoca[g] = (uint32_t)(((loca[g * 2] << 8) | loca[g * 2 + 1]) * 2);
        }
    }

    scan_composite_glyphs((UsedGlyphs *)ug, glyf, glyfLen, oldLoca, f->numGlyphs);

    /* Construct subset glyf and long loca */
    for (g = 0; g < f->numGlyphs; g++) {
        newLoca[g] = (uint32_t)newGlyfSize;
        if (is_glyph_marked(ug, g)) {
            uint32_t gOff = oldLoca[g];
            uint32_t gLen = oldLoca[g + 1] - gOff;
            if (gLen > 0 && gOff + gLen <= glyfLen) {
                while (newGlyfSize + gLen + 4 > newGlyfCap) {
                    newGlyfCap *= 2;
                    newGlyf = (uint8_t *)realloc(newGlyf, newGlyfCap);
                }
                memcpy(newGlyf + newGlyfSize, glyf + gOff, gLen);
                newGlyfSize += gLen;
                /* Pad to 4-byte boundary */
                while (newGlyfSize & 3) newGlyf[newGlyfSize++] = 0;
            }
        }
    }
    newLoca[f->numGlyphs] = (uint32_t)newGlyfSize;

    /* Rebuild loca table bytes */
    {
        uint32_t newLocaLen = (f->numGlyphs + 1) * 4;
        uint8_t *newLocaBytes = (uint8_t *)malloc(newLocaLen);
        for (g = 0; g <= f->numGlyphs; g++) {
            newLocaBytes[g * 4] = (uint8_t)(newLoca[g] >> 24);
            newLocaBytes[g * 4 + 1] = (uint8_t)(newLoca[g] >> 16);
            newLocaBytes[g * 4 + 2] = (uint8_t)(newLoca[g] >> 8);
            newLocaBytes[g * 4 + 3] = (uint8_t)newLoca[g];
        }

        /* Update head table: indexToLocFormat = 1 (long) */
        head[50] = 0;
        head[51] = 1;
        /* Clear checkSumAdjustment for recalculation */
        head[8] = head[9] = head[10] = head[11] = 0;

        /* Write SFNT TrueType container */
        {
            typedef struct TblEntry {
                uint32_t tag;
                const uint8_t *data;
                uint32_t len;
            } TblEntry;
            TblEntry tbls[7];
            int nTbls = 0, ti;
            uint32_t offset;

            if (os2) { tbls[nTbls].tag = MV_TAG('O','S','/','2'); tbls[nTbls].data = os2; tbls[nTbls++].len = os2Len; }
            tbls[nTbls].tag = MV_TAG('g','l','y','f'); tbls[nTbls].data = newGlyf; tbls[nTbls++].len = (uint32_t)newGlyfSize;
            tbls[nTbls].tag = MV_TAG('h','e','a','d'); tbls[nTbls].data = head; tbls[nTbls++].len = headLen;
            tbls[nTbls].tag = MV_TAG('h','h','e','a'); tbls[nTbls].data = hhea; tbls[nTbls++].len = hheaLen;
            tbls[nTbls].tag = MV_TAG('h','m','t','x'); tbls[nTbls].data = hmtx; tbls[nTbls++].len = hmtxLen;
            tbls[nTbls].tag = MV_TAG('l','o','c','a'); tbls[nTbls].data = newLocaBytes; tbls[nTbls++].len = newLocaLen;
            tbls[nTbls].tag = MV_TAG('m','a','x','p'); tbls[nTbls].data = maxp; tbls[nTbls++].len = maxpLen;

            /* Sort table entries by tag */
            for (ti = 0; ti < nTbls - 1; ti++) {
                int tj;
                for (tj = ti + 1; tj < nTbls; tj++) {
                    if (tbls[ti].tag > tbls[tj].tag) {
                        TblEntry tmp = tbls[ti]; tbls[ti] = tbls[tj]; tbls[tj] = tmp;
                    }
                }
            }

            /* Header: 0x00010000, numTables */
            {
                uint16_t nt = (uint16_t)nTbls;
                uint16_t maxPower2 = 1, entrySelector = 0;
                while (maxPower2 * 2 <= nt) { maxPower2 *= 2; entrySelector++; }
                uint16_t searchRange = maxPower2 * 16;
                uint16_t rangeShift = nt * 16 - searchRange;

                uint8_t hdr[12];
                hdr[0] = 0x00; hdr[1] = 0x01; hdr[2] = 0x00; hdr[3] = 0x00;
                hdr[4] = (uint8_t)(nt >> 8); hdr[5] = (uint8_t)nt;
                hdr[6] = (uint8_t)(searchRange >> 8); hdr[7] = (uint8_t)searchRange;
                hdr[8] = (uint8_t)(entrySelector >> 8); hdr[9] = (uint8_t)entrySelector;
                hdr[10] = (uint8_t)(rangeShift >> 8); hdr[11] = (uint8_t)rangeShift;
                buf_put(&sfnt, hdr, 12);
            }

            offset = 12 + nTbls * 16;
            for (ti = 0; ti < nTbls; ti++) {
                uint8_t rec[16];
                uint32_t csum = calc_table_checksum(tbls[ti].data, tbls[ti].len);
                rec[0] = (uint8_t)(tbls[ti].tag >> 24);
                rec[1] = (uint8_t)(tbls[ti].tag >> 16);
                rec[2] = (uint8_t)(tbls[ti].tag >> 8);
                rec[3] = (uint8_t)tbls[ti].tag;
                rec[4] = (uint8_t)(csum >> 24);
                rec[5] = (uint8_t)(csum >> 16);
                rec[6] = (uint8_t)(csum >> 8);
                rec[7] = (uint8_t)csum;
                rec[8] = (uint8_t)(offset >> 24);
                rec[9] = (uint8_t)(offset >> 16);
                rec[10] = (uint8_t)(offset >> 8);
                rec[11] = (uint8_t)offset;
                rec[12] = (uint8_t)(tbls[ti].len >> 24);
                rec[13] = (uint8_t)(tbls[ti].len >> 16);
                rec[14] = (uint8_t)(tbls[ti].len >> 8);
                rec[15] = (uint8_t)tbls[ti].len;
                buf_put(&sfnt, rec, 16);
                offset += (tbls[ti].len + 3) & ~3u;
            }

            for (ti = 0; ti < nTbls; ti++) {
                buf_put(&sfnt, tbls[ti].data, tbls[ti].len);
                while (sfnt.n & 3) buf_putc(&sfnt, 0);
            }
        }
        free(newLocaBytes);
    }

    free(oldLoca);
    free(newLoca);
    free(newGlyf);
    free(head); free(hhea); free(maxp); free(os2); free(hmtx); free(loca); free(glyf);

    *outLen = sfnt.n;
    return (uint8_t *)sfnt.p;
}

/* ---------------- Main PDF Exporter ---------------- */

int pdf_write(const Layout *L, const wchar_t *path, wchar_t *err, int errcap)
{
    PdfWriter pw;
    int catalogObj, pagesObj, infoObj;
    int fontCount = font_count();
    UsedGlyphs *usedGlyphs = NULL;
    int *fontType0Objs = NULL;
    int *imageObjs = NULL;
    int p, i, f;
    float ptW, ptH;

    if (!L || !path) {
        if (err && errcap > 0) wcsncpy_s(err, errcap, L"Invalid argument", _TRUNCATE);
        return -1;
    }

    memset(&pw, 0, sizeof pw);
    buf_puts(&pw.buf, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n\n");

    ptW = L->pageWidth * 0.75f;
    ptH = L->pageHeight * 0.75f;

    /* 1. Track used glyphs per font across all pages */
    usedGlyphs = (UsedGlyphs *)calloc(fontCount, sizeof(UsedGlyphs));
    fontType0Objs = (int *)calloc(fontCount, sizeof(int));
    for (f = 0; f < fontCount; f++) {
        Font *fnt = font_by_id(f);
        if (fnt && fnt->numGlyphs > 0) {
            usedGlyphs[f].maxGlyph = fnt->numGlyphs;
            usedGlyphs[f].flags = (uint8_t *)calloc((fnt->numGlyphs + 7) / 8, 1);
            mark_glyph(&usedGlyphs[f], 0); /* always mark .notdef */
        }
    }

    for (i = 0; i < L->nitems; i++) {
        const Item *it = &L->items[i];
        if (it->type == IT_GLYPHS && it->font) {
            int fid = it->font->id;
            if (fid >= 0 && fid < fontCount) {
                int g;
                for (g = 0; g < it->n; g++) {
                    mark_glyph(&usedGlyphs[fid], it->gids[g]);
                }
            }
        }
    }

    /* 2. Embed Image XObjects */
    {
        /* Collect unique images */
        Image *imgs[64];
        int nimgs = 0;
        for (i = 0; i < L->nitems; i++) {
            if (L->items[i].type == IT_IMAGE && L->items[i].img) {
                Image *img = L->items[i].img;
                int found = 0;
                int k;
                for (k = 0; k < nimgs; k++) {
                    if (imgs[k] == img) { found = 1; break; }
                }
                if (!found && nimgs < 64 && img->w > 0 && img->h > 0 && img->pixels) {
                    imgs[nimgs++] = img;
                }
            }
        }

        imageObjs = (int *)calloc(nimgs > 0 ? nimgs : 1, sizeof(int));
        for (i = 0; i < nimgs; i++) {
            Image *img = imgs[i];
            size_t total = (size_t)img->w * img->h;
            uint8_t *rgb = (uint8_t *)malloc(total * 3);
            uint8_t *alpha = img->hasAlpha ? (uint8_t *)malloc(total) : NULL;
            int maskObj = 0;
            size_t pxIdx;

            for (pxIdx = 0; pxIdx < total; pxIdx++) {
                uint32_t c = img->pixels[pxIdx];
                rgb[pxIdx * 3]     = (uint8_t)(c >> 16); /* R */
                rgb[pxIdx * 3 + 1] = (uint8_t)(c >> 8);  /* G */
                rgb[pxIdx * 3 + 2] = (uint8_t)c;         /* B */
                if (alpha) alpha[pxIdx] = (uint8_t)(c >> 24);
            }

            if (alpha) {
                maskObj = pdf_new_obj(&pw);
                buf_printf(&pw.buf, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8\n",
                           img->w, img->h);
                pdf_put_stream(&pw, alpha, total, 1);
                pdf_end_obj(&pw);
                free(alpha);
            }

            imageObjs[i] = pdf_new_obj(&pw);
            buf_printf(&pw.buf, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8\n",
                       img->w, img->h);
            if (maskObj) buf_printf(&pw.buf, "/SMask %d 0 R\n", maskObj);
            pdf_put_stream(&pw, rgb, total * 3, 1);
            pdf_end_obj(&pw);
            free(rgb);
        }
    }

    /* 3. Embed Fonts */
    for (f = 0; f < fontCount; f++) {
        Font *fnt = font_by_id(f);
        size_t fontFileSize = 0;
        uint8_t *fontFileData = NULL;
        int fontFileObj, descriptorObj, cidFontObj, toUnicodeObj;
        Buf toUni = {0};
        int gid;

        if (!fnt || !usedGlyphs[f].flags) continue;

        fontFileData = build_subset_font(fnt, &usedGlyphs[f], &fontFileSize);
        if (!fontFileData || fontFileSize == 0) continue;

        /* FontFile2 */
        fontFileObj = pdf_new_obj(&pw);
        buf_printf(&pw.buf, "<< /Length1 %u\n", (unsigned)fontFileSize);
        pdf_put_stream(&pw, fontFileData, fontFileSize, 1);
        pdf_end_obj(&pw);
        free(fontFileData);

        /* FontDescriptor */
        descriptorObj = pdf_new_obj(&pw);
        buf_printf(&pw.buf, "<< /Type /FontDescriptor /FontName /%s\n"
                            "/Flags %u /FontBBox [%d %d %d %d] /ItalicAngle %d\n"
                            "/Ascent %d /Descent %d /CapHeight %d /StemV %d\n"
                            "/FontFile2 %d 0 R >>\n",
                   fnt->psName,
                   32u | (fnt->fixedPitch ? 1u : 0u) | (fnt->italic ? 64u : 0u),
                   (int)((int64_t)fnt->xMin * 1000 / fnt->upem),
                   (int)((int64_t)fnt->yMin * 1000 / fnt->upem),
                   (int)((int64_t)fnt->xMax * 1000 / fnt->upem),
                   (int)((int64_t)fnt->yMax * 1000 / fnt->upem),
                   fnt->italicAngle,
                   (int)((int64_t)fnt->ascent * 1000 / fnt->upem),
                   (int)(-(int64_t)fnt->descent * 1000 / fnt->upem),
                   (int)((int64_t)fnt->capHeight * 1000 / fnt->upem),
                   fnt->bold ? 120 : 80,
                   fontFileObj);
        pdf_end_obj(&pw);

        /* CIDFontType2 */
        cidFontObj = pdf_new_obj(&pw);
        buf_printf(&pw.buf, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s\n"
                            "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>\n"
                            "/FontDescriptor %d 0 R /CIDToGIDMap /Identity\n"
                            "/DW 1000 /W [", fnt->psName, descriptorObj);
        for (gid = 0; gid < fnt->numGlyphs; gid++) {
            if (is_glyph_marked(&usedGlyphs[f], gid)) {
                int w = (int)((int64_t)font_adv(fnt, gid) * 1000 / fnt->upem);
                buf_printf(&pw.buf, " %d [%d]", gid, w);
            }
        }
        buf_puts(&pw.buf, " ] >>\n");
        pdf_end_obj(&pw);

        /* ToUnicode CMap */
        buf_puts(&toUni, "/CIDInit /ProcSet findresource begin\n"
                         "12 dict begin\n"
                         "begincmap\n"
                         "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                         "/CMapName /Custom-ToUnicode def\n"
                         "/CMapType 2 def\n"
                         "1 begincodespacerange\n"
                         "<0000> <FFFF>\n"
                         "endcodespacerange\n");

        /* Build mapping for used glyphs from Layout->text */
        {
            typedef struct { int gid; wchar_t ch1, ch2; } CMapEntry;
            CMapEntry *entries = (CMapEntry *)malloc((size_t)fnt->numGlyphs * sizeof(CMapEntry));
            int totalEntries = 0;
            if (entries) {
                for (i = 0; i < L->nitems; i++) {
                    const Item *it = &L->items[i];
                    if (it->type == IT_GLYPHS && it->font == fnt) {
                        int k;
                        for (k = 0; k < it->n; k++) {
                            int gidVal = it->gids[k];
                            if (is_glyph_marked(&usedGlyphs[f], gidVal)) {
                                int toff = it->toff[k];
                                if (toff >= 0 && toff < L->textlen && totalEntries < fnt->numGlyphs) {
                                    wchar_t ch = L->text[toff];
                                    entries[totalEntries].gid = gidVal;
                                    entries[totalEntries].ch1 = ch;
                                    entries[totalEntries].ch2 = (ch >= 0xD800 && ch <= 0xDBFF && toff + 1 < L->textlen) ? L->text[toff + 1] : 0;
                                    totalEntries++;
                                    /* Unmark so we don't emit duplicates */
                                    usedGlyphs[f].flags[gidVal >> 3] &= ~(1 << (gidVal & 7));
                                }
                            }
                        }
                    }
                }
                int idx = 0;
                while (idx < totalEntries) {
                    int batch = totalEntries - idx;
                    int b;
                    if (batch > 100) batch = 100;
                    buf_printf(&toUni, "%d beginbfchar\n", batch);
                    for (b = 0; b < batch; b++) {
                        CMapEntry *e = &entries[idx + b];
                        if (e->ch2) {
                            buf_printf(&toUni, "<%04X> <%04X%04X>\n", e->gid, (unsigned)e->ch1, (unsigned)e->ch2);
                        } else {
                            buf_printf(&toUni, "<%04X> <%04X>\n", e->gid, (unsigned)e->ch1);
                        }
                    }
                    buf_puts(&toUni, "endbfchar\n");
                    idx += batch;
                }
                free(entries);
            }
        }

        buf_puts(&toUni, "endcmap\n"
                         "CMapName currentdict /CMap defineresource pop\n"
                         "end\nend\n");

        toUnicodeObj = pdf_new_obj(&pw);
        pdf_put_stream(&pw, toUni.p, toUni.n, 1);
        pdf_end_obj(&pw);
        buf_free(&toUni);

        /* Type0 Font */
        fontType0Objs[f] = pdf_new_obj(&pw);
        buf_printf(&pw.buf, "<< /Type /Font /Subtype /Type0 /BaseFont /%s\n"
                            "/Encoding /Identity-H /DescendantFonts [%d 0 R]\n"
                            "/ToUnicode %d 0 R >>\n",
                   fnt->psName, cidFontObj, toUnicodeObj);
        pdf_end_obj(&pw);
    }

    /* 4. Pages and Content Streams */
    pw.pageCount = L->npages;
    pagesObj = ++pw.nextObj; /* Reserve object ID for Pages catalog */

    for (p = 0; p < L->npages; p++) {
        Buf content = {0};
        int contentObj, pageObj;
        Buf annots = {0};
        int numAnnots = 0;

        /* Content drawing commands */
        buf_puts(&content, "q\n");

        /* Layer 0 (background) then Layer 1 (foreground) */
        {
            int layer;
            for (layer = 0; layer < 2; layer++) {
                for (i = 0; i < L->nitems; i++) {
                    const Item *it = &L->items[i];
                    if (it->page != p || it->layer != layer) continue;

                    switch (it->type) {
                    case IT_FILL: {
                        float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                        float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                        float b = (float)(it->color & 0xFF) / 255.0f;
                        float x = it->x * 0.75f;
                        float y = ptH - (it->y + it->h) * 0.75f;
                        float w = it->w * 0.75f;
                        float h = it->h * 0.75f;
                        buf_printf(&content, "%.3f %.3f %.3f rg %.3f %.3f %.3f %.3f re f\n", r, g, b, x, y, w, h);
                        break;
                    }

                    case IT_STROKE: {
                        float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                        float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                        float b = (float)(it->color & 0xFF) / 255.0f;
                        float x = it->x * 0.75f;
                        float y = ptH - (it->y + it->h) * 0.75f;
                        float w = it->w * 0.75f;
                        float h = it->h * 0.75f;
                        buf_printf(&content, "%.3f %.3f %.3f RG %.3f w %.3f %.3f %.3f %.3f re S\n",
                                   r, g, b, it->lw * 0.75f, x, y, w, h);
                        break;
                    }

                    case IT_LINE: {
                        float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                        float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                        float b = (float)(it->color & 0xFF) / 255.0f;
                        float x1 = it->x * 0.75f;
                        float y1 = ptH - it->y * 0.75f;
                        float x2 = (it->x + it->w) * 0.75f;
                        float y2 = ptH - (it->y + it->h) * 0.75f;
                        buf_printf(&content, "%.3f %.3f %.3f RG %.3f w %.3f %.3f m %.3f %.3f l S\n",
                                   r, g, b, it->lw * 0.75f, x1, y1, x2, y2);
                        break;
                    }

                    case IT_DISC: {
                        float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                        float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                        float b = (float)(it->color & 0xFF) / 255.0f;
                        float x = it->x * 0.75f;
                        float y = ptH - (it->y + it->h) * 0.75f;
                        float w = it->w * 0.75f;
                        float h = it->h * 0.75f;
                        float cx = x + w * 0.5f, cy = y + h * 0.5f, rx = w * 0.5f, ry = h * 0.5f;
                        float kx = rx * 0.55228475f, ky = ry * 0.55228475f;
                        buf_printf(&content, "%.3f %.3f %.3f rg\n"
                                             "%.3f %.3f m %.3f %.3f %.3f %.3f %.3f %.3f c\n"
                                             "%.3f %.3f %.3f %.3f %.3f %.3f c\n"
                                             "%.3f %.3f %.3f %.3f %.3f %.3f c\n"
                                             "%.3f %.3f %.3f %.3f %.3f %.3f c f\n",
                                   r, g, b,
                                   cx + rx, cy, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry,
                                   cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy,
                                   cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry,
                                   cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
                        break;
                    }

                    case IT_CHECK: {
                        float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                        float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                        float b = (float)(it->color & 0xFF) / 255.0f;
                        float x = it->x * 0.75f;
                        float y = ptH - (it->y + it->h) * 0.75f;
                        float w = it->w * 0.75f;
                        float h = it->h * 0.75f;
                        buf_printf(&content, "%.3f %.3f %.3f RG %.3f w 1 J 1 j\n"
                                             "%.3f %.3f m %.3f %.3f l %.3f %.3f l S\n",
                                   r, g, b, it->lw * 0.75f,
                                   x + w * 0.2f, y + h * 0.45f,
                                   x + w * 0.42f, y + h * 0.25f,
                                   x + w * 0.8f, y + h * 0.72f);
                        break;
                    }

                    case IT_IMAGE: {
                        if (it->img && it->img->w > 0 && it->img->h > 0) {
                            float x = it->x * 0.75f;
                            float y = ptH - (it->y + it->h) * 0.75f;
                            float w = it->w * 0.75f;
                            float h = it->h * 0.75f;
                            buf_printf(&content, "q %.3f 0 0 %.3f %.3f %.3f cm /Im1 Do Q\n", w, h, x, y);
                        }
                        break;
                    }

                    case IT_GLYPHS: {
                        if (it->font && it->n > 0) {
                            int fid = it->font->id;
                            float r = (float)((it->color >> 16) & 0xFF) / 255.0f;
                            float g = (float)((it->color >> 8) & 0xFF) / 255.0f;
                            float b = (float)(it->color & 0xFF) / 255.0f;
                            float x = it->x * 0.75f;
                            float y = ptH - it->y * 0.75f;
                            float sz = it->size * 0.75f;
                            int k;

                            float naturalW = 0;
                            for (k = 0; k < it->n; k++) {
                                naturalW += (float)font_adv(it->font, it->gids[k]) * it->size / (float)it->font->upem;
                            }
                            float extraPerGlyph = (it->w - naturalW) / (float)it->n;
                            if (extraPerGlyph > 0.05f) {
                                float tc = extraPerGlyph * 0.75f;
                                buf_printf(&content, "BT\n/F%d %.3f Tf\n%.3f Tc\n%.3f %.3f %.3f rg\n%.3f %.3f Td\n<",
                                           fid + 1, sz, tc, r, g, b, x, y);
                                for (k = 0; k < it->n; k++) {
                                    buf_printf(&content, "%04X", it->gids[k]);
                                }
                                buf_puts(&content, "> Tj\n0 Tc\nET\n");
                            } else {
                                buf_printf(&content, "BT\n/F%d %.3f Tf\n%.3f %.3f %.3f rg\n%.3f %.3f Td\n<",
                                           fid + 1, sz, r, g, b, x, y);
                                for (k = 0; k < it->n; k++) {
                                    buf_printf(&content, "%04X", it->gids[k]);
                                }
                                buf_puts(&content, "> Tj\nET\n");
                            }
                        }
                        break;
                    }
                    }
                }
            }
        }
        buf_puts(&content, "Q\n");

        contentObj = pdf_new_obj(&pw);
        pdf_put_stream(&pw, content.p, content.n, 1);
        pdf_end_obj(&pw);
        buf_free(&content);

        /* Annotations for this page */
        for (i = 0; i < L->nlinks; i++) {
            const LinkArea *la = &L->links[i];
            if (la->page == p && la->link >= 0 && la->link < L->doc->nlinks) {
                const wchar_t *target = L->doc->links[la->link];
                float x1 = la->x * 0.75f;
                float y1 = ptH - (la->y + la->h) * 0.75f;
                float x2 = (la->x + la->w) * 0.75f;
                float y2 = ptH - la->y * 0.75f;

                if (target[0] == '#') {
                    const Anchor *anc = layout_find_anchor(L, target + 1);
                    if (anc) {
                        int annotObj = pdf_new_obj(&pw);
                        buf_printf(&pw.buf, "<< /Type /Annot /Subtype /Link /Rect [%.3f %.3f %.3f %.3f] /Border [0 0 0]\n"
                                            "/A << /S /GoTo /D [%d 0 R /XYZ 0 %.3f null] >> >>\n",
                                   x1, y1, x2, y2, pw.pageObjs[anc->page], ptH - anc->y * 0.75f);
                        pdf_end_obj(&pw);
                        buf_printf(&annots, "%d 0 R ", annotObj);
                        numAnnots++;
                    }
                } else {
                    int annotObj = pdf_new_obj(&pw);
                    char *utf8 = utf16_to_utf8(target, -1, NULL);
                    buf_printf(&pw.buf, "<< /Type /Annot /Subtype /Link /Rect [%.3f %.3f %.3f %.3f] /Border [0 0 0]\n"
                                        "/A << /S /URI /URI (", x1, y1, x2, y2);
                    buf_put_xml(&pw.buf, utf8 ? utf8 : "", -1);
                    buf_puts(&pw.buf, ") >> >>\n");
                    pdf_end_obj(&pw);
                    free(utf8);
                    buf_printf(&annots, "%d 0 R ", annotObj);
                    numAnnots++;
                }
            }
        }

        /* Page Object */
        pageObj = pdf_new_obj(&pw);
        pw.pageObjs[p] = pageObj;
        buf_printf(&pw.buf, "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.3f %.3f]\n"
                            "/Contents %d 0 R\n"
                            "/Resources << /Font <<", pagesObj, ptW, ptH, contentObj);
        for (f = 0; f < fontCount; f++) {
            if (fontType0Objs[f]) buf_printf(&pw.buf, " /F%d %d 0 R", f + 1, fontType0Objs[f]);
        }
        buf_puts(&pw.buf, " >>");
        if (imageObjs && imageObjs[0]) {
            buf_printf(&pw.buf, " /XObject << /Im1 %d 0 R >>", imageObjs[0]);
        }
        buf_puts(&pw.buf, " >>\n");
        if (numAnnots > 0) {
            buf_printf(&pw.buf, "/Annots [%s]\n", annots.p);
        }
        buf_puts(&pw.buf, ">>\n");
        pdf_end_obj(&pw);
        buf_free(&annots);
    }

    /* Pages catalog definition */
    pw.xref[pagesObj] = (uint32_t)pw.buf.n;
    buf_printf(&pw.buf, "%d 0 obj\n<< /Type /Pages /Count %d /Kids [", pagesObj, pw.pageCount);
    for (p = 0; p < pw.pageCount; p++) {
        buf_printf(&pw.buf, " %d 0 R", pw.pageObjs[p]);
    }
    buf_puts(&pw.buf, " ] >>\nendobj\n\n");

    /* 5. Document Outlines (Bookmarks) */
    {
        int outlineObj = 0;
        int headingCount = 0;
        for (i = 0; i < L->nanchors; i++) {
            if (L->anchors[i].level >= 1) headingCount++;
        }
        if (headingCount > 0) {
            outlineObj = pdf_new_obj(&pw);
            buf_printf(&pw.buf, "<< /Type /Outlines /Count %d >>\n", headingCount);
            pdf_end_obj(&pw);
        }

        /* Catalog */
        catalogObj = pdf_new_obj(&pw);
        buf_printf(&pw.buf, "<< /Type /Catalog /Pages %d 0 R\n", pagesObj);
        if (outlineObj) {
            buf_printf(&pw.buf, "/Outlines %d 0 R /PageMode /UseOutlines\n", outlineObj);
        }
        buf_puts(&pw.buf, ">>\n");
        pdf_end_obj(&pw);
    }

    /* 6. Document Info */
    infoObj = pdf_new_obj(&pw);
    buf_puts(&pw.buf, "<< /Producer (Lilac) /Title (");
    if (L->doc && L->doc->title) {
        char *utf8 = utf16_to_utf8(L->doc->title, -1, NULL);
        buf_puts(&pw.buf, utf8 ? utf8 : "Document");
        free(utf8);
    } else {
        buf_puts(&pw.buf, "Document");
    }
    buf_puts(&pw.buf, ") >>\n");
    pdf_end_obj(&pw);

    /* 7. Cross-reference Table & Trailer */
    {
        uint32_t xrefStart = (uint32_t)pw.buf.n;
        buf_printf(&pw.buf, "xref\n0 %d\n0000000000 65535 f \n", pw.nextObj + 1);
        for (i = 1; i <= pw.nextObj; i++) {
            buf_printf(&pw.buf, "%010u 00000 n \n", pw.xref[i]);
        }
        buf_printf(&pw.buf, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\n"
                            "startxref\n%u\n%%%%EOF\n",
                   pw.nextObj + 1, catalogObj, infoObj, xrefStart);
    }

    /* Cleanup memory */
    for (f = 0; f < fontCount; f++) free(usedGlyphs[f].flags);
    free(usedGlyphs);
    free(fontType0Objs);
    free(imageObjs);

    /* Write to file */
    if (file_write_all(path, pw.buf.p, pw.buf.n) != 0) {
        buf_free(&pw.buf);
        if (err && errcap > 0) wcsncpy_s(err, errcap, L"Failed to write PDF file", _TRUNCATE);
        return -1;
    }
    buf_free(&pw.buf);
    return 0;
}

int export_pdf(Doc *doc, const wchar_t *path, wchar_t *err, int errcap)
{
    LayoutOptions opt = {0};
    Layout *L;
    int rc;

    if (!doc || !path) return -1;
    doc_resolve_images(doc, 1);

    opt.width = 793.7f;      /* A4 width in DIP (8.27 in * 96) */
    opt.pageHeight = 1122.5f; /* A4 height in DIP (11.69 in * 96) */
    opt.marginL = 54.0f;
    opt.marginR = 54.0f;
    opt.marginT = 54.0f;
    opt.marginB = 54.0f;
    opt.baseSize = 14.0f;

    L = layout_build(doc, &opt);
    if (!L) {
        if (err && errcap > 0) wcsncpy_s(err, errcap, L"Failed to layout document for PDF", _TRUNCATE);
        return -1;
    }

    rc = pdf_write(L, path, err, errcap);
    layout_free(L);
    return rc;
}
