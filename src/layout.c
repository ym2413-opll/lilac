/*
 * layout.c - Document layout engine (continuous & paged display list generation).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wctype.h>
#include "layout.h"

/* ---------------- Palette ---------------- */
typedef struct Theme {
    uint32_t bg;
    uint32_t text;
    uint32_t heading;
    uint32_t link;
    uint32_t quoteBar;
    uint32_t quoteText;
    uint32_t codeBg;
    uint32_t codeBorder;
    uint32_t codeText;
    uint32_t tableBorder;
    uint32_t tableHeadBg;
    uint32_t hr;
    uint32_t markBg;
    uint32_t kbdBg;
    uint32_t kbdBorder;
    struct {
        uint32_t bar;
        uint32_t bg;
    } alert[5];
} Theme;

static const Theme g_light = {
    0xFFFFFF, 0x1F2328, 0x1F2328, 0x0969DA, 0xD0D7DE, 0x59636E,
    0xF6F8FA, 0xD0D7DE, 0x1F2328, 0xD0D7DE, 0xF6F8FA, 0xD0D7DE,
    0xFFF8C5, 0xF6F8FA, 0xAFB8C1,
    {
        {0x0969DA, 0xF0F7FF}, /* NOTE */
        {0x1A7F37, 0xF0FFF4}, /* TIP */
        {0x8250DF, 0xFBF5FF}, /* IMPORTANT */
        {0x9A6700, 0xFFFBEB}, /* WARNING */
        {0xD1242F, 0xFFF1F0}  /* CAUTION */
    }
};

static const Theme g_dark = {
    0x0D1117, 0xE6EDF3, 0xE6EDF3, 0x4493F8, 0x30363D, 0x8D96A0,
    0x161B22, 0x30363D, 0xE6EDF3, 0x30363D, 0x161B22, 0x30363D,
    0x5A4300, 0x161B22, 0x6E7681,
    {
        {0x4493F8, 0x111C2D},
        {0x3FB950, 0x112519},
        {0xA371F7, 0x211737},
        {0xD29922, 0x2A200F},
        {0xF85149, 0x2B1516}
    }
};

/* ---------------- Context ---------------- */
typedef struct LayCtx {
    Layout        *L;
    const Theme   *th;
    float          contentL, contentR, contentW;
    float          curY;
    int            curPage;
    float          pageTop, pageBottom, pageH;
    int            isPaged;
    float          baseSize;
} LayCtx;

static Item *add_item(LayCtx *ctx, int type, int layer)
{
    Layout *L = ctx->L;
    Item *it;
    if (L->nitems == L->capitems) {
        int nc = L->capitems ? L->capitems * 2 : 256;
        Item *ni = (Item *)arena_alloc(&L->arena, nc * sizeof(Item));
        if (L->nitems) memcpy(ni, L->items, L->nitems * sizeof(Item));
        L->items = ni;
        L->capitems = nc;
    }
    it = &L->items[L->nitems++];
    memset(it, 0, sizeof *it);
    it->type = (uint8_t)type;
    it->layer = (uint8_t)layer;
    it->page = ctx->curPage;
    return it;
}

static void add_link_area(LayCtx *ctx, float x, float y, float w, float h, int link)
{
    Layout *L = ctx->L;
    LinkArea *la;
    if (link < 0) return;
    if (L->nlinks == L->caplinks) {
        int nc = L->caplinks ? L->caplinks * 2 : 32;
        LinkArea *na = (LinkArea *)arena_alloc(&L->arena, nc * sizeof(LinkArea));
        if (L->nlinks) memcpy(na, L->links, L->nlinks * sizeof(LinkArea));
        L->links = na;
        L->caplinks = nc;
    }
    la = &L->links[L->nlinks++];
    la->page = ctx->curPage;
    la->x = x; la->y = y; la->w = w; la->h = h;
    la->link = link;
}

static void add_anchor(LayCtx *ctx, const wchar_t *name, const wchar_t *title, int level, float y)
{
    Layout *L = ctx->L;
    Anchor *a;
    if (!name) return;
    if (L->nanchors == L->capanchors) {
        int nc = L->capanchors ? L->capanchors * 2 : 16;
        Anchor *na = (Anchor *)arena_alloc(&L->arena, nc * sizeof(Anchor));
        if (L->nanchors) memcpy(na, L->anchors, L->nanchors * sizeof(Anchor));
        L->anchors = na;
        L->capanchors = nc;
    }
    a = &L->anchors[L->nanchors++];
    a->name = arena_wcsndup(&L->arena, name, -1);
    a->title = title ? arena_wcsndup(&L->arena, title, -1) : NULL;
    a->level = level;
    a->page = ctx->curPage;
    a->y = y;
}

static void new_page(LayCtx *ctx)
{
    ctx->curPage++;
    if (ctx->curPage >= ctx->L->npages) ctx->L->npages = ctx->curPage + 1;
    ctx->curY = ctx->pageTop;
}

static void check_page_space(LayCtx *ctx, float needed)
{
    if (!ctx->isPaged) return;
    if (ctx->curY + needed > ctx->pageBottom && ctx->curY > ctx->pageTop) {
        new_page(ctx);
    }
}

/* ---------------- Text measurement & line breaking ---------------- */

typedef struct GlyphInfo {
    Font    *font;
    uint16_t gid;
    float    adv;
    int      toff;
    int      link;
    unsigned style;
    uint32_t color;
    wchar_t  ch;
} GlyphInfo;

typedef struct VisualLine {
    int   firstGlyph;
    int   glyphCount;
    float w;
    float x;
    float y;
    float ascent;
    float descent;
} VisualLine;

static int is_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||   /* Hangul Jamo */
           (cp >= 0x2E80 && cp <= 0x9FFF) ||   /* CJK Radicals, Symbols, CJK Unified Ideographs */
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   /* Hangul Syllables */
           (cp >= 0xF900 && cp <= 0xFAFF);     /* CJK Compatibility */
}

/* Layout runs of an inline container (paragraph, heading, cell) into lines and emit items */
static void layout_inline_runs(LayCtx *ctx, const Run *runs, int nruns, float x0, float w0,
                               int align, float baseFontSize, float lineSpacingMul, float *outHeight)
{
    Layout *L = ctx->L;
    const Theme *th = ctx->th;
    GlyphInfo *glyphs = NULL;
    int glyphCap = 64, nGlyphs = 0;
    int r, i;
    float defaultAscent = 0, defaultDescent = 0, defaultLineH = 0;
    Font *bodyFont = font_primary(FAM_BODY, 0, 0);

    glyphs = (GlyphInfo *)malloc(glyphCap * sizeof(GlyphInfo));
    if (!glyphs) return;

    defaultAscent = (float)bodyFont->ascent * baseFontSize / (float)bodyFont->upem;
    defaultDescent = (float)bodyFont->descent * baseFontSize / (float)bodyFont->upem;
    defaultLineH = (defaultAscent + defaultDescent) * lineSpacingMul;

    /* 1. Resolve glyphs for all runs */
    for (r = 0; r < nruns; r++) {
        const Run *run = &runs[r];
        int fam = (run->style & ST_CODE) ? FAM_MONO : FAM_BODY;
        int bld = (run->style & ST_BOLD) != 0;
        int itl = (run->style & ST_ITALIC) != 0;
        float sz = baseFontSize;
        uint32_t col = th->text;

        if (run->style & ST_LINK) col = th->link;
        if (run->style & (ST_SUP | ST_SUB)) sz *= 0.75f;
        if (run->style & ST_CODE) sz *= 0.9f;

        if (run->type == RN_BREAK) {
            if (nGlyphs == glyphCap) {
                glyphCap *= 2;
                glyphs = (GlyphInfo *)realloc(glyphs, glyphCap * sizeof(GlyphInfo));
            }
            glyphs[nGlyphs].ch = '\n';
            glyphs[nGlyphs].adv = 0;
            glyphs[nGlyphs].gid = 0;
            glyphs[nGlyphs].font = NULL;
            glyphs[nGlyphs].link = -1;
            glyphs[nGlyphs].style = run->style;
            glyphs[nGlyphs].toff = L->textlen;
            nGlyphs++;
            continue;
        }

        if (run->type == RN_TEXT || run->type == RN_FOOTREF) {
            int len = run->len;
            int idx = 0;
            while (idx < len) {
                int startIdx = idx;
                uint32_t cp = utf16_next(run->text, len, &idx);
                int gid = 0;
                Font *f = font_resolve(fam, bld, itl, cp, &gid);
                float adv = (float)font_adv(f, gid) * sz / (float)f->upem;
                if (fam == FAM_MONO) {
                    /* Monospace font letter-spacing (자간) to prevent cramped glyphs */
                    adv += sz * 0.05f;
                }

                if (nGlyphs == glyphCap) {
                    glyphCap *= 2;
                    glyphs = (GlyphInfo *)realloc(glyphs, glyphCap * sizeof(GlyphInfo));
                }

                glyphs[nGlyphs].ch = (wchar_t)cp;
                glyphs[nGlyphs].font = f;
                glyphs[nGlyphs].gid = (uint16_t)gid;
                glyphs[nGlyphs].adv = adv;
                glyphs[nGlyphs].link = run->link;
                glyphs[nGlyphs].style = run->style;
                glyphs[nGlyphs].color = col;
                glyphs[nGlyphs].toff = L->textlen;
                nGlyphs++;

                /* append to Layout plain text */
                {
                    int k;
                    for (k = startIdx; k < idx; k++) {
                        if (L->textlen + 2 > L->textcap) {
                            L->textcap = L->textcap ? L->textcap * 2 : 512;
                            L->text = (wchar_t *)realloc(L->text, L->textcap * sizeof(wchar_t));
                        }
                        L->text[L->textlen++] = run->text[k];
                    }
                }
            }
        }
    }

    if (nGlyphs == 0) {
        free(glyphs);
        if (outHeight) *outHeight = 0;
        return;
    }

    /* 2. Break glyphs into lines */
    {
        VisualLine *lines = NULL;
        int lineCap = 16, nLines = 0;
        int start = 0;

        lines = (VisualLine *)malloc(lineCap * sizeof(VisualLine));

        while (start < nGlyphs) {
            int end = start;
            float curW = 0;
            int lastBreak = -1;
            float lastBreakW = 0;

            while (end < nGlyphs) {
                wchar_t c = glyphs[end].ch;
                if (c == '\n') {
                    end++;
                    break;
                }
                if (curW + glyphs[end].adv > w0 && end > start) {
                    if (lastBreak > start) {
                        end = lastBreak;
                        curW = lastBreakW;
                    }
                    break;
                }
                curW += glyphs[end].adv;

                /* Potential break opportunity: after space or after CJK character */
                if (c == ' ' || is_cjk(c)) {
                    lastBreak = end + 1;
                    lastBreakW = curW;
                }
                end++;
            }

            if (nLines == lineCap) {
                lineCap *= 2;
                lines = (VisualLine *)realloc(lines, lineCap * sizeof(VisualLine));
            }

            lines[nLines].firstGlyph = start;
            lines[nLines].glyphCount = end - start;
            lines[nLines].w = curW;
            lines[nLines].ascent = defaultAscent;
            lines[nLines].descent = defaultDescent;
            nLines++;

            start = end;
        }

        /* 3. Emit items for each line */
        for (i = 0; i < nLines; i++) {
            VisualLine *line = &lines[i];
            float lineH = (line->ascent + line->descent) * lineSpacingMul;
            float lx = x0;
            int g, runStart;

            check_page_space(ctx, lineH);

            if (align == AL_CENTER) lx += (w0 - line->w) * 0.5f;
            else if (align == AL_RIGHT) lx += (w0 - line->w);

            /* Group consecutive glyphs sharing same font, size, link, style into IT_GLYPHS */
            runStart = line->firstGlyph;
            while (runStart < line->firstGlyph + line->glyphCount) {
                GlyphInfo *g0 = &glyphs[runStart];
                int runEnd = runStart + 1;
                float runW = g0->adv;
                float curX;

                if (g0->ch == '\n') { runStart++; continue; }

                while (runEnd < line->firstGlyph + line->glyphCount) {
                    GlyphInfo *g1 = &glyphs[runEnd];
                    if (g1->ch == '\n') break;
                    if (g1->font != g0->font || g1->style != g0->style ||
                        g1->link != g0->link || g1->color != g0->color) break;
                    runW += g1->adv;
                    runEnd++;
                }

                {
                    int runLen = runEnd - runStart;
                    float sz = baseFontSize;
                    float leading = lineH - (line->ascent + line->descent);
                    float halfLeading = leading > 0 ? leading * 0.5f : 0;
                    float baseline = ctx->curY + halfLeading + line->ascent;
                    Item *it = add_item(ctx, IT_GLYPHS, 1);
                    it->x = lx;
                    it->y = baseline;
                    it->w = runW;
                    it->h = lineH;
                    it->size = sz;
                    it->font = g0->font;
                    it->color = g0->color;
                    it->link = g0->link;
                    it->n = runLen;
                    it->gids = (uint16_t *)arena_alloc(&L->arena, runLen * sizeof(uint16_t));
                    it->dx = (float *)arena_alloc(&L->arena, runLen * sizeof(float));
                    it->toff = (int *)arena_alloc(&L->arena, runLen * sizeof(int));

                    if (g0->style & ST_MARK) {
                        /* Highlight background */
                        Item *bg = add_item(ctx, IT_FILL, 0);
                        bg->x = lx;
                        bg->y = baseline - line->ascent - 1.0f;
                        bg->w = runW;
                        bg->h = line->ascent + line->descent + 2.0f;
                        bg->color = th->markBg;
                    }
                    if (g0->style & ST_CODE) {
                        /* Inline code background */
                        Item *bg = add_item(ctx, IT_FILL, 0);
                        float codePadY = 2.0f;
                        float codeTop = baseline - line->ascent;
                        float codeH = line->ascent + line->descent;
                        bg->x = lx - 3.0f;
                        bg->y = codeTop - codePadY;
                        bg->w = runW + 6.0f;
                        bg->h = codeH + codePadY * 2.0f;
                        bg->radius = 3.5f;
                        bg->color = th->codeBg;
                    }
                    if (g0->link >= 0) {
                        add_link_area(ctx, lx, baseline - line->ascent, runW, line->ascent + line->descent, g0->link);
                    }

                    curX = 0;
                    for (g = 0; g < runLen; g++) {
                        it->gids[g] = glyphs[runStart + g].gid;
                        it->dx[g] = curX;
                        it->toff[g] = glyphs[runStart + g].toff;
                        curX += glyphs[runStart + g].adv;
                    }
                    it->tend = (runStart + runLen < nGlyphs) ? glyphs[runStart + runLen].toff : L->textlen;

                    /* Strike-through and underline */
                    if (g0->style & ST_STRIKE) {
                        Item *st = add_item(ctx, IT_LINE, 1);
                        st->x = lx;
                        st->y = baseline - line->ascent * 0.35f;
                        st->w = runW;
                        st->h = 0;
                        st->lw = 1.0f;
                        st->color = g0->color;
                    }
                    if (g0->style & ST_UNDERLINE) {
                        Item *un = add_item(ctx, IT_LINE, 1);
                        un->x = lx;
                        un->y = baseline + 2.0f;
                        un->w = runW;
                        un->h = 0;
                        un->lw = 1.0f;
                        un->color = g0->color;
                    }

                    lx += runW;
                }
                runStart = runEnd;
            }

            ctx->curY += lineH;
        }

        if (outHeight) *outHeight = (float)nLines * defaultLineH;
        free(lines);
    }

    /* Append newline to Layout plain text so blocks are cleanly separated when copied or searched */
    if (L->text && L->textlen > 0 && L->text[L->textlen - 1] != L'\n') {
        if (L->textlen + 2 > L->textcap) {
            L->textcap = L->textcap ? L->textcap * 2 : 512;
            L->text = (wchar_t *)realloc(L->text, L->textcap * sizeof(wchar_t));
        }
        L->text[L->textlen++] = L'\n';
    }

    free(glyphs);
}

/* ---------------- Block Layout ---------------- */

static void layout_block(LayCtx *ctx, Block *b, float x, float w);

static void layout_children(LayCtx *ctx, Block *b, float x, float w, float spacing)
{
    int i;
    for (i = 0; i < b->nkids; i++) {
        if (i > 0 && spacing > 0) ctx->curY += spacing;
        layout_block(ctx, b->kids[i], x, w);
    }
}

static void layout_block(LayCtx *ctx, Block *b, float x, float w)
{
    const Theme *th = ctx->th;

    if (!b) return;

    switch (b->type) {
    case BK_DOCUMENT:
        layout_children(ctx, b, x, w, 8.0f);
        break;

    case BK_HEADING: {
        float scale = 1.0f;
        float topSpace = 16.0f, botSpace = 8.0f;
        switch (b->level) {
        case 1: scale = 2.0f; topSpace = 24.0f; break;
        case 2: scale = 1.5f; topSpace = 20.0f; break;
        case 3: scale = 1.25f; topSpace = 16.0f; break;
        case 4: scale = 1.0f; topSpace = 12.0f; break;
        case 5: scale = 0.875f; topSpace = 10.0f; break;
        case 6: scale = 0.85f; topSpace = 8.0f; break;
        }
        ctx->curY += topSpace;
        check_page_space(ctx, ctx->baseSize * scale * 2.0f);

        if (b->anchor) {
            wchar_t *title = doc_runs_text(b->runs, b->nruns);
            add_anchor(ctx, b->anchor, title, b->level, ctx->curY);
            free(title);
        }

        layout_inline_runs(ctx, b->runs, b->nruns, x, w, b->align, ctx->baseSize * scale, 1.25f, NULL);

        if (b->level == 1 || b->level == 2) {
            Item *hr = add_item(ctx, IT_LINE, 1);
            hr->x = x;
            hr->y = ctx->curY + 4.0f;
            hr->w = w;
            hr->h = 0;
            hr->lw = 1.0f;
            hr->color = th->hr;
            ctx->curY += 6.0f;
        }
        ctx->curY += botSpace;
        break;
    }

    case BK_PARAGRAPH: {
        int i;
        /* Check if paragraph has images */
        for (i = 0; i < b->nruns; i++) {
            if (b->runs[i].type == RN_IMAGE) {
                Run *r = &b->runs[i];
                if (r->img && !r->img->failed && r->img->w > 0 && r->img->h > 0) {
                    float iw = (float)r->img->w;
                    float ih = (float)r->img->h;
                    float dispW = iw, dispH = ih;
                    float imgX = x;

                    if (r->width > 0 && r->height > 0) {
                        dispW = r->width;
                        dispH = r->height;
                    } else if (r->width > 0) {
                        dispW = r->width;
                        dispH = ih * (dispW / iw);
                    } else if (r->height > 0) {
                        dispH = r->height;
                        dispW = iw * (dispH / ih);
                    }

                    if (dispW > w) {
                        dispH = dispH * (w / dispW);
                        dispW = w;
                    }

                    check_page_space(ctx, dispH + 8.0f);
                    if (b->align == AL_CENTER) imgX = x + (w - dispW) * 0.5f;
                    else if (b->align == AL_RIGHT) imgX = x + (w - dispW);

                    {
                        Item *it = add_item(ctx, IT_IMAGE, 1);
                        it->x = imgX;
                        it->y = ctx->curY;
                        it->w = dispW;
                        it->h = dispH;
                        it->img = r->img;
                        if (r->link >= 0) add_link_area(ctx, imgX, ctx->curY, dispW, dispH, r->link);
                    }
                    ctx->curY += dispH + 8.0f;
                }
            }
        }
        layout_inline_runs(ctx, b->runs, b->nruns, x, w, b->align, ctx->baseSize, 1.4f, NULL);
        if (!b->tight) ctx->curY += 4.0f;
        break;
    }

    case BK_QUOTE: {
        float startY = ctx->curY;
        float padL = 16.0f;
        ctx->curY += 4.0f;
        layout_children(ctx, b, x + padL, w - padL, 6.0f);
        {
            Item *bar = add_item(ctx, IT_FILL, 1);
            bar->x = x;
            bar->y = startY + 4.0f;
            bar->w = 3.5f;
            bar->h = ctx->curY - startY;
            bar->radius = 1.5f;
            bar->color = th->quoteBar;
        }
        ctx->curY += 6.0f;
        break;
    }

    case BK_ALERT: {
        float startY = ctx->curY;
        float pad = 12.0f;
        int kind = (b->alert >= 0 && b->alert < 5) ? b->alert : AL_NOTE;
        uint32_t barCol = th->alert[kind].bar;
        uint32_t bgCol = th->alert[kind].bg;
        Item *bg;
        Item *bar;

        ctx->curY += pad;

        /* Alert title */
        {
            const wchar_t *title = doc_alert_title(kind);
            Run trun = {0};
            trun.type = RN_TEXT;
            trun.style = ST_BOLD;
            trun.text = (wchar_t *)title;
            trun.len = (int)wcslen(title);
            layout_inline_runs(ctx, &trun, 1, x + pad, w - pad * 2, AL_DEFAULT, ctx->baseSize, 1.3f, NULL);
            ctx->curY += 6.0f;
        }

        layout_children(ctx, b, x + pad, w - pad * 2, 6.0f);
        ctx->curY += pad;

        bg = add_item(ctx, IT_FILL, 0);
        bg->x = x;
        bg->y = startY;
        bg->w = w;
        bg->h = ctx->curY - startY;
        bg->radius = 4.0f;
        bg->color = bgCol;

        bar = add_item(ctx, IT_FILL, 1);
        bar->x = x;
        bar->y = startY;
        bar->w = 4.0f;
        bar->h = ctx->curY - startY;
        bar->radius = 2.0f;
        bar->color = barCol;

        ctx->curY += 8.0f;
        break;
    }

    case BK_UL:
    case BK_OL: {
        Font *bodyFont = font_primary(FAM_BODY, 0, 0);
        float lineH = (float)(bodyFont->ascent + bodyFont->descent) * ctx->baseSize * 1.4f / (float)bodyFont->upem;
        int i;
        for (i = 0; i < b->nkids; i++) {
            Block *li = b->kids[i];
            float bulletW = 20.0f;
            float itemY = ctx->curY;

            check_page_space(ctx, ctx->baseSize * 1.5f);

            if (li->task) {
                float cbSz = 13.0f;
                Item *cb = add_item(ctx, IT_STROKE, 1);
                cb->x = x + 2.0f;
                cb->y = itemY + lineH * 0.5f - cbSz * 0.5f;
                cb->w = cbSz;
                cb->h = cbSz;
                cb->radius = 3.0f;
                cb->lw = 1.2f;
                cb->color = th->quoteBar;

                if (li->task == 2) {
                    Item *ck = add_item(ctx, IT_CHECK, 1);
                    ck->x = cb->x;
                    ck->y = cb->y;
                    ck->w = cbSz;
                    ck->h = cbSz;
                    ck->lw = 1.6f;
                    ck->color = th->link;
                }
            } else if (b->type == BK_UL) {
                Item *dot = add_item(ctx, IT_DISC, 1);
                dot->w = 5.5f;
                dot->h = 5.5f;
                dot->x = x + 6.0f;
                dot->y = itemY + lineH * 0.5f - 2.75f;
                dot->color = th->text;
            } else {
                wchar_t numStr[16];
                Run nrun = {0};
                swprintf(numStr, 16, L"%d.", b->start + i);
                nrun.type = RN_TEXT;
                nrun.text = numStr;
                nrun.len = (int)wcslen(numStr);
                layout_inline_runs(ctx, &nrun, 1, x, bulletW, AL_RIGHT, ctx->baseSize, 1.4f, NULL);
                ctx->curY = itemY; /* restore to line start for children */
            }

            layout_block(ctx, li, x + bulletW, w - bulletW);
            if (!b->tight) ctx->curY += 4.0f;
        }
        break;
    }

    case BK_LI:
        if (b->nruns > 0) {
            layout_inline_runs(ctx, b->runs, b->nruns, x, w, AL_DEFAULT, ctx->baseSize, 1.4f, NULL);
        }
        layout_children(ctx, b, x, w, 4.0f);
        break;

    case BK_CODE: {
        float pad = 10.0f;
        float startY;
        int lineCount = 1, i;
        float codeSize = ctx->baseSize * 0.90f;
        Font *mf = font_primary(FAM_MONO, 0, 0);
        float lineH = (float)(mf->ascent + mf->descent) * codeSize * 1.35f / (float)mf->upem;
        float totalH;
        Item *bg;
        Item *border;

        for (i = 0; i < b->textlen; i++) {
            if (b->text[i] == '\n') lineCount++;
        }
        totalH = pad * 2 + (float)lineCount * lineH;
        check_page_space(ctx, min(totalH, ctx->baseSize * 4.0f));

        startY = ctx->curY;
        bg = add_item(ctx, IT_FILL, 0);
        bg->x = x;
        bg->y = startY;
        bg->w = w;
        bg->h = totalH;
        bg->radius = 6.0f;
        bg->color = th->codeBg;

        border = add_item(ctx, IT_STROKE, 1);
        border->x = x;
        border->y = startY;
        border->w = w;
        border->h = totalH;
        border->radius = 6.0f;
        border->lw = 1.0f;
        border->color = th->codeBorder;

        ctx->curY += pad;

        /* Render code lines */
        {
            int lineStart = 0;
            while (lineStart < b->textlen) {
                int lineEnd = lineStart;
                Run r = {0};
                while (lineEnd < b->textlen && b->text[lineEnd] != '\n' && b->text[lineEnd] != '\r') lineEnd++;
                r.type = RN_TEXT;
                r.style = ST_CODE;
                r.text = b->text + lineStart;
                r.len = lineEnd - lineStart;

                layout_inline_runs(ctx, &r, 1, x + pad, w - pad * 2, AL_DEFAULT, codeSize, 1.35f, NULL);

                if (lineEnd < b->textlen && b->text[lineEnd] == '\r') lineEnd++;
                if (lineEnd < b->textlen && b->text[lineEnd] == '\n') lineEnd++;
                lineStart = lineEnd;
            }
        }

        ctx->curY += pad;
        bg->h = ctx->curY - startY;
        border->h = ctx->curY - startY;
        ctx->curY += 8.0f;
        break;
    }

    case BK_HR: {
        Item *hr;
        ctx->curY += 8.0f;
        check_page_space(ctx, 4.0f);
        hr = add_item(ctx, IT_LINE, 1);
        hr->x = x;
        hr->y = ctx->curY;
        hr->w = w;
        hr->h = 0;
        hr->lw = 1.0f;
        hr->color = th->hr;
        ctx->curY += 8.0f;
        break;
    }

    case BK_TABLE: {
        int ncols = b->ncols > 0 ? b->ncols : 1;
        float colW = w / (float)ncols;
        int r, c;

        ctx->curY += 4.0f;
        for (r = 0; r < b->nkids; r++) {
            Block *tr = b->kids[r];
            float rowStartY = ctx->curY;
            float maxCellH = 0;

            check_page_space(ctx, ctx->baseSize * 2.0f);

            if (tr->header) {
                Item *hbg = add_item(ctx, IT_FILL, 0);
                hbg->x = x;
                hbg->y = rowStartY;
                hbg->w = w;
                hbg->h = 24.0f; /* estimated, updated below */
                hbg->color = th->tableHeadBg;
            }

            for (c = 0; c < tr->nkids && c < ncols; c++) {
                Block *td = tr->kids[c];
                float cellX = x + (float)c * colW + 6.0f;
                float cellW = colW - 12.0f;
                float cellH = 0;
                ctx->curY = rowStartY + 4.0f;
                layout_inline_runs(ctx, td->runs, td->nruns, cellX, cellW, td->align,
                                   ctx->baseSize * 0.95f, 1.3f, &cellH);
                if (cellH + 8.0f > maxCellH) maxCellH = cellH + 8.0f;
            }

            if (maxCellH < 22.0f) maxCellH = 22.0f;
            ctx->curY = rowStartY + maxCellH;

            /* Row bottom border */
            {
                Item *bl = add_item(ctx, IT_LINE, 1);
                bl->x = x;
                bl->y = ctx->curY;
                bl->w = w;
                bl->h = 0;
                bl->lw = tr->header ? 1.5f : 1.0f;
                bl->color = th->tableBorder;
            }
        }
        ctx->curY += 8.0f;
        break;
    }

    case BK_FOOTNOTES: {
        Item *hr;
        ctx->curY += 16.0f;
        check_page_space(ctx, ctx->baseSize * 3.0f);
        hr = add_item(ctx, IT_LINE, 1);
        hr->x = x;
        hr->y = ctx->curY;
        hr->w = w * 0.35f;
        hr->h = 0;
        hr->lw = 1.0f;
        hr->color = th->hr;
        ctx->curY += 10.0f;
        layout_children(ctx, b, x, w, 6.0f);
        break;
    }

    case BK_FOOTNOTE: {
        wchar_t tag[32];
        Run frun = {0};
        swprintf(tag, 32, L"[%d] ", b->footnote);
        frun.type = RN_TEXT;
        frun.style = ST_BOLD;
        frun.text = tag;
        frun.len = (int)wcslen(tag);
        if (b->anchor) add_anchor(ctx, b->anchor, NULL, 0, ctx->curY);
        layout_inline_runs(ctx, &frun, 1, x, 30.0f, AL_DEFAULT, ctx->baseSize * 0.85f, 1.3f, NULL);
        layout_children(ctx, b, x + 30.0f, w - 30.0f, 4.0f);
        break;
    }

    default:
        layout_children(ctx, b, x, w, 6.0f);
        break;
    }
}

/* ---------------- Public API ---------------- */

Layout *layout_build(Doc *doc, const LayoutOptions *opt)
{
    Layout *L = (Layout *)calloc(1, sizeof(Layout));
    LayCtx ctx;
    float contentX, contentW;

    if (!L) return NULL;
    font_init();
    L->opt = *opt;
    L->doc = doc;
    L->npages = 1;
    L->pageWidth = opt->width;
    L->pageHeight = opt->pageHeight;

    ctx.L = L;
    ctx.th = opt->dark ? &g_dark : &g_light;
    L->background = ctx.th->bg;
    ctx.baseSize = opt->baseSize > 0 ? opt->baseSize : 15.0f;
    ctx.isPaged = (opt->pageHeight > 0);
    ctx.curPage = 0;

    contentX = opt->marginL;
    contentW = opt->width - opt->marginL - opt->marginR;
    if (opt->maxContentWidth > 0 && contentW > opt->maxContentWidth) {
        contentX += (contentW - opt->maxContentWidth) * 0.5f;
        contentW = opt->maxContentWidth;
    }

    ctx.contentL = contentX;
    ctx.contentR = contentX + contentW;
    ctx.contentW = contentW;
    ctx.pageTop = opt->marginT;
    ctx.pageBottom = ctx.isPaged ? (opt->pageHeight - opt->marginB) : 1e9f;
    ctx.pageH = opt->pageHeight;
    ctx.curY = ctx.pageTop;

    if (doc && doc->root) {
        layout_block(&ctx, doc->root, contentX, contentW);
    }

    if (!ctx.isPaged) {
        L->pageHeight = ctx.curY + opt->marginB;
    }

    if (L->text) {
        if (L->textlen >= L->textcap) {
            L->textcap = L->textlen + 1;
            L->text = (wchar_t *)realloc(L->text, L->textcap * sizeof(wchar_t));
        }
        L->text[L->textlen] = 0;
    }

    return L;
}

void layout_free(Layout *L)
{
    if (!L) return;
    free(L->text);
    arena_free(&L->arena);
    free(L);
}

int layout_hit_link(const Layout *L, int page, float x, float y)
{
    int i;
    if (!L) return -1;
    for (i = 0; i < L->nlinks; i++) {
        const LinkArea *la = &L->links[i];
        if (la->page == page &&
            x >= la->x && x <= la->x + la->w &&
            y >= la->y && y <= la->y + la->h) {
            return la->link;
        }
    }
    return -1;
}

int layout_hit_text(const Layout *L, int page, float x, float y)
{
    int i, g;
    int bestItem = -1;
    float bestDy = 1e9f;
    if (!L || L->textlen == 0) return -1;

    /* 1. First pass: look for exact line hit */
    for (i = 0; i < L->nitems; i++) {
        const Item *it = &L->items[i];
        if (it->type == IT_GLYPHS && it->page == page && it->n > 0) {
            float top = it->y - it->size;
            float bottom = it->y + it->size * 0.45f;
            if (y >= top && y <= bottom) {
                if (x <= it->x) return it->toff[0];
                if (x >= it->x + it->w) return it->tend;
                float cur = it->x;
                for (g = 0; g < it->n; g++) {
                    float gw = (g + 1 < it->n ? it->dx[g + 1] : it->w) - it->dx[g];
                    if (x <= cur + gw * 0.6f) return it->toff[g];
                    cur += gw;
                }
                return it->tend;
            }
            /* Track closest item vertically */
            float dy = (y < top) ? (top - y) : (y - bottom);
            if (dy < bestDy) {
                bestDy = dy;
                bestItem = i;
            }
        }
    }

    /* 2. Second pass: fallback to closest item vertically */
    if (bestItem >= 0) {
        const Item *it = &L->items[bestItem];
        if (x <= it->x) return it->toff[0];
        if (x >= it->x + it->w) return it->tend;
        float cur = it->x;
        for (g = 0; g < it->n; g++) {
            float gw = (g + 1 < it->n ? it->dx[g + 1] : it->w) - it->dx[g];
            if (x <= cur + gw * 0.6f) return it->toff[g];
            cur += gw;
        }
        return it->tend;
    }

    return -1;
}

const Anchor *layout_find_anchor(const Layout *L, const wchar_t *name)
{
    int i;
    if (!L || !name) return NULL;
    for (i = 0; i < L->nanchors; i++) {
        if (!_wcsicmp(L->anchors[i].name, name)) return &L->anchors[i];
    }
    return NULL;
}

int layout_text_pos(const Layout *L, int off, int *page, float *x, float *y)
{
    int i, g;
    if (!L || L->nitems == 0) return -1;
    for (i = 0; i < L->nitems; i++) {
        const Item *it = &L->items[i];
        if (it->type == IT_GLYPHS && it->n > 0) {
            if (off >= it->toff[0] && off <= it->tend) {
                for (g = 0; g < it->n; g++) {
                    int nextOff = (g + 1 < it->n) ? it->toff[g + 1] : it->tend;
                    if (off >= it->toff[g] && off < nextOff) {
                        if (page) *page = it->page;
                        if (x) *x = it->x + (it->dx ? it->dx[g] : 0);
                        if (y) *y = it->y;
                        return 0;
                    }
                }
                /* If exactly at tend */
                if (off == it->tend) {
                    if (page) *page = it->page;
                    if (x) *x = it->x + it->w;
                    if (y) *y = it->y;
                    return 0;
                }
            }
        }
    }
    return -1;
}
