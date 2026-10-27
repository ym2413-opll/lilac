/*
 * doc.c - Markdown -> document model (md4c based, GitHub flavoured).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "doc.h"
#include "image.h"
#include "../third_party/md4c/md4c.h"

/* ------------------------------------------------------------------------- */
/* Block / run helpers                                                        */
/* ------------------------------------------------------------------------- */

static Block *block_new(Doc *doc, int type, Block *parent)
{
    Block *b = (Block *)arena_alloc(&doc->arena, sizeof(Block));
    b->type = type;
    b->parent = parent;
    if (parent) {
        if (parent->nkids == parent->capkids) {
            int nc = parent->capkids ? parent->capkids * 2 : 4;
            Block **nk = (Block **)arena_alloc(&doc->arena, nc * sizeof(Block *));
            if (parent->nkids) memcpy(nk, parent->kids, parent->nkids * sizeof(Block *));
            parent->kids = nk;
            parent->capkids = nc;
        }
        parent->kids[parent->nkids++] = b;
    }
    return b;
}

static Run *run_add(Doc *doc, Block *b)
{
    Run *r;
    if (b->nruns == b->capruns) {
        int nc = b->capruns ? b->capruns * 2 : 8;
        Run *nr = (Run *)arena_alloc(&doc->arena, nc * sizeof(Run));
        if (b->nruns) memcpy(nr, b->runs, b->nruns * sizeof(Run));
        b->runs = nr;
        b->capruns = nc;
    }
    r = &b->runs[b->nruns++];
    memset(r, 0, sizeof *r);
    r->link = -1;
    return r;
}

static int doc_add_link(Doc *doc, const wchar_t *href, int len)
{
    if (doc->nlinks == doc->caplinks) {
        int nc = doc->caplinks ? doc->caplinks * 2 : 16;
        wchar_t **nl = (wchar_t **)arena_alloc(&doc->arena, nc * sizeof(wchar_t *));
        if (doc->nlinks) memcpy(nl, doc->links, doc->nlinks * sizeof(wchar_t *));
        doc->links = nl;
        doc->caplinks = nc;
    }
    doc->links[doc->nlinks] = arena_wcsndup(&doc->arena, href ? href : L"", href ? len : 0);
    return doc->nlinks++;
}

/* ------------------------------------------------------------------------- */
/* Entities                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct { const char *name; unsigned cp; } Entity;
static const Entity g_entities[] = {
    {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",0xA0},
    {"copy",0xA9},{"reg",0xAE},{"trade",0x2122},{"mdash",0x2014},{"ndash",0x2013},
    {"hellip",0x2026},{"laquo",0xAB},{"raquo",0xBB},{"ldquo",0x201C},{"rdquo",0x201D},
    {"lsquo",0x2018},{"rsquo",0x2019},{"sbquo",0x201A},{"bdquo",0x201E},{"middot",0xB7},
    {"times",0xD7},{"divide",0xF7},{"deg",0xB0},{"plusmn",0xB1},{"para",0xB6},{"sect",0xA7},
    {"euro",0x20AC},{"pound",0xA3},{"yen",0xA5},{"cent",0xA2},{"larr",0x2190},{"rarr",0x2192},
    {"uarr",0x2191},{"darr",0x2193},{"harr",0x2194},{"lArr",0x21D0},{"rArr",0x21D2},
    {"hArr",0x21D4},{"bull",0x2022},{"dagger",0x2020},{"Dagger",0x2021},{"permil",0x2030},
    {"prime",0x2032},{"Prime",0x2033},{"infin",0x221E},{"ne",0x2260},{"le",0x2264},{"ge",0x2265},
    {"asymp",0x2248},{"equiv",0x2261},{"sum",0x2211},{"prod",0x220F},{"radic",0x221A},
    {"micro",0xB5},{"alpha",0x3B1},{"beta",0x3B2},{"gamma",0x3B3},{"delta",0x3B4},{"pi",0x3C0},
    {"sigma",0x3C3},{"omega",0x3C9},{"lambda",0x3BB},{"mu",0x3BC},{"theta",0x3B8},
    {"Delta",0x394},{"Omega",0x3A9},{"Sigma",0x3A3},{"iexcl",0xA1},{"iquest",0xBF},
    {"shy",0xAD},{"ensp",0x2002},{"emsp",0x2003},{"thinsp",0x2009},{"zwj",0x200D},{"zwnj",0x200C},
    {"frac12",0xBD},{"frac14",0xBC},{"frac34",0xBE},{"sup1",0xB9},{"sup2",0xB2},{"sup3",0xB3},
    {"check",0x2713},{"cross",0x2717},{"star",0x2606},{"starf",0x2605},{"hearts",0x2665},
    {"spades",0x2660},{"clubs",0x2663},{"diams",0x2666},{"loz",0x25CA},{"tab",'\t'},{"newline",'\n'},
    {"Auml",0xC4},{"Ouml",0xD6},{"Uuml",0xDC},{"auml",0xE4},{"ouml",0xF6},{"uuml",0xFC},{"szlig",0xDF},
    {"eacute",0xE9},{"egrave",0xE8},{"aacute",0xE1},{"agrave",0xE0},{"ccedil",0xE7},{"ntilde",0xF1},
    {"Eacute",0xC9},{"oacute",0xF3},{"uacute",0xFA},{"iacute",0xED},
};

/* Decode an entity "&...;" (UTF-16 input). Writes up to 2 UTF-16 units to out. Returns count or 0. */
static int decode_entity_w(const wchar_t *s, int len, wchar_t *out)
{
    unsigned cp = 0;
    if (len < 3 || s[0] != '&' || s[len - 1] != ';') return 0;
    if (s[1] == '#') {
        int i = 2, hex = 0;
        if (i < len - 1 && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        for (; i < len - 1; i++) {
            wchar_t c = s[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + d;
            if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
        }
        if (cp == 0) cp = 0xFFFD;
    } else {
        char name[48];
        int n = len - 2, k;
        if (n <= 0 || n >= (int)sizeof name) return 0;
        for (k = 0; k < n; k++) name[k] = (char)s[k + 1];
        name[n] = 0;
        for (k = 0; k < (int)(sizeof g_entities / sizeof g_entities[0]); k++)
            if (!strcmp(g_entities[k].name, name)) { cp = g_entities[k].cp; break; }
        if (!cp) return 0;
    }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out[0] = (wchar_t)(0xD800 + (cp >> 10));
        out[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        return 2;
    }
    out[0] = (wchar_t)cp;
    return 1;
}

/* Decode all entities in a string into wb. */
static void decode_entities_into(WBuf *wb, const wchar_t *s, int len)
{
    int i = 0;
    while (i < len) {
        if (s[i] == '&') {
            int j = i + 1;
            while (j < len && j - i < 40 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
            if (j < len && s[j] == ';') {
                wchar_t out[2];
                int n = decode_entity_w(s + i, j - i + 1, out);
                if (n) { wbuf_put(wb, out, n); i = j + 1; continue; }
            }
        }
        wbuf_putc(wb, s[i]);
        i++;
    }
}

/* Decode an md4c attribute (with entity substrings) into an arena string. */
static wchar_t *attr_to_w(Doc *doc, const MD_ATTRIBUTE *a)
{
    WBuf wb = {0};
    wchar_t *res;
    unsigned i;
    if (!a || !a->text || a->size == 0) return arena_wcsndup(&doc->arena, L"", 0);
    for (i = 0; a->substr_offsets[i] < a->size; i++) {
        MD_OFFSET off = a->substr_offsets[i], end = a->substr_offsets[i + 1];
        int wl;
        wchar_t *w = utf8_to_utf16(a->text + off, (int)(end - off), &wl);
        if (a->substr_types[i] == MD_TEXT_ENTITY) {
            wchar_t out[2];
            int n = decode_entity_w(w, wl, out);
            if (n) wbuf_put(&wb, out, n); else wbuf_put(&wb, w, wl);
        } else if (a->substr_types[i] == MD_TEXT_NULLCHAR) {
            wbuf_putc(&wb, 0xFFFD);
        } else {
            wbuf_put(&wb, w, wl);
        }
        free(w);
    }
    res = arena_wcsndup(&doc->arena, wb.p ? wb.p : L"", wb.n);
    wbuf_free(&wb);
    return res;
}

/* ------------------------------------------------------------------------- */
/* Minimal HTML tag parsing                                                   */
/* ------------------------------------------------------------------------- */

typedef struct Tag {
    wchar_t name[16];
    int     closing;
    int     selfclose;
    const wchar_t *attrs;   /* raw attribute text */
    int     attrlen;
} Tag;

/* Parse "<name attr...>" at s (s[0]=='<'). Returns length consumed or 0 if not a tag. */
static int parse_tag(const wchar_t *s, int len, Tag *t)
{
    int i = 1, n = 0;
    memset(t, 0, sizeof *t);
    if (len < 3 || s[0] != '<') return 0;
    if (s[1] == '!' || s[1] == '?') {
        /* comment / doctype / processing instruction */
        if (len >= 4 && s[2] == '-' && s[3] == '-') {
            for (i = 4; i + 2 < len; i++)
                if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') { t->name[0] = '!'; return i + 3; }
            t->name[0] = '!';
            return len;
        }
        for (i = 2; i < len && s[i] != '>'; i++) {}
        t->name[0] = '!';
        return i < len ? i + 1 : len;
    }
    if (s[i] == '/') { t->closing = 1; i++; }
    if (i >= len || !iswalpha(s[i])) return 0;
    while (i < len && (iswalnum(s[i]) || s[i] == '-' || s[i] == ':')) {
        if (n < 15) t->name[n++] = (wchar_t)towlower(s[i]);
        i++;
    }
    t->name[n] = 0;
    t->attrs = s + i;
    {
        wchar_t q = 0;
        int st = i;
        for (; i < len; i++) {
            if (q) { if (s[i] == q) q = 0; continue; }
            if (s[i] == '"' || s[i] == '\'') { q = s[i]; continue; }
            if (s[i] == '>') break;
        }
        if (i >= len) return 0;
        t->attrlen = i - st;
        if (t->attrlen > 0 && s[i - 1] == '/') { t->selfclose = 1; t->attrlen--; }
        return i + 1;
    }
}

/* Get attribute value (malloc()ed, entity-decoded) or NULL. */
static wchar_t *tag_attr(const Tag *t, const wchar_t *name)
{
    const wchar_t *s = t->attrs;
    int len = t->attrlen, i = 0, nl = (int)wcslen(name);
    while (i < len) {
        int ks, ke, vs, ve;
        while (i < len && iswspace(s[i])) i++;
        ks = i;
        while (i < len && !iswspace(s[i]) && s[i] != '=' && s[i] != '>') i++;
        ke = i;
        while (i < len && iswspace(s[i])) i++;
        vs = ve = i;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && iswspace(s[i])) i++;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                wchar_t q = s[i++];
                vs = i;
                while (i < len && s[i] != q) i++;
                ve = i;
                if (i < len) i++;
            } else {
                vs = i;
                while (i < len && !iswspace(s[i])) i++;
                ve = i;
            }
        }
        if (ke - ks == nl && !_wcsnicmp(s + ks, name, nl)) {
            WBuf wb = {0};
            decode_entities_into(&wb, s + vs, ve - vs);
            if (!wb.p) wbuf_put(&wb, L"", 0);
            return wb.p;
        }
        if (ke == ks) i++;
    }
    return NULL;
}

static float parse_px(const wchar_t *v)
{
    float f;
    if (!v) return 0;
    f = (float)_wtof(v);
    if (wcschr(v, '%')) return 0; /* percentages unsupported -> natural size */
    return f > 0 ? f : 0;
}

/* ------------------------------------------------------------------------- */
/* Parser state                                                               */
/* ------------------------------------------------------------------------- */

#define MAX_LINK_STACK 32

typedef struct PState {
    Doc     *doc;
    Block   *cur;       /* current container */
    Block   *inl;       /* current inline block (P/H/CELL/implicit P) */
    int      cnt[16];   /* nesting counters per style bit */
    int      linkStack[MAX_LINK_STACK];
    int      nlink;
    int      htmlLinkDepth;
    int      imgDepth;
    Run     *imgRun;
    Block   *imgBlock;
    int      imgRunIndex;
    WBuf     alt;
    int      inHead;
    int      inCode, inHtml;
    WBuf     raw;       /* code / html block text */
    Block   *rawBlock;
} PState;

static unsigned cur_style(PState *ps)
{
    unsigned s = 0;
    int i;
    for (i = 0; i < 16; i++) if (ps->cnt[i] > 0) s |= (1u << i);
    if (ps->nlink > 0) s |= ST_LINK;
    return s;
}

static int bit_index(unsigned bit)
{
    int i = 0;
    while (bit > 1) { bit >>= 1; i++; }
    return i;
}

static void style_push(PState *ps, unsigned bit, int delta)
{
    int i = bit_index(bit);
    ps->cnt[i] += delta;
    if (ps->cnt[i] < 0) ps->cnt[i] = 0;
}

static Block *ensure_inline(PState *ps)
{
    if (!ps->inl) {
        Block *p = block_new(ps->doc, BK_PARAGRAPH, ps->cur);
        p->tight = 1;
        ps->inl = p;
    }
    return ps->inl;
}

static void add_text(PState *ps, const wchar_t *w, int len, unsigned extraStyle)
{
    Block *b;
    Run *r;
    if (len <= 0) return;
    if (ps->imgDepth > 0) { wbuf_put(&ps->alt, w, len); return; }
    b = ensure_inline(ps);
    r = run_add(ps->doc, b);
    r->type = RN_TEXT;
    r->style = cur_style(ps) | extraStyle;
    r->link = ps->nlink > 0 ? ps->linkStack[ps->nlink - 1] : -1;
    r->text = arena_wcsndup(&ps->doc->arena, w, len);
    r->len = len;
}

static void add_break(PState *ps)
{
    Run *r;
    if (ps->imgDepth > 0) { wbuf_putc(&ps->alt, ' '); return; }
    r = run_add(ps->doc, ensure_inline(ps));
    r->type = RN_BREAK;
    r->style = cur_style(ps);
}

static Run *add_image(PState *ps, const wchar_t *src, const wchar_t *alt, const wchar_t *title, float w, float h)
{
    Run *r = run_add(ps->doc, ensure_inline(ps));
    r->type = RN_IMAGE;
    r->style = cur_style(ps);
    r->link = ps->nlink > 0 ? ps->linkStack[ps->nlink - 1] : -1;
    r->src = arena_wcsndup(&ps->doc->arena, src ? src : L"", -1);
    r->text = arena_wcsndup(&ps->doc->arena, alt ? alt : L"", -1);
    r->len = (int)wcslen(r->text);
    r->title = title ? arena_wcsndup(&ps->doc->arena, title, -1) : NULL;
    r->width = w;
    r->height = h;
    return r;
}

static void link_push(PState *ps, int idx)
{
    if (ps->nlink < MAX_LINK_STACK) ps->linkStack[ps->nlink] = idx;
    ps->nlink++;
}

static void link_pop(PState *ps)
{
    if (ps->nlink > 0) ps->nlink--;
}

/* Inline formatting tags shared by inline HTML and HTML blocks. Returns 1 if handled. */
static int handle_inline_tag(PState *ps, const Tag *t)
{
    static const struct { const wchar_t *name; unsigned bit; } map[] = {
        {L"b", ST_BOLD}, {L"strong", ST_BOLD}, {L"i", ST_ITALIC}, {L"em", ST_ITALIC},
        {L"cite", ST_ITALIC}, {L"var", ST_ITALIC}, {L"dfn", ST_ITALIC},
        {L"u", ST_UNDERLINE}, {L"ins", ST_UNDERLINE}, {L"s", ST_STRIKE}, {L"del", ST_STRIKE},
        {L"strike", ST_STRIKE}, {L"code", ST_CODE}, {L"tt", ST_CODE}, {L"samp", ST_CODE},
        {L"kbd", ST_KBD}, {L"sup", ST_SUP}, {L"sub", ST_SUB}, {L"mark", ST_MARK},
    };
    int i;
    if (!wcscmp(t->name, L"br")) { add_break(ps); return 1; }
    for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++) {
        if (!wcscmp(t->name, map[i].name)) {
            if (!t->selfclose) style_push(ps, map[i].bit, t->closing ? -1 : 1);
            return 1;
        }
    }
    if (!wcscmp(t->name, L"a")) {
        if (t->closing) {
            if (ps->htmlLinkDepth > 0) { ps->htmlLinkDepth--; link_pop(ps); }
        } else {
            wchar_t *href = tag_attr(t, L"href");
            if (href) {
                link_push(ps, doc_add_link(ps->doc, href, (int)wcslen(href)));
                ps->htmlLinkDepth++;
                free(href);
            }
        }
        return 1;
    }
    if (!wcscmp(t->name, L"img") && !t->closing) {
        wchar_t *src = tag_attr(t, L"src"), *alt = tag_attr(t, L"alt"), *title = tag_attr(t, L"title");
        wchar_t *w = tag_attr(t, L"width"), *h = tag_attr(t, L"height");
        if (src) add_image(ps, src, alt, title, parse_px(w), parse_px(h));
        free(src); free(alt); free(title); free(w); free(h);
        return 1;
    }
    return 0;
}

/* Raw HTML block -> blocks. A pragmatic subset: paragraphs, headings, alignment,
 * images, links, line breaks, inline formatting, horizontal rules. */
static void convert_html_block(PState *ps, const wchar_t *s, int len)
{
    Block *container = ps->cur;
    int align = AL_DEFAULT;
    int alignStack[32], nalign = 0;
    int i = 0, skip = 0;
    int lastSpace = 1;
    WBuf text = {0};

#define FLUSH_TEXT() do { if (text.n) { add_text(ps, text.p, text.n, 0); text.n = 0; } } while (0)
#define END_PARA() do { FLUSH_TEXT(); ps->inl = NULL; lastSpace = 1; } while (0)

    ps->inl = NULL;
    while (i < len) {
        if (s[i] == '<') {
            Tag t;
            int n = parse_tag(s + i, len - i, &t);
            if (n > 0) {
                i += n;
                if (t.name[0] == '!') continue;
                if (!wcscmp(t.name, L"script") || !wcscmp(t.name, L"style")) { skip = !t.closing; continue; }
                if (skip) continue;
                if (!wcscmp(t.name, L"p") || !wcscmp(t.name, L"div") || !wcscmp(t.name, L"center") ||
                    !wcscmp(t.name, L"details") || !wcscmp(t.name, L"section") || !wcscmp(t.name, L"article") ||
                    !wcscmp(t.name, L"header") || !wcscmp(t.name, L"footer") || !wcscmp(t.name, L"figure") ||
                    !wcscmp(t.name, L"blockquote") || !wcscmp(t.name, L"table") || !wcscmp(t.name, L"tr") ||
                    !wcscmp(t.name, L"ul") || !wcscmp(t.name, L"ol") || !wcscmp(t.name, L"pre") ||
                    !wcscmp(t.name, L"figcaption") || !wcscmp(t.name, L"dl") || !wcscmp(t.name, L"dd") ||
                    !wcscmp(t.name, L"dt") || !wcscmp(t.name, L"picture") == 2) {
                    END_PARA();
                    if (!t.selfclose) {
                        if (!t.closing) {
                            wchar_t *a = tag_attr(&t, L"align");
                            int na = align;
                            if (!wcscmp(t.name, L"center")) na = AL_CENTER;
                            if (a) {
                                if (!_wcsicmp(a, L"center") || !_wcsicmp(a, L"middle")) na = AL_CENTER;
                                else if (!_wcsicmp(a, L"right")) na = AL_RIGHT;
                                else if (!_wcsicmp(a, L"left")) na = AL_LEFT;
                                free(a);
                            }
                            if (nalign < 32) alignStack[nalign] = align;
                            nalign++;
                            align = na;
                        } else if (nalign > 0) {
                            nalign--;
                            if (nalign < 32) align = alignStack[nalign];
                        }
                    }
                    continue;
                }
                if (t.name[0] == 'h' && t.name[1] >= '1' && t.name[1] <= '6' && !t.name[2]) {
                    END_PARA();
                    if (!t.closing) {
                        wchar_t *a = tag_attr(&t, L"align");
                        Block *h = block_new(ps->doc, BK_HEADING, container);
                        h->level = t.name[1] - '0';
                        h->align = align;
                        if (a) {
                            if (!_wcsicmp(a, L"center")) h->align = AL_CENTER;
                            else if (!_wcsicmp(a, L"right")) h->align = AL_RIGHT;
                            free(a);
                        }
                        ps->inl = h;
                    }
                    continue;
                }
                if (!wcscmp(t.name, L"hr")) {
                    END_PARA();
                    block_new(ps->doc, BK_HR, container);
                    continue;
                }
                if (!wcscmp(t.name, L"summary")) {
                    if (!t.closing) { END_PARA(); style_push(ps, ST_BOLD, 1); }
                    else { FLUSH_TEXT(); style_push(ps, ST_BOLD, -1); END_PARA(); }
                    continue;
                }
                if (!wcscmp(t.name, L"li")) {
                    END_PARA();
                    if (!t.closing) { text.n = 0; wbuf_put(&text, L"\x2022 ", 2); }
                    continue;
                }
                if (!wcscmp(t.name, L"td") || !wcscmp(t.name, L"th")) {
                    if (t.closing) { wbuf_putc(&text, ' '); lastSpace = 1; }
                    continue;
                }
                FLUSH_TEXT();
                if (!ps->inl && (!wcscmp(t.name, L"img") || !wcscmp(t.name, L"br"))) {
                    Block *p = block_new(ps->doc, BK_PARAGRAPH, container);
                    p->align = align;
                    ps->inl = p;
                }
                if (handle_inline_tag(ps, &t)) {
                    if (!wcscmp(t.name, L"br")) lastSpace = 1;
                    else if (!wcscmp(t.name, L"img")) lastSpace = 0;
                }
                continue;
            }
        }
        if (skip) { i++; continue; }
        /* text */
        {
            wchar_t c = s[i];
            if (c == '&') {
                int j = i + 1;
                while (j < len && j - i < 40 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
                if (j < len && s[j] == ';') {
                    wchar_t out[2];
                    int n = decode_entity_w(s + i, j - i + 1, out);
                    if (n) {
                        if (!ps->inl) { Block *p = block_new(ps->doc, BK_PARAGRAPH, container); p->align = align; ps->inl = p; }
                        wbuf_put(&text, out, n);
                        lastSpace = 0;
                        i = j + 1;
                        continue;
                    }
                }
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!lastSpace && ps->inl) { wbuf_putc(&text, ' '); lastSpace = 1; }
            } else {
                if (!ps->inl) { Block *p = block_new(ps->doc, BK_PARAGRAPH, container); p->align = align; ps->inl = p; }
                wbuf_putc(&text, c);
                lastSpace = 0;
            }
            i++;
        }
    }
    END_PARA();
    wbuf_free(&text);
#undef FLUSH_TEXT
#undef END_PARA
}

/* ------------------------------------------------------------------------- */
/* Finalisation of inline blocks                                              */
/* ------------------------------------------------------------------------- */

/* Merge adjacent text runs with identical attributes, collapse whitespace and trim. */
static void finalize_runs(Doc *doc, Block *b)
{
    int i, j, n = 0;
    Run *out;
    WBuf wb = {0};
    int lastSpace = 1;

    if (!b->nruns) return;
    out = (Run *)arena_alloc(&doc->arena, b->nruns * sizeof(Run));
    for (i = 0; i < b->nruns; ) {
        Run r = b->runs[i];
        if (r.type != RN_TEXT) {
            out[n++] = r;
            if (r.type == RN_BREAK) {
                /* trim trailing space before a hard break */
                if (n >= 2 && out[n - 2].type == RN_TEXT && !(out[n - 2].style & ST_CODE)) {
                    while (out[n - 2].len > 0 && out[n - 2].text[out[n - 2].len - 1] == ' ') out[n - 2].len--;
                }
                lastSpace = 1;
            } else {
                lastSpace = 0;
            }
            i++;
            continue;
        }
        wb.n = 0;
        for (j = i; j < b->nruns && b->runs[j].type == RN_TEXT &&
                    b->runs[j].style == r.style && b->runs[j].link == r.link; j++) {
            const Run *s = &b->runs[j];
            int k;
            for (k = 0; k < s->len; k++) {
                wchar_t c = s->text[k];
                if (!(r.style & ST_CODE)) {
                    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
                    if (c == ' ') {
                        if (lastSpace) continue;
                        lastSpace = 1;
                    } else lastSpace = 0;
                } else {
                    if (c == '\n' || c == '\r') c = ' ';
                    lastSpace = 0;
                }
                wbuf_putc(&wb, c);
            }
        }
        if (wb.n > 0) {
            r.text = arena_wcsndup(&doc->arena, wb.p, wb.n);
            r.len = wb.n;
            out[n++] = r;
        }
        i = j;
    }
    wbuf_free(&wb);
    /* trim trailing whitespace of the block */
    while (n > 0) {
        Run *l = &out[n - 1];
        if (l->type == RN_TEXT && !(l->style & ST_CODE)) {
            while (l->len > 0 && l->text[l->len - 1] == ' ') l->len--;
            if (l->len == 0) { n--; continue; }
        } else if (l->type == RN_BREAK) {
            n--;
            continue;
        }
        break;
    }
    b->runs = out;
    b->nruns = n;
    b->capruns = n;
}

wchar_t *doc_runs_text(const Run *runs, int nruns)
{
    WBuf wb = {0};
    int i;
    for (i = 0; i < nruns; i++) {
        if (runs[i].type == RN_TEXT) wbuf_put(&wb, runs[i].text, runs[i].len);
        else if (runs[i].type == RN_BREAK) wbuf_putc(&wb, ' ');
        else if (runs[i].type == RN_IMAGE) wbuf_put(&wb, runs[i].text, runs[i].len);
    }
    if (!wb.p) wbuf_put(&wb, L"", 0);
    return wb.p;
}

static int is_slug_punct(wchar_t c)
{
    if (c < 0x80) return !(iswalnum(c) || c == '-' || c == '_' || c == ' ');
    if (c >= 0x2000 && c <= 0x206F) return 1;   /* general punctuation */
    if (c >= 0x3000 && c <= 0x303F) return 1;   /* CJK punctuation */
    if (c >= 0xFF01 && c <= 0xFF0F) return 1;   /* fullwidth punctuation */
    if (c == 0xA0) return 0;
    if (c >= 0xD800 && c <= 0xDFFF) return 1;   /* emoji etc. are dropped by GitHub */
    return 0;
}

static void make_slug(Doc *doc, Block *h)
{
    wchar_t *t = doc_runs_text(h->runs, h->nruns);
    WBuf wb = {0};
    int i, n, suffix = 0;
    for (i = 0; t[i]; i++) {
        wchar_t c = t[i];
        if (c == ' ' || c == 0xA0) { wbuf_putc(&wb, '-'); continue; }
        if (is_slug_punct(c)) continue;
        if (c < 0x80) c = (wchar_t)towlower(c);
        else { wchar_t tmp[2] = { c, 0 }; CharLowerW(tmp); c = tmp[0]; }
        wbuf_putc(&wb, c);
    }
    if (!wb.p) wbuf_put(&wb, L"", 0);
    n = wb.n;
    /* uniqueness: compare against previous headings (walk whole tree would be costly; keep a simple list) */
    for (;;) {
        int clash = 0;
        Block *stack[64];
        int sp = 0;
        wchar_t cand[512];
        if (suffix) swprintf(cand, 512, L"%.*s-%d", n > 480 ? 480 : n, wb.p, suffix);
        else swprintf(cand, 512, L"%.*s", n > 500 ? 500 : n, wb.p);
        stack[sp++] = doc->root;
        while (sp && !clash) {
            Block *b = stack[--sp];
            int k;
            if (b != h && b->type == BK_HEADING && b->anchor && !wcscmp(b->anchor, cand)) clash = 1;
            for (k = 0; k < b->nkids && sp < 64; k++) stack[sp++] = b->kids[k];
        }
        if (!clash) { h->anchor = arena_wcsndup(&doc->arena, cand, -1); break; }
        suffix++;
    }
    wbuf_free(&wb);
    if (!doc->title && h->level == 1 && t[0]) doc->title = arena_wcsndup(&doc->arena, t, -1);
    free(t);
}

/* ------------------------------------------------------------------------- */
/* md4c callbacks                                                             */
/* ------------------------------------------------------------------------- */

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
    PState *ps = (PState *)ud;
    Doc *doc = ps->doc;
    Block *b;

    if (type != MD_BLOCK_DOC) ps->inl = NULL;
    switch (type) {
    case MD_BLOCK_DOC:
        ps->cur = doc->root;
        break;
    case MD_BLOCK_QUOTE:
        ps->cur = block_new(doc, BK_QUOTE, ps->cur);
        break;
    case MD_BLOCK_ADMONITION: {
        MD_BLOCK_ADMONITION_DETAIL *d = (MD_BLOCK_ADMONITION_DETAIL *)detail;
        wchar_t *k = attr_to_w(doc, &d->type);
        b = block_new(doc, BK_ALERT, ps->cur);
        if (!_wcsicmp(k, L"tip")) b->alert = AL_TIP;
        else if (!_wcsicmp(k, L"important")) b->alert = AL_IMPORTANT;
        else if (!_wcsicmp(k, L"warning")) b->alert = AL_WARNING;
        else if (!_wcsicmp(k, L"caution")) b->alert = AL_CAUTION;
        else b->alert = AL_NOTE;
        ps->cur = b;
        break;
    }
    case MD_BLOCK_UL: {
        MD_BLOCK_UL_DETAIL *d = (MD_BLOCK_UL_DETAIL *)detail;
        b = block_new(doc, BK_UL, ps->cur);
        b->tight = d->is_tight;
        ps->cur = b;
        break;
    }
    case MD_BLOCK_OL: {
        MD_BLOCK_OL_DETAIL *d = (MD_BLOCK_OL_DETAIL *)detail;
        b = block_new(doc, BK_OL, ps->cur);
        b->tight = d->is_tight;
        b->start = (int)d->start;
        ps->cur = b;
        break;
    }
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *d = (MD_BLOCK_LI_DETAIL *)detail;
        b = block_new(doc, BK_LI, ps->cur);
        if (d->is_task) b->task = (d->task_mark == 'x' || d->task_mark == 'X') ? 2 : 1;
        ps->cur = b;
        break;
    }
    case MD_BLOCK_HR:
        block_new(doc, BK_HR, ps->cur);
        break;
    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
        b = block_new(doc, BK_HEADING, ps->cur);
        b->level = (int)d->level;
        ps->inl = b;
        break;
    }
    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL *)detail;
        b = block_new(doc, BK_CODE, ps->cur);
        if (d->lang.size) b->lang = attr_to_w(doc, &d->lang);
        ps->rawBlock = b;
        ps->inCode = 1;
        ps->raw.n = 0;
        break;
    }
    case MD_BLOCK_HTML:
        ps->inHtml = 1;
        ps->raw.n = 0;
        break;
    case MD_BLOCK_P:
        ps->inl = block_new(doc, BK_PARAGRAPH, ps->cur);
        break;
    case MD_BLOCK_TABLE: {
        MD_BLOCK_TABLE_DETAIL *d = (MD_BLOCK_TABLE_DETAIL *)detail;
        b = block_new(doc, BK_TABLE, ps->cur);
        b->ncols = (int)d->col_count;
        ps->cur = b;
        break;
    }
    case MD_BLOCK_THEAD: ps->inHead = 1; break;
    case MD_BLOCK_TBODY: ps->inHead = 0; break;
    case MD_BLOCK_TR:
        b = block_new(doc, BK_TR, ps->cur);
        b->header = ps->inHead;
        ps->cur = b;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        MD_BLOCK_TD_DETAIL *d = (MD_BLOCK_TD_DETAIL *)detail;
        b = block_new(doc, BK_CELL, ps->cur);
        b->header = (type == MD_BLOCK_TH);
        b->align = (int)d->align;
        ps->inl = b;
        break;
    }
    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        ps->cur = block_new(doc, BK_FOOTNOTES, ps->cur);
        break;
    case MD_BLOCK_FOOTNOTE_DEF: {
        MD_BLOCK_FOOTNOTE_DEF_DETAIL *d = (MD_BLOCK_FOOTNOTE_DEF_DETAIL *)detail;
        wchar_t tmp[32];
        b = block_new(doc, BK_FOOTNOTE, ps->cur);
        b->footnote = (int)d->id;
        swprintf(tmp, 32, L"fn-%u", d->id);
        b->anchor = arena_wcsndup(&doc->arena, tmp, -1);
        ps->cur = b;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
    PState *ps = (PState *)ud;
    Doc *doc = ps->doc;
    (void)detail;

    switch (type) {
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_ADMONITION:
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
    case MD_BLOCK_LI:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_TR:
    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
    case MD_BLOCK_FOOTNOTE_DEF:
        if (ps->inl && ps->inl->parent == ps->cur) { finalize_runs(doc, ps->inl); ps->inl = NULL; }
        if (ps->cur->parent) ps->cur = ps->cur->parent;
        break;
    case MD_BLOCK_H:
    case MD_BLOCK_P:
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (ps->inl) {
            finalize_runs(doc, ps->inl);
            if (ps->inl->type == BK_HEADING) make_slug(doc, ps->inl);
        }
        ps->inl = NULL;
        break;
    case MD_BLOCK_CODE: {
        Block *b = ps->rawBlock;
        int n = ps->raw.n;
        while (n > 0 && (ps->raw.p[n - 1] == '\n' || ps->raw.p[n - 1] == '\r')) n--;
        b->text = arena_wcsndup(&doc->arena, n ? ps->raw.p : L"", n);
        b->textlen = n;
        ps->inCode = 0;
        ps->rawBlock = NULL;
        break;
    }
    case MD_BLOCK_HTML: {
        int savedCnt[16];
        memcpy(savedCnt, ps->cnt, sizeof savedCnt);
        ps->inHtml = 0;
        convert_html_block(ps, ps->raw.p ? ps->raw.p : L"", ps->raw.n);
        /* finalize generated inline blocks */
        {
            int k;
            for (k = 0; k < ps->cur->nkids; k++) {
                Block *c = ps->cur->kids[k];
                if ((c->type == BK_PARAGRAPH || c->type == BK_HEADING) && c->capruns != c->nruns) {
                    finalize_runs(doc, c);
                    if (c->type == BK_HEADING && !c->anchor) make_slug(doc, c);
                }
            }
        }
        /* drop empty paragraphs produced by the conversion */
        {
            int k, m = 0;
            for (k = 0; k < ps->cur->nkids; k++) {
                Block *c = ps->cur->kids[k];
                if (c->type == BK_PARAGRAPH && c->nruns == 0) continue;
                ps->cur->kids[m++] = c;
            }
            ps->cur->nkids = m;
        }
        memcpy(ps->cnt, savedCnt, sizeof savedCnt);
        while (ps->htmlLinkDepth > 0) { ps->htmlLinkDepth--; link_pop(ps); }
        ps->inl = NULL;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *ud)
{
    PState *ps = (PState *)ud;
    Doc *doc = ps->doc;
    switch (type) {
    case MD_SPAN_EM: style_push(ps, ST_ITALIC, 1); break;
    case MD_SPAN_STRONG: style_push(ps, ST_BOLD, 1); break;
    case MD_SPAN_CODE: style_push(ps, ST_CODE, 1); break;
    case MD_SPAN_DEL: style_push(ps, ST_STRIKE, 1); break;
    case MD_SPAN_U: case MD_SPAN_INS: style_push(ps, ST_UNDERLINE, 1); break;
    case MD_SPAN_SUPERSCRIPT: style_push(ps, ST_SUP, 1); break;
    case MD_SPAN_SUBSCRIPT: style_push(ps, ST_SUB, 1); break;
    case MD_SPAN_MARK: style_push(ps, ST_MARK, 1); break;
    case MD_SPAN_A: {
        MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
        wchar_t *href = attr_to_w(doc, &d->href);
        link_push(ps, doc_add_link(doc, href, (int)wcslen(href)));
        break;
    }
    case MD_SPAN_IMG: {
        MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
        if (ps->imgDepth == 0) {
            wchar_t *src = attr_to_w(doc, &d->src);
            wchar_t *title = d->title.size ? attr_to_w(doc, &d->title) : NULL;
            Block *b = ensure_inline(ps);
            add_image(ps, src, L"", title, 0, 0);
            ps->imgBlock = b;
            ps->imgRunIndex = b->nruns - 1;
            ps->alt.n = 0;
        }
        ps->imgDepth++;
        break;
    }
    case MD_SPAN_FOOTNOTE_REF: {
        MD_SPAN_FOOTNOTE_REF_DETAIL *d = (MD_SPAN_FOOTNOTE_REF_DETAIL *)detail;
        wchar_t lbl[24], href[32];
        Run *r;
        if (ps->imgDepth > 0) break;
        swprintf(lbl, 24, L"%u", d->id);
        swprintf(href, 32, L"#fn-%u", d->id);
        r = run_add(doc, ensure_inline(ps));
        r->type = RN_FOOTREF;
        r->style = cur_style(ps) | ST_SUP | ST_LINK;
        r->link = doc_add_link(doc, href, (int)wcslen(href));
        r->text = arena_wcsndup(&doc->arena, lbl, -1);
        r->len = (int)wcslen(lbl);
        r->footnote = (int)d->id;
        break;
    }
    case MD_SPAN_WIKILINK: {
        MD_SPAN_WIKILINK_DETAIL *d = (MD_SPAN_WIKILINK_DETAIL *)detail;
        wchar_t *t = attr_to_w(doc, &d->target);
        link_push(ps, doc_add_link(doc, t, (int)wcslen(t)));
        break;
    }
    default: break;
    }
    return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *ud)
{
    PState *ps = (PState *)ud;
    (void)detail;
    switch (type) {
    case MD_SPAN_EM: style_push(ps, ST_ITALIC, -1); break;
    case MD_SPAN_STRONG: style_push(ps, ST_BOLD, -1); break;
    case MD_SPAN_CODE: style_push(ps, ST_CODE, -1); break;
    case MD_SPAN_DEL: style_push(ps, ST_STRIKE, -1); break;
    case MD_SPAN_U: case MD_SPAN_INS: style_push(ps, ST_UNDERLINE, -1); break;
    case MD_SPAN_SUPERSCRIPT: style_push(ps, ST_SUP, -1); break;
    case MD_SPAN_SUBSCRIPT: style_push(ps, ST_SUB, -1); break;
    case MD_SPAN_MARK: style_push(ps, ST_MARK, -1); break;
    case MD_SPAN_A: case MD_SPAN_WIKILINK: link_pop(ps); break;
    case MD_SPAN_IMG:
        ps->imgDepth--;
        if (ps->imgDepth == 0 && ps->imgBlock) {
            Run *r = &ps->imgBlock->runs[ps->imgRunIndex];
            r->text = arena_wcsndup(&ps->doc->arena, ps->alt.p ? ps->alt.p : L"", ps->alt.n);
            r->len = ps->alt.n;
            ps->imgBlock = NULL;
        }
        break;
    default: break;
    }
    return 0;
}

static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud)
{
    PState *ps = (PState *)ud;
    int wl;
    wchar_t *w;

    if (ps->inCode || ps->inHtml) {
        w = utf8_to_utf16(text, (int)size, &wl);
        if (type == MD_TEXT_NULLCHAR) wbuf_putc(&ps->raw, 0xFFFD);
        else wbuf_put(&ps->raw, w, wl);
        free(w);
        return 0;
    }
    switch (type) {
    case MD_TEXT_BR:
        add_break(ps);
        return 0;
    case MD_TEXT_SOFTBR:
        add_text(ps, L" ", 1, 0);
        return 0;
    case MD_TEXT_NULLCHAR:
        add_text(ps, L"\xFFFD", 1, 0);
        return 0;
    default:
        break;
    }
    w = utf8_to_utf16(text, (int)size, &wl);
    if (type == MD_TEXT_ENTITY) {
        wchar_t out[2];
        int n = decode_entity_w(w, wl, out);
        if (n) add_text(ps, out, n, 0);
        else add_text(ps, w, wl, 0);
    } else if (type == MD_TEXT_HTML) {
        Tag t;
        if (parse_tag(w, wl, &t) > 0 && t.name[0] != '!') {
            if (ps->imgDepth == 0) handle_inline_tag(ps, &t);
        }
    } else {
        add_text(ps, w, wl, 0);
    }
    free(w);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

Doc *doc_parse(const char *utf8, size_t len, const wchar_t *path)
{
    Doc *doc = (Doc *)calloc(1, sizeof(Doc));
    PState ps;
    MD_PARSER parser;
    const char *body = utf8;
    size_t bodylen = len;
    Block *front = NULL;

    if (!doc) return NULL;
    doc->root = block_new(doc, BK_DOCUMENT, NULL);
    if (path) {
        wchar_t *d = path_dir(path);
        doc->path = arena_wcsndup(&doc->arena, path, -1);
        doc->dir = arena_wcsndup(&doc->arena, d, -1);
        free(d);
    }
    doc->images = imagecache_new();

    /* YAML front matter: shown as a code block */
    if (len >= 4 && !strncmp(utf8, "---", 3) && (utf8[3] == '\n' || utf8[3] == '\r')) {
        size_t i = 3;
        while (i < len && utf8[i] != '\n') i++;
        i++;
        while (i < len) {
            size_t ls = i, le;
            while (i < len && utf8[i] != '\n') i++;
            le = i;
            if (le > ls && utf8[le - 1] == '\r') le--;
            if ((le - ls == 3 && (!strncmp(utf8 + ls, "---", 3) || !strncmp(utf8 + ls, "...", 3)))) {
                size_t cs = 4;
                int wl;
                wchar_t *w;
                while (cs < ls && (utf8[cs] == '\n' || utf8[cs] == '\r')) cs++;
                w = utf8_to_utf16(utf8 + cs, (int)(ls - cs), &wl);
                while (wl > 0 && (w[wl - 1] == '\n' || w[wl - 1] == '\r')) wl--;
                front = block_new(doc, BK_CODE, doc->root);
                front->lang = arena_wcsndup(&doc->arena, L"yaml", -1);
                front->text = arena_wcsndup(&doc->arena, w, wl);
                front->textlen = wl;
                free(w);
                body = utf8 + (i < len ? i + 1 : len);
                bodylen = len - (size_t)(body - utf8);
                break;
            }
            i++;
        }
    }

    memset(&ps, 0, sizeof ps);
    ps.doc = doc;
    ps.cur = doc->root;
    memset(&parser, 0, sizeof parser);
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_UNDERLINE * 0;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span = cb_enter_span;
    parser.leave_span = cb_leave_span;
    parser.text = cb_text;
    md_parse(body, (MD_SIZE)bodylen, &parser, &ps);
    wbuf_free(&ps.raw);
    wbuf_free(&ps.alt);

    if (!doc->title) {
        if (path) {
            const wchar_t *n = path_name(path);
            const wchar_t *dot = wcsrchr(n, '.');
            doc->title = arena_wcsndup(&doc->arena, n, dot ? (int)(dot - n) : -1);
        } else {
            doc->title = arena_wcsndup(&doc->arena, L"Untitled", -1);
        }
    }
    return doc;
}

Doc *doc_load_file(const wchar_t *path)
{
    uint8_t *data;
    size_t len;
    char *utf8 = NULL;
    int ulen = 0;
    Doc *doc;
    wchar_t full[MAX_PATH * 4];

    if (GetFullPathNameW(path, MAX_PATH * 4, full, NULL)) path = full;
    if (file_read_all(path, &data, &len)) return NULL;

    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        utf8 = (char *)malloc(len - 3 + 1);
        memcpy(utf8, data + 3, len - 3);
        ulen = (int)(len - 3);
        utf8[ulen] = 0;
    } else if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        size_t n = (len - 2) / 2, i;
        wchar_t *w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        int be = data[0] == 0xFE;
        for (i = 0; i < n; i++) {
            uint8_t a = data[2 + i * 2], b = data[3 + i * 2];
            w[i] = be ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
        }
        utf8 = utf16_to_utf8(w, (int)n, &ulen);
        free(w);
    } else if (len == 0 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, (int)len, NULL, 0) > 0) {
        utf8 = (char *)malloc(len + 1);
        memcpy(utf8, data, len);
        utf8[len] = 0;
        ulen = (int)len;
    } else {
        /* legacy code page (e.g. CP949 for Korean Windows) */
        int wl = MultiByteToWideChar(CP_ACP, 0, (const char *)data, (int)len, NULL, 0);
        wchar_t *w = (wchar_t *)malloc(((size_t)wl + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, (const char *)data, (int)len, w, wl);
        utf8 = utf16_to_utf8(w, wl, &ulen);
        free(w);
    }
    free(data);
    doc = doc_parse(utf8, (size_t)ulen, path);
    free(utf8);
    return doc;
}

static int is_remote(const wchar_t *s)
{
    return !_wcsnicmp(s, L"http://", 7) || !_wcsnicmp(s, L"https://", 8);
}

/* Resolve an image src to an image cache key. Returns malloc()ed string or NULL. */
static wchar_t *resolve_image_key(Doc *doc, const wchar_t *src)
{
    WBuf wb = {0};
    int i, n;
    wchar_t *res;
    if (!src || !src[0]) return NULL;
    if (is_remote(src) || !_wcsnicmp(src, L"data:", 5)) return _wcsdup(src);
    if (!_wcsnicmp(src, L"file:///", 8)) src += 8;
    else if (!_wcsnicmp(src, L"file://", 7)) src += 7;
    /* strip query / fragment, percent-decode (as UTF-8) */
    n = (int)wcslen(src);
    for (i = 0; i < n; i++) if (src[i] == '?' || src[i] == '#') { n = i; break; }
    {
        Buf b = {0};
        char *u = utf16_to_utf8(src, n, NULL);
        int k, ul = (int)strlen(u);
        for (k = 0; k < ul; k++) {
            if (u[k] == '%' && k + 2 < ul && isxdigit((unsigned char)u[k + 1]) && isxdigit((unsigned char)u[k + 2])) {
                char hx[3] = { u[k + 1], u[k + 2], 0 };
                buf_putc(&b, (char)strtol(hx, NULL, 16));
                k += 2;
            } else buf_putc(&b, u[k]);
        }
        free(u);
        {
            wchar_t *w = utf8_to_utf16(b.p ? b.p : "", (int)b.n, NULL);
            wbuf_put(&wb, w, -1);
            free(w);
        }
        buf_free(&b);
    }
    res = path_join(doc->dir, wb.p ? wb.p : L"");
    wbuf_free(&wb);
    return res;
}

static void resolve_block_images(Doc *doc, Block *b, int allowRemote)
{
    int i;
    for (i = 0; i < b->nruns; i++) {
        Run *r = &b->runs[i];
        if (r->type == RN_IMAGE) {
            wchar_t *key = resolve_image_key(doc, r->src);
            if (key) {
                r->img = imagecache_get(doc->images, key, allowRemote);
                free(key);
            }
        }
    }
    for (i = 0; i < b->nkids; i++) resolve_block_images(doc, b->kids[i], allowRemote);
}

void doc_resolve_images(Doc *doc, int allowRemote)
{
    if (!doc) return;
    resolve_block_images(doc, doc->root, allowRemote);
}

void doc_free(Doc *doc)
{
    if (!doc) return;
    imagecache_free(doc->images);
    arena_free(&doc->arena);
    free(doc);
}

const wchar_t *doc_alert_title(int kind)
{
    switch (kind) {
    case AL_TIP: return L"Tip";
    case AL_IMPORTANT: return L"Important";
    case AL_WARNING: return L"Warning";
    case AL_CAUTION: return L"Caution";
    default: return L"Note";
    }
}
