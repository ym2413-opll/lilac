/*
 * syntax.h - Markdown code block syntax highlighter.
 * Pure C, zero external dependencies. Operates on UTF-8 strings.
 */
#ifndef MV_SYNTAX_H
#define MV_SYNTAX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SynTokType {
    SYN_DEFAULT = 0,
    SYN_KEYWORD,    /* control flow, declarations, storage */
    SYN_TYPE,       /* primitive types, classes, structs */
    SYN_STRING,     /* string literals, character literals */
    SYN_NUMBER,     /* numeric literals (hex, float, dec) */
    SYN_COMMENT,    /* line and block comments */
    SYN_PREPROC,    /* #include, #define, @decorator, annotations */
    SYN_BUILTIN,    /* true, false, null, None, self, this */
    SYN_FUNCTION,   /* function names, method calls */
    SYN_OPERATOR    /* =, +, -, *, /, ==, !=, ->, etc. */
} SynTokType;

typedef struct SynToken {
    int        start;   /* UTF-8 byte offset in code text */
    int        len;     /* UTF-8 byte length */
    SynTokType type;    /* token type */
} SynToken;

typedef struct SynColors {
    uint32_t text;
    uint32_t keyword;
    uint32_t type;
    uint32_t string;
    uint32_t number;
    uint32_t comment;
    uint32_t preproc;
    uint32_t builtin;
    uint32_t function;
    uint32_t operator_col;
} SynColors;

/* Retrieve palette for light or dark theme */
const SynColors *syntax_get_colors(int dark);

/* Map a token type to its 0xRRGGBB color */
uint32_t syntax_token_color(const SynColors *colors, SynTokType type);

/* Tokenize UTF-8 code text into an array of SynToken.
 * Returns dynamically allocated SynToken array; *outCount receives count.
 * Caller frees with free().
 * If lang is NULL, empty, or unrecognized, smart generic highlighting is applied.
 * If lang is "text", "plain", or "plaintext", no tokens are returned (*outCount = 0).
 */
SynToken *syntax_highlight(const char *lang, const char *text, int textlen, int *outCount);

#ifdef __cplusplus
}
#endif
#endif
