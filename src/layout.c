/*
 * layout.c - Document layout engine (continuous & paged display list generation).
 * All internal strings and Layout.text are UTF-8.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>
#include "layout.h"
#include "syntax.h"
#include "math.h"

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
    int            trackFirstLine;
    int            firstLineFound;
    int            firstLinePage;
    float          firstLineBaseline;
    float          firstLineSize;
} LayCtx;

static void lay_record_first_line(LayCtx *ctx, float baseline, float fontSize)
{
    if (ctx->trackFirstLine && !ctx->firstLineFound) {
        ctx->firstLineFound = 1;
        ctx->firstLinePage = ctx->curPage;
        ctx->firstLineBaseline = baseline;
        ctx->firstLineSize = fontSize;
    }
}

static Item *add_item(LayCtx *ctx, int type, int layer)
{
    Layout *L = ctx->L;
    Item *it;
    if (L->nitems == L->capitems) {
        int nc = L->capitems ? L->capitems * 2 : 256;
        Item *ni = (Item *)arena_alloc(&L->arena, (size_t)nc * sizeof(Item));
        if (L->nitems) memcpy(ni, L->items, (size_t)L->nitems * sizeof(Item));
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
        LinkArea *na = (LinkArea *)arena_alloc(&L->arena, (size_t)nc * sizeof(LinkArea));
        if (L->nlinks) memcpy(na, L->links, (size_t)L->nlinks * sizeof(LinkArea));
        L->links = na;
        L->caplinks = nc;
    }
    la = &L->links[L->nlinks++];
    la->page = ctx->curPage;
    la->x = x; la->y = y; la->w = w; la->h = h;
    la->link = link;
}

static void add_anchor(LayCtx *ctx, const char *name, const char *title, int level, float y)
{
    Layout *L = ctx->L;
    Anchor *a;
    if (!name) return;
    if (L->nanchors == L->capanchors) {
        int nc = L->capanchors ? L->capanchors * 2 : 16;
        Anchor *na = (Anchor *)arena_alloc(&L->arena, (size_t)nc * sizeof(Anchor));
        if (L->nanchors) memcpy(na, L->anchors, (size_t)L->nanchors * sizeof(Anchor));
        L->anchors = na;
        L->capanchors = nc;
    }
    a = &L->anchors[L->nanchors++];
    a->name = arena_strndup(&L->arena, name, -1);
    a->title = title ? arena_strndup(&L->arena, title, -1) : NULL;
    a->level = level;
    a->page = ctx->curPage;
    a->y = y;
}

static int layout_grow_text(Layout *L, int extra)
{
    int needed, nc;
    char *nt;
    if (!L || extra < 0 || L->textlen > INT_MAX - extra - 2) return -1;
    needed = L->textlen + extra + 2;
    if (needed <= L->textcap) return 0;
    nc = L->textcap > 0 ? L->textcap : 512;
    while (nc < needed) {
        if (nc > INT_MAX / 2) { nc = needed; break; }
        nc *= 2;
    }
    nt = (char *)realloc(L->text, (size_t)nc);
    if (!nt) return -1;
    L->text = nt;
    L->textcap = nc;
    return 0;
}

static int layout_ensure_array(void **ptr, int *cap, int count, size_t elemSize)
{
    int nc;
    void *np;
    if (count < *cap) return 0;
    if (*cap <= 0) nc = 16;
    else if (*cap > INT_MAX / 2) return -1;
    else nc = (*cap) * 2;
    if (nc <= count) nc = count + 1;
    if ((size_t)nc > SIZE_MAX / elemSize) return -1;
    np = realloc(*ptr, (size_t)nc * elemSize);
    if (!np) return -1;
    *ptr = np;
    *cap = nc;
    return 0;
}

static int ltext_put_cp(Layout *L, uint32_t cp)
{
    char u8[4];
    int n = utf8_encode(cp, u8);
    int off = L->textlen;
    if (layout_grow_text(L, n) != 0) return off;
    memcpy(L->text + L->textlen, u8, (size_t)n);
    L->textlen += n;
    L->text[L->textlen] = 0;
    return off;
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
    MathBox *math;
    uint16_t gid;
    float    adv;
    int      link;
    unsigned style;
    uint32_t color;
    uint32_t cp;
} GlyphInfo;

typedef struct VisualLine {
    int   firstGlyph;
    int   glyphCount;
    float w;
    float x;
    float y;
    float ascent;
    float descent;
    int   centerMath;
} VisualLine;

static int is_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||   /* Hangul Jamo */
           (cp >= 0x2E80 && cp <= 0x9FFF) ||   /* CJK Radicals, Symbols, CJK Unified Ideographs */
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   /* Hangul Syllables */
           (cp >= 0xF900 && cp <= 0xFAFF);     /* CJK Compatibility */
}

/* Measure unbreakable minimum width, preferred single-line width, and wrapped width sum
 * for table column sizing. */
static void measure_cell_widths(LayCtx *ctx, const Run *runs, int nruns, float baseFontSize,
                                float *outMinW, float *outPrefW, float *outTotalW)
{
    Layout *L = ctx->L;
    float minW = 0.0f, prefW = 0.0f, curLineW = 0.0f, curTokW = 0.0f, totalW = 0.0f;
    int r;

    for (r = 0; r < nruns; r++) {
        const Run *run = &runs[r];
        int fam = (run->style & ST_CODE) ? FAM_MONO : FAM_BODY;
        int bld = (run->style & ST_BOLD) ? 1 : 0;
        int itl = (run->style & ST_ITALIC) ? 1 : 0;
        float sz = baseFontSize;

        if (run->style & (ST_SUP | ST_SUB)) sz *= 0.75f;
        if ((run->style & ST_CODE) && !(run->style & ST_BLOCKCODE)) sz *= 0.9f;

        if (run->type == RN_BREAK) {
            if (curTokW > minW) minW = curTokW;
            curTokW = 0.0f;
            if (curLineW > prefW) prefW = curLineW;
            curLineW = 0.0f;
            continue;
        }
        if (run->type == RN_IMAGE) {
            float iw = 60.0f;
            if (curTokW > minW) minW = curTokW;
            curTokW = 0.0f;
            if (iw > minW) minW = iw;
            curLineW += iw;
            totalW += iw;
            continue;
        }
        if (run->type == RN_MATH) {
            int isDisp = (run->style & ST_MATH_DISPLAY) != 0;
            float mathSz = isDisp ? baseFontSize * 1.12f : sz;
            MathBox *mb = math_layout(&L->arena, run->text, run->len, mathSz, isDisp, 0);
            float mw = mb ? mb->width : 0.0f;

            if (curTokW > minW) minW = curTokW;
            curTokW = 0.0f;
            if (mw > minW) minW = mw;
            if (isDisp) {
                if (curLineW > prefW) prefW = curLineW;
                curLineW = mw;
                if (curLineW > prefW) prefW = curLineW;
                curLineW = 0.0f;
            } else {
                curLineW += mw;
            }
            totalW += mw;
            continue;
        }
        if (run->type == RN_TEXT || run->type == RN_FOOTREF) {
            int len = run->len;
            int idx = 0;
            while (idx < len) {
                uint32_t cp = utf8_next(run->text, len, &idx);
                int gid = 0;
                Font *f;
                float adv;
                if (cp == '\t') {
                    f = font_resolve(fam, bld, itl, ' ', &gid);
                    adv = (f && f->upem > 0)
                        ? ((float)font_adv(f, gid) * sz / (float)f->upem + (fam == FAM_MONO ? sz * 0.05f : 0.0f)) * 4.0f
                        : sz * 2.0f;
                } else {
                    f = font_resolve(fam, bld, itl, cp, &gid);
                    adv = (f && f->upem > 0)
                        ? (float)font_adv(f, gid) * sz / (float)f->upem
                        : sz * 0.5f;
                    if (fam == FAM_MONO) adv += sz * 0.05f;
                }

                if (cp == ' ' || cp == '\t' || cp == '\n') {
                    if (curTokW > minW) minW = curTokW;
                    curTokW = 0.0f;
                } else if (is_cjk(cp)) {
                    if (curTokW > minW) minW = curTokW;
                    curTokW = 0.0f;
                    if (adv > minW) minW = adv;
                } else {
                    curTokW += adv;
                }

                if (cp == '\n') {
                    if (curLineW > prefW) prefW = curLineW;
                    curLineW = 0.0f;
                } else {
                    curLineW += adv;
                    totalW += adv;
                }
            }
        }
    }

    if (curTokW > minW) minW = curTokW;
    if (curLineW > prefW) prefW = curLineW;
    if (outMinW) *outMinW = minW;
    if (outPrefW) *outPrefW = prefW;
    if (outTotalW) *outTotalW = totalW;
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

    glyphs = (GlyphInfo *)malloc((size_t)glyphCap * sizeof(GlyphInfo));
    if (!glyphs) return;

    if (bodyFont && bodyFont->upem > 0) {
        defaultAscent = (float)bodyFont->ascent * baseFontSize / (float)bodyFont->upem;
        defaultDescent = (float)bodyFont->descent * baseFontSize / (float)bodyFont->upem;
    } else {
        defaultAscent = baseFontSize * 0.8f;
        defaultDescent = baseFontSize * 0.25f;
    }
    defaultLineH = (defaultAscent + defaultDescent) * lineSpacingMul;

    /* 1. Resolve glyphs (and inline/display math boxes) for all runs */
    for (r = 0; r < nruns; r++) {
        const Run *run = &runs[r];
        int fam = (run->style & (ST_CODE | ST_BLOCKCODE)) ? FAM_MONO : FAM_BODY;
        int bld = (run->style & ST_BOLD) != 0;
        int itl = (run->style & ST_ITALIC) != 0;
        float sz = baseFontSize;
        uint32_t col = run->color ? run->color : (fam == FAM_MONO ? th->codeText : th->text);

        if (run->style & ST_LINK) col = th->link;
        if (run->style & (ST_SUP | ST_SUB)) sz *= 0.75f;
        if ((run->style & ST_CODE) && !(run->style & ST_BLOCKCODE)) sz *= 0.9f;

        if (run->type == RN_BREAK) {
            if (layout_ensure_array((void **)&glyphs, &glyphCap, nGlyphs, sizeof(GlyphInfo)) != 0) {
                free(glyphs);
                return;
            }
            memset(&glyphs[nGlyphs], 0, sizeof(GlyphInfo));
            glyphs[nGlyphs].cp = '\n';
            glyphs[nGlyphs].link = -1;
            glyphs[nGlyphs].style = run->style;
            nGlyphs++;
            continue;
        }

        if (run->type == RN_MATH) {
            int isDisp = (run->style & ST_MATH_DISPLAY) != 0;
            float mathSz = isDisp ? baseFontSize * 1.12f : sz;
            MathBox *mb = math_layout(&L->arena, run->text, run->len, mathSz, isDisp, col);

            if (isDisp && nGlyphs > 0 && glyphs[nGlyphs - 1].cp != '\n') {
                if (layout_ensure_array((void **)&glyphs, &glyphCap, nGlyphs, sizeof(GlyphInfo)) != 0) {
                    free(glyphs);
                    return;
                }
                memset(&glyphs[nGlyphs], 0, sizeof(GlyphInfo));
                glyphs[nGlyphs].cp = '\n';
                glyphs[nGlyphs].link = -1;
                nGlyphs++;
            }

            if (layout_ensure_array((void **)&glyphs, &glyphCap, nGlyphs, sizeof(GlyphInfo)) != 0) {
                free(glyphs);
                return;
            }
            memset(&glyphs[nGlyphs], 0, sizeof(GlyphInfo));
            glyphs[nGlyphs].cp = 0xFFFC;
            glyphs[nGlyphs].math = mb;
            glyphs[nGlyphs].adv = mb ? mb->width : 0.0f;
            glyphs[nGlyphs].link = run->link;
            glyphs[nGlyphs].style = run->style;
            glyphs[nGlyphs].color = col;
            nGlyphs++;

            if (isDisp && r + 1 < nruns) {
                if (layout_ensure_array((void **)&glyphs, &glyphCap, nGlyphs, sizeof(GlyphInfo)) != 0) {
                    free(glyphs);
                    return;
                }
                memset(&glyphs[nGlyphs], 0, sizeof(GlyphInfo));
                glyphs[nGlyphs].cp = '\n';
                glyphs[nGlyphs].link = -1;
                nGlyphs++;
            }
            continue;
        }

        if (run->type == RN_TEXT || run->type == RN_FOOTREF) {
            int len = run->len;
            int idx = 0;
            while (idx < len) {
                uint32_t cp = utf8_next(run->text, len, &idx);
                int gid = 0;
                Font *f;
                float adv;
                if (cp == '\t') {
                    f = font_resolve(fam, bld, itl, ' ', &gid);
                    adv = (f && f->upem > 0)
                        ? ((float)font_adv(f, gid) * sz / (float)f->upem + (fam == FAM_MONO ? sz * 0.05f : 0.0f)) * 4.0f
                        : sz * 2.0f;
                } else {
                    f = font_resolve(fam, bld, itl, cp, &gid);
                    adv = (f && f->upem > 0)
                        ? (float)font_adv(f, gid) * sz / (float)f->upem
                        : sz * 0.5f;
                    if (fam == FAM_MONO) {
                        /* Monospace font letter-spacing (자간) to prevent cramped glyphs */
                        adv += sz * 0.05f;
                    }
                }

                if (layout_ensure_array((void **)&glyphs, &glyphCap, nGlyphs, sizeof(GlyphInfo)) != 0) {
                    free(glyphs);
                    return;
                }
                memset(&glyphs[nGlyphs], 0, sizeof(GlyphInfo));
                glyphs[nGlyphs].cp = cp;
                glyphs[nGlyphs].font = f;
                glyphs[nGlyphs].gid = (uint16_t)gid;
                glyphs[nGlyphs].adv = adv;
                glyphs[nGlyphs].link = run->link;
                glyphs[nGlyphs].style = run->style;
                glyphs[nGlyphs].color = col;
                nGlyphs++;
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
        float totalBlockH = 0;

        lines = (VisualLine *)malloc((size_t)lineCap * sizeof(VisualLine));
        if (!lines) {
            free(glyphs);
            if (outHeight) *outHeight = 0;
            return;
        }

        while (start < nGlyphs) {
            int end = start;
            float curW = 0;
            int lastBreak = -1;
            float lastBreakW = 0;
            float lineAsc = defaultAscent;
            float lineDesc = defaultDescent;
            int centerMath = 0;
            int k;

            while (end < nGlyphs) {
                uint32_t c = glyphs[end].cp;
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

                /* Potential break opportunity: after space, CJK character, or inline math box */
                if (c == ' ' || is_cjk(c) || glyphs[end].math != NULL) {
                    lastBreak = end + 1;
                    lastBreakW = curW;
                }
                end++;
            }

            for (k = start; k < end; k++) {
                if (glyphs[k].math) {
                    float ma = glyphs[k].math->ascent + 2.0f;
                    float md = glyphs[k].math->descent + 2.0f;
                    if (ma > lineAsc) lineAsc = ma;
                    if (md > lineDesc) lineDesc = md;
                    if (glyphs[k].style & ST_MATH_DISPLAY) centerMath = 1;
                }
            }

            if (layout_ensure_array((void **)&lines, &lineCap, nLines, sizeof(VisualLine)) != 0) {
                free(lines);
                free(glyphs);
                if (outHeight) *outHeight = 0;
                return;
            }

            lines[nLines].firstGlyph = start;
            lines[nLines].glyphCount = end - start;
            lines[nLines].w = curW;
            lines[nLines].ascent = lineAsc;
            lines[nLines].descent = lineDesc;
            lines[nLines].centerMath = centerMath;
            nLines++;

            start = end;
        }

        /* 3. Emit items for each line */
        for (i = 0; i < nLines; i++) {
            VisualLine *line = &lines[i];
            float rawH = line->ascent + line->descent;
            float lineH = rawH * lineSpacingMul;
            float lx = x0;
            int g, runStart;

            if (line->centerMath) {
                lineH = rawH + 10.0f;
            }
            check_page_space(ctx, lineH);

            if (ctx->trackFirstLine && !ctx->firstLineFound) {
                int hasVis = 0, k;
                for (k = line->firstGlyph; k < line->firstGlyph + line->glyphCount; k++) {
                    if (glyphs[k].cp != '\n') { hasVis = 1; break; }
                }
                if (hasVis) {
                    float leading = lineH - rawH;
                    float halfLeading = leading > 0 ? leading * 0.5f : 0;
                    lay_record_first_line(ctx, ctx->curY + halfLeading + line->ascent, baseFontSize);
                }
            }

            if (align == AL_CENTER || line->centerMath) {
                if (w0 > line->w) lx += (w0 - line->w) * 0.5f;
            } else if (align == AL_RIGHT) {
                if (w0 > line->w) lx += (w0 - line->w);
            }

            /* Group consecutive glyphs sharing same font, size, link, style into IT_GLYPHS */
            runStart = line->firstGlyph;
            while (runStart < line->firstGlyph + line->glyphCount) {
                GlyphInfo *g0 = &glyphs[runStart];
                int runEnd = runStart + 1;
                float runW = g0->adv;
                float curX;
                float leading = lineH - rawH;
                float halfLeading = leading > 0 ? leading * 0.5f : 0;
                float baseline = ctx->curY + halfLeading + line->ascent;

                if (g0->cp == '\n') {
                    ltext_put_cp(L, '\n');
                    runStart++;
                    continue;
                }

                if (g0->math) {
                    math_emit(g0->math, L, ctx->curPage, lx, baseline, g0->link);
                    lx += g0->adv;
                    runStart++;
                    continue;
                }

                while (runEnd < line->firstGlyph + line->glyphCount) {
                    GlyphInfo *g1 = &glyphs[runEnd];
                    if (g1->cp == '\n' || g1->math != NULL) break;
                    if (g1->font != g0->font || g1->style != g0->style ||
                        g1->link != g0->link || g1->color != g0->color) break;
                    runW += g1->adv;
                    runEnd++;
                }

                {
                    int runLen = runEnd - runStart;
                    float sz = baseFontSize;
                    float drawBaseline = baseline;
                    Item *it;

                    if (g0->style & (ST_SUP | ST_SUB)) {
                        sz *= 0.75f;
                        if (g0->style & ST_SUP) drawBaseline -= defaultAscent * 0.35f;
                        else drawBaseline += defaultDescent * 0.30f;
                    }
                    if ((g0->style & ST_CODE) && !(g0->style & ST_BLOCKCODE)) {
                        sz *= 0.9f;
                    }

                    it = add_item(ctx, IT_GLYPHS, 1);
                    it->x = lx;
                    it->y = drawBaseline;
                    it->w = runW;
                    it->h = lineH;
                    it->size = sz;
                    it->font = g0->font;
                    it->color = g0->color;
                    it->link = g0->link;
                    it->n = runLen;
                    it->gids = (uint16_t *)arena_alloc(&L->arena, (size_t)runLen * sizeof(uint16_t));
                    it->dx = (float *)arena_alloc(&L->arena, (size_t)runLen * sizeof(float));
                    it->toff = (int *)arena_alloc(&L->arena, (size_t)runLen * sizeof(int));

                    if (g0->style & ST_MARK) {
                        /* Highlight background */
                        Item *bg = add_item(ctx, IT_FILL, 0);
                        bg->x = lx;
                        bg->y = baseline - defaultAscent - 1.0f;
                        bg->w = runW;
                        bg->h = defaultAscent + defaultDescent + 2.0f;
                        bg->color = th->markBg;
                    }
                    if ((g0->style & ST_CODE) && !(g0->style & ST_BLOCKCODE)) {
                        /* Inline code background */
                        Item *bg = add_item(ctx, IT_FILL, 0);
                        float codePadY = 2.0f;
                        float codeTop = baseline - defaultAscent;
                        float codeH = defaultAscent + defaultDescent;
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
                        it->toff[g] = ltext_put_cp(L, glyphs[runStart + g].cp);
                        curX += glyphs[runStart + g].adv;
                    }
                    it->tend = L->textlen;

                    /* Strike-through and underline */
                    if (g0->style & ST_STRIKE) {
                        Item *st = add_item(ctx, IT_LINE, 1);
                        st->x = lx;
                        st->y = drawBaseline - defaultAscent * 0.35f;
                        st->w = runW;
                        st->h = 0;
                        st->lw = 1.0f;
                        st->color = g0->color;
                    }
                    if (g0->style & ST_UNDERLINE) {
                        Item *un = add_item(ctx, IT_LINE, 1);
                        un->x = lx;
                        un->y = drawBaseline + 2.0f;
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
            totalBlockH += lineH;
        }

        if (outHeight) *outHeight = totalBlockH > 0 ? totalBlockH : (float)nLines * defaultLineH;
        free(lines);
    }

    /* Append newline to Layout plain text so blocks are cleanly separated when copied or searched */
    if (L->text && L->textlen > 0 && L->text[L->textlen - 1] != '\n') {
        ltext_put_cp(L, '\n');
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

typedef struct CodeRunList {
    Run *runs;
    Run stackRuns[64];
    int count;
    int cap;
} CodeRunList;

static void cr_init(CodeRunList *list)
{
    list->runs = list->stackRuns;
    list->count = 0;
    list->cap = 64;
}

static void cr_add(CodeRunList *list, const char *text, int len, SynTokType type, const SynColors *cols)
{
    Run *r;
    if (len <= 0) return;
    if (list->count == list->cap) {
        int newCap = list->cap * 2;
        Run *newBuf = (Run *)malloc((size_t)newCap * sizeof(Run));
        if (!newBuf) return;
        memcpy(newBuf, list->runs, (size_t)list->count * sizeof(Run));
        if (list->runs != list->stackRuns) free(list->runs);
        list->runs = newBuf;
        list->cap = newCap;
    }
    r = &list->runs[list->count++];
    memset(r, 0, sizeof(Run));
    r->type = RN_TEXT;
    r->style = ST_BLOCKCODE;
    if (type == SYN_COMMENT) r->style |= ST_ITALIC;
    r->color = syntax_token_color(cols, type);
    r->text = (char *)text;
    r->len = len;
}

static void cr_free(CodeRunList *list)
{
    if (list->runs && list->runs != list->stackRuns) {
        free(list->runs);
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
            char *title = doc_runs_text(b->runs, b->nruns);
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
                    lay_record_first_line(ctx, ctx->curY + min(dispH, ctx->baseSize), ctx->baseSize);
                    ctx->curY += dispH + 8.0f;
                }
            }
        }
        layout_inline_runs(ctx, b->runs, b->nruns, x, w, b->align, ctx->baseSize, 1.4f, NULL);
        if (!b->tight) ctx->curY += 4.0f;
        break;
    }

    case BK_MATH: {
        Layout *L = ctx->L;
        MathBox *mb = math_layout(&L->arena, b->text ? b->text : "", b->textlen,
                                  ctx->baseSize * 1.15f, 1, th->text);
        if (mb) {
            float padY = 10.0f;
            float totalH = mb->ascent + mb->descent + padY * 2.0f;
            float mx = x;
            check_page_space(ctx, totalH);
            if (b->align == AL_RIGHT) {
                if (w > mb->width) mx = x + (w - mb->width);
            } else if (b->align == AL_LEFT) {
                mx = x;
            } else {
                if (w > mb->width) mx = x + (w - mb->width) * 0.5f;
            }
            math_emit(mb, L, ctx->curPage, mx, ctx->curY + padY + mb->ascent, -1);
            lay_record_first_line(ctx, ctx->curY + padY + mb->ascent, ctx->baseSize * 1.15f);
            ctx->curY += totalH;
            if (L->text && L->textlen > 0 && L->text[L->textlen - 1] != '\n') {
                ltext_put_cp(L, '\n');
            }
        }
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
            const char *title = doc_alert_title(kind);
            Run trun = {0};
            trun.type = RN_TEXT;
            trun.style = ST_BOLD;
            trun.text = (char *)title;
            trun.len = (int)strlen(title);
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
        float fontAsc = (bodyFont && bodyFont->upem > 0)
            ? (float)bodyFont->ascent * ctx->baseSize / (float)bodyFont->upem
            : ctx->baseSize * 0.8f;
        float fontDesc = (bodyFont && bodyFont->upem > 0)
            ? (float)bodyFont->descent * ctx->baseSize / (float)bodyFont->upem
            : ctx->baseSize * 0.25f;
        float rawH = fontAsc + fontDesc;
        float lineH = rawH * 1.4f;
        int i;
        for (i = 0; i < b->nkids; i++) {
            Block *li = b->kids[i];
            float bulletW = 20.0f;
            float itemY;
            int itemPage;
            int markerStart, markerEnd;
            int prevTrack = ctx->trackFirstLine;
            int prevFound = ctx->firstLineFound;
            int prevPage = ctx->firstLinePage;
            float prevBase = ctx->firstLineBaseline;
            float prevSize = ctx->firstLineSize;

            check_page_space(ctx, ctx->baseSize * 1.5f);
            itemY = ctx->curY;
            itemPage = ctx->curPage;

            /* Emit marker without recording itself as list item content */
            ctx->trackFirstLine = 0;
            ctx->firstLineFound = 0;
            markerStart = ctx->L->nitems;

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
                dot->w = 6.0f;
                dot->h = 6.0f;
                dot->x = x + 6.0f;
                dot->y = itemY + lineH * 0.5f - 3.0f;
                dot->color = th->text;
            } else {
                char numStr[16];
                Run nrun = {0};
                snprintf(numStr, sizeof(numStr), "%d.", b->start + i);
                nrun.type = RN_TEXT;
                nrun.text = numStr;
                nrun.len = (int)strlen(numStr);
                layout_inline_runs(ctx, &nrun, 1, x, bulletW, AL_RIGHT, ctx->baseSize, 1.4f, NULL);
                ctx->curY = itemY; /* restore to line start for children */
                ctx->curPage = itemPage;
            }
            markerEnd = ctx->L->nitems;

            /* Layout item children and align marker vertically to the actual first line baseline */
            ctx->trackFirstLine = 1;
            ctx->firstLineFound = 0;
            layout_block(ctx, li, x + bulletW, w - bulletW);

            {
                int targetPage = ctx->firstLineFound ? ctx->firstLinePage : itemPage;
                float targetBase = ctx->firstLineFound
                    ? ctx->firstLineBaseline
                    : (itemY + (lineH - rawH) * 0.5f + fontAsc);
                float targetSize = ctx->firstLineFound ? ctx->firstLineSize : ctx->baseSize;
                float capCenterY = targetBase - targetSize * 0.36f;
                int k;

                if (li->task || b->type == BK_UL) {
                    for (k = markerStart; k < markerEnd; k++) {
                        Item *mk = &ctx->L->items[k];
                        mk->page = targetPage;
                        mk->y = capCenterY - mk->h * 0.5f;
                    }
                } else {
                    float markerBase = targetBase;
                    int hasMarkerGlyph = 0;
                    for (k = markerStart; k < markerEnd; k++) {
                        if (ctx->L->items[k].type == IT_GLYPHS) {
                            markerBase = ctx->L->items[k].y;
                            hasMarkerGlyph = 1;
                            break;
                        }
                    }
                    if (hasMarkerGlyph) {
                        float deltaY = targetBase - markerBase;
                        for (k = markerStart; k < markerEnd; k++) {
                            ctx->L->items[k].page = targetPage;
                            ctx->L->items[k].y += deltaY;
                        }
                    }
                }
            }

            if (prevTrack && !prevFound && ctx->firstLineFound) {
                /* Parent item had no text before this nested list */
                ctx->trackFirstLine = prevTrack;
            } else {
                ctx->trackFirstLine = prevTrack;
                ctx->firstLineFound = prevFound;
                ctx->firstLinePage = prevPage;
                ctx->firstLineBaseline = prevBase;
                ctx->firstLineSize = prevSize;
            }

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
        int tokCount = 0;
        SynToken *tokens = syntax_highlight(b->lang, b->text, b->textlen, &tokCount);
        const SynColors *synColors = syntax_get_colors(ctx->th == &g_dark);
        Layout *L = ctx->L;

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
            int tokIdx = 0;
            while (lineStart < b->textlen) {
                int lineEnd = lineStart;
                while (lineEnd < b->textlen && b->text[lineEnd] != '\n' && b->text[lineEnd] != '\r') lineEnd++;

                if (lineStart == lineEnd) {
                    /* Blank line */
                    check_page_space(ctx, lineH);
                    ctx->curY += lineH;
                    if (L->text && L->textlen > 0 && L->text[L->textlen - 1] != '\n') {
                        ltext_put_cp(L, '\n');
                    }
                } else {
                    CodeRunList crl;
                    cr_init(&crl);

                    while (tokIdx < tokCount && tokens[tokIdx].start + tokens[tokIdx].len <= lineStart) {
                        tokIdx++;
                    }

                    {
                        int t = tokIdx;
                        int pos = lineStart;

                        while (pos < lineEnd) {
                            if (t < tokCount && tokens[t].start < lineEnd) {
                                if (tokens[t].start > pos) {
                                    cr_add(&crl, b->text + pos, tokens[t].start - pos, SYN_DEFAULT, synColors);
                                    pos = tokens[t].start;
                                }
                                {
                                    int tokStart = pos > tokens[t].start ? pos : tokens[t].start;
                                    int tokEnd = lineEnd < tokens[t].start + tokens[t].len ? lineEnd : tokens[t].start + tokens[t].len;
                                    if (tokEnd > tokStart) {
                                        cr_add(&crl, b->text + tokStart, tokEnd - tokStart, tokens[t].type, synColors);
                                        pos = tokEnd;
                                    }
                                }
                                if (tokens[t].start + tokens[t].len <= lineEnd) {
                                    t++;
                                }
                            } else {
                                cr_add(&crl, b->text + pos, lineEnd - pos, SYN_DEFAULT, synColors);
                                pos = lineEnd;
                            }
                        }
                    }

                    if (crl.count > 0) {
                        layout_inline_runs(ctx, crl.runs, crl.count, x + pad, w - pad * 2, AL_DEFAULT, codeSize, 1.35f, NULL);
                    }
                    cr_free(&crl);
                }

                if (lineEnd < b->textlen && b->text[lineEnd] == '\r') lineEnd++;
                if (lineEnd < b->textlen && b->text[lineEnd] == '\n') lineEnd++;
                lineStart = lineEnd;
            }
        }

        if (tokens) free(tokens);

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
        float cellFontSize = ctx->baseSize * 0.95f;
        float cellPadX = 7.0f;
        float *colMinW = (float *)calloc((size_t)ncols, sizeof(float));
        float *colPrefW = (float *)calloc((size_t)ncols, sizeof(float));
        float *colTotW = (float *)calloc((size_t)ncols, sizeof(float));
        float *colW = (float *)calloc((size_t)ncols, sizeof(float));
        float *colX = (float *)calloc((size_t)ncols, sizeof(float));
        int r, c;

        if (!colMinW || !colPrefW || !colTotW || !colW || !colX) {
            free(colMinW); free(colPrefW); free(colTotW); free(colW); free(colX);
            break;
        }

        for (c = 0; c < ncols; c++) {
            colMinW[c] = 42.0f;
            colPrefW[c] = 42.0f;
        }

        /* Pass 1: measure per-column minimum, preferred single-line, and total content widths */
        for (r = 0; r < b->nkids; r++) {
            Block *tr = b->kids[r];
            for (c = 0; c < tr->nkids && c < ncols; c++) {
                Block *td = tr->kids[c];
                float mn = 0.0f, pf = 0.0f, tot = 0.0f;
                float needMin, needPref;
                measure_cell_widths(ctx, td->runs, td->nruns, cellFontSize, &mn, &pf, &tot);
                needMin = mn + cellPadX * 2.0f;
                needPref = pf + cellPadX * 2.0f;
                if (needMin > w * 0.45f) needMin = w * 0.45f;
                if (needMin < 42.0f) needMin = 42.0f;
                if (needMin > colMinW[c]) colMinW[c] = needMin;
                if (needPref > colPrefW[c]) colPrefW[c] = needPref;
                if (tot + cellPadX * 2.0f > colTotW[c]) colTotW[c] = tot + cellPadX * 2.0f;
            }
        }
        for (c = 0; c < ncols; c++) {
            if (colPrefW[c] < colMinW[c]) colPrefW[c] = colMinW[c];
            if (colTotW[c] < colPrefW[c]) colTotW[c] = colPrefW[c];
        }

        /* Pass 2: distribute available table width w across columns proportionally */
        {
            float sumMin = 0.0f, sumPref = 0.0f;
            for (c = 0; c < ncols; c++) {
                sumMin += colMinW[c];
                sumPref += colPrefW[c];
            }
            if (sumPref <= w && sumPref > 0.0f) {
                float rem = w - sumPref;
                for (c = 0; c < ncols; c++) {
                    colW[c] = colPrefW[c] + rem * (colPrefW[c] / sumPref);
                }
            } else if (sumMin < w && sumPref > sumMin) {
                float rem = w - sumMin;
                float sumWeight = 0.0f;
                for (c = 0; c < ncols; c++) {
                    float wt = (colTotW[c] > colMinW[c]) ? (colTotW[c] - colMinW[c]) : (colPrefW[c] - colMinW[c]);
                    sumWeight += wt;
                }
                for (c = 0; c < ncols; c++) {
                    float wt = (colTotW[c] > colMinW[c]) ? (colTotW[c] - colMinW[c]) : (colPrefW[c] - colMinW[c]);
                    colW[c] = colMinW[c] + (sumWeight > 0.0f ? rem * (wt / sumWeight) : rem / (float)ncols);
                }
            } else if (sumMin > 0.0f) {
                float scale = w / sumMin;
                for (c = 0; c < ncols; c++) {
                    colW[c] = colMinW[c] * scale;
                }
            } else {
                for (c = 0; c < ncols; c++) colW[c] = w / (float)ncols;
            }
        }

        colX[0] = x;
        for (c = 1; c < ncols; c++) {
            colX[c] = colX[c - 1] + colW[c - 1];
        }

        ctx->curY += 6.0f;
        for (r = 0; r < b->nkids; r++) {
            Block *tr = b->kids[r];
            float rowStartY;
            float maxCellH = 0;
            Item *hbg = NULL;
            float estH = cellFontSize * 2.2f;
            int savedPaged;

            /* Estimate wrapped row height so page breaks occur before the row rather than mid-cell */
            for (c = 0; c < tr->nkids && c < ncols; c++) {
                Block *td = tr->kids[c];
                float mn = 0.0f, pf = 0.0f, tot = 0.0f;
                float availW = colW[c] - cellPadX * 2.0f;
                int brks = 1, k;
                float lines, hEst;
                if (availW < 8.0f) availW = 8.0f;
                measure_cell_widths(ctx, td->runs, td->nruns, cellFontSize, &mn, &pf, &tot);
                for (k = 0; k < td->nruns; k++) {
                    if (td->runs[k].type == RN_BREAK) brks++;
                }
                lines = (tot / availW) + (float)brks;
                if (lines < 1.2f) lines = 1.2f;
                hEst = lines * cellFontSize * 1.4f + 10.0f;
                if (hEst > estH) estH = hEst;
            }
            if (ctx->isPaged && estH > (ctx->pageBottom - ctx->pageTop) * 0.85f) {
                estH = (ctx->pageBottom - ctx->pageTop) * 0.85f;
            }
            check_page_space(ctx, estH);
            rowStartY = ctx->curY;

            if (r == 0) {
                Item *tl = add_item(ctx, IT_LINE, 1);
                tl->x = x;
                tl->y = rowStartY;
                tl->w = w;
                tl->h = 0;
                tl->lw = 1.0f;
                tl->color = th->tableBorder;
            }

            if (tr->header) {
                hbg = add_item(ctx, IT_FILL, 0);
                hbg->x = x;
                hbg->y = rowStartY;
                hbg->w = w;
                hbg->h = 24.0f; /* updated below */
                hbg->color = th->tableHeadBg;
            }

            savedPaged = ctx->isPaged;
            ctx->isPaged = 0;
            for (c = 0; c < tr->nkids && c < ncols; c++) {
                Block *td = tr->kids[c];
                float cellX = colX[c] + cellPadX;
                float cellW = colW[c] - cellPadX * 2.0f;
                float cellH = 0;
                if (cellW < 8.0f) cellW = 8.0f;
                ctx->curY = rowStartY + 5.0f;
                layout_inline_runs(ctx, td->runs, td->nruns, cellX, cellW, td->align,
                                   cellFontSize, 1.35f, &cellH);
                if (cellH + 10.0f > maxCellH) maxCellH = cellH + 10.0f;
            }
            ctx->isPaged = savedPaged;

            if (maxCellH < 24.0f) maxCellH = 24.0f;
            if (hbg) hbg->h = maxCellH;

            /* Vertical borders (outer left/right + column dividers) */
            for (c = 0; c <= ncols; c++) {
                Item *vl = add_item(ctx, IT_LINE, 1);
                vl->x = (c < ncols) ? colX[c] : (x + w);
                vl->y = rowStartY;
                vl->w = 0;
                vl->h = maxCellH;
                vl->lw = 1.0f;
                vl->color = th->tableBorder;
            }

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
        free(colMinW);
        free(colPrefW);
        free(colTotW);
        free(colW);
        free(colX);
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
        char tag[32];
        Run frun = {0};
        snprintf(tag, sizeof(tag), "[%d] ", b->footnote);
        frun.type = RN_TEXT;
        frun.style = ST_BOLD;
        frun.text = tag;
        frun.len = (int)strlen(tag);
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
    LayCtx ctx = {0};
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
        if (layout_grow_text(L, 0) == 0 && L->textlen < L->textcap) {
            L->text[L->textlen] = 0;
        }
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
                {
                    float cur = it->x;
                    for (g = 0; g < it->n; g++) {
                        float gw = (g + 1 < it->n ? it->dx[g + 1] : it->w) - it->dx[g];
                        if (x <= cur + gw * 0.6f) return it->toff[g];
                        cur += gw;
                    }
                }
                return it->tend;
            }
            /* Track closest item vertically */
            {
                float dy = (y < top) ? (top - y) : (y - bottom);
                if (dy < bestDy) {
                    bestDy = dy;
                    bestItem = i;
                }
            }
        }
    }

    /* 2. Second pass: fallback to closest item vertically */
    if (bestItem >= 0) {
        const Item *it = &L->items[bestItem];
        if (x <= it->x) return it->toff[0];
        if (x >= it->x + it->w) return it->tend;
        {
            float cur = it->x;
            for (g = 0; g < it->n; g++) {
                float gw = (g + 1 < it->n ? it->dx[g + 1] : it->w) - it->dx[g];
                if (x <= cur + gw * 0.6f) return it->toff[g];
                cur += gw;
            }
        }
        return it->tend;
    }

    return -1;
}

const Anchor *layout_find_anchor(const Layout *L, const char *name)
{
    int i;
    if (!L || !name) return NULL;
    for (i = 0; i < L->nanchors; i++) {
        if (!ascii_icmp(L->anchors[i].name, name)) return &L->anchors[i];
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
