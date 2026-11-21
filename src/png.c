/*
 * png.c - Layout -> PNG raster exporter (via GDI DIBSection + image_encode_png).
 * Supports continuous single-strip output and page-split output.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "export.h"
#include "font.h"
#include "image.h"
#include "util.h"

#define PNG_MAX_PIXELS 0x04000000ULL /* 64M pixels max per output image */
#define PNG_DEFAULT_MAX_PAGES 1000

static void set_err(char *err, int errcap, const char *msg)
{
    if (err && errcap > 0 && msg) {
        strncpy_s(err, (size_t)errcap, msg, _TRUNCATE);
    }
}

static COLORREF rgb_from_u32(uint32_t c)
{
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static int round_i(float v)
{
    return (int)lroundf(v);
}

static char *build_paged_png_path(const char *basePath, int pageNum)
{
    size_t len = strlen(basePath);
    size_t stemLen = len;
    size_t outCap;
    char *out;

    if (len >= 4 && _stricmp(basePath + len - 4, ".png") == 0) {
        stemLen = len - 4;
    }
    outCap = stemLen + 32;
    out = (char *)malloc(outCap);
    if (!out) return NULL;
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s_%d.png", (int)stemLen, basePath, pageNum);
    return out;
}

static int render_page_to_png_file(const Layout *L, int page, int w, int h,
                                   const char *outPath, char *err, int errcap)
{
    BITMAPINFO bi;
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ oldBm;
    uint32_t *bits = NULL;
    RECT rc = {0, 0, w, h};
    HBRUSH bgBrush;
    int i, layer;
    size_t pxIdx, totalPx;
    uint8_t *pngData = NULL;
    size_t pngLen = 0;

    if ((uint64_t)w * (uint64_t)h > PNG_MAX_PIXELS) {
        set_err(err, errcap, "문서가 너무 길어 단일 PNG로 변환할 수 없습니다. 페이지 분할 옵션을 사용해 주세요.");
        return -1;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        set_err(err, errcap, "메모리 DC 생성에 실패했습니다.");
        return -1;
    }

    hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(hdc);
        set_err(err, errcap, "문서가 너무 길어 단일 PNG로 변환할 수 없습니다. 페이지 분할 옵션을 사용해 주세요.");
        return -1;
    }

    oldBm = SelectObject(hdc, hbm);

    bgBrush = CreateSolidBrush(rgb_from_u32(L->background));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_BASELINE | TA_LEFT);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);

    for (layer = 0; layer < 2; layer++) {
        for (i = 0; i < L->nitems; i++) {
            const Item *it = &L->items[i];
            if (it->page != page || it->layer != layer) continue;

            switch (it->type) {
            case IT_FILL: {
                HBRUSH br = CreateSolidBrush(rgb_from_u32(it->color));
                RECT r = {round_i(it->x), round_i(it->y), round_i(it->x + it->w), round_i(it->y + it->h)};
                if (it->radius > 0.0f) {
                    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                    HGDIOBJ oldP = SelectObject(hdc, nullPen);
                    HGDIOBJ oldB = SelectObject(hdc, br);
                    RoundRect(hdc, r.left, r.top, r.right, r.bottom, (int)(it->radius * 2.0f), (int)(it->radius * 2.0f));
                    SelectObject(hdc, oldB);
                    SelectObject(hdc, oldP);
                } else {
                    FillRect(hdc, &r, br);
                }
                DeleteObject(br);
                break;
            }

            case IT_STROKE: {
                int lw = round_i(it->lw);
                HPEN pen = CreatePen(PS_SOLID, lw > 1 ? lw : 1, rgb_from_u32(it->color));
                HGDIOBJ oldP = SelectObject(hdc, pen);
                HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                RECT r = {round_i(it->x), round_i(it->y), round_i(it->x + it->w), round_i(it->y + it->h)};
                if (it->radius > 0.0f) {
                    RoundRect(hdc, r.left, r.top, r.right, r.bottom, (int)(it->radius * 2.0f), (int)(it->radius * 2.0f));
                } else {
                    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                }
                SelectObject(hdc, oldB);
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_LINE: {
                int lw = round_i(it->lw);
                HPEN pen = CreatePen(PS_SOLID, lw > 1 ? lw : 1, rgb_from_u32(it->color));
                HGDIOBJ oldP = SelectObject(hdc, pen);
                MoveToEx(hdc, round_i(it->x), round_i(it->y), NULL);
                LineTo(hdc, round_i(it->x + it->w), round_i(it->y + it->h));
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_DISC: {
                HBRUSH br = CreateSolidBrush(rgb_from_u32(it->color));
                HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ oldP = SelectObject(hdc, nullPen);
                HGDIOBJ oldB = SelectObject(hdc, br);
                int ix = round_i(it->x);
                int iy = round_i(it->y);
                int iw = round_i(it->w);
                int ih = round_i(it->h);
                if (iw < 2) iw = 2;
                if (ih < 2) ih = 2;
                Ellipse(hdc, ix, iy, ix + iw, iy + ih);
                SelectObject(hdc, oldB);
                SelectObject(hdc, oldP);
                DeleteObject(br);
                break;
            }

            case IT_CIRCLE: {
                int lw = round_i(it->lw);
                HPEN pen = CreatePen(PS_SOLID, lw > 1 ? lw : 1, rgb_from_u32(it->color));
                HGDIOBJ oldP = SelectObject(hdc, pen);
                HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                int ix = round_i(it->x);
                int iy = round_i(it->y);
                int iw = round_i(it->w);
                int ih = round_i(it->h);
                if (iw < 2) iw = 2;
                if (ih < 2) ih = 2;
                Ellipse(hdc, ix, iy, ix + iw, iy + ih);
                SelectObject(hdc, oldB);
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_CHECK: {
                int lw = round_i(it->lw);
                HPEN pen = CreatePen(PS_SOLID, lw > 2 ? lw : 2, rgb_from_u32(it->color));
                HGDIOBJ oldP = SelectObject(hdc, pen);
                POINT pts[3];
                pts[0].x = round_i(it->x + it->w * 0.2f);
                pts[0].y = round_i(it->y + it->h * 0.55f);
                pts[1].x = round_i(it->x + it->w * 0.42f);
                pts[1].y = round_i(it->y + it->h * 0.75f);
                pts[2].x = round_i(it->x + it->w * 0.8f);
                pts[2].y = round_i(it->y + it->h * 0.28f);
                Polyline(hdc, pts, 3);
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_IMAGE: {
                Image *img = it->img;
                if (img && img->w > 0 && img->h > 0 && img->pixels) {
                    BITMAPINFO ibi;
                    memset(&ibi, 0, sizeof(ibi));
                    ibi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    ibi.bmiHeader.biWidth = img->w;
                    ibi.bmiHeader.biHeight = -img->h;
                    ibi.bmiHeader.biPlanes = 1;
                    ibi.bmiHeader.biBitCount = 32;
                    ibi.bmiHeader.biCompression = BI_RGB;
                    StretchDIBits(hdc,
                                  round_i(it->x), round_i(it->y), round_i(it->w), round_i(it->h),
                                  0, 0, img->w, img->h,
                                  img->pixels, &ibi, DIB_RGB_COLORS, SRCCOPY);
                }
                break;
            }

            case IT_GLYPHS: {
                if (it->font && it->n > 0) {
                    int pixelSize = round_i(it->size);
                    HFONT hf = (HFONT)font_hfont(it->font, pixelSize);
                    if (hf) {
                        HGDIOBJ oldF = SelectObject(hdc, hf);
                        int stackDx[256];
                        int *dx = (it->n <= 256) ? stackDx : (int *)malloc((size_t)it->n * sizeof(int));
                        int g;
                        SetTextColor(hdc, rgb_from_u32(it->color));
                        if (dx && it->dx) {
                            for (g = 0; g < it->n - 1; g++) {
                                dx[g] = round_i(it->dx[g + 1]) - round_i(it->dx[g]);
                            }
                            dx[it->n - 1] = round_i(it->w) - round_i(it->dx[it->n - 1]);
                        }
                        ExtTextOutW(hdc, round_i(it->x), round_i(it->y),
                                    ETO_GLYPH_INDEX, NULL, (LPCWSTR)it->gids, it->n,
                                    (dx && it->dx) ? dx : NULL);
                        if (dx && dx != stackDx) free(dx);
                        SelectObject(hdc, oldF);
                    }
                }
                break;
            }
            }
        }
    }

    GdiFlush();
    totalPx = (size_t)w * (size_t)h;
    for (pxIdx = 0; pxIdx < totalPx; pxIdx++) {
        bits[pxIdx] |= 0xFF000000u;
    }

    if (image_encode_png(bits, w, h, &pngData, &pngLen) != 0) {
        SelectObject(hdc, oldBm);
        DeleteObject(hbm);
        DeleteDC(hdc);
        set_err(err, errcap, "PNG 인코딩에 실패했습니다.");
        return -1;
    }

    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);

    if (file_write_all(outPath, pngData, pngLen) != 0) {
        free(pngData);
        set_err(err, errcap, "PNG 파일 쓰기에 실패했습니다.");
        return -1;
    }
    free(pngData);
    return 0;
}

static void build_png_layout_opts(const PngExportOptions *opt, LayoutOptions *out,
                                  int *outW, int *outPaged)
{
    int w = (opt && opt->width > 0) ? opt->width : 1050;
    int paged = (opt && opt->paged) ? 1 : 0;
    int dark = (opt && opt->dark) ? 1 : 0;
    float margin;

    if (w < 320) w = 320;
    if (w > 4096) w = 4096;
    margin = (w >= 700) ? 54.0f : (float)(w / 14);

    memset(out, 0, sizeof(*out));
    out->width = (float)w;
    out->marginL = margin;
    out->marginR = margin;
    out->marginT = margin;
    out->marginB = margin;
    out->maxContentWidth = 0.0f;
    out->baseSize = 15.0f;
    out->dark = dark;
    out->pageHeight = paged ? ((float)w * (1122.5f / 793.7f)) : 0.0f;

    if (outW) *outW = w;
    if (outPaged) *outPaged = paged;
}

int export_png_page_count(Doc *doc, const PngExportOptions *opt)
{
    LayoutOptions lopt;
    Layout *L;
    int npages;

    if (!doc) return -1;
    doc_resolve_images(doc, 1);
    build_png_layout_opts(opt, &lopt, NULL, NULL);
    L = layout_build(doc, &lopt);
    if (!L) return -1;
    npages = L->npages;
    layout_free(L);
    return npages;
}

int export_png_check_overwrite(const char *path, int pageCount, char *firstConflict, size_t cap)
{
    int p;
    if (firstConflict && cap > 0) firstConflict[0] = 0;
    if (!path || !path[0] || pageCount <= 1) return 0;

    for (p = 1; p <= pageCount; p++) {
        char *pagePath = build_paged_png_path(path, p);
        if (!pagePath) return 0;
        if (file_exists(pagePath)) {
            if (firstConflict && cap > 0) {
                strncpy_s(firstConflict, cap, pagePath, _TRUNCATE);
            }
            free(pagePath);
            return 1;
        }
        free(pagePath);
    }
    return 0;
}

int export_png(Doc *doc, const char *path, const PngExportOptions *opt,
               int *outPagesWritten, char *err, int errcap)
{
    LayoutOptions lopt;
    Layout *L;
    int w, paged, maxPages;
    int h, p, written = 0;

    if (outPagesWritten) *outPagesWritten = 0;
    if (!doc || !path || !path[0]) {
        set_err(err, errcap, "잘못된 인자입니다.");
        return -1;
    }

    doc_resolve_images(doc, 1);
    build_png_layout_opts(opt, &lopt, &w, &paged);
    maxPages = (opt && opt->maxPages > 0) ? opt->maxPages : PNG_DEFAULT_MAX_PAGES;

    L = layout_build(doc, &lopt);
    if (!L) {
        set_err(err, errcap, "PNG 조판에 실패했습니다.");
        return -1;
    }

    h = (int)ceilf(L->pageHeight);
    if (h < 1) h = 1;

    if ((uint64_t)w * (uint64_t)h > PNG_MAX_PIXELS) {
        layout_free(L);
        set_err(err, errcap, "문서가 너무 길어 단일 PNG로 변환할 수 없습니다. 페이지 분할 옵션을 사용해 주세요.");
        return -1;
    }

    if (paged && L->npages > maxPages) {
        layout_free(L);
        set_err(err, errcap, "페이지 수가 너무 많아 PNG 내보내기를 중단했습니다.");
        return -1;
    }

    if (!paged || L->npages <= 1) {
        if (render_page_to_png_file(L, 0, w, h, path, err, errcap) != 0) {
            layout_free(L);
            return -1;
        }
        written = 1;
    } else {
        for (p = 0; p < L->npages; p++) {
            char *pagePath = build_paged_png_path(path, p + 1);
            int rc;
            if (!pagePath) {
                layout_free(L);
                set_err(err, errcap, "메모리가 부족합니다.");
                return -1;
            }
            rc = render_page_to_png_file(L, p, w, h, pagePath, err, errcap);
            free(pagePath);
            if (rc != 0) {
                layout_free(L);
                return -1;
            }
            written++;
        }
    }

    if (outPagesWritten) *outPagesWritten = written;
    layout_free(L);
    return 0;
}
