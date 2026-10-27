/*
 * docx.c - Markdown -> Microsoft Word (.docx / Office Open XML) exporter.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "export.h"
#include "zip.h"
#include "util.h"
#include "image.h"
#include "font.h"

/* Helper to sanitize a bookmark name for Word */
static void sanitize_bookmark(const wchar_t *slug, wchar_t *out, int maxlen)
{
    int o = 0, i;
    wcscpy_s(out, maxlen, L"bm_");
    o = 3;
    for (i = 0; slug[i] && o < maxlen - 1; i++) {
        wchar_t c = slug[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out[o++] = c;
        } else {
            out[o++] = '_';
        }
    }
    out[o] = 0;
}

typedef struct DocxCtx {
    Doc      *doc;
    ZipWriter *z;
    Buf       body;
    Buf       rels;
    Buf       footnotes;
    int       nextRelId;
    int       nextBookmarkId;
    int       hasFootnotes;
    int       numInstances;
    struct {
        Image *img;
        int    relId;
        char   filename[64];
    } images[128];
    int nimages;
} DocxCtx;

static int add_docx_rel(DocxCtx *ctx, const char *type, const char *target, int isExternal)
{
    int id = ++ctx->nextRelId;
    buf_printf(&ctx->rels, "<Relationship Id=\"rId%d\" Type=\"%s\" Target=\"", id, type);
    buf_put_xml(&ctx->rels, target, -1);
    if (isExternal) buf_puts(&ctx->rels, "\" TargetMode=\"External\"/>");
    else buf_puts(&ctx->rels, "\"/>");
    return id;
}

static int get_image_rel(DocxCtx *ctx, Image *img)
{
    int i, relId;
    char path[128];
    const char *ext = "png";
    const uint8_t *bytes = NULL;
    size_t byteLen = 0;
    uint8_t *encoded = NULL;

    if (!img || img->failed || (!img->data && !img->pixels)) return 0;

    for (i = 0; i < ctx->nimages; i++) {
        if (ctx->images[i].img == img) return ctx->images[i].relId;
    }
    if (ctx->nimages >= 128) return 0;

    if (img->format == IMG_JPEG) ext = "jpeg";
    else if (img->format == IMG_GIF) ext = "gif";
    else if (img->format == IMG_BMP) ext = "bmp";

    if (img->data && img->size > 0 && img->format != IMG_UNKNOWN) {
        bytes = img->data;
        byteLen = img->size;
    } else if (img->pixels && img->w > 0 && img->h > 0) {
        if (image_encode_png(img->pixels, img->w, img->h, &encoded, &byteLen) == 0) {
            bytes = encoded;
            ext = "png";
        }
    }

    if (!bytes || byteLen == 0) { free(encoded); return 0; }

    i = ctx->nimages++;
    ctx->images[i].img = img;
    sprintf_s(ctx->images[i].filename, sizeof(ctx->images[i].filename), "media/image%d.%s", i + 1, ext);

    sprintf_s(path, sizeof path, "word/%s", ctx->images[i].filename);
    zip_add(ctx->z, path, bytes, byteLen, 1);
    free(encoded);

    relId = add_docx_rel(ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image",
                         ctx->images[i].filename, 0);
    ctx->images[i].relId = relId;
    return relId;
}

/* Emit inline runs into a <w:p> */
static void emit_runs(DocxCtx *ctx, Buf *b, const Run *runs, int nruns)
{
    int i = 0;
    while (i < nruns) {
        const Run *r = &runs[i];

        if (r->type == RN_BREAK) {
            buf_puts(b, "<w:r><w:br/></w:r>");
            i++;
            continue;
        }

        if (r->type == RN_FOOTREF) {
            buf_printf(b, "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"%d\"/></w:r>", r->footnote);
            i++;
            continue;
        }

        if (r->type == RN_IMAGE) {
            int relId = get_image_rel(ctx, r->img);
            if (relId > 0 && r->img && r->img->w > 0 && r->img->h > 0) {
                int64_t cx = (int64_t)r->img->w * 9525;
                int64_t cy = (int64_t)r->img->h * 9525;
                int64_t maxW = 5760000; /* ~6 inches in EMUs */

                if (r->width > 0 && r->height > 0) {
                    cx = (int64_t)r->width * 9525;
                    cy = (int64_t)r->height * 9525;
                } else if (r->width > 0) {
                    cx = (int64_t)r->width * 9525;
                    cy = (int64_t)((double)r->img->h * (double)cx / (double)r->img->w);
                }

                if (cx > maxW) {
                    cy = (int64_t)((double)cy * (double)maxW / (double)cx);
                    cx = maxW;
                }

                buf_printf(b, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                              "<wp:extent cx=\"%lld\" cy=\"%lld\"/>"
                              "<wp:docPr id=\"%d\" name=\"Picture %d\"/>"
                              "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
                              "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                              "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                              "<pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"Image\"/><pic:cNvPicPr/></pic:nvPicPr>"
                              "<pic:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                              "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
                              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
                              "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>",
                           cx, cy, relId, relId, relId, relId, cx, cy);
            } else {
                buf_puts(b, "<w:r><w:rPr><w:i/><w:color w:val=\"888888\"/></w:rPr><w:t xml:space=\"preserve\">[Image: ");
                buf_put_xml_w(b, r->text ? r->text : L"image", -1);
                buf_puts(b, "]</w:t></w:r>");
            }
            i++;
            continue;
        }

        /* Group runs under hyperlink */
        if (r->link >= 0) {
            int linkIdx = r->link;
            const wchar_t *target = (linkIdx < ctx->doc->nlinks) ? ctx->doc->links[linkIdx] : L"";
            int isAnchor = (target[0] == '#');
            int relId = 0;

            if (isAnchor) {
                wchar_t bm[64];
                sanitize_bookmark(target + 1, bm, 64);
                buf_printf(b, "<w:hyperlink w:anchor=\"%ls\">", bm);
            } else {
                char *utf8 = utf16_to_utf8(target, -1, NULL);
                relId = add_docx_rel(ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink",
                                     utf8 ? utf8 : "", 1);
                free(utf8);
                buf_printf(b, "<w:hyperlink r:id=\"rId%d\">", relId);
            }

            while (i < nruns && runs[i].link == linkIdx && runs[i].type == RN_TEXT) {
                const Run *kr = &runs[i];
                buf_puts(b, "<w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/>");
                if (kr->style & ST_BOLD) buf_puts(b, "<w:b/>");
                if (kr->style & ST_ITALIC) buf_puts(b, "<w:i/>");
                if (kr->style & ST_STRIKE) buf_puts(b, "<w:strike/>");
                if (kr->style & ST_CODE) buf_puts(b, "<w:rStyle w:val=\"VerbatimChar\"/>");
                buf_puts(b, "</w:rPr><w:t xml:space=\"preserve\">");
                buf_put_xml_w(b, kr->text, kr->len);
                buf_puts(b, "</w:t></w:r>");
                i++;
            }
            buf_puts(b, "</w:hyperlink>");
            continue;
        }

        /* Regular text run */
        buf_puts(b, "<w:r><w:rPr>");
        if (r->style & ST_BOLD) buf_puts(b, "<w:b/>");
        if (r->style & ST_ITALIC) buf_puts(b, "<w:i/>");
        if (r->style & ST_STRIKE) buf_puts(b, "<w:strike/>");
        if (r->style & ST_UNDERLINE) buf_puts(b, "<w:u w:val=\"single\"/>");
        if (r->style & ST_CODE) buf_puts(b, "<w:rStyle w:val=\"VerbatimChar\"/>");
        if (r->style & ST_SUP) buf_puts(b, "<w:vertAlign w:val=\"superscript\"/>");
        if (r->style & ST_SUB) buf_puts(b, "<w:vertAlign w:val=\"subscript\"/>");
        if (r->style & ST_MARK) buf_puts(b, "<w:highlight w:val=\"yellow\"/>");
        buf_puts(b, "</w:rPr><w:t xml:space=\"preserve\">");
        buf_put_xml_w(b, r->text, r->len);
        buf_puts(b, "</w:t></w:r>");
        i++;
    }
}

/* Recursive block emitter */
static void emit_block(DocxCtx *ctx, Buf *b, Block *blk, int listNumId, int listLvl, int inQuote)
{
    if (!blk) return;

    switch (blk->type) {
    case BK_DOCUMENT: {
        int k;
        for (k = 0; k < blk->nkids; k++) {
            emit_block(ctx, b, blk->kids[k], 0, 0, 0);
        }
        break;
    }

    case BK_HEADING: {
        int bmId = ++ctx->nextBookmarkId;
        wchar_t bm[64] = {0};
        if (blk->anchor) sanitize_bookmark(blk->anchor, bm, 64);

        buf_printf(b, "<w:p><w:pPr><w:pStyle w:val=\"Heading%d\"/>", blk->level >= 1 && blk->level <= 6 ? blk->level : 1);
        if (blk->align == AL_CENTER) buf_puts(b, "<w:jc w:val=\"center\"/>");
        else if (blk->align == AL_RIGHT) buf_puts(b, "<w:jc w:val=\"right\"/>");
        buf_puts(b, "</w:pPr>");

        if (bm[0]) buf_printf(b, "<w:bookmarkStart w:id=\"%d\" w:name=\"%ls\"/>", bmId, bm);
        emit_runs(ctx, b, blk->runs, blk->nruns);
        if (bm[0]) buf_printf(b, "<w:bookmarkEnd w:id=\"%d\"/>", bmId);
        buf_puts(b, "</w:p>");
        break;
    }

    case BK_PARAGRAPH: {
        buf_puts(b, "<w:p><w:pPr>");
        if (inQuote) buf_puts(b, "<w:pStyle w:val=\"Quote\"/>");
        if (listNumId > 0) {
            buf_printf(b, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", listLvl, listNumId);
        }
        if (blk->align == AL_CENTER) buf_puts(b, "<w:jc w:val=\"center\"/>");
        else if (blk->align == AL_RIGHT) buf_puts(b, "<w:jc w:val=\"right\"/>");
        if (blk->tight) buf_puts(b, "<w:spacing w:after=\"60\"/>");
        buf_puts(b, "</w:pPr>");
        emit_runs(ctx, b, blk->runs, blk->nruns);
        buf_puts(b, "</w:p>");
        break;
    }

    case BK_QUOTE: {
        int k;
        for (k = 0; k < blk->nkids; k++) {
            emit_block(ctx, b, blk->kids[k], 0, 0, 1);
        }
        break;
    }

    case BK_ALERT: {
        int kind = (blk->alert >= 0 && blk->alert < 5) ? blk->alert : AL_NOTE;
        const wchar_t *title = doc_alert_title(kind);
        const char *hex = "0969DA";
        const char *bgHex = "F0F6FC";
        int k;

        if (kind == AL_TIP) { hex = "1A7F37"; bgHex = "F0FDF4"; }
        else if (kind == AL_IMPORTANT) { hex = "8250DF"; bgHex = "FBF5FF"; }
        else if (kind == AL_WARNING) { hex = "9A6700"; bgHex = "FFF8E6"; }
        else if (kind == AL_CAUTION) { hex = "D1242F"; bgHex = "FFF0EE"; }

        buf_printf(b, "<w:p><w:pPr><w:pStyle w:val=\"Quote\"/>"
                      "<w:pBdr><w:left w:val=\"single\" w:sz=\"24\" w:space=\"12\" w:color=\"%s\"/></w:pBdr>"
                      "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%s\"/>"
                      "<w:spacing w:before=\"120\" w:after=\"60\"/>"
                      "</w:pPr>"
                      "<w:r><w:rPr><w:b/><w:color w:val=\"%s\"/></w:rPr><w:t>%ls</w:t></w:r></w:p>",
                   hex, bgHex, hex, title);

        for (k = 0; k < blk->nkids; k++) {
            Block *kb = blk->kids[k];
            if (kb->type == BK_PARAGRAPH) {
                buf_printf(b, "<w:p><w:pPr><w:pStyle w:val=\"Quote\"/>"
                              "<w:pBdr><w:left w:val=\"single\" w:sz=\"24\" w:space=\"12\" w:color=\"%s\"/></w:pBdr>"
                              "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%s\"/>"
                              "<w:spacing w:after=\"120\"/>"
                              "</w:pPr>", hex, bgHex);
                emit_runs(ctx, b, kb->runs, kb->nruns);
                buf_puts(b, "</w:p>");
            } else {
                emit_block(ctx, b, kb, 0, 0, 1);
            }
        }
        break;
    }

    case BK_UL:
    case BK_OL: {
        int numId = (blk->type == BK_UL) ? 1 : (++ctx->numInstances + 1);
        int k;
        for (k = 0; k < blk->nkids; k++) {
            Block *li = blk->kids[k];
            if (li->task) {
                int leftInd = 720 + listLvl * 720;
                buf_printf(b, "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:ind w:left=\"%d\"/>", leftInd);
                if (blk->tight) buf_puts(b, "<w:spacing w:after=\"60\"/>");
                buf_puts(b, "</w:pPr>");
                /* Unicode check symbol */
                buf_puts(b, "<w:r><w:rPr><w:rFonts w:ascii=\"Segoe UI Symbol\" w:hAnsi=\"Segoe UI Symbol\"/><w:sz w:val=\"22\"/></w:rPr><w:t xml:space=\"preserve\">");
                buf_puts(b, (li->task == 2) ? "\xE2\x98\x91 " : "\xE2\x98\x90 ");
                buf_puts(b, "</w:t></w:r>");
                if (li->nkids > 0 && li->kids[0]->type == BK_PARAGRAPH) {
                    emit_runs(ctx, b, li->kids[0]->runs, li->kids[0]->nruns);
                } else {
                    emit_runs(ctx, b, li->runs, li->nruns);
                }
                buf_puts(b, "</w:p>");
            } else {
                buf_printf(b, "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>",
                           listLvl, numId);
                if (blk->tight) buf_puts(b, "<w:spacing w:after=\"60\"/>");
                buf_puts(b, "</w:pPr>");
                if (li->nkids > 0 && li->kids[0]->type == BK_PARAGRAPH) {
                    emit_runs(ctx, b, li->kids[0]->runs, li->kids[0]->nruns);
                } else {
                    emit_runs(ctx, b, li->runs, li->nruns);
                }
                buf_puts(b, "</w:p>");
            }
            /* Remaining children of list item (sub-lists, nested blocks, etc.) */
            {
                int ck = (li->nkids > 0 && li->kids[0]->type == BK_PARAGRAPH) ? 1 : 0;
                for (; ck < li->nkids; ck++) {
                    emit_block(ctx, b, li->kids[ck], (li->kids[ck]->type == BK_UL || li->kids[ck]->type == BK_OL) ? 0 : numId, listLvl + 1, inQuote);
                }
            }
        }
        break;
    }

    case BK_CODE: {
        int lineStart = 0;
        buf_puts(b, "<w:p><w:pPr><w:pStyle w:val=\"SourceCode\"/></w:pPr>");
        while (lineStart < blk->textlen) {
            int lineEnd = lineStart;
            int p;
            while (lineEnd < blk->textlen && blk->text[lineEnd] != '\n' && blk->text[lineEnd] != '\r') lineEnd++;
            buf_puts(b, "<w:r><w:t xml:space=\"preserve\">");
            for (p = lineStart; p < lineEnd; p++) {
                if (blk->text[p] == '\t') {
                    buf_puts(b, "    ");
                } else {
                    buf_put_xml_w(b, blk->text + p, 1);
                }
            }
            buf_puts(b, "</w:t>");
            if (lineEnd < blk->textlen) buf_puts(b, "<w:br/>");
            buf_puts(b, "</w:r>");
            if (lineEnd < blk->textlen && blk->text[lineEnd] == '\r') lineEnd++;
            if (lineEnd < blk->textlen && blk->text[lineEnd] == '\n') lineEnd++;
            lineStart = lineEnd;
        }
        buf_puts(b, "</w:p>");
        break;
    }

    case BK_HR:
        buf_puts(b, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"1\" w:color=\"D0D7DE\"/></w:pBdr><w:spacing w:after=\"200\"/></w:pPr></w:p>");
        break;

    case BK_TABLE: {
        int r, c;
        int ncols = blk->ncols > 0 ? blk->ncols : 1;
        int colW = 9638 / ncols;

        buf_puts(b, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"9638\" w:type=\"dxa\"/>"
                    "<w:tblCellMar><w:top w:w=\"120\" w:type=\"dxa\"/><w:bottom w:w=\"120\" w:type=\"dxa\"/>"
                    "<w:left w:w=\"160\" w:type=\"dxa\"/><w:right w:w=\"160\" w:type=\"dxa\"/></w:tblCellMar>"
                    "<w:tblLook w:val=\"04A0\"/></w:tblPr>");
        buf_puts(b, "<w:tblGrid>");
        for (c = 0; c < ncols; c++) buf_printf(b, "<w:gridCol w:w=\"%d\"/>", colW);
        buf_puts(b, "</w:tblGrid>");

        for (r = 0; r < blk->nkids; r++) {
            Block *tr = blk->kids[r];
            buf_puts(b, "<w:tr>");
            if (tr->header) buf_puts(b, "<w:trPr><w:tblHeader/></w:trPr>");

            for (c = 0; c < ncols; c++) {
                Block *td = (c < tr->nkids) ? tr->kids[c] : NULL;
                buf_printf(b, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>", colW);
                if (tr->header) buf_puts(b, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F6F8FA\"/>");
                buf_puts(b, "</w:tcPr><w:p><w:pPr>");
                if (td && td->align == AL_CENTER) buf_puts(b, "<w:jc w:val=\"center\"/>");
                else if (td && td->align == AL_RIGHT) buf_puts(b, "<w:jc w:val=\"right\"/>");
                buf_puts(b, "<w:spacing w:after=\"60\"/></w:pPr>");
                if (td && td->runs && td->nruns > 0) {
                    if (tr->header) {
                        int q;
                        for (q = 0; q < td->nruns; q++) {
                            Run hr = td->runs[q];
                            hr.style |= ST_BOLD;
                            emit_runs(ctx, b, &hr, 1);
                        }
                    } else {
                        emit_runs(ctx, b, td->runs, td->nruns);
                    }
                }
                buf_puts(b, "</w:p></w:tc>");
            }
            buf_puts(b, "</w:tr>");
        }
        buf_puts(b, "</w:tbl><w:p/>");
        break;
    }

    case BK_FOOTNOTES: {
        /* Write to word/footnotes.xml */
        int k;
        ctx->hasFootnotes = 1;
        for (k = 0; k < blk->nkids; k++) {
            Block *fn = blk->kids[k];
            buf_printf(&ctx->footnotes, "<w:footnote w:id=\"%d\"><w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr>"
                                        "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r>"
                                        "<w:r><w:t xml:space=\"preserve\"> </w:t></w:r>", fn->footnote);
            emit_runs(ctx, &ctx->footnotes, fn->runs, fn->nruns);
            buf_puts(&ctx->footnotes, "</w:p></w:footnote>");
        }
        break;
    }

    default:
        break;
    }
}

int docx_write(Doc *doc, const wchar_t *path, wchar_t *err, int errcap)
{
    DocxCtx ctx;
    ZipWriter *z = zip_new();
    Buf docBuf = {0};
    Buf numBuf = {0};
    int i, rc;

    if (!doc || !path) {
        if (err && errcap > 0) wcsncpy_s(err, errcap, L"Invalid argument", _TRUNCATE);
        return -1;
    }

    doc_resolve_images(doc, 1);

    memset(&ctx, 0, sizeof ctx);
    ctx.doc = doc;
    ctx.z = z;

    /* Base relationships */
    add_docx_rel(&ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles", "styles.xml", 0);
    add_docx_rel(&ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering", "numbering.xml", 0);
    add_docx_rel(&ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings", "settings.xml", 0);

    /* Generate body */
    emit_block(&ctx, &ctx.body, doc->root, 0, 0, 0);

    /* Section properties: A4, 2cm margins */
    buf_puts(&ctx.body, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
                        "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\"/>"
                        "</w:sectPr>");

    /* Document.xml wrapping */
    buf_puts(&docBuf, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                      "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                      "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                      "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">\r\n"
                      "<w:body>\r\n");
    buf_put(&docBuf, ctx.body.p, ctx.body.n);
    buf_puts(&docBuf, "</w:body></w:document>");
    zip_add(z, "word/document.xml", docBuf.p, docBuf.n, 1);
    buf_free(&docBuf);
    buf_free(&ctx.body);

    /* Footnotes part if any */
    if (ctx.hasFootnotes) {
        Buf fnFile = {0};
        add_docx_rel(&ctx, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes", "footnotes.xml", 0);
        buf_puts(&fnFile, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                          "<w:footnotes xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\r\n"
                          "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>\r\n"
                          "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>\r\n");
        buf_put(&fnFile, ctx.footnotes.p, ctx.footnotes.n);
        buf_puts(&fnFile, "</w:footnotes>");
        zip_add(z, "word/footnotes.xml", fnFile.p, fnFile.n, 1);
        buf_free(&fnFile);
    }
    buf_free(&ctx.footnotes);

    /* document.xml.rels */
    {
        Buf rfile = {0};
        buf_puts(&rfile, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\r\n");
        buf_put(&rfile, ctx.rels.p, ctx.rels.n);
        buf_puts(&rfile, "</Relationships>");
        zip_add(z, "word/_rels/document.xml.rels", rfile.p, rfile.n, 1);
        buf_free(&rfile);
        buf_free(&ctx.rels);
    }

    /* [Content_Types].xml */
    {
        const char *ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                         "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\r\n"
                         "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\r\n"
                         "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\r\n"
                         "  <Default Extension=\"png\" ContentType=\"image/png\"/>\r\n"
                         "  <Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>\r\n"
                         "  <Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>\r\n"
                         "  <Default Extension=\"gif\" ContentType=\"image/gif\"/>\r\n"
                         "  <Default Extension=\"bmp\" ContentType=\"image/bmp\"/>\r\n"
                         "  <Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\r\n"
                         "  <Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>\r\n"
                         "  <Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>\r\n"
                         "  <Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>\r\n"
                         "  <Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>\r\n"
                         "  <Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>\r\n"
                         "</Types>";
        zip_add(z, "[Content_Types].xml", ct, strlen(ct), 1);
    }

    /* _rels/.rels */
    {
        const char *pkgRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\r\n"
                              "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\r\n"
                              "  <Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>\r\n"
                              "  <Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>\r\n"
                              "</Relationships>";
        zip_add(z, "_rels/.rels", pkgRels, strlen(pkgRels), 1);
    }

    /* docProps/core.xml */
    {
        Buf core = {0};
        buf_puts(&core, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\r\n"
                        "  <dc:title>");
        buf_put_xml_w(&core, doc->title ? doc->title : L"Document", -1);
        buf_puts(&core, "</dc:title>\r\n  <dc:creator>Lilac</dc:creator>\r\n</cp:coreProperties>");
        zip_add(z, "docProps/core.xml", core.p, core.n, 1);
        buf_free(&core);
    }

    /* docProps/app.xml */
    {
        const char *app = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                          "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">\r\n"
                          "  <Application>Lilac</Application>\r\n"
                          "</Properties>";
        zip_add(z, "docProps/app.xml", app, strlen(app), 1);
    }

    /* word/settings.xml */
    {
        const char *sett = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                           "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\r\n"
                           "  <w:defaultTabStop w:val=\"720\"/>\r\n"
                           "</w:settings>";
        zip_add(z, "word/settings.xml", sett, strlen(sett), 1);
    }

    /* word/styles.xml */
    {
        const wchar_t *bodyFace = font_get_family_face(FAM_BODY);
        const wchar_t *monoFace = font_get_family_face(FAM_MONO);
        Buf sbuf = {0};

        buf_printf(&sbuf,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\r\n"
            "  <w:docDefaults>\r\n"
            "    <w:rPrDefault>\r\n"
            "      <w:rPr>\r\n"
            "        <w:rFonts w:ascii=\"%ls\" w:hAnsi=\"%ls\" w:cs=\"%ls\" w:eastAsia=\"Malgun Gothic\"/>\r\n"
            "        <w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>\r\n"
            "        <w:color w:val=\"1F2328\"/>\r\n"
            "        <w:lang w:val=\"en-US\" w:eastAsia=\"ko-KR\"/>\r\n"
            "      </w:rPr>\r\n"
            "    </w:rPrDefault>\r\n"
            "    <w:pPrDefault>\r\n"
            "      <w:pPr><w:spacing w:after=\"160\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr>\r\n"
            "    </w:pPrDefault>\r\n"
            "  </w:docDefaults>\r\n"
            "  <w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">\r\n"
            "    <w:name w:val=\"Normal\"/>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading1\">\r\n"
            "    <w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"360\" w:after=\"160\"/><w:outlineLvl w:val=\"0\"/>"
            "           <w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"D1D9E0\"/></w:pBdr></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"40\"/><w:szCs w:val=\"40\"/><w:color w:val=\"1F2328\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading2\">\r\n"
            "    <w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"300\" w:after=\"140\"/><w:outlineLvl w:val=\"1\"/>"
            "           <w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"D1D9E0\"/></w:pBdr></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"30\"/><w:szCs w:val=\"30\"/><w:color w:val=\"1F2328\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading3\">\r\n"
            "    <w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"240\" w:after=\"120\"/><w:outlineLvl w:val=\"2\"/></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/><w:color w:val=\"1F2328\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading4\">\r\n"
            "    <w:name w:val=\"heading 4\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"200\" w:after=\"100\"/><w:outlineLvl w:val=\"3\"/></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:color w:val=\"1F2328\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading5\">\r\n"
            "    <w:name w:val=\"heading 5\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"160\" w:after=\"80\"/><w:outlineLvl w:val=\"4\"/></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/><w:color w:val=\"1F2328\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Heading6\">\r\n"
            "    <w:name w:val=\"heading 6\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"140\" w:after=\"60\"/><w:outlineLvl w:val=\"5\"/></w:pPr>\r\n"
            "    <w:rPr><w:b/><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/><w:color w:val=\"59636E\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"SourceCode\">\r\n"
            "    <w:name w:val=\"Source Code\"/><w:basedOn w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:spacing w:before=\"100\" w:after=\"100\" w:line=\"240\" w:lineRule=\"auto\"/>"
            "           <w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F6F8FA\"/>"
            "           <w:pBdr><w:top w:val=\"single\" w:sz=\"4\" w:space=\"6\" w:color=\"D0D7DE\"/>"
            "                   <w:left w:val=\"single\" w:sz=\"4\" w:space=\"6\" w:color=\"D0D7DE\"/>"
            "                   <w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"6\" w:color=\"D0D7DE\"/>"
            "                   <w:right w:val=\"single\" w:sz=\"4\" w:space=\"6\" w:color=\"D0D7DE\"/></w:pBdr></w:pPr>\r\n"
            "    <w:rPr><w:rFonts w:ascii=\"%ls\" w:hAnsi=\"%ls\" w:cs=\"%ls\" w:eastAsia=\"Malgun Gothic\"/>"
            "           <w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/><w:spacing w:val=\"10\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"character\" w:styleId=\"VerbatimChar\">\r\n"
            "    <w:name w:val=\"Verbatim Char\"/>\r\n"
            "    <w:rPr><w:rFonts w:ascii=\"%ls\" w:hAnsi=\"%ls\" w:cs=\"%ls\" w:eastAsia=\"Malgun Gothic\"/>"
            "           <w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"EFF1F3\"/><w:sz w:val=\"19\"/><w:spacing w:val=\"10\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"character\" w:styleId=\"Hyperlink\">\r\n"
            "    <w:name w:val=\"Hyperlink\"/>\r\n"
            "    <w:rPr><w:color w:val=\"0969DA\"/><w:u w:val=\"single\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"Quote\">\r\n"
            "    <w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:ind w:left=\"360\"/><w:pBdr><w:left w:val=\"single\" w:sz=\"24\" w:space=\"12\" w:color=\"D0D7DE\"/></w:pBdr></w:pPr>\r\n"
            "    <w:rPr><w:color w:val=\"59636E\"/></w:rPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\">\r\n"
            "    <w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/>\r\n"
            "    <w:pPr><w:ind w:left=\"720\"/><w:spacing w:after=\"80\"/></w:pPr>\r\n"
            "  </w:style>\r\n"
            "  <w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableGrid\">\r\n"
            "    <w:name w:val=\"Table Grid\"/>\r\n"
            "    <w:tblPr><w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/>"
            "                           <w:left w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/>"
            "                           <w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/>"
            "                           <w:right w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/>"
            "                           <w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/>"
            "                           <w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"D0D7DE\"/></w:tblBorders></w:tblPr>\r\n"
            "  </w:style>\r\n"
            "</w:styles>",
            bodyFace, bodyFace, bodyFace,
            monoFace, monoFace, monoFace,
            monoFace, monoFace, monoFace);
        zip_add(z, "word/styles.xml", sbuf.p, sbuf.n, 1);
        buf_free(&sbuf);
    }

    /* word/numbering.xml */
    {
        buf_puts(&numBuf, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                          "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\r\n"
                          "  <w:abstractNum w:abstractNumId=\"0\">\r\n"
                          "    <w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x2022;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25E6;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"1440\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"2\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25AA;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"2160\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"3\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x2022;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"2880\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"4\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25E6;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"3600\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"5\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25AA;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"4320\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"6\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x2022;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"5040\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"7\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25E6;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"5760\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"8\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"&#x25AA;\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"6480\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "  </w:abstractNum>\r\n"
                          "  <w:abstractNum w:abstractNumId=\"1\">\r\n"
                          "    <w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%2.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"1440\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"2\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%3.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"2160\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"3\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%4.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"2880\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"4\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%5.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"3600\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"5\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%6.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"4320\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"6\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%7.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"5040\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"7\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%8.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"5760\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "    <w:lvl w:ilvl=\"8\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%9.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"6480\" w:hanging=\"360\"/></w:pPr></w:lvl>\r\n"
                          "  </w:abstractNum>\r\n"
                          "  <w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>\r\n");

        for (i = 1; i <= ctx.numInstances; i++) {
            buf_printf(&numBuf, "  <w:num w:numId=\"%d\"><w:abstractNumId w:val=\"1\"/></w:num>\r\n", i + 1);
        }
        buf_puts(&numBuf, "</w:numbering>");
        zip_add(z, "word/numbering.xml", numBuf.p, numBuf.n, 1);
        buf_free(&numBuf);
    }

    rc = zip_close(z, path);
    if (rc != 0 && err && errcap > 0) {
        wcsncpy_s(err, errcap, L"Failed to write DOCX file", _TRUNCATE);
    }
    return rc;
}
