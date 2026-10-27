/*
 * font.c - TrueType font metrics, cmap parsing, GDI font fallback & extraction.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "font.h"

#define MAX_FONTS 64

static Font *g_fonts[MAX_FONTS];
static int   g_numFonts = 0;
static HDC   g_fontHdc = NULL;

typedef struct HFontEntry {
    int   size;
    HFONT hf;
} HFontEntry;

#define MAX_HFONTS 16
typedef struct HFontCache {
    HFontEntry entries[MAX_HFONTS];
    int count;
} HFontCache;

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  read_i16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t read_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }

void font_init(void)
{
    if (!g_fontHdc) g_fontHdc = CreateCompatibleDC(NULL);
}

void font_shutdown(void)
{
    int i, j;
    for (i = 0; i < g_numFonts; i++) {
        Font *f = g_fonts[i];
        if (f) {
            free(f->adv);
            free(f->cmapData);
            if (f->hfonts) {
                HFontCache *hc = (HFontCache *)f->hfonts;
                for (j = 0; j < hc->count; j++) {
                    if (hc->entries[j].hf) DeleteObject(hc->entries[j].hf);
                }
                free(hc);
            }
            free(f);
            g_fonts[i] = NULL;
        }
    }
    g_numFonts = 0;
    if (g_fontHdc) {
        DeleteDC(g_fontHdc);
        g_fontHdc = NULL;
    }
}

int font_count(void) { return g_numFonts; }
Font *font_by_id(int id) { return (id >= 0 && id < g_numFonts) ? g_fonts[id] : NULL; }

uint8_t *font_table(const Font *f, uint32_t tag, uint32_t *len)
{
    DWORD dwTag = _byteswap_ulong(tag);
    DWORD sz;
    HFONT hf;
    HGDIOBJ old;
    uint8_t *buf;

    if (!g_fontHdc) font_init();
    hf = CreateFontW(32, 0, 0, 0, f->bold ? FW_BOLD : FW_NORMAL, f->italic ? TRUE : FALSE,
                     FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, f->face);
    if (!hf) return NULL;
    old = SelectObject(g_fontHdc, hf);

    sz = GetFontData(g_fontHdc, dwTag, 0, NULL, 0);
    if (sz == GDI_ERROR || sz == 0) {
        SelectObject(g_fontHdc, old);
        DeleteObject(hf);
        return NULL;
    }
    buf = (uint8_t *)malloc(sz);
    if (buf) {
        GetFontData(g_fontHdc, dwTag, 0, buf, sz);
        if (len) *len = sz;
    }
    SelectObject(g_fontHdc, old);
    DeleteObject(hf);
    return buf;
}

static void parse_name(Font *f, const uint8_t *data, uint32_t len)
{
    uint16_t count, stringOffset, i;
    const uint8_t *records;
    if (len < 6) return;
    count = read_u16(data + 2);
    stringOffset = read_u16(data + 4);
    records = data + 6;
    if (len < (uint32_t)(6 + count * 12)) return;

    for (i = 0; i < count; i++) {
        const uint8_t *rec = records + i * 12;
        uint16_t platformId = read_u16(rec);
        uint16_t nameId = read_u16(rec + 6);
        uint16_t nlen = read_u16(rec + 8);
        uint16_t offset = read_u16(rec + 10);
        if (nameId == 6 && (uint32_t)(stringOffset + offset + nlen) <= len) {
            const uint8_t *s = data + stringOffset + offset;
            int k, o = 0;
            if (platformId == 3 || platformId == 0) { /* UTF-16BE */
                for (k = 0; k < nlen && o < 63; k += 2) {
                    char c = (char)s[k + 1];
                    if (c > 32 && c < 127) f->psName[o++] = c;
                }
            } else { /* Mac or Roman */
                for (k = 0; k < nlen && o < 63; k++) {
                    char c = (char)s[k];
                    if (c > 32 && c < 127) f->psName[o++] = c;
                }
            }
            f->psName[o] = 0;
            if (o > 0) return;
        }
    }
}

static void setup_cmap(Font *f, const uint8_t *data, uint32_t len)
{
    uint16_t numTables, i;
    uint32_t sub12 = 0, sub4 = 0;
    if (len < 4) return;
    numTables = read_u16(data + 2);
    if (len < (uint32_t)(4 + numTables * 8)) return;

    for (i = 0; i < numTables; i++) {
        uint16_t platformId = read_u16(data + 4 + i * 8);
        uint16_t encodingId = read_u16(data + 4 + i * 8 + 2);
        uint32_t offset = read_u32(data + 4 + i * 8 + 4);
        if (offset + 4 <= len) {
            uint16_t format = read_u16(data + offset);
            if (format == 12 && (platformId == 3 && (encodingId == 10 || encodingId == 1))) sub12 = offset;
            else if (format == 4 && (platformId == 3 || platformId == 0)) {
                if (!sub4) sub4 = offset;
            }
        }
    }

    if (sub12) {
        f->cmapSub = sub12;
        f->cmapFormat = 12;
    } else if (sub4) {
        f->cmapSub = sub4;
        f->cmapFormat = 4;
    }
}

Font *font_get(const wchar_t *face, int bold, int italic)
{
    int i;
    Font *f;
    uint8_t *head = NULL, *maxp = NULL, *hhea = NULL, *hmtx = NULL;
    uint8_t *os2 = NULL, *post = NULL, *name = NULL, *cmap = NULL;
    uint32_t headLen = 0, maxpLen = 0, hheaLen = 0, hmtxLen = 0;
    uint32_t os2Len = 0, postLen = 0, nameLen = 0, cmapLen = 0;
    uint16_t numOfHMetrics = 0;

    for (i = 0; i < g_numFonts; i++) {
        if (!wcscmp(g_fonts[i]->face, face) &&
            g_fonts[i]->bold == bold &&
            g_fonts[i]->italic == italic) {
            return g_fonts[i];
        }
    }

    if (g_numFonts >= MAX_FONTS) return NULL;
    f = (Font *)calloc(1, sizeof(Font));
    if (!f) return NULL;

    f->id = g_numFonts;
    wcsncpy_s(f->face, 32, face, _TRUNCATE);
    f->bold = bold;
    f->italic = italic;

    /* Check if TrueType font data exists */
    head = font_table(f, MV_TAG('h','e','a','d'), &headLen);
    if (!head || headLen < 54) { free(head); free(f); return NULL; }

    f->isTrueType = 1;
    f->upem = read_u16(head + 18);
    f->xMin = read_i16(head + 36);
    f->yMin = read_i16(head + 38);
    f->xMax = read_i16(head + 40);
    f->yMax = read_i16(head + 42);
    free(head);

    maxp = font_table(f, MV_TAG('m','a','x','p'), &maxpLen);
    if (maxp && maxpLen >= 6) {
        f->numGlyphs = read_u16(maxp + 4);
    }
    free(maxp);
    if (f->numGlyphs <= 0) { free(f); return NULL; }

    hhea = font_table(f, MV_TAG('h','h','e','a'), &hheaLen);
    if (hhea && hheaLen >= 36) {
        f->ascent = read_i16(hhea + 4);
        f->descent = -read_i16(hhea + 6);
        numOfHMetrics = read_u16(hhea + 34);
    }
    free(hhea);

    hmtx = font_table(f, MV_TAG('h','m','t','x'), &hmtxLen);
    if (hmtx && numOfHMetrics > 0) {
        f->adv = (uint16_t *)malloc(f->numGlyphs * sizeof(uint16_t));
        if (f->adv) {
            uint16_t lastAdv = 0;
            int g;
            for (g = 0; g < f->numGlyphs; g++) {
                if (g < numOfHMetrics && (uint32_t)(g * 4 + 2) <= hmtxLen) {
                    lastAdv = read_u16(hmtx + g * 4);
                    f->adv[g] = lastAdv;
                } else {
                    f->adv[g] = lastAdv;
                }
            }
        }
    }
    free(hmtx);

    os2 = font_table(f, MV_TAG('O','S','/','2'), &os2Len);
    if (os2 && os2Len >= 78) {
        f->weightClass = read_u16(os2 + 4);
        f->ascent = read_u16(os2 + 74);
        f->descent = read_u16(os2 + 76);
        if (os2Len >= 90) f->capHeight = read_i16(os2 + 88);
    }
    free(os2);
    if (f->capHeight <= 0) f->capHeight = (f->ascent * 7) / 10;

    post = font_table(f, MV_TAG('p','o','s','t'), &postLen);
    if (post && postLen >= 16) {
        f->italicAngle = (int)(read_i16(post + 4));
        f->underlinePos = read_i16(post + 8);
        f->underlineThickness = read_i16(post + 10);
        f->fixedPitch = read_u32(post + 12) != 0;
    }
    free(post);

    name = font_table(f, MV_TAG('n','a','m','e'), &nameLen);
    if (name) {
        parse_name(f, name, nameLen);
        free(name);
    }
    if (!f->psName[0]) {
        int k, o = 0;
        for (k = 0; f->face[k] && o < 50; k++) {
            if ((f->face[k] >= 'A' && f->face[k] <= 'Z') ||
                (f->face[k] >= 'a' && f->face[k] <= 'z') ||
                (f->face[k] >= '0' && f->face[k] <= '9')) {
                f->psName[o++] = (char)f->face[k];
            }
        }
        if (bold && italic) strcpy_s(f->psName + o, 64 - o, "-BoldItalic");
        else if (bold) strcpy_s(f->psName + o, 64 - o, "-Bold");
        else if (italic) strcpy_s(f->psName + o, 64 - o, "-Italic");
        else f->psName[o] = 0;
    }

    cmap = font_table(f, MV_TAG('c','m','a','p'), &cmapLen);
    if (cmap) {
        f->cmapData = cmap;
        f->cmapLen = cmapLen;
        setup_cmap(f, cmap, cmapLen);
    }

    g_fonts[g_numFonts++] = f;
    return f;
}

int font_glyph(Font *f, uint32_t cp)
{
    if (!f || !f->cmapData) return 0;

    if (f->cmapFormat == 4) {
        const uint8_t *sub = f->cmapData + f->cmapSub;
        uint16_t segCountX2 = read_u16(sub + 6);
        uint16_t segCount = segCountX2 / 2;
        const uint8_t *endCode = sub + 14;
        const uint8_t *startCode = endCode + segCountX2 + 2;
        const uint8_t *idDelta = startCode + segCountX2;
        const uint8_t *idRangeOffset = idDelta + segCountX2;
        uint16_t i;

        if (cp > 0xFFFF) return 0;
        for (i = 0; i < segCount; i++) {
            uint16_t end = read_u16(endCode + i * 2);
            if (end >= cp) {
                uint16_t start = read_u16(startCode + i * 2);
                if (start <= cp) {
                    uint16_t rangeOff = read_u16(idRangeOffset + i * 2);
                    int16_t delta = read_i16(idDelta + i * 2);
                    if (rangeOff == 0) {
                        return (uint16_t)((cp + delta) & 0xFFFF);
                    } else {
                        const uint8_t *ptr = idRangeOffset + i * 2 + rangeOff + (cp - start) * 2;
                        if ((uint32_t)(ptr - f->cmapData + 2) <= f->cmapLen) {
                            uint16_t gid = read_u16(ptr);
                            if (gid != 0) return (uint16_t)((gid + delta) & 0xFFFF);
                        }
                    }
                }
                break;
            }
        }
    } else if (f->cmapFormat == 12) {
        const uint8_t *sub = f->cmapData + f->cmapSub;
        uint32_t nGroups = read_u32(sub + 12);
        const uint8_t *groups = sub + 16;
        int l = 0, r = (int)nGroups - 1;
        while (l <= r) {
            int m = (l + r) / 2;
            const uint8_t *grp = groups + m * 12;
            uint32_t start = read_u32(grp);
            uint32_t end = read_u32(grp + 4);
            if (cp < start) r = m - 1;
            else if (cp > end) l = m + 1;
            else {
                uint32_t startGid = read_u32(grp + 8);
                return (int)(startGid + (cp - start));
            }
        }
    }
    return 0;
}

#define MAX_FALLBACKS 16

typedef struct FontChain {
    wchar_t faces[MAX_FALLBACKS][64];
    int count;
    wchar_t formatted[512];
} FontChain;

static FontChain g_bodyChain;
static FontChain g_monoChain;
static int g_chainInited = 0;

static const wchar_t g_defaultBodyChain[] =
    L"Pretendard, 맑은 고딕, Segoe UI, Segoe UI Emoji, Segoe UI Symbol, Arial, Gulim";
static const wchar_t g_defaultMonoChain[] =
    L"D2Coding, Consolas, 맑은 고딕, Segoe UI Emoji, Segoe UI Symbol, Courier New";

static void chain_set(FontChain *fc, const wchar_t *str, const wchar_t *defaultStr)
{
    const wchar_t *p = (str && str[0]) ? str : defaultStr;
    fc->count = 0;
    while (*p && fc->count < MAX_FALLBACKS) {
        while (*p == L' ' || *p == L'\t' || *p == L',' || *p == L';') p++;
        if (!*p) break;
        const wchar_t *start = p;
        while (*p && *p != L',' && *p != L';') p++;
        const wchar_t *end = p;
        while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) end--;
        int len = (int)(end - start);
        if (len > 0 && len < 64) {
            int dup = 0;
            int i;
            for (i = 0; i < fc->count; i++) {
                if (!_wcsnicmp(fc->faces[i], start, len) && fc->faces[i][len] == 0) {
                    dup = 1; break;
                }
            }
            if (!dup) {
                wcsncpy_s(fc->faces[fc->count], 64, start, len);
                fc->faces[fc->count][len] = 0;
                fc->count++;
            }
        }
    }

    /* Ensure essential fallback fonts for Emojis and Symbols are always present */
    {
        const wchar_t *essential[] = { L"Segoe UI Emoji", L"Segoe UI Symbol", L"Malgun Gothic", NULL };
        int e;
        for (e = 0; essential[e]; e++) {
            int found = 0;
            int i;
            for (i = 0; i < fc->count; i++) {
                if (!_wcsicmp(fc->faces[i], essential[e])) { found = 1; break; }
            }
            if (!found && fc->count < MAX_FALLBACKS) {
                wcsncpy_s(fc->faces[fc->count++], 64, essential[e], _TRUNCATE);
            }
        }
    }

    /* Format into comma-separated string */
    fc->formatted[0] = 0;
    for (int i = 0; i < fc->count; i++) {
        if (i > 0) wcscat_s(fc->formatted, 512, L", ");
        wcscat_s(fc->formatted, 512, fc->faces[i]);
    }
}

static void ensure_chains_inited(void)
{
    if (!g_chainInited) {
        chain_set(&g_bodyChain, NULL, g_defaultBodyChain);
        chain_set(&g_monoChain, NULL, g_defaultMonoChain);
        g_chainInited = 1;
    }
}

void font_set_family_chain(int family, const wchar_t *chain)
{
    ensure_chains_inited();
    if (family == FAM_MONO) {
        chain_set(&g_monoChain, chain, g_defaultMonoChain);
    } else {
        chain_set(&g_bodyChain, chain, g_defaultBodyChain);
    }
}

const wchar_t *font_get_family_chain(int family)
{
    ensure_chains_inited();
    return (family == FAM_MONO) ? g_monoChain.formatted : g_bodyChain.formatted;
}

void font_set_family_face(int family, const wchar_t *face)
{
    ensure_chains_inited();
    if (!face || !face[0]) return;
    wchar_t newChain[512];
    const wchar_t *curChain = font_get_family_chain(family);
    _snwprintf_s(newChain, 512, _TRUNCATE, L"%ls, %ls", face, curChain);
    font_set_family_chain(family, newChain);
}

const wchar_t *font_get_family_face(int family)
{
    ensure_chains_inited();
    FontChain *fc = (family == FAM_MONO) ? &g_monoChain : &g_bodyChain;
    return (fc->count > 0) ? fc->faces[0] : (family == FAM_MONO ? L"Consolas" : L"Segoe UI");
}

Font *font_primary(int family, int bold, int italic)
{
    ensure_chains_inited();
    FontChain *fc = (family == FAM_MONO) ? &g_monoChain : &g_bodyChain;
    int i;
    for (i = 0; i < fc->count; i++) {
        Font *f = font_get(fc->faces[i], bold, italic);
        if (f) return f;
        if (bold || italic) {
            f = font_get(fc->faces[i], 0, 0);
            if (f) return f;
        }
    }
    return font_get(L"Segoe UI", 0, 0);
}

Font *font_resolve(int family, int bold, int italic, uint32_t cp, int *gid)
{
    ensure_chains_inited();
    FontChain *fc = (family == FAM_MONO) ? &g_monoChain : &g_bodyChain;
    Font *primary = NULL;
    int i;

    for (i = 0; i < fc->count; i++) {
        Font *f = font_get(fc->faces[i], bold, italic);
        if (!f && (bold || italic)) f = font_get(fc->faces[i], 0, 0);
        if (f) {
            int g;
            if (!primary) primary = f;
            g = font_glyph(f, cp);
            if (g > 0) {
                if (gid) *gid = g;
                return f;
            }
        }
    }

    /* Fallback to Segoe UI Emoji and Segoe UI Symbol */
    {
        const wchar_t *extra[] = { L"Segoe UI Emoji", L"Segoe UI Symbol", L"Malgun Gothic", NULL };
        for (i = 0; extra[i]; i++) {
            Font *f = font_get(extra[i], 0, 0);
            if (f) {
                int g = font_glyph(f, cp);
                if (g > 0) {
                    if (gid) *gid = g;
                    return f;
                }
            }
        }
    }

    if (!primary) primary = font_primary(family, bold, italic);
    if (gid) *gid = 0;
    return primary;
}

void *font_hfont(Font *f, int pixelSize)
{
    HFontCache *hc;
    int i;
    HFONT hf;

    if (!f) return NULL;
    if (!f->hfonts) f->hfonts = calloc(1, sizeof(HFontCache));
    hc = (HFontCache *)f->hfonts;

    for (i = 0; i < hc->count; i++) {
        if (hc->entries[i].size == pixelSize) return hc->entries[i].hf;
    }

    hf = CreateFontW(-pixelSize, 0, 0, 0, f->bold ? FW_BOLD : FW_NORMAL,
                     f->italic ? TRUE : FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, f->face);
    if (hc->count < MAX_HFONTS) {
        hc->entries[hc->count].size = pixelSize;
        hc->entries[hc->count].hf = hf;
        hc->count++;
    }
    return hf;
}
