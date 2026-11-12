/*
 * doc.h - Markdown document model.
 *
 * The Markdown source is parsed (md4c, GitHub dialect + LaTeX math) into a tree
 * of Blocks. Blocks that carry inline content (paragraphs, headings, table cells)
 * hold a flat array of Runs, each with a style bitmask. All strings are UTF-8
 * and all memory is owned by Doc.arena.
 */
#ifndef MV_DOC_H
#define MV_DOC_H

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Image;

typedef enum BlockType {
    BK_DOCUMENT = 0,
    BK_PARAGRAPH,   /* runs */
    BK_HEADING,     /* runs, level 1..6, anchor */
    BK_QUOTE,       /* children */
    BK_ALERT,       /* GitHub alert "> [!NOTE]": children, alert = AlertKind */
    BK_UL,          /* children = BK_LI; tight */
    BK_OL,          /* children = BK_LI; tight, start */
    BK_LI,          /* children; task */
    BK_CODE,        /* text/textlen (no trailing newline), lang (may be NULL) */
    BK_MATH,        /* display math block: text/textlen (LaTeX math source) */
    BK_HR,
    BK_TABLE,       /* children = BK_TR; ncols */
    BK_TR,          /* children = BK_CELL; header (1 for the header row) */
    BK_CELL,        /* runs; align; header */
    BK_FOOTNOTES,   /* footnote section at end of document; children = BK_FOOTNOTE */
    BK_FOOTNOTE     /* children; footnote = 1-based id; anchor = "fn-<id>" */
} BlockType;

typedef enum RunType {
    RN_TEXT = 0,    /* text/len (UTF-8) */
    RN_BREAK,       /* hard line break */
    RN_IMAGE,       /* src, text/len = alt text, title, width/height hints, img */
    RN_FOOTREF,     /* footnote reference: text = label shown (e.g. "1"), footnote = id */
    RN_MATH         /* LaTeX math span: text/len = LaTeX source; ST_MATH_DISPLAY if display */
} RunType;

/* Run style bits */
#define ST_BOLD         0x0001
#define ST_ITALIC       0x0002
#define ST_STRIKE       0x0004
#define ST_UNDERLINE    0x0008
#define ST_CODE         0x0010  /* inline code span */
#define ST_LINK         0x0020  /* inside a hyperlink; Run.link is valid */
#define ST_SUP          0x0040
#define ST_SUB          0x0080
#define ST_MARK         0x0100  /* ==highlight== */
#define ST_KBD          0x0200  /* <kbd> */
#define ST_BLOCKCODE    0x0400  /* code block syntax token */
#define ST_MATH_DISPLAY 0x0800  /* display math ($$...$$ or \[...\]) */

typedef enum AlertKind {
    AL_NOTE = 0, AL_TIP, AL_IMPORTANT, AL_WARNING, AL_CAUTION
} AlertKind;

typedef enum Align {
    AL_DEFAULT = 0, AL_LEFT, AL_CENTER, AL_RIGHT
} Align;

typedef struct Run {
    int      type;      /* RunType */
    unsigned style;     /* ST_* bits */
    uint32_t color;     /* 0 = default theme color, or 0xRRGGBB */
    int      link;      /* index into Doc.links, or -1 */
    char    *text;      /* UTF-8: RN_TEXT: text; RN_IMAGE: alt text; RN_FOOTREF: label; RN_MATH: LaTeX */
    int      len;       /* UTF-8 byte length */
    char    *src;       /* UTF-8: RN_IMAGE: source as written in the document (URL or relative path) */
    char    *title;     /* UTF-8: RN_IMAGE / link title, may be NULL */
    float    width;     /* RN_IMAGE: requested width in px (0 = natural) */
    float    height;    /* RN_IMAGE: requested height in px (0 = natural) */
    int      footnote;  /* RN_FOOTREF: footnote id (1-based) */
    struct Image *img;  /* RN_IMAGE: resolved image (set by doc_resolve_images); may be NULL or failed */
} Run;

typedef struct Block {
    int            type;    /* BlockType */
    struct Block  *parent;
    struct Block **kids;
    int            nkids, capkids;
    Run           *runs;
    int            nruns, capruns;

    int      level;     /* BK_HEADING: 1..6 */
    int      start;     /* BK_OL: first number */
    int      tight;     /* BK_UL/BK_OL: tight list. BK_PARAGRAPH: 1 = implicit paragraph of a tight list item (no margins) */
    int      task;      /* BK_LI: 0 = not a task item, 1 = unchecked, 2 = checked */
    int      align;     /* BK_CELL / BK_PARAGRAPH / BK_HEADING: Align */
    int      header;    /* BK_TR / BK_CELL: part of the header row */
    int      ncols;     /* BK_TABLE: column count */
    int      alert;     /* BK_ALERT: AlertKind */
    int      footnote;  /* BK_FOOTNOTE: id */
    char    *lang;      /* UTF-8: BK_CODE: language from info string, NULL if none */
    char    *text;      /* UTF-8: BK_CODE / BK_MATH: content (without final newline) */
    int      textlen;   /* UTF-8 byte length */
    char    *anchor;    /* UTF-8: BK_HEADING: GitHub-style slug ("my-heading"); BK_FOOTNOTE: "fn-<id>" */
} Block;

typedef struct Doc {
    Block   *root;          /* BK_DOCUMENT */
    char   **links;         /* UTF-8 hyperlink targets as written (e.g. "https://x", "#anchor", "other.md") */
    int      nlinks, caplinks;
    char    *path;          /* UTF-8 full path of the source file, or NULL */
    char    *dir;           /* UTF-8 directory used to resolve relative links/images, or NULL */
    char    *title;         /* UTF-8 document title: first H1 text, else file name */
    struct ImageCache *images; /* owned image cache (see image.h) */
    Arena    arena;
} Doc;

/* Parse UTF-8 markdown. path (UTF-8) may be NULL. Never returns NULL (except OOM). */
Doc *doc_parse(const char *utf8, size_t len, const char *path);
/* Load a file from UTF-8 path (UTF-8 / UTF-16 BOM / legacy ANSI auto-detected) and parse it. NULL on failure. */
Doc *doc_load_file(const char *path);
/* Resolve and load all images referenced by RN_IMAGE runs (fills Run.img).
 * allowRemote: download http(s) images synchronously (used by exporters). */
void doc_resolve_images(Doc *doc, int allowRemote);
void doc_free(Doc *doc);

/* Plain UTF-8 text of a run list (for titles/bookmarks); malloc()ed. */
char *doc_runs_text(const Run *runs, int nruns);

/* Display name for an alert kind in UTF-8, e.g. "Note". */
const char *doc_alert_title(int kind);

#ifdef __cplusplus
}
#endif
#endif
