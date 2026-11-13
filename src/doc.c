/*
 * doc.c - Markdown -> document model (md4c based, GitHub flavoured + LaTeX math).
 * All internal strings are UTF-8 (char *); UTF-16 is used only at Win32 API boundaries.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
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
            Block **nk = (Block **)arena_alloc(&doc->arena, (size_t)nc * sizeof(Block *));
            if (parent->nkids) memcpy(nk, parent->kids, (size_t)parent->nkids * sizeof(Block *));
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
        Run *nr = (Run *)arena_alloc(&doc->arena, (size_t)nc * sizeof(Run));
        if (b->nruns) memcpy(nr, b->runs, (size_t)b->nruns * sizeof(Run));
        b->runs = nr;
        b->capruns = nc;
    }
    r = &b->runs[b->nruns++];
    memset(r, 0, sizeof *r);
    r->link = -1;
    return r;
}

static int doc_add_link(Doc *doc, const char *href, int len)
{
    if (doc->nlinks == doc->caplinks) {
        int nc = doc->caplinks ? doc->caplinks * 2 : 16;
        char **nl = (char **)arena_alloc(&doc->arena, (size_t)nc * sizeof(char *));
        if (doc->nlinks) memcpy(nl, doc->links, (size_t)doc->nlinks * sizeof(char *));
        doc->links = nl;
        doc->caplinks = nc;
    }
    doc->links[doc->nlinks] = arena_strndup(&doc->arena, href ? href : "", href ? len : 0);
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

/* Decode an entity "&...;" (UTF-8 input). Writes 1..4 UTF-8 bytes to out. Returns byte count or 0. */
static int decode_entity_u8(const char *s, int len, char out[4])
{
    uint32_t cp = 0;
    if (len < 3 || s[0] != '&' || s[len - 1] != ';') return 0;
    if (s[1] == '#') {
        int i = 2, hex = 0;
        if (i < len - 1 && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        for (; i < len - 1; i++) {
            char c = s[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            cp = cp * (uint32_t)(hex ? 16 : 10) + (uint32_t)d;
            if (cp > 0x10FFFF) { cp = 0xFFFD; break; }
        }
        if (cp == 0) cp = 0xFFFD;
    } else {
        char name[48];
        int n = len - 2, k;
        if (n <= 0 || n >= (int)sizeof name) return 0;
        memcpy(name, s + 1, (size_t)n);
        name[n] = 0;
        for (k = 0; k < (int)(sizeof g_entities / sizeof g_entities[0]); k++)
            if (!strcmp(g_entities[k].name, name)) { cp = g_entities[k].cp; break; }
        if (!cp) return 0;
    }
    return utf8_encode(cp, out);
}

/* Decode all entities in a UTF-8 string into b. */
static void decode_entities_into(Buf *b, const char *s, int len)
{
    int i = 0;
    while (i < len) {
        if (s[i] == '&') {
            int j = i + 1;
            while (j < len && j - i < 40 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
            if (j < len && s[j] == ';') {
                char out[4];
                int n = decode_entity_u8(s + i, j - i + 1, out);
                if (n) { buf_put(b, out, (size_t)n); i = j + 1; continue; }
            }
        }
        buf_putc(b, s[i]);
        i++;
    }
}

/* Decode an md4c attribute (with entity substrings) into a UTF-8 arena string. */
static char *attr_to_u8(Doc *doc, const MD_ATTRIBUTE *a)
{
    Buf b = {0};
    char *res;
    unsigned i;
    if (!a || !a->text || a->size == 0) return arena_strndup(&doc->arena, "", 0);
    for (i = 0; a->substr_offsets[i] < a->size; i++) {
        MD_OFFSET off = a->substr_offsets[i], end = a->substr_offsets[i + 1];
        const char *sub = a->text + off;
        int slen = (int)(end - off);
        if (a->substr_types[i] == MD_TEXT_ENTITY) {
            char out[4];
            int n = decode_entity_u8(sub, slen, out);
            if (n) buf_put(&b, out, (size_t)n);
            else buf_put(&b, sub, (size_t)slen);
        } else if (a->substr_types[i] == MD_TEXT_NULLCHAR) {
            buf_put_cp(&b, 0xFFFD);
        } else {
            buf_put(&b, sub, (size_t)slen);
        }
    }
    res = arena_strndup(&doc->arena, b.p ? b.p : "", (int)b.n);
    buf_free(&b);
    return res;
}

/* ------------------------------------------------------------------------- */
/* Minimal HTML tag parsing (UTF-8)                                           */
/* ------------------------------------------------------------------------- */

typedef struct Tag {
    char        name[16];
    int         closing;
    int         selfclose;
    const char *attrs;      /* raw attribute text */
    int         attrlen;
} Tag;

static int is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int is_ascii_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_ascii_alnum(char c)
{
    return is_ascii_alpha(c) || (c >= '0' && c <= '9');
}

/* Parse "<name attr...>" at s (s[0]=='<'). Returns length consumed or 0 if not a tag. */
static int parse_tag(const char *s, int len, Tag *t)
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
    if (i >= len || !is_ascii_alpha(s[i])) return 0;
    while (i < len && (is_ascii_alnum(s[i]) || s[i] == '-' || s[i] == ':')) {
        if (n < 15) t->name[n++] = (char)tolower((unsigned char)s[i]);
        i++;
    }
    t->name[n] = 0;
    t->attrs = s + i;
    {
        char q = 0;
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

/* Get attribute value (malloc()ed UTF-8, entity-decoded) or NULL. */
static char *tag_attr(const Tag *t, const char *name)
{
    const char *s = t->attrs;
    int len = t->attrlen, i = 0, nl = (int)strlen(name);
    while (i < len) {
        int ks, ke, vs, ve;
        while (i < len && is_ascii_space(s[i])) i++;
        ks = i;
        while (i < len && !is_ascii_space(s[i]) && s[i] != '=' && s[i] != '>') i++;
        ke = i;
        while (i < len && is_ascii_space(s[i])) i++;
        vs = ve = i;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && is_ascii_space(s[i])) i++;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                vs = i;
                while (i < len && s[i] != q) i++;
                ve = i;
                if (i < len) i++;
            } else {
                vs = i;
                while (i < len && !is_ascii_space(s[i])) i++;
                ve = i;
            }
        }
        if (ke - ks == nl && !ascii_nicmp(s + ks, name, (size_t)nl)) {
            Buf b = {0};
            decode_entities_into(&b, s + vs, ve - vs);
            if (!b.p) buf_put(&b, "", 0);
            return b.p;
        }
        if (ke == ks) i++;
    }
    return NULL;
}

static float parse_px(const char *v)
{
    float f;
    if (!v) return 0;
    f = (float)atof(v);
    if (strchr(v, '%')) return 0; /* percentages unsupported -> natural size */
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
    Buf      alt;
    int      inHead;
    int      inCode, inHtml;
    int      inMath;    /* 1 = inline math, 2 = display math */
    Buf      mathBuf;
    Buf      raw;       /* code / html block text (UTF-8) */
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

static void add_text(PState *ps, const char *s, int len, unsigned extraStyle)
{
    Block *b;
    Run *r;
    if (len <= 0) return;
    if (ps->imgDepth > 0) { buf_put(&ps->alt, s, (size_t)len); return; }
    b = ensure_inline(ps);
    r = run_add(ps->doc, b);
    r->type = RN_TEXT;
    r->style = cur_style(ps) | extraStyle;
    r->link = ps->nlink > 0 ? ps->linkStack[ps->nlink - 1] : -1;
    r->text = arena_strndup(&ps->doc->arena, s, len);
    r->len = len;
}

static void add_break(PState *ps)
{
    Run *r;
    if (ps->imgDepth > 0) { buf_putc(&ps->alt, ' '); return; }
    r = run_add(ps->doc, ensure_inline(ps));
    r->type = RN_BREAK;
    r->style = cur_style(ps);
}

static Run *add_image(PState *ps, const char *src, const char *alt, const char *title, float w, float h)
{
    Run *r = run_add(ps->doc, ensure_inline(ps));
    r->type = RN_IMAGE;
    r->style = cur_style(ps);
    r->link = ps->nlink > 0 ? ps->linkStack[ps->nlink - 1] : -1;
    r->src = arena_strndup(&ps->doc->arena, src ? src : "", -1);
    r->text = arena_strndup(&ps->doc->arena, alt ? alt : "", -1);
    r->len = (int)strlen(r->text);
    r->title = title ? arena_strndup(&ps->doc->arena, title, -1) : NULL;
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
    static const struct { const char *name; unsigned bit; } map[] = {
        {"b", ST_BOLD}, {"strong", ST_BOLD}, {"i", ST_ITALIC}, {"em", ST_ITALIC},
        {"cite", ST_ITALIC}, {"var", ST_ITALIC}, {"dfn", ST_ITALIC},
        {"u", ST_UNDERLINE}, {"ins", ST_UNDERLINE}, {"s", ST_STRIKE}, {"del", ST_STRIKE},
        {"strike", ST_STRIKE}, {"code", ST_CODE}, {"tt", ST_CODE}, {"samp", ST_CODE},
        {"kbd", ST_KBD}, {"sup", ST_SUP}, {"sub", ST_SUB}, {"mark", ST_MARK},
    };
    int i;
    if (!strcmp(t->name, "br")) { add_break(ps); return 1; }
    for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++) {
        if (!strcmp(t->name, map[i].name)) {
            if (!t->selfclose) style_push(ps, map[i].bit, t->closing ? -1 : 1);
            return 1;
        }
    }
    if (!strcmp(t->name, "a")) {
        if (t->closing) {
            if (ps->htmlLinkDepth > 0) { ps->htmlLinkDepth--; link_pop(ps); }
        } else {
            char *href = tag_attr(t, "href");
            if (href) {
                link_push(ps, doc_add_link(ps->doc, href, (int)strlen(href)));
                ps->htmlLinkDepth++;
                free(href);
            }
        }
        return 1;
    }
    if (!strcmp(t->name, "img") && !t->closing) {
        char *src = tag_attr(t, "src"), *alt = tag_attr(t, "alt"), *title = tag_attr(t, "title");
        char *w = tag_attr(t, "width"), *h = tag_attr(t, "height");
        if (src) add_image(ps, src, alt, title, parse_px(w), parse_px(h));
        free(src); free(alt); free(title); free(w); free(h);
        return 1;
    }
    return 0;
}

/* Raw HTML block -> blocks. A pragmatic subset: paragraphs, headings, alignment,
 * images, links, line breaks, inline formatting, horizontal rules. */
static void convert_html_block(PState *ps, const char *s, int len)
{
    Block *container = ps->cur;
    int align = AL_DEFAULT;
    int alignStack[32], nalign = 0;
    int i = 0, skip = 0;
    int lastSpace = 1;
    Buf text = {0};

#define FLUSH_TEXT() do { if (text.n) { add_text(ps, text.p, (int)text.n, 0); text.n = 0; } } while (0)
#define END_PARA() do { FLUSH_TEXT(); ps->inl = NULL; lastSpace = 1; } while (0)

    ps->inl = NULL;
    while (i < len) {
        if (s[i] == '<') {
            Tag t;
            int n = parse_tag(s + i, len - i, &t);
            if (n > 0) {
                i += n;
                if (t.name[0] == '!') continue;
                if (!strcmp(t.name, "script") || !strcmp(t.name, "style")) { skip = !t.closing; continue; }
                if (skip) continue;
                if (!strcmp(t.name, "p") || !strcmp(t.name, "div") || !strcmp(t.name, "center") ||
                    !strcmp(t.name, "details") || !strcmp(t.name, "section") || !strcmp(t.name, "article") ||
                    !strcmp(t.name, "header") || !strcmp(t.name, "footer") || !strcmp(t.name, "figure") ||
                    !strcmp(t.name, "blockquote") || !strcmp(t.name, "table") || !strcmp(t.name, "tr") ||
                    !strcmp(t.name, "ul") || !strcmp(t.name, "ol") || !strcmp(t.name, "pre") ||
                    !strcmp(t.name, "figcaption") || !strcmp(t.name, "dl") || !strcmp(t.name, "dd") ||
                    !strcmp(t.name, "dt") || !strcmp(t.name, "picture")) {
                    END_PARA();
                    if (!t.selfclose) {
                        if (!t.closing) {
                            char *a = tag_attr(&t, "align");
                            int na = align;
                            if (!strcmp(t.name, "center")) na = AL_CENTER;
                            if (a) {
                                if (!ascii_icmp(a, "center") || !ascii_icmp(a, "middle")) na = AL_CENTER;
                                else if (!ascii_icmp(a, "right")) na = AL_RIGHT;
                                else if (!ascii_icmp(a, "left")) na = AL_LEFT;
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
                        char *a = tag_attr(&t, "align");
                        Block *h = block_new(ps->doc, BK_HEADING, container);
                        h->level = t.name[1] - '0';
                        h->align = align;
                        if (a) {
                            if (!ascii_icmp(a, "center")) h->align = AL_CENTER;
                            else if (!ascii_icmp(a, "right")) h->align = AL_RIGHT;
                            free(a);
                        }
                        ps->inl = h;
                    }
                    continue;
                }
                if (!strcmp(t.name, "hr")) {
                    END_PARA();
                    block_new(ps->doc, BK_HR, container);
                    continue;
                }
                if (!strcmp(t.name, "summary")) {
                    if (!t.closing) { END_PARA(); style_push(ps, ST_BOLD, 1); }
                    else { FLUSH_TEXT(); style_push(ps, ST_BOLD, -1); END_PARA(); }
                    continue;
                }
                if (!strcmp(t.name, "li")) {
                    END_PARA();
                    if (!t.closing) { text.n = 0; buf_puts(&text, "\xE2\x80\xA2 "); }
                    continue;
                }
                if (!strcmp(t.name, "td") || !strcmp(t.name, "th")) {
                    if (t.closing) { buf_putc(&text, ' '); lastSpace = 1; }
                    continue;
                }
                FLUSH_TEXT();
                if (!ps->inl && (!strcmp(t.name, "img") || !strcmp(t.name, "br"))) {
                    Block *p = block_new(ps->doc, BK_PARAGRAPH, container);
                    p->align = align;
                    ps->inl = p;
                }
                if (handle_inline_tag(ps, &t)) {
                    if (!strcmp(t.name, "br")) lastSpace = 1;
                    else if (!strcmp(t.name, "img")) lastSpace = 0;
                }
                continue;
            }
        }
        if (skip) { i++; continue; }
        /* text */
        {
            char c = s[i];
            if (c == '&') {
                int j = i + 1;
                while (j < len && j - i < 40 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
                if (j < len && s[j] == ';') {
                    char out[4];
                    int n = decode_entity_u8(s + i, j - i + 1, out);
                    if (n) {
                        if (!ps->inl) { Block *p = block_new(ps->doc, BK_PARAGRAPH, container); p->align = align; ps->inl = p; }
                        buf_put(&text, out, (size_t)n);
                        lastSpace = 0;
                        i = j + 1;
                        continue;
                    }
                }
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!lastSpace && ps->inl) { buf_putc(&text, ' '); lastSpace = 1; }
            } else {
                if (!ps->inl) { Block *p = block_new(ps->doc, BK_PARAGRAPH, container); p->align = align; ps->inl = p; }
                buf_putc(&text, c);
                lastSpace = 0;
            }
            i++;
        }
    }
    END_PARA();
    buf_free(&text);
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
    Buf tb = {0};
    int lastSpace = 1;

    if (!b->nruns) return;
    out = (Run *)arena_alloc(&doc->arena, (size_t)b->nruns * sizeof(Run));
    for (i = 0; i < b->nruns; ) {
        Run r = b->runs[i];
        if (r.type != RN_TEXT) {
            out[n++] = r;
            if (r.type == RN_BREAK) {
                /* trim trailing space before a hard break */
                if (n >= 2 && out[n - 2].type == RN_TEXT && !(out[n - 2].style & ST_CODE)) {
                    while (out[n - 2].len > 0 && out[n - 2].text[out[n - 2].len - 1] == ' ')
                        out[n - 2].text[--out[n - 2].len] = 0;
                }
                lastSpace = 1;
            } else {
                lastSpace = 0;
            }
            i++;
            continue;
        }
        tb.n = 0;
        for (j = i; j < b->nruns && b->runs[j].type == RN_TEXT &&
                    b->runs[j].style == r.style && b->runs[j].link == r.link; j++) {
            const Run *s = &b->runs[j];
            int k;
            for (k = 0; k < s->len; k++) {
                char c = s->text[k];
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
                buf_putc(&tb, c);
            }
        }
        if (tb.n > 0) {
            r.text = arena_strndup(&doc->arena, tb.p, (int)tb.n);
            r.len = (int)tb.n;
            out[n++] = r;
        }
        i = j;
    }
    buf_free(&tb);
    /* trim trailing whitespace of the block */
    while (n > 0) {
        Run *l = &out[n - 1];
        if (l->type == RN_TEXT && !(l->style & ST_CODE)) {
            while (l->len > 0 && l->text[l->len - 1] == ' ') l->text[--l->len] = 0;
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

    /* Promote a paragraph consisting solely of a display math span ($$...$$ or \[...\])
     * into a standalone BK_MATH block (unless inside a tight list item). */
    if (b->type == BK_PARAGRAPH && !b->tight && n == 1 &&
        out[0].type == RN_MATH && (out[0].style & ST_MATH_DISPLAY)) {
        b->type = BK_MATH;
        b->text = out[0].text;
        b->textlen = out[0].len;
        b->nruns = 0;
        b->capruns = 0;
        b->runs = NULL;
    }
}

char *doc_runs_text(const Run *runs, int nruns)
{
    Buf b = {0};
    int i;
    for (i = 0; i < nruns; i++) {
        if (runs[i].type == RN_TEXT || runs[i].type == RN_MATH || runs[i].type == RN_IMAGE)
            buf_put(&b, runs[i].text, (size_t)runs[i].len);
        else if (runs[i].type == RN_BREAK)
            buf_putc(&b, ' ');
    }
    if (!b.p) buf_put(&b, "", 0);
    return b.p;
}

static int is_slug_punct_cp(uint32_t c)
{
    if (c < 0x80) return !(is_ascii_alnum((char)c) || c == '-' || c == '_' || c == ' ');
    if (c >= 0x2000 && c <= 0x206F) return 1;   /* general punctuation */
    if (c >= 0x3000 && c <= 0x303F) return 1;   /* CJK punctuation */
    if (c >= 0xFF01 && c <= 0xFF0F) return 1;   /* fullwidth punctuation */
    if (c == 0xA0) return 0;
    if (c >= 0x10000) return 1;                 /* emoji etc. are dropped by GitHub */
    return 0;
}

static void make_slug(Doc *doc, Block *h)
{
    char *t = doc_runs_text(h->runs, h->nruns);
    int tlen = (int)strlen(t);
    Buf b = {0};
    int i = 0, n, suffix = 0;
    while (i < tlen) {
        uint32_t cp = utf8_next(t, tlen, &i);
        if (cp == ' ' || cp == 0xA0) { buf_putc(&b, '-'); continue; }
        if (is_slug_punct_cp(cp)) continue;
        if (cp < 0x80) cp = (uint32_t)tolower((int)cp);
        else if (cp < 0x10000) {
            wchar_t tmp[2] = { (wchar_t)cp, 0 };
            CharLowerW(tmp);
            cp = (uint32_t)tmp[0];
        }
        buf_put_cp(&b, cp);
    }
    if (!b.p) buf_put(&b, "", 0);
    n = (int)b.n;
    /* uniqueness: compare against previous headings */
    for (;;) {
        int clash = 0;
        Block *stack[64];
        int sp = 0;
        char cand[512];
        if (suffix) _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%.*s-%d", n > 480 ? 480 : n, b.p, suffix);
        else _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%.*s", n > 500 ? 500 : n, b.p);
        stack[sp++] = doc->root;
        while (sp && !clash) {
            Block *cur = stack[--sp];
            int k;
            if (cur != h && cur->type == BK_HEADING && cur->anchor && !strcmp(cur->anchor, cand)) clash = 1;
            for (k = 0; k < cur->nkids && sp < 64; k++) stack[sp++] = cur->kids[k];
        }
        if (!clash) { h->anchor = arena_strndup(&doc->arena, cand, -1); break; }
        suffix++;
    }
    buf_free(&b);
    if (!doc->title && h->level == 1 && t[0]) doc->title = arena_strndup(&doc->arena, t, -1);
    free(t);
}

/* ------------------------------------------------------------------------- */
/* md4c callbacks                                                             */
/* ------------------------------------------------------------------------- */

static int is_math_lang(const char *lang)
{
    char buf[32];
    int n = 0;
    if (!lang) return 0;
    while (*lang == ' ' || *lang == '\t') lang++;
    while (*lang && *lang != ' ' && *lang != '\t' && *lang != '{' && *lang != ':' && n < 31) {
        buf[n++] = (char)tolower((unsigned char)*lang++);
    }
    buf[n] = 0;
    return (!strcmp(buf, "math") || !strcmp(buf, "latex") || !strcmp(buf, "tex") || !strcmp(buf, "katex"));
}

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
        char *k = attr_to_u8(doc, &d->type);
        b = block_new(doc, BK_ALERT, ps->cur);
        if (!ascii_icmp(k, "tip")) b->alert = AL_TIP;
        else if (!ascii_icmp(k, "important")) b->alert = AL_IMPORTANT;
        else if (!ascii_icmp(k, "warning")) b->alert = AL_WARNING;
        else if (!ascii_icmp(k, "caution")) b->alert = AL_CAUTION;
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
        if (d->lang.size) b->lang = attr_to_u8(doc, &d->lang);
        if (is_math_lang(b->lang)) b->type = BK_MATH;
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
        char tmp[32];
        b = block_new(doc, BK_FOOTNOTE, ps->cur);
        b->footnote = (int)d->id;
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "fn-%u", d->id);
        b->anchor = arena_strndup(&doc->arena, tmp, -1);
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
        int n = (int)ps->raw.n;
        while (n > 0 && (ps->raw.p[n - 1] == '\n' || ps->raw.p[n - 1] == '\r')) n--;
        b->text = arena_strndup(&doc->arena, n ? ps->raw.p : "", n);
        b->textlen = n;
        ps->inCode = 0;
        ps->rawBlock = NULL;
        break;
    }
    case MD_BLOCK_HTML: {
        int savedCnt[16];
        memcpy(savedCnt, ps->cnt, sizeof savedCnt);
        ps->inHtml = 0;
        convert_html_block(ps, ps->raw.p ? ps->raw.p : "", (int)ps->raw.n);
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
    case MD_SPAN_LATEXMATH:
        ps->inMath = 1;
        ps->mathBuf.n = 0;
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        ps->inMath = 2;
        ps->mathBuf.n = 0;
        break;
    case MD_SPAN_A: {
        MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
        char *href = attr_to_u8(doc, &d->href);
        link_push(ps, doc_add_link(doc, href, (int)strlen(href)));
        break;
    }
    case MD_SPAN_IMG: {
        MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
        if (ps->imgDepth == 0) {
            char *src = attr_to_u8(doc, &d->src);
            char *title = d->title.size ? attr_to_u8(doc, &d->title) : NULL;
            Block *b = ensure_inline(ps);
            add_image(ps, src, "", title, 0, 0);
            ps->imgBlock = b;
            ps->imgRunIndex = b->nruns - 1;
            ps->alt.n = 0;
        }
        ps->imgDepth++;
        break;
    }
    case MD_SPAN_FOOTNOTE_REF: {
        MD_SPAN_FOOTNOTE_REF_DETAIL *d = (MD_SPAN_FOOTNOTE_REF_DETAIL *)detail;
        char lbl[24], href[32];
        Run *r;
        if (ps->imgDepth > 0) break;
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%u", d->id);
        _snprintf_s(href, sizeof(href), _TRUNCATE, "#fn-%u", d->id);
        r = run_add(doc, ensure_inline(ps));
        r->type = RN_FOOTREF;
        r->style = cur_style(ps) | ST_SUP | ST_LINK;
        r->link = doc_add_link(doc, href, (int)strlen(href));
        r->text = arena_strndup(&doc->arena, lbl, -1);
        r->len = (int)strlen(lbl);
        r->footnote = (int)d->id;
        break;
    }
    case MD_SPAN_WIKILINK: {
        MD_SPAN_WIKILINK_DETAIL *d = (MD_SPAN_WIKILINK_DETAIL *)detail;
        char *t = attr_to_u8(doc, &d->target);
        link_push(ps, doc_add_link(doc, t, (int)strlen(t)));
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
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
        if (ps->inMath) {
            const char *s = ps->mathBuf.p ? ps->mathBuf.p : "";
            int len = (int)ps->mathBuf.n;
            while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; len--; }
            while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
            if (ps->imgDepth > 0) {
                buf_put(&ps->alt, s, (size_t)len);
            } else if (len > 0) {
                Run *r = run_add(ps->doc, ensure_inline(ps));
                r->type = RN_MATH;
                r->style = cur_style(ps) | (ps->inMath == 2 ? ST_MATH_DISPLAY : 0);
                r->link = ps->nlink > 0 ? ps->linkStack[ps->nlink - 1] : -1;
                r->text = arena_strndup(&ps->doc->arena, s, len);
                r->len = len;
            }
            ps->inMath = 0;
        }
        break;
    case MD_SPAN_A: case MD_SPAN_WIKILINK: link_pop(ps); break;
    case MD_SPAN_IMG:
        ps->imgDepth--;
        if (ps->imgDepth == 0 && ps->imgBlock) {
            Run *r = &ps->imgBlock->runs[ps->imgRunIndex];
            r->text = arena_strndup(&ps->doc->arena, ps->alt.p ? ps->alt.p : "", (int)ps->alt.n);
            r->len = (int)ps->alt.n;
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

    if (ps->inCode || ps->inHtml) {
        if (type == MD_TEXT_NULLCHAR) buf_put_cp(&ps->raw, 0xFFFD);
        else buf_put(&ps->raw, text, (size_t)size);
        return 0;
    }
    if (ps->inMath) {
        if (type == MD_TEXT_NULLCHAR) buf_put_cp(&ps->mathBuf, 0xFFFD);
        else if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) buf_putc(&ps->mathBuf, '\n');
        else buf_put(&ps->mathBuf, text, (size_t)size);
        return 0;
    }
    switch (type) {
    case MD_TEXT_BR:
        add_break(ps);
        return 0;
    case MD_TEXT_SOFTBR:
        add_text(ps, " ", 1, 0);
        return 0;
    case MD_TEXT_NULLCHAR:
        add_text(ps, "\xEF\xBF\xBD", 3, 0);
        return 0;
    case MD_TEXT_ENTITY: {
        char out[4];
        int n = decode_entity_u8(text, (int)size, out);
        if (n) add_text(ps, out, n, 0);
        else add_text(ps, text, (int)size, 0);
        return 0;
    }
    case MD_TEXT_HTML: {
        Tag t;
        if (parse_tag(text, (int)size, &t) > 0 && t.name[0] != '!') {
            if (ps->imgDepth == 0) handle_inline_tag(ps, &t);
        }
        return 0;
    }
    default:
        add_text(ps, text, (int)size, 0);
        return 0;
    }
}

/* Count unescaped separator pipes '|' outside inline backticks and closed $...$/$$...$$ spans */
static int scan_table_pipes(const char *s, size_t len)
{
    int pipes = 0;
    size_t k = 0;

    while (k < len) {
        if (s[k] == '\\' && k + 1 < len) {
            k += 2;
            continue;
        }
        if (s[k] == '`') {
            size_t bt = 0, q;
            while (k + bt < len && s[k + bt] == '`') bt++;
            q = k + bt;
            while (q < len) {
                if (s[q] == '`') {
                    size_t c = 0;
                    while (q + c < len && s[q + c] == '`') c++;
                    if (c == bt) {
                        k = q + c;
                        goto next_char;
                    }
                    q += c;
                } else {
                    q++;
                }
            }
            k += bt;
            continue;
        }
        if (s[k] == '$') {
            int is_dd = (k + 1 < len && s[k + 1] == '$');
            size_t dlen = is_dd ? 2 : 1;
            size_t q = k + dlen;
            while (q < len) {
                if (s[q] == '\\' && q + 1 < len) {
                    q += 2;
                    continue;
                }
                if (s[q] == '$') {
                    if (is_dd) {
                        if (q + 1 < len && s[q + 1] == '$') {
                            k = q + 2;
                            goto next_char;
                        }
                    } else {
                        k = q + 1;
                        goto next_char;
                    }
                }
                q++;
            }
            k += dlen;
            continue;
        }
        if (s[k] == '|') {
            pipes++;
        }
        k++;
    next_char:;
    }
    return pipes;
}

/* Check whether trimmed line [0, len) is a GFM table delimiter row (e.g., | :--- | --- |) */
static int is_table_delim_line(const char *s, size_t len, int *out_pipes, int *out_cols, int *out_has_lead)
{
    size_t a = 0, b = len, pos;
    int pipes = 0, cols = 0, has_dash = 0;

    if (len < 3) return 0;
    for (pos = 0; pos < len; pos++) {
        if (s[pos] == '|') pipes++;
        else if (s[pos] == '-') has_dash = 1;
    }
    if (pipes == 0 || !has_dash) return 0;

    if (out_has_lead) *out_has_lead = (s[0] == '|');
    if (s[a] == '|') a++;
    if (b > a && s[b - 1] == '|') b--;

    pos = a;
    while (pos <= b) {
        size_t seg_end = pos;
        size_t x, y;
        while (seg_end < b && s[seg_end] != '|') seg_end++;
        x = pos;
        y = seg_end;
        while (x < y && (s[x] == ' ' || s[x] == '\t')) x++;
        while (y > x && (s[y - 1] == ' ' || s[y - 1] == '\t')) y--;
        if (x >= y) return 0;
        if (s[x] == ':') x++;
        if (y > x && s[y - 1] == ':') y--;
        if (x >= y) return 0;
        while (x < y) {
            if (s[x] != '-') return 0;
            x++;
        }
        cols++;
        if (seg_end >= b) break;
        pos = seg_end + 1;
    }
    if (cols < 1) return 0;
    if (out_pipes) *out_pipes = pipes;
    if (out_cols) *out_cols = cols;
    return 1;
}

static int is_hr_line(const char *s, size_t len)
{
    char marker = 0;
    int cnt = 0;
    size_t k;
    for (k = 0; k < len; k++) {
        if (s[k] == ' ' || s[k] == '\t') continue;
        if (s[k] == '-' || s[k] == '*' || s[k] == '_') {
            if (!marker) marker = s[k];
            else if (s[k] != marker) return 0;
            cnt++;
        } else {
            return 0;
        }
    }
    return cnt >= 3;
}

static void flush_table_row(Buf *b, Buf *row_buf, int *in_table_row,
                            int *row_pipes, int *row_lines, int *row_saw_blank)
{
    if (row_buf->n > 0) {
        buf_put(b, row_buf->p, row_buf->n);
        buf_putc(b, '\n');
        row_buf->n = 0;
    }
    *in_table_row = 0;
    *row_pipes = 0;
    *row_lines = 0;
    *row_saw_blank = 0;
}

/* Preprocess multi-line display math blocks ($$...$$ and \[...\]) and multi-line pipe tables
 * outside code fences so internal blank lines or wrapped cell lines keep structure intact. */
static char *preprocess_display_math(const char *src, size_t len, size_t *out_len)
{
    Buf b = {0};
    Buf row_buf = {0};
    size_t i = 0;
    char fence_ch = 0;
    int fence_len = 0;
    int in_math_block = 0; /* 1 = $$, 2 = \[ */
    int prev_line_valid = 0;
    int in_table = 0;
    int table_pipes = 0;
    int table_cols = 0;
    int table_has_lead = 0;
    int in_table_row = 0;
    int row_pipes = 0;
    int row_lines = 0;
    int row_saw_blank = 0;

    buf_reserve(&b, len + 16);
    while (i < len) {
        size_t ls = i, le, p, te;
        int is_fence = 0;
        while (i < len && src[i] != '\n') i++;
        le = i;
        if (i < len && src[i] == '\n') i++;

        p = ls;
        while (p < le && (src[p] == ' ' || src[p] == '\t')) p++;
        te = le;
        while (te > p && (src[te - 1] == ' ' || src[te - 1] == '\t' || src[te - 1] == '\r')) te--;

        /* Check code fences (``` or ~~~) when not inside math block */
        if (!in_math_block && (p < te && (src[p] == '`' || src[p] == '~'))) {
            char ch = src[p];
            int cnt = 0;
            size_t q = p;
            while (q < te && src[q] == ch) { cnt++; q++; }
            if (cnt >= 3) {
                if (!fence_ch) {
                    fence_ch = ch;
                    fence_len = cnt;
                    is_fence = 1;
                } else if (ch == fence_ch && cnt >= fence_len) {
                    fence_ch = 0;
                    fence_len = 0;
                    is_fence = 1;
                }
            }
        }

        if (fence_ch || is_fence) {
            if (in_table_row)
                flush_table_row(&b, &row_buf, &in_table_row, &row_pipes, &row_lines, &row_saw_blank);
            in_table = 0;
            prev_line_valid = 0;
            buf_put(&b, src + ls, i - ls);
            continue;
        }

        if (in_table) {
            int stop_block = (te > p && (src[p] == '#' || src[p] == '>' || is_hr_line(src + p, te - p)));
            if (in_table_row) {
                if (stop_block || row_lines >= 32) {
                    flush_table_row(&b, &row_buf, &in_table_row, &row_pipes, &row_lines, &row_saw_blank);
                    in_table = 0;
                    /* fall through to process current line normally */
                } else if (te == p) {
                    row_saw_blank = 1;
                    continue;
                } else {
                    int rp = scan_table_pipes(src + p, te - p);
                    int lead = (src[p] == '|');
                    if (lead && row_pipes >= table_cols &&
                        (rp >= 2 || (!row_saw_blank && rp >= 1 && te > p + 1))) {
                        flush_table_row(&b, &row_buf, &in_table_row, &row_pipes, &row_lines, &row_saw_blank);
                        /* re-evaluate current line under !in_table_row below */
                    } else {
                        if (row_buf.n > 0 && row_buf.p[row_buf.n - 1] != ' ')
                            buf_putc(&row_buf, ' ');
                        buf_put(&row_buf, src + p, te - p);
                        row_pipes += rp;
                        row_lines++;
                        row_saw_blank = 0;
                        if (row_pipes >= table_pipes) {
                            flush_table_row(&b, &row_buf, &in_table_row, &row_pipes, &row_lines, &row_saw_blank);
                        }
                        continue;
                    }
                }
            }

            if (in_table && !in_table_row) {
                if (te == p || stop_block) {
                    in_table = 0;
                    prev_line_valid = 0;
                } else {
                    int rp = scan_table_pipes(src + p, te - p);
                    if (rp == 0) {
                        in_table = 0;
                        prev_line_valid = 0;
                    } else if (rp < table_pipes && (table_has_lead || rp < table_cols)) {
                        row_buf.n = 0;
                        buf_put(&row_buf, src + p, te - p);
                        in_table_row = 1;
                        row_pipes = rp;
                        row_lines = 1;
                        row_saw_blank = 0;
                        continue;
                    } else {
                        buf_put(&b, src + ls, i - ls);
                        prev_line_valid = 0;
                        continue;
                    }
                }
            }
        }

        if (in_math_block) {
            if (te == p) {
                /* Blank line inside multi-line display math: skip so MD4C keeps the paragraph intact */
                continue;
            }
            /* Check if this line closes the math block */
            if (in_math_block == 1 && te - p >= 2 &&
                (!strncmp(src + p, "$$", 2) || !strncmp(src + te - 2, "$$", 2))) {
                in_math_block = 0;
            } else if (in_math_block == 2 && te - p >= 2 &&
                       (!strncmp(src + p, "\\]", 2) || !strncmp(src + te - 2, "\\]", 2))) {
                in_math_block = 0;
            }
            prev_line_valid = 0;
        } else {
            /* Check if line opens a multi-line $$ or \[ block */
            if (te - p >= 2 && !strncmp(src + p, "$$", 2)) {
                if (te - p == 2 || strncmp(src + te - 2, "$$", 2) != 0) {
                    in_math_block = 1;
                }
                prev_line_valid = 0;
            } else if (te - p >= 2 && !strncmp(src + p, "\\[", 2)) {
                if (te - p == 2 || strncmp(src + te - 2, "\\]", 2) != 0) {
                    in_math_block = 2;
                }
                prev_line_valid = 0;
            } else if (te == p || src[p] == '#' || src[p] == '>' || is_hr_line(src + p, te - p)) {
                prev_line_valid = 0;
            } else {
                int dp = 0, dc = 0, dlead = 0;
                if (prev_line_valid && is_table_delim_line(src + p, te - p, &dp, &dc, &dlead)) {
                    in_table = 1;
                    table_pipes = dp;
                    table_cols = dc;
                    table_has_lead = dlead;
                    prev_line_valid = 0;
                } else {
                    prev_line_valid = (scan_table_pipes(src + p, te - p) > 0);
                }
            }
        }

        buf_put(&b, src + ls, i - ls);
    }

    if (in_table_row)
        flush_table_row(&b, &row_buf, &in_table_row, &row_pipes, &row_lines, &row_saw_blank);
    free(row_buf.p);

    *out_len = b.n;
    return b.p ? b.p : _strdup("");
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

Doc *doc_parse(const char *utf8, size_t len, const char *path)
{
    Doc *doc = (Doc *)calloc(1, sizeof(Doc));
    PState ps;
    MD_PARSER parser;
    const char *body = utf8;
    size_t bodylen = len;
    char *pre_body = NULL;
    size_t pre_len = 0;
    Block *front = NULL;

    if (!doc) return NULL;
    doc->root = block_new(doc, BK_DOCUMENT, NULL);
    if (path && path[0]) {
        char *d = path_dir(path);
        doc->path = arena_strndup(&doc->arena, path, -1);
        doc->dir = arena_strndup(&doc->arena, d, -1);
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
                size_t cs = 4, ce = ls;
                while (cs < ce && (utf8[cs] == '\n' || utf8[cs] == '\r')) cs++;
                while (ce > cs && (utf8[ce - 1] == '\n' || utf8[ce - 1] == '\r')) ce--;
                front = block_new(doc, BK_CODE, doc->root);
                front->lang = arena_strndup(&doc->arena, "yaml", -1);
                front->text = arena_strndup(&doc->arena, utf8 + cs, (int)(ce - cs));
                front->textlen = (int)(ce - cs);
                body = utf8 + (i < len ? i + 1 : len);
                bodylen = len - (size_t)(body - utf8);
                break;
            }
            i++;
        }
    }

    pre_body = preprocess_display_math(body, bodylen, &pre_len);

    memset(&ps, 0, sizeof ps);
    ps.doc = doc;
    ps.cur = doc->root;
    memset(&parser, 0, sizeof parser);
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span = cb_enter_span;
    parser.leave_span = cb_leave_span;
    parser.text = cb_text;
    md_parse(pre_body, (MD_SIZE)pre_len, &parser, &ps);
    free(pre_body);
    buf_free(&ps.raw);
    buf_free(&ps.alt);
    buf_free(&ps.mathBuf);

    if (!doc->title) {
        if (path && path[0]) {
            const char *n = path_name(path);
            const char *dot = strrchr(n, '.');
            doc->title = arena_strndup(&doc->arena, n, dot ? (int)(dot - n) : -1);
        } else {
            doc->title = arena_strndup(&doc->arena, "Untitled", -1);
        }
    }
    return doc;
}

/* Decode raw bytes as UTF-8, replacing any malformed byte sequence with U+FFFD. */
static char *decode_utf8_sanitized(const uint8_t *data, size_t len, int *out_len)
{
    Buf b = {0};
    int pos = 0;
    int ilen = (int)len;

    buf_reserve(&b, len + 1);
    while (pos < ilen) {
        int ascii_start = pos;
        while (pos < ilen && data[pos] > 0 && data[pos] < 0x80) pos++;
        if (pos > ascii_start) {
            buf_put(&b, (const char *)(data + ascii_start), (size_t)(pos - ascii_start));
        }
        if (pos < ilen) {
            if (data[pos] == 0) {
                buf_put_cp(&b, 0xFFFD);
                pos++;
            } else {
                uint32_t cp = utf8_next((const char *)data, ilen, &pos);
                buf_put_cp(&b, cp);
            }
        }
    }
    if (!b.p) buf_put(&b, "", 0);
    if (out_len) *out_len = (int)b.n;
    return b.p;
}

static char *decode_multibyte_cp(UINT cp, DWORD flags, const uint8_t *data, size_t len, int *out_len)
{
    int wl;
    wchar_t *w;
    char *utf8 = NULL;

    if (len == 0) {
        if (out_len) *out_len = 0;
        return _strdup("");
    }
    wl = MultiByteToWideChar(cp, flags, (const char *)data, (int)len, NULL, 0);
    if (wl <= 0) return NULL;
    w = (wchar_t *)malloc(((size_t)wl + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(cp, flags, (const char *)data, (int)len, w, wl) > 0) {
        w[wl] = 0;
        utf8 = utf16_to_utf8(w, wl, out_len);
    }
    free(w);
    return utf8;
}

static void scan_encoding_stats(const uint8_t *data, size_t len,
                                size_t *out_u8_2, size_t *out_u8_3, size_t *out_u8_4, size_t *out_bad_u8,
                                size_t *out_cp949_pairs, size_t *out_cp949_wansung, size_t *out_bad_cp949,
                                size_t *out_even_nul, size_t *out_odd_nul)
{
    size_t i = 0;
    size_t u8_2 = 0, u8_3 = 0, u8_4 = 0, bad_u8 = 0;
    size_t cp949_pairs = 0, cp949_wansung = 0, bad_cp949 = 0;
    size_t even_nul = 0, odd_nul = 0;

    while (i < len) {
        uint8_t c = data[i];
        if (c < 0x80) {
            if (c == 0) {
                if (i & 1) odd_nul++;
                else even_nul++;
            }
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (c >= 0xC2 && i + 1 < len && (data[i + 1] & 0xC0) == 0x80) {
                u8_2++;
                i += 2;
            } else {
                bad_u8++;
                i++;
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < len && (data[i + 1] & 0xC0) == 0x80 && (data[i + 2] & 0xC0) == 0x80) {
                uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                              ((uint32_t)(data[i + 1] & 0x3F) << 6) |
                              (uint32_t)(data[i + 2] & 0x3F);
                if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                    u8_3++;
                    i += 3;
                    continue;
                }
            }
            bad_u8++;
            i++;
        } else if ((c & 0xF8) == 0xF0) {
            if (c <= 0xF4 && i + 3 < len && (data[i + 1] & 0xC0) == 0x80 &&
                (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
                uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                              ((uint32_t)(data[i + 1] & 0x3F) << 12) |
                              ((uint32_t)(data[i + 2] & 0x3F) << 6) |
                              (uint32_t)(data[i + 3] & 0x3F);
                if (cp >= 0x10000 && cp <= 0x10FFFF) {
                    u8_4++;
                    i += 4;
                    continue;
                }
            }
            bad_u8++;
            i++;
        } else {
            bad_u8++;
            i++;
        }
    }

    i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0x81 && c <= 0xFE && i + 1 < len) {
            uint8_t t = data[i + 1];
            int valid_trail = (c >= 0xC7)
                ? (t >= 0xA1 && t <= 0xFE)
                : ((t >= 0x41 && t <= 0x5A) || (t >= 0x61 && t <= 0x7A) || (t >= 0x81 && t <= 0xFE));
            if (valid_trail) {
                cp949_pairs++;
                if (c >= 0xA1 && t >= 0xA1) cp949_wansung++;
                i += 2;
            } else {
                bad_cp949++;
                i++;
            }
        } else {
            bad_cp949++;
            i++;
        }
    }

    *out_u8_2 = u8_2;
    *out_u8_3 = u8_3;
    *out_u8_4 = u8_4;
    *out_bad_u8 = bad_u8;
    *out_cp949_pairs = cp949_pairs;
    *out_cp949_wansung = cp949_wansung;
    *out_bad_cp949 = bad_cp949;
    *out_even_nul = even_nul;
    *out_odd_nul = odd_nul;
}

Doc *doc_load_file(const char *path)
{
    uint8_t *data;
    size_t len;
    char *utf8 = NULL;
    int ulen = 0;
    Doc *doc;
    char *fullPath = path_full(path);

    if (!fullPath || file_read_all(fullPath, &data, &len)) {
        free(fullPath);
        return NULL;
    }
    if (len > 0x10000000) {
        free(data);
        free(fullPath);
        return NULL;
    }

    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        utf8 = decode_utf8_sanitized(data + 3, len - 3, &ulen);
    } else if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        size_t n = (len - 2) / 2, i;
        wchar_t *w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        if (w) {
            int be = data[0] == 0xFE;
            for (i = 0; i < n; i++) {
                uint8_t a = data[2 + i * 2], b = data[3 + i * 2];
                w[i] = be ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
            }
            w[n] = 0;
            utf8 = utf16_to_utf8(w, (int)n, &ulen);
            free(w);
        }
    } else if (len == 0) {
        utf8 = _strdup("");
        ulen = 0;
    } else {
        size_t u8_2, u8_3, u8_4, bad_u8;
        size_t cp949_pairs, cp949_wansung, bad_cp949;
        size_t even_nul, odd_nul;
        scan_encoding_stats(data, len, &u8_2, &u8_3, &u8_4, &bad_u8,
                            &cp949_pairs, &cp949_wansung, &bad_cp949,
                            &even_nul, &odd_nul);

        if ((len % 2) == 0 && len >= 4 &&
            ((odd_nul >= len / 4 && even_nul == 0) || (even_nul >= len / 4 && odd_nul == 0))) {
            /* UTF-16 without BOM */
            size_t n = len / 2, i;
            wchar_t *w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
            if (w) {
                int be = (even_nul >= len / 4 && odd_nul == 0);
                for (i = 0; i < n; i++) {
                    uint8_t a = data[i * 2], b = data[i * 2 + 1];
                    w[i] = be ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
                }
                w[n] = 0;
                utf8 = utf16_to_utf8(w, (int)n, &ulen);
                free(w);
            }
        } else if (bad_u8 == 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, (int)len, NULL, 0) > 0) {
            /* Clean UTF-8 (strip embedded NUL if any via decode_utf8_sanitized) */
            if (even_nul + odd_nul == 0) {
                utf8 = (char *)malloc(len + 1);
                if (utf8) {
                    memcpy(utf8, data, len);
                    utf8[len] = 0;
                    ulen = (int)len;
                }
            } else {
                utf8 = decode_utf8_sanitized(data, len, &ulen);
            }
        } else {
            /* Mixed/partially corrupted UTF-8 vs legacy codepage (CP949 / CP_ACP) */
            int cp949_strict = (MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, (const char *)data, (int)len, NULL, 0) > 0);
            size_t u8_multibyte_score = u8_3 * 2 + u8_4 * 3;
            int prefer_sanitized_utf8 = (u8_3 + u8_4 > 0) && (u8_multibyte_score > bad_u8 * 4) &&
                                        (!cp949_strict || bad_cp949 > bad_u8);

            if (prefer_sanitized_utf8) {
                utf8 = decode_utf8_sanitized(data, len, &ulen);
            } else if ((cp949_strict && cp949_wansung > 0) ||
                       (GetACP() != 949 && cp949_wansung >= 2 && cp949_pairs > bad_cp949 * 4)) {
                utf8 = decode_multibyte_cp(949, cp949_strict ? MB_ERR_INVALID_CHARS : 0, data, len, &ulen);
            }
            if (!utf8) {
                utf8 = decode_multibyte_cp(CP_ACP, 0, data, len, &ulen);
            }
            if (!utf8) {
                utf8 = decode_utf8_sanitized(data, len, &ulen);
            }
        }
    }
    free(data);
    if (!utf8) {
        free(fullPath);
        return NULL;
    }
    doc = doc_parse(utf8, (size_t)ulen, fullPath);
    free(utf8);
    free(fullPath);
    return doc;
}

/* Resolve an image src to a UTF-8 image cache key. Returns malloc()ed string or NULL. */
static char *resolve_image_key(Doc *doc, const char *src)
{
    Buf b = {0};
    int i, n;
    char *res;
    const char *p;

    if (!src || !src[0]) return NULL;
    if (!ascii_nicmp(src, "data:", 5)) return _strdup(src);
    if (!ascii_nicmp(src, "http://", 7) || !ascii_nicmp(src, "https://", 8)) return _strdup(src);

    p = src;
    if (!ascii_nicmp(p, "file:///", 8)) {
        p += 8;
    } else if (!ascii_nicmp(p, "file://localhost/", 17)) {
        p += 17;
    } else if (!ascii_nicmp(p, "file://", 7)) {
        return _strdup(src);
    } else if (!ascii_nicmp(p, "file:", 5)) {
        p += 5;
    }

    /* strip query / fragment, percent-decode (as UTF-8) */
    n = (int)strlen(p);
    for (i = 0; i < n; i++) if (p[i] == '?' || p[i] == '#') { n = i; break; }
    for (i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            char hx[3] = { p[i + 1], p[i + 2], 0 };
            buf_putc(&b, (char)strtol(hx, NULL, 16));
            i += 2;
        } else {
            buf_putc(&b, p[i]);
        }
    }

    /* Check decoded path for UNC/remote BEFORE path_join */
    if (b.p && path_is_remote(b.p)) {
        res = _strdup(b.p);
        buf_free(&b);
        return res;
    }

    if (!doc || !doc->dir || !doc->dir[0]) {
        buf_free(&b);
        return NULL;
    }

    res = path_join(doc->dir, b.p ? b.p : "");
    buf_free(&b);
    /* Prevent arbitrary local file inclusion outside the document directory */
    if (res && !path_is_within(res, doc->dir)) {
        free(res);
        return NULL;
    }
    return res;
}

static void resolve_block_images(Doc *doc, Block *b, int allowRemote)
{
    int i;
    for (i = 0; i < b->nruns; i++) {
        Run *r = &b->runs[i];
        if (r->type == RN_IMAGE) {
            char *key = resolve_image_key(doc, r->src);
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

const char *doc_alert_title(int kind)
{
    switch (kind) {
    case AL_TIP: return "Tip";
    case AL_IMPORTANT: return "Important";
    case AL_WARNING: return "Warning";
    case AL_CAUTION: return "Caution";
    default: return "Note";
    }
}
