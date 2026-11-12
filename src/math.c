/*
 * math.c - LaTeX math parser, 2D box layout engine, display-list emitter,
 * and DOCX Office Math ML (OMML) exporter.
 *
 * Pure C, zero external dependencies. All internal strings are UTF-8.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include "math.h"

/* ========================================================================= */
/* 1. AST & Symbol Tables                                                    */
/* ========================================================================= */

typedef enum MathStyle {
    MSTYLE_AUTO = 0,    /* italic for Latin/lowercase Greek, upright for digits/operators */
    MSTYLE_ROMAN,       /* \mathrm, \textrm, \operatorname */
    MSTYLE_ITALIC,      /* \mathit */
    MSTYLE_BOLD,        /* \mathbf */
    MSTYLE_BOLDITALIC,  /* \boldsymbol, \bm */
    MSTYLE_MONO,        /* \mathtt */
    MSTYLE_SANS,        /* \mathsf */
    MSTYLE_BB,          /* \mathbb (double-struck) */
    MSTYLE_CAL,         /* \mathcal, \mathscr */
    MSTYLE_FRAK,        /* \mathfrak */
    MSTYLE_TEXT         /* \text (preserves spaces, upright body font) */
} MathStyle;

typedef enum MathAtomClass {
    MATOM_ORD = 0,
    MATOM_OP,           /* large operator or named function (\sum, \int, \sin, \lim) */
    MATOM_BIN,          /* binary operator (+, -, \times, \pm) */
    MATOM_REL,          /* relation (=, <, >, \leq, \to, \in, \subset) */
    MATOM_OPEN,         /* opening delimiter ((, [, \{, \langle) */
    MATOM_CLOSE,        /* closing delimiter (), ], \}, \rangle) */
    MATOM_PUNCT         /* punctuation (,, ;, :) */
} MathAtomClass;

typedef enum MNodeType {
    MN_ROW = 0,         /* horizontal sequence of child nodes */
    MN_SYM,             /* single symbol or text token */
    MN_SPACE,           /* explicit horizontal space (in ems) */
    MN_SCRIPT,          /* base with sub and/or sup */
    MN_FRAC,            /* fraction or binomial (\frac, \dfrac, \tfrac, \binom, \over, \choose) */
    MN_SQRT,            /* radical (\sqrt[deg]{base}) */
    MN_DELIM,           /* stretchy delimiters (\left ... \right) */
    MN_ACCENT,          /* accent / overline / underline / overbrace / underbrace */
    MN_MATRIX,          /* matrix / cases / aligned / array */
    MN_STACK,           /* \overset / \underset / \stackrel */
    MN_BOXED,           /* \boxed{...} */
    MN_PHANTOM,         /* \phantom / \hphantom / \vphantom */
    MN_COLOR            /* \color / \textcolor */
} MNodeType;

typedef enum MAccentKind {
    MACC_HAT = 0,
    MACC_WIDEHAT,
    MACC_CHECK,
    MACC_TILDE,
    MACC_WIDETILDE,
    MACC_ACUTE,
    MACC_GRAVE,
    MACC_DOT,
    MACC_DDOT,
    MACC_DDDOT,
    MACC_BREVE,
    MACC_BAR,
    MACC_OVERLINE,
    MACC_UNDERLINE,
    MACC_VEC,
    MACC_OVERRIGHTARROW,
    MACC_OVERLEFTARROW,
    MACC_OVERLEFTRIGHTARROW,
    MACC_OVERBRACE,
    MACC_UNDERBRACE
} MAccentKind;

typedef struct MNode MNode;

struct MNode {
    MNodeType     type;
    MathAtomClass atom;
    MathStyle     style;

    /* MN_ROW */
    MNode       **kids;
    int           nkids, capkids;

    /* MN_SYM */
    uint32_t      cp;           /* Unicode code point (0 if text string) */
    const char   *text;         /* UTF-8 string for multi-char operators (\sin, \lim, \text{...}) */
    int           textlen;
    int           largeOp;      /* 1 = large symbol (\sum, \prod, \int), 2 = text op with limits (\lim, \max) */
    float         delimScale;   /* >1.0 for \big, \Big, \bigg, \Bigg */

    /* MN_SPACE */
    float         spaceEm;

    /* MN_SCRIPT / MN_FRAC / MN_SQRT / MN_DELIM / MN_ACCENT / MN_STACK / MN_BOXED / MN_PHANTOM / MN_COLOR */
    MNode        *base;         /* or numerator for MN_FRAC */
    MNode        *sub;          /* or denominator for MN_FRAC */
    MNode        *sup;          /* or degree for MN_SQRT */
    int           limits;       /* MN_SCRIPT: 0 = auto, 1 = \limits, -1 = \nolimits */
    int           hasBar;       /* MN_FRAC: 1 = fraction bar, 0 = atop/binom */
    int           fracMode;     /* MN_FRAC: 0 = auto, 1 = \dfrac, 2 = \tfrac */
    uint32_t      leftDelim;    /* MN_DELIM / MN_FRAC (\binom) / MN_MATRIX */
    uint32_t      rightDelim;
    MAccentKind   accent;       /* MN_ACCENT */
    int           phantomMode;  /* MN_PHANTOM: 0 = full, 1 = \hphantom, 2 = \vphantom */
    uint32_t      color;        /* MN_COLOR: 0xRRGGBB */

    /* MN_MATRIX */
    MNode       **cells;        /* rows * cols array of MN_ROW nodes */
    int           rows, cols;
    int           envKind;      /* 0=matrix, 1=cases, 2=aligned/align, 3=smallmatrix */
};

typedef struct LatexSym {
    const char   *name;
    uint32_t      cp;
    MathAtomClass atom;
    int           largeOp;      /* 1 = symbol large op (\sum, \int) */
    int           defaultLimits;/* 1 = limits above/below in display mode */
} LatexSym;

static const LatexSym g_latexSyms[] = {
    /* Lowercase Greek */
    {"alpha",       0x03B1, MATOM_ORD, 0, 0},
    {"beta",        0x03B2, MATOM_ORD, 0, 0},
    {"gamma",       0x03B3, MATOM_ORD, 0, 0},
    {"delta",       0x03B4, MATOM_ORD, 0, 0},
    {"epsilon",     0x03F5, MATOM_ORD, 0, 0},
    {"varepsilon",  0x03B5, MATOM_ORD, 0, 0},
    {"zeta",        0x03B6, MATOM_ORD, 0, 0},
    {"eta",         0x03B7, MATOM_ORD, 0, 0},
    {"theta",       0x03B8, MATOM_ORD, 0, 0},
    {"vartheta",    0x03D1, MATOM_ORD, 0, 0},
    {"iota",        0x03B9, MATOM_ORD, 0, 0},
    {"kappa",       0x03BA, MATOM_ORD, 0, 0},
    {"varkappa",    0x03F0, MATOM_ORD, 0, 0},
    {"lambda",      0x03BB, MATOM_ORD, 0, 0},
    {"mu",          0x03BC, MATOM_ORD, 0, 0},
    {"nu",          0x03BD, MATOM_ORD, 0, 0},
    {"xi",          0x03BE, MATOM_ORD, 0, 0},
    {"omicron",     0x03BF, MATOM_ORD, 0, 0},
    {"pi",          0x03C0, MATOM_ORD, 0, 0},
    {"varpi",       0x03D6, MATOM_ORD, 0, 0},
    {"rho",         0x03C1, MATOM_ORD, 0, 0},
    {"varrho",      0x03F1, MATOM_ORD, 0, 0},
    {"sigma",       0x03C3, MATOM_ORD, 0, 0},
    {"varsigma",    0x03C2, MATOM_ORD, 0, 0},
    {"tau",         0x03C4, MATOM_ORD, 0, 0},
    {"upsilon",     0x03C5, MATOM_ORD, 0, 0},
    {"phi",         0x03D5, MATOM_ORD, 0, 0},
    {"varphi",      0x03C6, MATOM_ORD, 0, 0},
    {"chi",         0x03C7, MATOM_ORD, 0, 0},
    {"psi",         0x03C8, MATOM_ORD, 0, 0},
    {"omega",       0x03C9, MATOM_ORD, 0, 0},

    /* Uppercase Greek */
    {"Gamma",       0x0393, MATOM_ORD, 0, 0},
    {"Delta",       0x0394, MATOM_ORD, 0, 0},
    {"Theta",       0x0398, MATOM_ORD, 0, 0},
    {"Lambda",      0x039B, MATOM_ORD, 0, 0},
    {"Xi",          0x039E, MATOM_ORD, 0, 0},
    {"Pi",          0x03A0, MATOM_ORD, 0, 0},
    {"Sigma",       0x03A3, MATOM_ORD, 0, 0},
    {"Upsilon",     0x03A5, MATOM_ORD, 0, 0},
    {"Phi",         0x03A6, MATOM_ORD, 0, 0},
    {"Psi",         0x03A8, MATOM_ORD, 0, 0},
    {"Omega",       0x03A9, MATOM_ORD, 0, 0},
    {"varGamma",    0x0393, MATOM_ORD, 0, 0},
    {"varDelta",    0x0394, MATOM_ORD, 0, 0},
    {"varTheta",    0x0398, MATOM_ORD, 0, 0},
    {"varLambda",   0x039B, MATOM_ORD, 0, 0},
    {"varXi",       0x039E, MATOM_ORD, 0, 0},
    {"varPi",       0x03A0, MATOM_ORD, 0, 0},
    {"varSigma",    0x03A3, MATOM_ORD, 0, 0},
    {"varUpsilon",  0x03A5, MATOM_ORD, 0, 0},
    {"varPhi",      0x03A6, MATOM_ORD, 0, 0},
    {"varPsi",      0x03A8, MATOM_ORD, 0, 0},
    {"varOmega",    0x03A9, MATOM_ORD, 0, 0},

    /* Large Operators */
    {"sum",         0x2211, MATOM_OP,  1, 1},
    {"prod",        0x220F, MATOM_OP,  1, 1},
    {"coprod",      0x2210, MATOM_OP,  1, 1},
    {"int",         0x222B, MATOM_OP,  1, 0},
    {"iint",        0x222C, MATOM_OP,  1, 0},
    {"iiint",       0x222D, MATOM_OP,  1, 0},
    {"oint",        0x222E, MATOM_OP,  1, 0},
    {"oiint",       0x222F, MATOM_OP,  1, 0},
    {"oiiint",      0x2230, MATOM_OP,  1, 0},
    {"bigcap",      0x22C2, MATOM_OP,  1, 1},
    {"bigcup",      0x22C3, MATOM_OP,  1, 1},
    {"bigvee",      0x22C1, MATOM_OP,  1, 1},
    {"bigwedge",    0x22C0, MATOM_OP,  1, 1},
    {"bigoplus",    0x2A01, MATOM_OP,  1, 1},
    {"bigotimes",   0x2A02, MATOM_OP,  1, 1},
    {"bigodot",     0x2A00, MATOM_OP,  1, 1},
    {"biguplus",    0x2A04, MATOM_OP,  1, 1},
    {"bigsqcup",    0x2A06, MATOM_OP,  1, 1},
    {"smallint",    0x222B, MATOM_OP,  0, 0},

    /* Binary Operators */
    {"pm",          0x00B1, MATOM_BIN, 0, 0},
    {"mp",          0x2213, MATOM_BIN, 0, 0},
    {"times",       0x00D7, MATOM_BIN, 0, 0},
    {"div",         0x00F7, MATOM_BIN, 0, 0},
    {"cdot",        0x22C5, MATOM_BIN, 0, 0},
    {"centerdot",   0x00B7, MATOM_BIN, 0, 0},
    {"ast",         0x2217, MATOM_BIN, 0, 0},
    {"star",        0x22C6, MATOM_BIN, 0, 0},
    {"circ",        0x2218, MATOM_BIN, 0, 0},
    {"bullet",      0x2219, MATOM_BIN, 0, 0},
    {"oplus",       0x2295, MATOM_BIN, 0, 0},
    {"ominus",      0x2296, MATOM_BIN, 0, 0},
    {"otimes",      0x2297, MATOM_BIN, 0, 0},
    {"oslash",      0x2298, MATOM_BIN, 0, 0},
    {"odot",        0x2299, MATOM_BIN, 0, 0},
    {"cap",         0x2229, MATOM_BIN, 0, 0},
    {"cup",         0x222A, MATOM_BIN, 0, 0},
    {"uplus",       0x228E, MATOM_BIN, 0, 0},
    {"sqcap",       0x2293, MATOM_BIN, 0, 0},
    {"sqcup",       0x2294, MATOM_BIN, 0, 0},
    {"vee",         0x2228, MATOM_BIN, 0, 0},
    {"lor",         0x2228, MATOM_BIN, 0, 0},
    {"wedge",       0x2227, MATOM_BIN, 0, 0},
    {"land",        0x2227, MATOM_BIN, 0, 0},
    {"setminus",    0x2216, MATOM_BIN, 0, 0},
    {"smallsetminus",0x2216,MATOM_BIN, 0, 0},
    {"wr",          0x2240, MATOM_BIN, 0, 0},
    {"diamond",     0x22C4, MATOM_BIN, 0, 0},
    {"bigtriangleup",0x25B3,MATOM_BIN, 0, 0},
    {"bigtriangledown",0x25BD,MATOM_BIN,0,0},
    {"triangleleft",0x25C3, MATOM_BIN, 0, 0},
    {"triangleright",0x25B9,MATOM_BIN, 0, 0},
    {"amalg",       0x2A3F, MATOM_BIN, 0, 0},
    {"dagger",      0x2020, MATOM_BIN, 0, 0},
    {"ddagger",     0x2021, MATOM_BIN, 0, 0},
    {"ltimes",      0x22C9, MATOM_BIN, 0, 0},
    {"rtimes",      0x22CA, MATOM_BIN, 0, 0},
    {"bowtie",      0x22C8, MATOM_REL, 0, 0},

    /* Relations */
    {"le",          0x2264, MATOM_REL, 0, 0},
    {"leq",         0x2264, MATOM_REL, 0, 0},
    {"leqslant",    0x2A7D, MATOM_REL, 0, 0},
    {"ge",          0x2265, MATOM_REL, 0, 0},
    {"geq",         0x2265, MATOM_REL, 0, 0},
    {"geqslant",    0x2A7E, MATOM_REL, 0, 0},
    {"ne",          0x2260, MATOM_REL, 0, 0},
    {"neq",         0x2260, MATOM_REL, 0, 0},
    {"equiv",       0x2261, MATOM_REL, 0, 0},
    {"not",         0x0338, MATOM_REL, 0, 0},
    {"approx",      0x2248, MATOM_REL, 0, 0},
    {"thickapprox", 0x2248, MATOM_REL, 0, 0},
    {"sim",         0x223C, MATOM_REL, 0, 0},
    {"thicksim",    0x223C, MATOM_REL, 0, 0},
    {"simeq",       0x2243, MATOM_REL, 0, 0},
    {"cong",        0x2245, MATOM_REL, 0, 0},
    {"ncong",       0x2247, MATOM_REL, 0, 0},
    {"asymp",       0x224D, MATOM_REL, 0, 0},
    {"propto",      0x221D, MATOM_REL, 0, 0},
    {"varpropto",   0x221D, MATOM_REL, 0, 0},
    {"doteq",       0x2250, MATOM_REL, 0, 0},
    {"triangleq",   0x225C, MATOM_REL, 0, 0},
    {"ll",          0x226A, MATOM_REL, 0, 0},
    {"gg",          0x226B, MATOM_REL, 0, 0},
    {"lll",         0x22D8, MATOM_REL, 0, 0},
    {"ggg",         0x22D9, MATOM_REL, 0, 0},
    {"prec",        0x227A, MATOM_REL, 0, 0},
    {"succ",        0x227B, MATOM_REL, 0, 0},
    {"preceq",      0x2AAF, MATOM_REL, 0, 0},
    {"succeq",      0x2AB0, MATOM_REL, 0, 0},
    {"subset",      0x2282, MATOM_REL, 0, 0},
    {"supset",      0x2283, MATOM_REL, 0, 0},
    {"subseteq",    0x2286, MATOM_REL, 0, 0},
    {"supseteq",    0x2287, MATOM_REL, 0, 0},
    {"nsubseteq",   0x2288, MATOM_REL, 0, 0},
    {"nsupseteq",   0x2289, MATOM_REL, 0, 0},
    {"subsetneq",   0x228A, MATOM_REL, 0, 0},
    {"supsetneq",   0x228B, MATOM_REL, 0, 0},
    {"sqsubset",    0x228F, MATOM_REL, 0, 0},
    {"sqsupset",    0x2290, MATOM_REL, 0, 0},
    {"sqsubseteq",  0x2291, MATOM_REL, 0, 0},
    {"sqsupseteq",  0x2292, MATOM_REL, 0, 0},
    {"in",          0x2208, MATOM_REL, 0, 0},
    {"notin",       0x2209, MATOM_REL, 0, 0},
    {"ni",          0x220B, MATOM_REL, 0, 0},
    {"owns",        0x220B, MATOM_REL, 0, 0},
    {"vdash",       0x22A2, MATOM_REL, 0, 0},
    {"dashv",       0x22A3, MATOM_REL, 0, 0},
    {"models",      0x22A8, MATOM_REL, 0, 0},
    {"vDash",       0x22A8, MATOM_REL, 0, 0},
    {"Vdash",       0x22A9, MATOM_REL, 0, 0},
    {"perp",        0x22A5, MATOM_REL, 0, 0},
    {"mid",         0x2223, MATOM_REL, 0, 0},
    {"nmid",        0x2224, MATOM_REL, 0, 0},
    {"parallel",    0x2225, MATOM_REL, 0, 0},
    {"nparallel",   0x2226, MATOM_REL, 0, 0},
    {"sim",         0x223C, MATOM_REL, 0, 0},
    {"nsim",        0x2241, MATOM_REL, 0, 0},
    {"lessgtr",     0x2276, MATOM_REL, 0, 0},
    {"gtrless",     0x2277, MATOM_REL, 0, 0},

    /* Arrows */
    {"to",                  0x2192, MATOM_REL, 0, 0},
    {"rightarrow",          0x2192, MATOM_REL, 0, 0},
    {"gets",                0x2190, MATOM_REL, 0, 0},
    {"leftarrow",           0x2190, MATOM_REL, 0, 0},
    {"leftrightarrow",      0x2194, MATOM_REL, 0, 0},
    {"uparrow",             0x2191, MATOM_REL, 0, 0},
    {"downarrow",           0x2193, MATOM_REL, 0, 0},
    {"updownarrow",         0x2195, MATOM_REL, 0, 0},
    {"Rightarrow",          0x21D2, MATOM_REL, 0, 0},
    {"Leftarrow",           0x21D0, MATOM_REL, 0, 0},
    {"Leftrightarrow",      0x21D4, MATOM_REL, 0, 0},
    {"iff",                 0x21D4, MATOM_REL, 0, 0},
    {"implies",             0x21D2, MATOM_REL, 0, 0},
    {"impliedby",           0x21D0, MATOM_REL, 0, 0},
    {"Uparrow",             0x21D1, MATOM_REL, 0, 0},
    {"Downarrow",           0x21D3, MATOM_REL, 0, 0},
    {"Updownarrow",         0x21D5, MATOM_REL, 0, 0},
    {"mapsto",              0x21A6, MATOM_REL, 0, 0},
    {"longmapsto",          0x27FC, MATOM_REL, 0, 0},
    {"longrightarrow",      0x27F6, MATOM_REL, 0, 0},
    {"longleftarrow",       0x27F5, MATOM_REL, 0, 0},
    {"longleftrightarrow",  0x27F7, MATOM_REL, 0, 0},
    {"Longrightarrow",      0x27F9, MATOM_REL, 0, 0},
    {"Longleftarrow",       0x27F8, MATOM_REL, 0, 0},
    {"Longleftrightarrow",  0x27FA, MATOM_REL, 0, 0},
    {"hookrightarrow",      0x21AA, MATOM_REL, 0, 0},
    {"hookleftarrow",       0x21A9, MATOM_REL, 0, 0},
    {"nearrow",             0x2197, MATOM_REL, 0, 0},
    {"searrow",             0x2198, MATOM_REL, 0, 0},
    {"swarrow",             0x2199, MATOM_REL, 0, 0},
    {"nwarrow",             0x2196, MATOM_REL, 0, 0},
    {"rightharpoonup",      0x21C0, MATOM_REL, 0, 0},
    {"rightharpoondown",    0x21C1, MATOM_REL, 0, 0},
    {"leftharpoonup",       0x21BC, MATOM_REL, 0, 0},
    {"leftharpoondown",     0x21BD, MATOM_REL, 0, 0},
    {"rightleftharpoons",   0x21CC, MATOM_REL, 0, 0},
    {"leadsto",             0x21DD, MATOM_REL, 0, 0},

    /* Miscellaneous Symbols */
    {"infty",       0x221E, MATOM_ORD, 0, 0},
    {"partial",     0x2202, MATOM_ORD, 0, 0},
    {"nabla",       0x2207, MATOM_ORD, 0, 0},
    {"forall",      0x2200, MATOM_ORD, 0, 0},
    {"exists",      0x2203, MATOM_ORD, 0, 0},
    {"nexists",     0x2204, MATOM_ORD, 0, 0},
    {"emptyset",    0x2205, MATOM_ORD, 0, 0},
    {"varnothing",  0x2205, MATOM_ORD, 0, 0},
    {"neg",         0x00AC, MATOM_ORD, 0, 0},
    {"lnot",        0x00AC, MATOM_ORD, 0, 0},
    {"aleph",       0x2135, MATOM_ORD, 0, 0},
    {"beth",        0x2136, MATOM_ORD, 0, 0},
    {"gimel",       0x2137, MATOM_ORD, 0, 0},
    {"daleth",      0x2138, MATOM_ORD, 0, 0},
    {"hbar",        0x210F, MATOM_ORD, 0, 0},
    {"hslash",      0x210F, MATOM_ORD, 0, 0},
    {"ell",         0x2113, MATOM_ORD, 0, 0},
    {"wp",          0x2118, MATOM_ORD, 0, 0},
    {"Re",          0x211C, MATOM_ORD, 0, 0},
    {"Im",          0x2111, MATOM_ORD, 0, 0},
    {"imath",       0x0131, MATOM_ORD, 0, 0},
    {"jmath",       0x0237, MATOM_ORD, 0, 0},
    {"prime",       0x2032, MATOM_ORD, 0, 0},
    {"backprime",   0x2035, MATOM_ORD, 0, 0},
    {"top",         0x22A4, MATOM_ORD, 0, 0},
    {"bot",         0x22A5, MATOM_ORD, 0, 0},
    {"angle",       0x2220, MATOM_ORD, 0, 0},
    {"measuredangle",0x2221,MATOM_ORD, 0, 0},
    {"sphericalangle",0x2222,MATOM_ORD,0, 0},
    {"triangle",    0x25B3, MATOM_ORD, 0, 0},
    {"square",      0x25A1, MATOM_ORD, 0, 0},
    {"Box",         0x25A1, MATOM_ORD, 0, 0},
    {"blacksquare", 0x25A0, MATOM_ORD, 0, 0},
    {"lozenge",     0x25CA, MATOM_ORD, 0, 0},
    {"Diamond",     0x25CA, MATOM_ORD, 0, 0},
    {"surd",        0x221A, MATOM_ORD, 0, 0},
    {"checkmark",   0x2713, MATOM_ORD, 0, 0},
    {"maltese",     0x2720, MATOM_ORD, 0, 0},
    {"clubsuit",    0x2663, MATOM_ORD, 0, 0},
    {"diamondsuit", 0x2662, MATOM_ORD, 0, 0},
    {"heartsuit",   0x2661, MATOM_ORD, 0, 0},
    {"spadesuit",   0x2660, MATOM_ORD, 0, 0},
    {"flat",        0x266D, MATOM_ORD, 0, 0},
    {"natural",     0x266E, MATOM_ORD, 0, 0},
    {"sharp",       0x266F, MATOM_ORD, 0, 0},
    {"backslash",   '\\',   MATOM_ORD, 0, 0},
    {"degree",      0x00B0, MATOM_ORD, 0, 0},
    {"therefore",   0x2234, MATOM_REL, 0, 0},
    {"because",     0x2235, MATOM_REL, 0, 0},

    /* Dots */
    {"dots",        0x2026, MATOM_ORD, 0, 0},
    {"ldots",       0x2026, MATOM_ORD, 0, 0},
    {"cdots",       0x22EF, MATOM_ORD, 0, 0},
    {"vdots",       0x22EE, MATOM_ORD, 0, 0},
    {"ddots",       0x22F1, MATOM_ORD, 0, 0},
    {"iddots",      0x22F0, MATOM_ORD, 0, 0},

    /* Delimiters */
    {"langle",      0x27E8, MATOM_OPEN,  0, 0},
    {"rangle",      0x27E9, MATOM_CLOSE, 0, 0},
    {"lfloor",      0x230A, MATOM_OPEN,  0, 0},
    {"rfloor",      0x230B, MATOM_CLOSE, 0, 0},
    {"lceil",       0x2308, MATOM_OPEN,  0, 0},
    {"rceil",       0x2309, MATOM_CLOSE, 0, 0},
    {"lbrace",      '{',    MATOM_OPEN,  0, 0},
    {"rbrace",      '}',    MATOM_CLOSE, 0, 0},
    {"lbrack",      '[',    MATOM_OPEN,  0, 0},
    {"rbrack",      ']',    MATOM_CLOSE, 0, 0},
    {"vert",        '|',    MATOM_ORD,   0, 0},
    {"Vert",        0x2016, MATOM_ORD,   0, 0},
    {"lvert",       '|',    MATOM_OPEN,  0, 0},
    {"rvert",       '|',    MATOM_CLOSE, 0, 0},
    {"lVert",       0x2016, MATOM_OPEN,  0, 0},
    {"rVert",       0x2016, MATOM_CLOSE, 0, 0},

    /* Colon punctuation */
    {"colon",       ':',    MATOM_PUNCT, 0, 0},
    {NULL, 0, MATOM_ORD, 0, 0}
};

typedef struct NamedOp {
    const char *cmd;
    const char *label;
    int         hasLimits; /* 1 = limits above/below in display mode */
} NamedOp;

static const NamedOp g_namedOps[] = {
    {"sin",     "sin",      0},
    {"cos",     "cos",      0},
    {"tan",     "tan",      0},
    {"cot",     "cot",      0},
    {"sec",     "sec",      0},
    {"csc",     "csc",      0},
    {"arcsin",  "arcsin",   0},
    {"arccos",  "arccos",   0},
    {"arctan",  "arctan",   0},
    {"sinh",    "sinh",     0},
    {"cosh",    "cosh",     0},
    {"tanh",    "tanh",     0},
    {"coth",    "coth",     0},
    {"ln",      "ln",       0},
    {"log",     "log",      0},
    {"lg",      "lg",       0},
    {"exp",     "exp",      0},
    {"deg",     "deg",      0},
    {"dim",     "dim",      0},
    {"ker",     "ker",      0},
    {"hom",     "hom",      0},
    {"arg",     "arg",      0},
    {"bmod",    "mod",      0},
    {"pmod",    "mod",      0},
    {"mod",     "mod",      0},
    {"lim",     "lim",      1},
    {"limsup",  "lim sup",  1},
    {"liminf",  "lim inf",  1},
    {"sup",     "sup",      1},
    {"inf",     "inf",      1},
    {"max",     "max",      1},
    {"min",     "min",      1},
    {"det",     "det",      1},
    {"Pr",      "Pr",       1},
    {"gcd",     "gcd",      1},
    {"lcm",     "lcm",      1},
    {"argmax",  "arg max",  1},
    {"argmin",  "arg min",  1},
    {"injlim",  "inj lim",  1},
    {"projlim", "proj lim", 1},
    {NULL, NULL, 0}
};

static const LatexSym *lookup_latex_sym(const char *cmd, int len)
{
    int i;
    for (i = 0; g_latexSyms[i].name; i++) {
        if ((int)strlen(g_latexSyms[i].name) == len &&
            !memcmp(g_latexSyms[i].name, cmd, (size_t)len)) {
            return &g_latexSyms[i];
        }
    }
    return NULL;
}

static const NamedOp *lookup_named_op(const char *cmd, int len)
{
    int i;
    for (i = 0; g_namedOps[i].cmd; i++) {
        if ((int)strlen(g_namedOps[i].cmd) == len &&
            !memcmp(g_namedOps[i].cmd, cmd, (size_t)len)) {
            return &g_namedOps[i];
        }
    }
    return NULL;
}

/* ========================================================================= */
/* 2. LaTeX Math Parser                                                      */
/* ========================================================================= */

#define MAX_MATH_DEPTH   128
#define MAX_MATRIX_ROWS  256
#define MAX_MATRIX_COLS  64
#define MAX_MATRIX_CELLS 4096

typedef struct MParser {
    Arena      *arena;
    const char *s;
    int         len;
    int         pos;
    int         depth;
} MParser;

static MNode *mnode_new(Arena *a, MNodeType type)
{
    MNode *n = (MNode *)arena_alloc(a, sizeof(MNode));
    n->type = type;
    return n;
}

static void mrow_add(Arena *a, MNode *row, MNode *child)
{
    if (!child) return;
    if (row->nkids == row->capkids) {
        int nc = row->capkids ? row->capkids * 2 : 8;
        MNode **nk = (MNode **)arena_alloc(a, (size_t)nc * sizeof(MNode *));
        if (row->nkids) memcpy(nk, row->kids, (size_t)row->nkids * sizeof(MNode *));
        row->kids = nk;
        row->capkids = nc;
    }
    row->kids[row->nkids++] = child;
}

static void mp_skip_ws(MParser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else if (c == '%') {
            /* LaTeX comment to end of line */
            while (p->pos < p->len && p->s[p->pos] != '\n') p->pos++;
        } else {
            break;
        }
    }
}

static MathAtomClass classify_ascii(uint32_t cp)
{
    switch (cp) {
    case '+': case '-': case '*': case '/':
    case 0x00B1: case 0x00D7: case 0x00F7: case 0x2212:
        return MATOM_BIN;
    case '=': case '<': case '>': case '~':
    case 0x2264: case 0x2265: case 0x2260: case 0x2248: case 0x2261:
        return MATOM_REL;
    case '(': case '[': case '{':
        return MATOM_OPEN;
    case ')': case ']': case '}':
        return MATOM_CLOSE;
    case ',': case ';': case ':':
        return MATOM_PUNCT;
    default:
        return MATOM_ORD;
    }
}

/* Forward declarations */
static MNode *mp_parse_row(MParser *p, MathStyle style, const char *stopCmd);
static MNode *mp_parse_atom(MParser *p, MathStyle style);

/* Parse a delimiter token for \left, \right, \middle, \big, etc. */
static uint32_t mp_parse_delim(MParser *p)
{
    mp_skip_ws(p);
    if (p->pos >= p->len) return 0;
    if (p->s[p->pos] == '.') {
        p->pos++;
        return 0; /* empty delimiter \left. or \right. */
    }
    if (p->s[p->pos] == '\\') {
        p->pos++;
        if (p->pos >= p->len) return 0;
        if (!isalpha((unsigned char)p->s[p->pos])) {
            char c = p->s[p->pos++];
            if (c == '|') return 0x2016; /* \| */
            return (uint32_t)(unsigned char)c;
        }
        {
            int st = p->pos;
            const LatexSym *sym;
            while (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) p->pos++;
            sym = lookup_latex_sym(p->s + st, p->pos - st);
            if (sym) return sym->cp;
            return 0;
        }
    }
    return utf8_next(p->s, p->len, &p->pos);
}

/* Parse a braced plain string like \begin{pmatrix} or \color{red} */
static int mp_parse_braced_str(MParser *p, char *buf, int bufcap)
{
    int n = 0;
    mp_skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != '{') return 0;
    p->pos++;
    while (p->pos < p->len && p->s[p->pos] != '}') {
        if (n + 1 < bufcap) buf[n++] = p->s[p->pos];
        p->pos++;
    }
    if (p->pos < p->len && p->s[p->pos] == '}') p->pos++;
    buf[n] = 0;
    return n;
}

static uint32_t parse_named_color(const char *name)
{
    if (!name || !name[0]) return 0;
    if (name[0] == '#' && strlen(name) == 7) {
        return (uint32_t)strtoul(name + 1, NULL, 16);
    }
    if (!ascii_icmp(name, "red"))     return 0xD1242F;
    if (!ascii_icmp(name, "blue"))    return 0x0969DA;
    if (!ascii_icmp(name, "green"))   return 0x1A7F37;
    if (!ascii_icmp(name, "teal"))    return 0x0E7490;
    if (!ascii_icmp(name, "purple") || !ascii_icmp(name, "violet") || !ascii_icmp(name, "magenta")) return 0x8250DF;
    if (!ascii_icmp(name, "orange") || !ascii_icmp(name, "brown"))  return 0x9A6700;
    if (!ascii_icmp(name, "gray") || !ascii_icmp(name, "grey"))     return 0x6E7781;
    if (!ascii_icmp(name, "cyan"))    return 0x0550AE;
    return 0;
}

static MNode *mp_parse_primary(MParser *p, MathStyle style);

/* Parse \begin{env} ... \end{env} */
static MNode *mp_parse_environment(MParser *p, MathStyle style)
{
    char env[32];
    MNode *mat;
    int capCells = 16;
    MNode **cells;
    int col = 0, maxCols = 1, row = 0;
    MNode *curCell;

    if (p->depth >= MAX_MATH_DEPTH) return NULL;
    p->depth++;

    if (!mp_parse_braced_str(p, env, sizeof(env))) {
        p->depth--;
        return NULL;
    }

    /* Optional column spec for array/matrix*: e.g. \begin{array}{ccc} or \begin{bmatrix*}[r] */
    mp_skip_ws(p);
    if (!strcmp(env, "array") || !strcmp(env, "subarray")) {
        char colspec[64];
        mp_parse_braced_str(p, colspec, sizeof(colspec));
    } else if (p->pos < p->len && p->s[p->pos] == '[') {
        while (p->pos < p->len && p->s[p->pos] != ']') p->pos++;
        if (p->pos < p->len) p->pos++;
    }

    mat = mnode_new(p->arena, MN_MATRIX);
    mat->atom = MATOM_ORD;
    mat->style = style;
    if (!strcmp(env, "pmatrix") || !strcmp(env, "pmatrix*")) {
        mat->leftDelim = '('; mat->rightDelim = ')';
    } else if (!strcmp(env, "bmatrix") || !strcmp(env, "bmatrix*")) {
        mat->leftDelim = '['; mat->rightDelim = ']';
    } else if (!strcmp(env, "Bmatrix") || !strcmp(env, "Bmatrix*")) {
        mat->leftDelim = '{'; mat->rightDelim = '}';
    } else if (!strcmp(env, "vmatrix") || !strcmp(env, "vmatrix*")) {
        mat->leftDelim = '|'; mat->rightDelim = '|';
    } else if (!strcmp(env, "Vmatrix") || !strcmp(env, "Vmatrix*")) {
        mat->leftDelim = 0x2016; mat->rightDelim = 0x2016;
    } else if (!strcmp(env, "cases") || !strcmp(env, "dcases") || !strcmp(env, "rcases")) {
        if (!strcmp(env, "rcases")) { mat->leftDelim = 0; mat->rightDelim = '}'; }
        else { mat->leftDelim = '{'; mat->rightDelim = 0; }
        mat->envKind = 1;
    } else if (!strcmp(env, "aligned") || !strcmp(env, "align") || !strcmp(env, "align*") ||
               !strcmp(env, "split") || !strcmp(env, "eqnarray") || !strcmp(env, "eqnarray*")) {
        mat->envKind = 2;
    } else if (!strcmp(env, "smallmatrix")) {
        mat->envKind = 3;
    }

    /* Temp dynamic array of (row, col, cell) */
    {
        typedef struct CellEntry { int r, c; MNode *node; } CellEntry;
        CellEntry *entries = (CellEntry *)malloc((size_t)capCells * sizeof(CellEntry));
        int nEntries = 0;
        int allocFailed = (entries == NULL);

        curCell = mnode_new(p->arena, MN_ROW);
        curCell->style = style;

        while (!allocFailed && p->pos < p->len) {
            mp_skip_ws(p);
            if (p->pos >= p->len) break;

            if (p->s[p->pos] == '&') {
                p->pos++;
                if (nEntries < MAX_MATRIX_CELLS && row < MAX_MATRIX_ROWS && col < MAX_MATRIX_COLS) {
                    if (nEntries == capCells) {
                        int nc = (capCells <= MAX_MATRIX_CELLS / 2) ? capCells * 2 : MAX_MATRIX_CELLS;
                        CellEntry *tmp = (nc > capCells) ? (CellEntry *)realloc(entries, (size_t)nc * sizeof(CellEntry)) : NULL;
                        if (!tmp) { allocFailed = 1; break; }
                        entries = tmp;
                        capCells = nc;
                    }
                    entries[nEntries].r = row;
                    entries[nEntries].c = col;
                    entries[nEntries].node = curCell;
                    nEntries++;
                }
                if (col + 1 < MAX_MATRIX_COLS) col++;
                if (col + 1 > maxCols) maxCols = col + 1;
                curCell = mnode_new(p->arena, MN_ROW);
                curCell->style = style;
                continue;
            }

            if (p->s[p->pos] == '\\' && p->pos + 1 < p->len && p->s[p->pos + 1] == '\\') {
                p->pos += 2;
                /* Optional \\[4pt] spacing spec */
                mp_skip_ws(p);
                if (p->pos < p->len && p->s[p->pos] == '[') {
                    while (p->pos < p->len && p->s[p->pos] != ']') p->pos++;
                    if (p->pos < p->len) p->pos++;
                }
                if (nEntries < MAX_MATRIX_CELLS && row < MAX_MATRIX_ROWS && col < MAX_MATRIX_COLS) {
                    if (nEntries == capCells) {
                        int nc = (capCells <= MAX_MATRIX_CELLS / 2) ? capCells * 2 : MAX_MATRIX_CELLS;
                        CellEntry *tmp = (nc > capCells) ? (CellEntry *)realloc(entries, (size_t)nc * sizeof(CellEntry)) : NULL;
                        if (!tmp) { allocFailed = 1; break; }
                        entries = tmp;
                        capCells = nc;
                    }
                    entries[nEntries].r = row;
                    entries[nEntries].c = col;
                    entries[nEntries].node = curCell;
                    nEntries++;
                }
                if (col + 1 > maxCols) maxCols = col + 1;
                if (row + 1 < MAX_MATRIX_ROWS) row++;
                col = 0;
                curCell = mnode_new(p->arena, MN_ROW);
                curCell->style = style;
                continue;
            }

            /* Check for \end{...} or \cr */
            if (p->s[p->pos] == '\\') {
                int save = p->pos;
                int st = save + 1, clen;
                while (st < p->len && isalpha((unsigned char)p->s[st])) st++;
                clen = st - (save + 1);
                if (clen == 3 && !memcmp(p->s + save + 1, "end", 3)) {
                    char endEnv[32];
                    p->pos = st;
                    mp_parse_braced_str(p, endEnv, sizeof(endEnv));
                    break;
                }
                if (clen == 2 && !memcmp(p->s + save + 1, "cr", 2)) {
                    p->pos = st;
                    if (nEntries < MAX_MATRIX_CELLS && row < MAX_MATRIX_ROWS && col < MAX_MATRIX_COLS) {
                        if (nEntries == capCells) {
                            int nc = (capCells <= MAX_MATRIX_CELLS / 2) ? capCells * 2 : MAX_MATRIX_CELLS;
                            CellEntry *tmp = (nc > capCells) ? (CellEntry *)realloc(entries, (size_t)nc * sizeof(CellEntry)) : NULL;
                            if (!tmp) { allocFailed = 1; break; }
                            entries = tmp;
                            capCells = nc;
                        }
                        entries[nEntries].r = row;
                        entries[nEntries].c = col;
                        entries[nEntries].node = curCell;
                        nEntries++;
                    }
                    if (col + 1 > maxCols) maxCols = col + 1;
                    if (row + 1 < MAX_MATRIX_ROWS) row++;
                    col = 0;
                    curCell = mnode_new(p->arena, MN_ROW);
                    curCell->style = style;
                    continue;
                }
                if (clen == 5 && !memcmp(p->s + save + 1, "hline", 5)) {
                    p->pos = st;
                    continue;
                }
            }

            if (p->s[p->pos] == '}') break;

            {
                MNode *atom = mp_parse_atom(p, style);
                if (atom) mrow_add(p->arena, curCell, atom);
                else if (p->pos < p->len) p->pos++;
            }
        }

        if (!allocFailed && (curCell->nkids > 0 || col > 0 || nEntries == 0)) {
            if (nEntries < MAX_MATRIX_CELLS && row < MAX_MATRIX_ROWS && col < MAX_MATRIX_COLS) {
                if (nEntries == capCells) {
                    int nc = (capCells <= MAX_MATRIX_CELLS / 2) ? capCells * 2 : MAX_MATRIX_CELLS;
                    CellEntry *tmp = (nc > capCells) ? (CellEntry *)realloc(entries, (size_t)nc * sizeof(CellEntry)) : NULL;
                    if (!tmp) { allocFailed = 1; }
                    else { entries = tmp; capCells = nc; }
                }
                if (!allocFailed) {
                    entries[nEntries].r = row;
                    entries[nEntries].c = col;
                    entries[nEntries].node = curCell;
                    nEntries++;
                }
            }
            if (col + 1 > maxCols) maxCols = col + 1;
            if (row + 1 < MAX_MATRIX_ROWS) row++;
        }

        if (row <= 0) row = 1;
        if (maxCols <= 0) maxCols = 1;
        if (row > MAX_MATRIX_ROWS) row = MAX_MATRIX_ROWS;
        if (maxCols > MAX_MATRIX_COLS) maxCols = MAX_MATRIX_COLS;
        mat->rows = row;
        mat->cols = maxCols;
        if (!allocFailed && entries && (size_t)row <= MAX_MATRIX_CELLS / (size_t)maxCols) {
            size_t totalCells = (size_t)row * (size_t)maxCols;
            cells = (MNode **)arena_alloc(p->arena, totalCells * sizeof(MNode *));
            {
                int k;
                for (k = 0; k < nEntries; k++) {
                    if (entries[k].r >= 0 && entries[k].r < row &&
                        entries[k].c >= 0 && entries[k].c < maxCols) {
                        cells[(size_t)entries[k].r * (size_t)maxCols + (size_t)entries[k].c] = entries[k].node;
                    }
                }
            }
            mat->cells = cells;
        }
        free(entries);
    }

    p->depth--;
    return mat;
}

/* Parse a raw text argument \text{...} preserving spaces (and nested $...$ math) */
static MNode *mp_parse_text_arg(MParser *p)
{
    MNode *row;
    int depth = 1;
    int segStart;
    mp_skip_ws(p);
    if (p->pos >= p->len) return NULL;
    if (p->s[p->pos] != '{') {
        MNode *sym = mnode_new(p->arena, MN_SYM);
        sym->style = MSTYLE_TEXT;
        sym->cp = utf8_next(p->s, p->len, &p->pos);
        return sym;
    }
    p->pos++; /* skip '{' */
    row = mnode_new(p->arena, MN_ROW);
    row->style = MSTYLE_TEXT;
    segStart = p->pos;

    while (p->pos < p->len && depth > 0) {
        char c = p->s[p->pos];
        if (c == '{') {
            depth++;
            p->pos++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                if (p->pos > segStart) {
                    MNode *sym = mnode_new(p->arena, MN_SYM);
                    sym->style = MSTYLE_TEXT;
                    sym->text = arena_strndup(p->arena, p->s + segStart, p->pos - segStart);
                    sym->textlen = p->pos - segStart;
                    mrow_add(p->arena, row, sym);
                }
                p->pos++;
                break;
            }
            p->pos++;
        } else if (c == '$' && depth == 1) {
            if (p->pos > segStart) {
                MNode *sym = mnode_new(p->arena, MN_SYM);
                sym->style = MSTYLE_TEXT;
                sym->text = arena_strndup(p->arena, p->s + segStart, p->pos - segStart);
                sym->textlen = p->pos - segStart;
                mrow_add(p->arena, row, sym);
            }
            p->pos++;
            {
                int mstart = p->pos;
                while (p->pos < p->len && p->s[p->pos] != '$') p->pos++;
                if (p->pos > mstart && p->depth < MAX_MATH_DEPTH) {
                    MParser subp = { p->arena, p->s + mstart, p->pos - mstart, 0, p->depth + 1 };
                    mrow_add(p->arena, row, mp_parse_row(&subp, MSTYLE_AUTO, NULL));
                }
                if (p->pos < p->len && p->s[p->pos] == '$') p->pos++;
            }
            segStart = p->pos;
        } else {
            p->pos++;
        }
    }
    return row;
}

/* Parse a single primary unit (group {...}, command \..., or single character), WITHOUT scripts */
static MNode *mp_parse_primary_inner(MParser *p, MathStyle style);

static MNode *mp_parse_primary(MParser *p, MathStyle style)
{
    MNode *res;
    if (p->depth >= MAX_MATH_DEPTH) {
        if (p->pos < p->len) p->pos++;
        return NULL;
    }
    p->depth++;
    res = mp_parse_primary_inner(p, style);
    p->depth--;
    return res;
}

static MNode *mp_parse_primary_inner(MParser *p, MathStyle style)
{
    char c;
    mp_skip_ws(p);
    if (p->pos >= p->len) return NULL;

    c = p->s[p->pos];
    if (c == '}' || c == '&') return NULL;

    /* Group { ... } */
    if (c == '{') {
        p->pos++;
        return mp_parse_row(p, style, "}");
    }

    /* Command \... */
    if (c == '\\') {
        int st, clen;
        const char *cmd;
        p->pos++;
        if (p->pos >= p->len) return NULL;
        c = p->s[p->pos];

        /* Escaped single non-letter character */
        if (!isalpha((unsigned char)c)) {
            p->pos++;
            if (c == ',' || c == ':' || c == '>' || c == ';' || c == '!' || c == ' ') {
                MNode *sp = mnode_new(p->arena, MN_SPACE);
                if (c == ',') sp->spaceEm = 0.17f;
                else if (c == ':' || c == '>') sp->spaceEm = 0.22f;
                else if (c == ';') sp->spaceEm = 0.28f;
                else if (c == '!') sp->spaceEm = -0.17f;
                else sp->spaceEm = 0.33f;
                return sp;
            }
            {
                MNode *sym = mnode_new(p->arena, MN_SYM);
                sym->style = style;
                if (c == '|') { sym->cp = 0x2016; sym->atom = MATOM_ORD; }
                else if (c == '{') { sym->cp = '{'; sym->atom = MATOM_OPEN; }
                else if (c == '}') { sym->cp = '}'; sym->atom = MATOM_CLOSE; }
                else { sym->cp = (uint32_t)(unsigned char)c; sym->atom = MATOM_ORD; }
                return sym;
            }
        }

        st = p->pos;
        while (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) p->pos++;
        cmd = p->s + st;
        clen = p->pos - st;

        /* Check for '*' suffix on operatorname* */
        if (clen == 12 && !memcmp(cmd, "operatorname", 12)) {
            int hasLim = 0;
            char opbuf[64];
            if (p->pos < p->len && p->s[p->pos] == '*') { hasLim = 1; p->pos++; }
            if (mp_parse_braced_str(p, opbuf, sizeof(opbuf)) > 0) {
                MNode *sym = mnode_new(p->arena, MN_SYM);
                sym->style = MSTYLE_ROMAN;
                sym->atom = MATOM_OP;
                sym->largeOp = hasLim ? 2 : 0;
                sym->text = arena_strndup(p->arena, opbuf, -1);
                sym->textlen = (int)strlen(opbuf);
                return sym;
            }
        }

        /* Environments \begin{...} */
        if (clen == 5 && !memcmp(cmd, "begin", 5)) {
            return mp_parse_environment(p, style);
        }

        /* Explicit spaces */
        if (clen == 4 && !memcmp(cmd, "quad", 4)) {
            MNode *sp = mnode_new(p->arena, MN_SPACE); sp->spaceEm = 1.0f; return sp;
        }
        if (clen == 5 && !memcmp(cmd, "qquad", 5)) {
            MNode *sp = mnode_new(p->arena, MN_SPACE); sp->spaceEm = 2.0f; return sp;
        }
        if ((clen == 9 && !memcmp(cmd, "thinspace", 9)) || (clen == 6 && !memcmp(cmd, "enspace", 6))) {
            MNode *sp = mnode_new(p->arena, MN_SPACE); sp->spaceEm = (clen == 9) ? 0.17f : 0.5f; return sp;
        }
        if ((clen == 6 && !memcmp(cmd, "hspace", 6)) || (clen == 4 && !memcmp(cmd, "kern", 4))) {
            char sbuf[32];
            if (p->pos < p->len && p->s[p->pos] == '*') p->pos++;
            if (p->pos < p->len && p->s[p->pos] == '{') mp_parse_braced_str(p, sbuf, sizeof(sbuf));
            {
                MNode *sp = mnode_new(p->arena, MN_SPACE);
                sp->spaceEm = 0.5f;
                return sp;
            }
        }

        /* Style / size switches inside group (e.g. {\bf x}, \displaystyle, \textstyle) */
        if ((clen == 12 && !memcmp(cmd, "displaystyle", 12)) ||
            (clen == 9 && !memcmp(cmd, "textstyle", 9)) ||
            (clen == 11 && !memcmp(cmd, "scriptstyle", 11)) ||
            (clen == 17 && !memcmp(cmd, "scriptscriptstyle", 17)) ||
            (clen == 4 && !memcmp(cmd, "left", 4) && 0)) {
            return mp_parse_primary(p, style);
        }

        /* Fractions and Binomials: \frac, \dfrac, \tfrac, \cfrac, \binom, \dbinom, \tbinom */
        if ((clen == 4 && !memcmp(cmd, "frac", 4)) ||
            (clen == 5 && (!memcmp(cmd, "dfrac", 5) || !memcmp(cmd, "tfrac", 5) || !memcmp(cmd, "cfrac", 5))) ||
            (clen == 5 && !memcmp(cmd, "binom", 5)) ||
            (clen == 6 && (!memcmp(cmd, "dbinom", 6) || !memcmp(cmd, "tbinom", 6)))) {
            int isBinom = (cmd[clen - 5] == 'b');
            MNode *fn = mnode_new(p->arena, MN_FRAC);
            /* Optional [l]/[r] for \cfrac */
            mp_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == '[') {
                while (p->pos < p->len && p->s[p->pos] != ']') p->pos++;
                if (p->pos < p->len) p->pos++;
            }
            fn->style = style;
            fn->hasBar = !isBinom;
            if (isBinom) { fn->leftDelim = '('; fn->rightDelim = ')'; }
            if (cmd[0] == 'd' || cmd[0] == 'c') fn->fracMode = 1;
            else if (cmd[0] == 't') fn->fracMode = 2;
            fn->base = mp_parse_primary(p, style);
            fn->sub  = mp_parse_primary(p, style);
            return fn;
        }

        /* Radicals: \sqrt[deg]{base} */
        if (clen == 4 && !memcmp(cmd, "sqrt", 4)) {
            MNode *sq = mnode_new(p->arena, MN_SQRT);
            sq->style = style;
            mp_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == '[') {
                p->pos++;
                sq->sup = mp_parse_row(p, style, "]");
            }
            sq->base = mp_parse_primary(p, style);
            return sq;
        }

        /* Stretchy delimiters: \left ... \right */
        if (clen == 4 && !memcmp(cmd, "left", 4)) {
            MNode *dl = mnode_new(p->arena, MN_DELIM);
            dl->style = style;
            dl->leftDelim = mp_parse_delim(p);
            dl->base = mp_parse_row(p, style, "\\right");
            dl->rightDelim = mp_parse_delim(p);
            return dl;
        }

        /* Manual sized delimiters: \big, \Big, \bigg, \Bigg (and l/r/m variants) */
        if ((clen == 3 || clen == 4 || clen == 5) &&
            (!memcmp(cmd, "big", 3) || !memcmp(cmd, "Big", 3))) {
            float sc = 1.2f;
            uint32_t dcp;
            MNode *sym;
            if (!memcmp(cmd, "Bigg", 4)) sc = 2.2f;
            else if (!memcmp(cmd, "bigg", 4)) sc = 1.8f;
            else if (!memcmp(cmd, "Big", 3)) sc = 1.45f;
            dcp = mp_parse_delim(p);
            sym = mnode_new(p->arena, MN_SYM);
            sym->style = style;
            sym->cp = dcp ? dcp : '|';
            sym->delimScale = sc;
            sym->atom = classify_ascii(sym->cp);
            return sym;
        }

        /* Accents & Over/Under bars/arrows/braces */
        {
            static const struct { const char *name; MAccentKind kind; } accMap[] = {
                {"hat",                 MACC_HAT},
                {"widehat",             MACC_WIDEHAT},
                {"check",               MACC_CHECK},
                {"tilde",               MACC_TILDE},
                {"widetilde",           MACC_WIDETILDE},
                {"acute",               MACC_ACUTE},
                {"grave",               MACC_GRAVE},
                {"dot",                 MACC_DOT},
                {"ddot",                MACC_DDOT},
                {"dddot",               MACC_DDDOT},
                {"breve",               MACC_BREVE},
                {"bar",                 MACC_BAR},
                {"overline",            MACC_OVERLINE},
                {"underline",           MACC_UNDERLINE},
                {"vec",                 MACC_VEC},
                {"overrightarrow",      MACC_OVERRIGHTARROW},
                {"overleftarrow",       MACC_OVERLEFTARROW},
                {"overleftrightarrow",  MACC_OVERLEFTRIGHTARROW},
                {"overbrace",           MACC_OVERBRACE},
                {"underbrace",          MACC_UNDERBRACE},
                {NULL, MACC_HAT}
            };
            int k;
            for (k = 0; accMap[k].name; k++) {
                if ((int)strlen(accMap[k].name) == clen && !memcmp(accMap[k].name, cmd, (size_t)clen)) {
                    MNode *ac = mnode_new(p->arena, MN_ACCENT);
                    ac->style = style;
                    ac->accent = accMap[k].kind;
                    ac->base = mp_parse_primary(p, style);
                    if (ac->accent == MACC_OVERBRACE || ac->accent == MACC_UNDERBRACE) {
                        ac->atom = MATOM_OP;
                        ac->largeOp = 2; /* allow limits above/below overbrace/underbrace */
                    }
                    return ac;
                }
            }
        }

        /* Stacking: \overset{top}{base}, \underset{bot}{base}, \stackrel{top}{base} */
        if ((clen == 7 && !memcmp(cmd, "overset", 7)) ||
            (clen == 8 && !memcmp(cmd, "stackrel", 8))) {
            MNode *stNode = mnode_new(p->arena, MN_STACK);
            stNode->style = style;
            stNode->sup = mp_parse_primary(p, style);
            stNode->base = mp_parse_primary(p, style);
            stNode->atom = stNode->base ? stNode->base->atom : MATOM_REL;
            return stNode;
        }
        if (clen == 8 && !memcmp(cmd, "underset", 8)) {
            MNode *stNode = mnode_new(p->arena, MN_STACK);
            stNode->style = style;
            stNode->sub = mp_parse_primary(p, style);
            stNode->base = mp_parse_primary(p, style);
            stNode->atom = stNode->base ? stNode->base->atom : MATOM_REL;
            return stNode;
        }
        if ((clen == 11 && !memcmp(cmd, "xrightarrow", 11)) ||
            (clen == 10 && !memcmp(cmd, "xleftarrow", 10))) {
            MNode *stNode = mnode_new(p->arena, MN_STACK);
            MNode *arr = mnode_new(p->arena, MN_SYM);
            arr->cp = (clen == 11) ? 0x27F6 : 0x27F5;
            arr->atom = MATOM_REL;
            stNode->style = style;
            stNode->atom = MATOM_REL;
            mp_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == '[') {
                p->pos++;
                stNode->sub = mp_parse_row(p, style, "]");
            }
            stNode->sup = mp_parse_primary(p, style);
            stNode->base = arr;
            return stNode;
        }

        /* \boxed{...} */
        if (clen == 5 && !memcmp(cmd, "boxed", 5)) {
            MNode *bx = mnode_new(p->arena, MN_BOXED);
            bx->style = style;
            bx->base = mp_parse_primary(p, style);
            return bx;
        }

        /* \phantom, \hphantom, \vphantom */
        if ((clen == 7 && !memcmp(cmd, "phantom", 7)) ||
            (clen == 8 && (!memcmp(cmd, "hphantom", 8) || !memcmp(cmd, "vphantom", 8)))) {
            MNode *ph = mnode_new(p->arena, MN_PHANTOM);
            ph->style = style;
            if (cmd[0] == 'h') ph->phantomMode = 1;
            else if (cmd[0] == 'v') ph->phantomMode = 2;
            ph->base = mp_parse_primary(p, style);
            return ph;
        }

        /* \textcolor{col}{...} or \color{col} */
        if ((clen == 9 && !memcmp(cmd, "textcolor", 9)) ||
            (clen == 5 && !memcmp(cmd, "color", 5))) {
            char cbuf[32];
            uint32_t colVal = 0;
            if (mp_parse_braced_str(p, cbuf, sizeof(cbuf)) > 0) {
                colVal = parse_named_color(cbuf);
            }
            if (clen == 9) {
                MNode *cn = mnode_new(p->arena, MN_COLOR);
                cn->color = colVal;
                cn->base = mp_parse_primary(p, style);
                return cn;
            }
            return mp_parse_primary(p, style);
        }

        /* Font style commands */
        if ((clen == 4 && !memcmp(cmd, "text", 4)) ||
            (clen == 6 && (!memcmp(cmd, "textrm", 6) || !memcmp(cmd, "textbf", 6) ||
                           !memcmp(cmd, "textit", 6) || !memcmp(cmd, "texttt", 6) ||
                           !memcmp(cmd, "textsf", 6))) ||
            (clen == 4 && !memcmp(cmd, "mbox", 4)) ||
            (clen == 4 && !memcmp(cmd, "hbox", 4))) {
            return mp_parse_text_arg(p);
        }
        if (clen == 6 && !memcmp(cmd, "mathrm", 6)) return mp_parse_primary(p, MSTYLE_ROMAN);
        if (clen == 6 && !memcmp(cmd, "mathit", 6)) return mp_parse_primary(p, MSTYLE_ITALIC);
        if (clen == 6 && !memcmp(cmd, "mathbf", 6)) return mp_parse_primary(p, MSTYLE_BOLD);
        if ((clen == 10 && !memcmp(cmd, "boldsymbol", 10)) ||
            (clen == 2 && !memcmp(cmd, "bm", 2)) ||
            (clen == 6 && !memcmp(cmd, "mathbf", 6))) return mp_parse_primary(p, MSTYLE_BOLDITALIC);
        if (clen == 6 && !memcmp(cmd, "mathtt", 6)) return mp_parse_primary(p, MSTYLE_MONO);
        if (clen == 6 && !memcmp(cmd, "mathsf", 6)) return mp_parse_primary(p, MSTYLE_SANS);
        if ((clen == 6 && !memcmp(cmd, "mathbb", 6)) || (clen == 3 && !memcmp(cmd, "Bbb", 3))) return mp_parse_primary(p, MSTYLE_BB);
        if ((clen == 7 && !memcmp(cmd, "mathcal", 7)) || (clen == 7 && !memcmp(cmd, "mathscr", 7)) || (clen == 3 && !memcmp(cmd, "cal", 3))) return mp_parse_primary(p, MSTYLE_CAL);
        if ((clen == 8 && !memcmp(cmd, "mathfrak", 8)) || (clen == 4 && !memcmp(cmd, "frak", 4))) return mp_parse_primary(p, MSTYLE_FRAK);

        /* Named operators (\sin, \cos, \lim, \max, ...) */
        {
            const NamedOp *nop = lookup_named_op(cmd, clen);
            if (nop) {
                MNode *sym = mnode_new(p->arena, MN_SYM);
                sym->style = MSTYLE_ROMAN;
                sym->atom = MATOM_OP;
                sym->largeOp = nop->hasLimits ? 2 : 0;
                sym->text = nop->label;
                sym->textlen = (int)strlen(nop->label);
                return sym;
            }
        }

        /* Symbol table lookup (\alpha, \sum, \int, \leq, ...) */
        {
            const LatexSym *lsym = lookup_latex_sym(cmd, clen);
            if (lsym) {
                /* Special handling for \not followed by relation (=, \in, \subset, \equiv) */
                if (lsym->cp == 0x0338) {
                    MNode *next = mp_parse_primary(p, style);
                    if (next && next->type == MN_SYM) {
                        if (next->cp == '=') next->cp = 0x2260;
                        else if (next->cp == 0x2208) next->cp = 0x2209;
                        else if (next->cp == 0x2261) next->cp = 0x2262;
                        else if (next->cp == 0x2282) next->cp = 0x2284;
                        else if (next->cp == 0x2286) next->cp = 0x2288;
                        else if (next->cp == 0x223C) next->cp = 0x2241;
                        return next;
                    }
                }
                {
                    MNode *sym = mnode_new(p->arena, MN_SYM);
                    sym->style = style;
                    sym->cp = lsym->cp;
                    sym->atom = lsym->atom;
                    sym->largeOp = lsym->largeOp ? (lsym->defaultLimits ? 3 : 1) : 0;
                    return sym;
                }
            }
        }

        /* Unknown command: render command name in upright text as graceful fallback */
        {
            MNode *sym = mnode_new(p->arena, MN_SYM);
            sym->style = MSTYLE_ROMAN;
            sym->text = arena_strndup(p->arena, cmd, clen);
            sym->textlen = clen;
            return sym;
        }
    }

    /* Regular character */
    {
        uint32_t cp = utf8_next(p->s, p->len, &p->pos);
        MNode *sym = mnode_new(p->arena, MN_SYM);
        sym->style = style;
        if (cp == '-') cp = 0x2212; /* mathematical minus sign */
        sym->cp = cp;
        sym->atom = classify_ascii(cp);
        return sym;
    }
}

/* Parse an atom along with any trailing \limits, \nolimits, primes ', subscripts _, and superscripts ^ */
static MNode *mp_parse_atom(MParser *p, MathStyle style)
{
    MNode *base = mp_parse_primary(p, style);
    MNode *sub = NULL, *sup = NULL;
    int limits = 0;

    if (!base) return NULL;

    for (;;) {
        mp_skip_ws(p);
        if (p->pos >= p->len) break;

        /* Check for \limits or \nolimits */
        if (p->s[p->pos] == '\\') {
            int save = p->pos;
            int st = save + 1, clen;
            while (st < p->len && isalpha((unsigned char)p->s[st])) st++;
            clen = st - (save + 1);
            if (clen == 6 && !memcmp(p->s + save + 1, "limits", 6)) {
                limits = 1;
                p->pos = st;
                continue;
            }
            if (clen == 8 && !memcmp(p->s + save + 1, "nolimits", 8)) {
                limits = -1;
                p->pos = st;
                continue;
            }
        }

        /* Primes: f'(x), f''(x) */
        if (p->s[p->pos] == '\'') {
            int nprimes = 0;
            MNode *psym;
            while (p->pos < p->len && p->s[p->pos] == '\'') { nprimes++; p->pos++; }
            psym = mnode_new(p->arena, MN_SYM);
            psym->style = MSTYLE_AUTO;
            if (nprimes == 1) psym->cp = 0x2032;
            else if (nprimes == 2) psym->cp = 0x2033;
            else if (nprimes == 3) psym->cp = 0x2034;
            else psym->cp = 0x2032;
            if (!sup) {
                sup = psym;
            } else {
                MNode *r = mnode_new(p->arena, MN_ROW);
                mrow_add(p->arena, r, sup);
                mrow_add(p->arena, r, psym);
                sup = r;
            }
            continue;
        }

        if (p->s[p->pos] == '^') {
            MNode *part;
            p->pos++;
            part = mp_parse_primary(p, style);
            if (!sup) sup = part;
            else if (part) {
                MNode *r = mnode_new(p->arena, MN_ROW);
                mrow_add(p->arena, r, sup);
                mrow_add(p->arena, r, part);
                sup = r;
            }
            continue;
        }

        if (p->s[p->pos] == '_') {
            MNode *part;
            p->pos++;
            part = mp_parse_primary(p, style);
            if (!sub) sub = part;
            else if (part) {
                MNode *r = mnode_new(p->arena, MN_ROW);
                mrow_add(p->arena, r, sub);
                mrow_add(p->arena, r, part);
                sub = r;
            }
            continue;
        }

        break;
    }

    if (sub || sup || limits != 0) {
        MNode *sc = mnode_new(p->arena, MN_SCRIPT);
        sc->style = style;
        sc->atom = base->atom;
        sc->base = base;
        sc->sub = sub;
        sc->sup = sup;
        sc->limits = limits;
        return sc;
    }
    return base;
}

/* Parse a sequence of atoms until EOF, '}', ']', '\right', or infix \over / \choose / \atop */
static MNode *mp_parse_row_inner(MParser *p, MathStyle style, const char *stopCmd);

static MNode *mp_parse_row(MParser *p, MathStyle style, const char *stopCmd)
{
    MNode *res;
    if (p->depth >= MAX_MATH_DEPTH) {
        return mnode_new(p->arena, MN_ROW);
    }
    p->depth++;
    res = mp_parse_row_inner(p, style, stopCmd);
    p->depth--;
    return res;
}

static MNode *mp_parse_row_inner(MParser *p, MathStyle style, const char *stopCmd)
{
    MNode *row = mnode_new(p->arena, MN_ROW);
    int infixType = 0; /* 1 = \over, 2 = \choose, 3 = \atop */
    int infixSplit = -1;
    row->style = style;

    while (p->pos < p->len) {
        mp_skip_ws(p);
        if (p->pos >= p->len) break;

        if (stopCmd && stopCmd[0] != '\\' && p->s[p->pos] == stopCmd[0]) {
            p->pos++;
            break;
        }
        if (p->s[p->pos] == '}') {
            break;
        }

        /* Check for stopCmd like "\right" or infix commands "\over", "\choose", "\atop" */
        if (p->s[p->pos] == '\\') {
            int save = p->pos;
            int st = save + 1, clen;
            while (st < p->len && isalpha((unsigned char)p->s[st])) st++;
            clen = st - (save + 1);

            if (stopCmd && stopCmd[0] == '\\' &&
                (int)strlen(stopCmd + 1) == clen &&
                !memcmp(p->s + save + 1, stopCmd + 1, (size_t)clen)) {
                p->pos = st;
                break;
            }
            if (clen == 6 && !memcmp(p->s + save + 1, "middle", 6)) {
                uint32_t dcp;
                MNode *sym;
                p->pos = st;
                dcp = mp_parse_delim(p);
                sym = mnode_new(p->arena, MN_SYM);
                sym->style = style;
                sym->cp = dcp ? dcp : '|';
                sym->delimScale = 1.35f;
                sym->atom = MATOM_REL;
                mrow_add(p->arena, row, sym);
                continue;
            }
            if (clen == 4 && !memcmp(p->s + save + 1, "over", 4)) {
                p->pos = st;
                infixType = 1;
                infixSplit = row->nkids;
                continue;
            }
            if (clen == 6 && !memcmp(p->s + save + 1, "choose", 6)) {
                p->pos = st;
                infixType = 2;
                infixSplit = row->nkids;
                continue;
            }
            if (clen == 4 && !memcmp(p->s + save + 1, "atop", 4)) {
                p->pos = st;
                infixType = 3;
                infixSplit = row->nkids;
                continue;
            }
        }

        {
            MNode *atom = mp_parse_atom(p, style);
            if (atom) {
                mrow_add(p->arena, row, atom);
            } else if (p->pos < p->len) {
                p->pos++;
            }
        }
    }

    if (infixType > 0 && infixSplit >= 0) {
        MNode *num = mnode_new(p->arena, MN_ROW);
        MNode *den = mnode_new(p->arena, MN_ROW);
        MNode *fn  = mnode_new(p->arena, MN_FRAC);
        int i;
        for (i = 0; i < infixSplit; i++) mrow_add(p->arena, num, row->kids[i]);
        for (i = infixSplit; i < row->nkids; i++) mrow_add(p->arena, den, row->kids[i]);
        fn->style = style;
        fn->base = num;
        fn->sub = den;
        fn->hasBar = (infixType == 1);
        if (infixType == 2) { fn->leftDelim = '('; fn->rightDelim = ')'; }
        return fn;
    }

    return row;
}

/* ========================================================================= */
/* 3. 2D Box Layout Engine & Primitive List                                  */
/* ========================================================================= */

typedef enum MPrimType {
    MP_GLYPH = 0,
    MP_LINE
} MPrimType;

typedef struct MPrim {
    MPrimType     type;
    float         x, y;         /* relative to box left (x=0) and baseline (y=0) */
    float         w, h;         /* MP_LINE: vector dx, dy; MP_GLYPH: advance width, font size */
    float         lw;           /* MP_LINE: stroke width */
    uint32_t      color;
    Font         *font;         /* MP_GLYPH */
    uint16_t      gid;          /* MP_GLYPH */
    uint32_t      cp;           /* MP_GLYPH: Unicode code point for search/copy/PDF CMap */
    struct MPrim *next;
} MPrim;

static MathBox *mbox_new(Arena *a)
{
    MathBox *b = (MathBox *)arena_alloc(a, sizeof(MathBox));
    return b;
}

static void mbox_add_prim(MathBox *b, MPrim *pr)
{
    pr->next = NULL;
    if (!b->primHead) {
        b->primHead = b->primTail = pr;
    } else {
        ((MPrim *)b->primTail)->next = pr;
        b->primTail = pr;
    }
}

static void mbox_add_line(Arena *a, MathBox *b, float x, float y, float dx, float dy, float lw, uint32_t color)
{
    MPrim *pr = (MPrim *)arena_alloc(a, sizeof(MPrim));
    pr->type = MP_LINE;
    pr->x = x;
    pr->y = y;
    pr->w = dx;
    pr->h = dy;
    pr->lw = lw;
    pr->color = color;
    mbox_add_prim(b, pr);
}

/* Append child box primitives into parent box shifted by (dx, dy) */
static void mbox_merge(MathBox *dst, MathBox *src, float dx, float dy)
{
    MPrim *p;
    if (!src || !src->primHead) return;
    for (p = (MPrim *)src->primHead; p; p = p->next) {
        p->x += dx;
        p->y += dy;
    }
    if (!dst->primHead) {
        dst->primHead = src->primHead;
        dst->primTail = src->primTail;
    } else {
        ((MPrim *)dst->primTail)->next = (MPrim *)src->primHead;
        dst->primTail = src->primTail;
    }
    src->primHead = src->primTail = NULL;
}

/* Map ASCII letter/digit to Unicode Mathematical Alphanumeric Symbols block when available */
static uint32_t map_math_alphanumeric(uint32_t cp, MathStyle style)
{
    if (style == MSTYLE_BB) {
        if (cp == 'C') return 0x2102;
        if (cp == 'H') return 0x210D;
        if (cp == 'N') return 0x2115;
        if (cp == 'P') return 0x2119;
        if (cp == 'Q') return 0x211A;
        if (cp == 'R') return 0x211D;
        if (cp == 'Z') return 0x2124;
        if (cp >= 'A' && cp <= 'Z') return 0x1D538 + (cp - 'A');
        if (cp >= 'a' && cp <= 'z') return 0x1D552 + (cp - 'a');
        if (cp >= '0' && cp <= '9') return 0x1D7D8 + (cp - '0');
    } else if (style == MSTYLE_CAL) {
        if (cp == 'B') return 0x212C;
        if (cp == 'E') return 0x2130;
        if (cp == 'F') return 0x2131;
        if (cp == 'H') return 0x210B;
        if (cp == 'I') return 0x2110;
        if (cp == 'L') return 0x2112;
        if (cp == 'M') return 0x2133;
        if (cp == 'R') return 0x211B;
        if (cp >= 'A' && cp <= 'Z') return 0x1D49C + (cp - 'A');
        if (cp == 'e') return 0x212F;
        if (cp == 'g') return 0x210A;
        if (cp == 'o') return 0x2134;
        if (cp >= 'a' && cp <= 'z') return 0x1D4B6 + (cp - 'a');
    } else if (style == MSTYLE_FRAK) {
        if (cp == 'C') return 0x212D;
        if (cp == 'H') return 0x210C;
        if (cp == 'I') return 0x2111;
        if (cp == 'R') return 0x211C;
        if (cp == 'Z') return 0x2128;
        if (cp >= 'A' && cp <= 'Z') return 0x1D504 + (cp - 'A');
        if (cp >= 'a' && cp <= 'z') return 0x1D51E + (cp - 'a');
    }
    return cp;
}

static MathBox *mbox_from_glyph(Arena *a, uint32_t cp, MathStyle style, float sz, uint32_t color, int emitPrim)
{
    MathBox *b = mbox_new(a);
    int fam = FAM_MATH;
    int bold = 0, italic = 0;
    int gid = 0;
    uint32_t mapped;
    Font *f;
    float adv;

    if (style == MSTYLE_MONO) fam = FAM_MONO;
    else if (style == MSTYLE_SANS || style == MSTYLE_TEXT) fam = FAM_BODY;

    if (style == MSTYLE_BOLD || style == MSTYLE_BOLDITALIC) bold = 1;
    if (style == MSTYLE_ITALIC || style == MSTYLE_BOLDITALIC) {
        italic = 1;
    } else if (style == MSTYLE_AUTO) {
        /* Standard TeX math convention: Latin letters and lowercase Greek are italic */
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= 0x03B1 && cp <= 0x03C9) || cp == 0x03D1 || cp == 0x03D5 ||
            cp == 0x03D6 || cp == 0x03F0 || cp == 0x03F1 || cp == 0x03F5) {
            italic = 1;
        }
    }

    mapped = map_math_alphanumeric(cp, style);
    f = font_resolve(fam, bold, (mapped != cp) ? 0 : italic, mapped, &gid);
    if (gid == 0 && mapped != cp) {
        /* Fallback if font lacks SMP math alphanumerics */
        if (style == MSTYLE_BB) bold = 1;
        else if (style == MSTYLE_CAL) italic = 1;
        f = font_resolve(fam, bold, italic, cp, &gid);
        mapped = cp;
    }

    adv = (f && f->upem > 0) ? ((float)font_adv(f, gid) * sz / (float)f->upem) : (sz * 0.5f);
    if (italic && ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))) {
        adv += sz * 0.03f; /* slight italic overhang breathing room */
    }
    b->width = adv;
    b->ascent = sz * 0.78f;
    b->descent = sz * 0.24f;

    if (emitPrim && f) {
        MPrim *pr = (MPrim *)arena_alloc(a, sizeof(MPrim));
        pr->type = MP_GLYPH;
        pr->x = 0;
        pr->y = 0;
        pr->w = adv;
        pr->h = sz;
        pr->color = color;
        pr->font = f;
        pr->gid = (uint16_t)gid;
        pr->cp = mapped;
        mbox_add_prim(b, pr);
    }
    return b;
}

static MathBox *mbox_from_text(Arena *a, const char *text, int len, MathStyle style, float sz, uint32_t color, int emitPrim)
{
    MathBox *b = mbox_new(a);
    int i = 0;
    float x = 0;
    b->ascent = sz * 0.78f;
    b->descent = sz * 0.24f;
    while (i < len) {
        uint32_t cp = utf8_next(text, len, &i);
        MathBox *gb = mbox_from_glyph(a, cp, style, sz, color, emitPrim);
        mbox_merge(b, gb, x, 0);
        x += gb->width;
    }
    b->width = x;
    return b;
}

/* Draw a stretchy delimiter (or return empty box if cp == 0) of total height (topY .. botY)
 * where topY is negative (above baseline) and botY is positive (below baseline). */
static MathBox *mbox_stretchy_delim(Arena *a, uint32_t cp, float asc, float desc, float sz, uint32_t color, int emitPrim)
{
    MathBox *b = mbox_new(a);
    float totalH = asc + desc;
    float lw = sz * 0.065f;
    if (lw < 1.0f) lw = 1.0f;

    if (cp == 0) {
        b->width = 0;
        b->ascent = asc;
        b->descent = desc;
        return b;
    }

    /* If standard height suffices, use a single glyph (slightly scaled if needed) */
    if (totalH <= sz * 1.25f) {
        float gsz = (totalH > sz * 1.05f) ? totalH : sz;
        MathBox *gb = mbox_from_glyph(a, cp, MSTYLE_ROMAN, gsz, color, emitPrim);
        float shiftY = ((asc - desc) - (gb->ascent - gb->descent)) * -0.5f;
        if (fabsf(shiftY) > 0.5f) {
            MPrim *p;
            for (p = (MPrim *)gb->primHead; p; p = p->next) p->y += shiftY;
        }
        gb->ascent = asc;
        gb->descent = desc;
        return gb;
    }

    /* Vector stretchy delimiter for tall expressions (matrices, tall fractions, cases) */
    {
        float pad = sz * 0.12f;
        float w = sz * 0.42f;
        float topY = -asc;
        float botY = desc;
        float midY = (topY + botY) * 0.5f;
        float h = botY - topY;

        b->ascent = asc;
        b->descent = desc;
        b->width = w + pad * 2.0f;

        if (!emitPrim) return b;

        if (cp == '(') {
            float xL = pad + w * 0.15f;
            float xR = pad + w * 0.85f;
            float tipH = min(h * 0.22f, sz * 0.55f);
            mbox_add_line(a, b, xR, topY, xL - xR, tipH, lw, color);
            mbox_add_line(a, b, xL, topY + tipH, 0, h - tipH * 2.0f, lw, color);
            mbox_add_line(a, b, xL, botY - tipH, xR - xL, tipH, lw, color);
        } else if (cp == ')') {
            float xL = pad + w * 0.15f;
            float xR = pad + w * 0.85f;
            float tipH = min(h * 0.22f, sz * 0.55f);
            mbox_add_line(a, b, xL, topY, xR - xL, tipH, lw, color);
            mbox_add_line(a, b, xR, topY + tipH, 0, h - tipH * 2.0f, lw, color);
            mbox_add_line(a, b, xR, botY - tipH, xL - xR, tipH, lw, color);
        } else if (cp == '[' || cp == 0x2308 || cp == 0x230A) {
            float xL = pad + w * 0.20f;
            float xR = pad + w * 0.85f;
            mbox_add_line(a, b, xL, topY, 0, h, lw, color);
            if (cp != 0x230A) mbox_add_line(a, b, xL, topY, xR - xL, 0, lw, color);
            if (cp != 0x2308) mbox_add_line(a, b, xL, botY, xR - xL, 0, lw, color);
        } else if (cp == ']' || cp == 0x2309 || cp == 0x230B) {
            float xL = pad + w * 0.15f;
            float xR = pad + w * 0.80f;
            mbox_add_line(a, b, xR, topY, 0, h, lw, color);
            if (cp != 0x230B) mbox_add_line(a, b, xL, topY, xR - xL, 0, lw, color);
            if (cp != 0x2309) mbox_add_line(a, b, xL, botY, xR - xL, 0, lw, color);
        } else if (cp == '{') {
            float xL = pad + w * 0.10f;
            float xM = pad + w * 0.48f;
            float xR = pad + w * 0.88f;
            float tip = min(h * 0.14f, sz * 0.32f);
            mbox_add_line(a, b, xR, topY, xM - xR, tip, lw, color);
            mbox_add_line(a, b, xM, topY + tip, 0, (midY - tip) - (topY + tip), lw, color);
            mbox_add_line(a, b, xM, midY - tip, xL - xM, tip, lw, color);
            mbox_add_line(a, b, xL, midY, xM - xL, tip, lw, color);
            mbox_add_line(a, b, xM, midY + tip, 0, (botY - tip) - (midY + tip), lw, color);
            mbox_add_line(a, b, xM, botY - tip, xR - xM, tip, lw, color);
        } else if (cp == '}') {
            float xL = pad + w * 0.12f;
            float xM = pad + w * 0.52f;
            float xR = pad + w * 0.90f;
            float tip = min(h * 0.14f, sz * 0.32f);
            mbox_add_line(a, b, xL, topY, xM - xL, tip, lw, color);
            mbox_add_line(a, b, xM, topY + tip, 0, (midY - tip) - (topY + tip), lw, color);
            mbox_add_line(a, b, xM, midY - tip, xR - xM, tip, lw, color);
            mbox_add_line(a, b, xR, midY, xM - xR, tip, lw, color);
            mbox_add_line(a, b, xM, midY + tip, 0, (botY - tip) - (midY + tip), lw, color);
            mbox_add_line(a, b, xM, botY - tip, xL - xM, tip, lw, color);
        } else if (cp == '|') {
            float xM = pad + w * 0.5f;
            b->width = pad * 2.0f + w * 0.6f;
            mbox_add_line(a, b, xM * 0.75f, topY, 0, h, lw, color);
        } else if (cp == 0x2016) { /* \| */
            float x1 = pad + w * 0.3f;
            float x2 = pad + w * 0.7f;
            mbox_add_line(a, b, x1, topY, 0, h, lw, color);
            mbox_add_line(a, b, x2, topY, 0, h, lw, color);
        } else if (cp == 0x27E8) { /* \langle */
            float xL = pad + w * 0.15f;
            float xR = pad + w * 0.85f;
            mbox_add_line(a, b, xR, topY, xL - xR, midY - topY, lw, color);
            mbox_add_line(a, b, xL, midY, xR - xL, botY - midY, lw, color);
        } else if (cp == 0x27E9) { /* \rangle */
            float xL = pad + w * 0.15f;
            float xR = pad + w * 0.85f;
            mbox_add_line(a, b, xL, topY, xR - xL, midY - topY, lw, color);
            mbox_add_line(a, b, xR, midY, xL - xR, botY - midY, lw, color);
        } else {
            return mbox_from_glyph(a, cp, MSTYLE_ROMAN, sz * 1.4f, color, emitPrim);
        }
        return b;
    }
}

static int s_mathLayoutDepth = 0;
static MathBox *layout_mnode_inner(Arena *a, const MNode *n, float sz, int displayMode, uint32_t color, int emitPrim);

static MathBox *layout_mnode(Arena *a, const MNode *n, float sz, int displayMode, uint32_t color, int emitPrim)
{
    MathBox *res;
    if (!n || s_mathLayoutDepth >= MAX_MATH_DEPTH) {
        MathBox *empty = mbox_new(a);
        empty->ascent = sz * 0.5f;
        empty->descent = 0;
        return empty;
    }
    s_mathLayoutDepth++;
    res = layout_mnode_inner(a, n, sz, displayMode, color, emitPrim);
    s_mathLayoutDepth--;
    return res;
}

static float inter_atom_space(MathAtomClass left, MathAtomClass right, int displayMode)
{
    /* TeX spacing table (in ems) */
    if (left == MATOM_OPEN || right == MATOM_CLOSE) return 0.0f;
    if (left == MATOM_PUNCT) return 0.17f;
    if (left == MATOM_REL || right == MATOM_REL) return 0.26f;
    if (left == MATOM_BIN || right == MATOM_BIN) return displayMode >= 0 ? 0.20f : 0.0f;
    if (left == MATOM_OP && (right == MATOM_ORD || right == MATOM_OP)) return 0.16f;
    if ((left == MATOM_ORD || left == MATOM_CLOSE) && right == MATOM_OP) return 0.16f;
    return 0.0f;
}

static MathBox *layout_mnode_inner(Arena *a, const MNode *n, float sz, int displayMode, uint32_t color, int emitPrim)
{
    if (!n) {
        MathBox *empty = mbox_new(a);
        empty->ascent = sz * 0.5f;
        empty->descent = 0;
        return empty;
    }

    switch (n->type) {
    case MN_ROW: {
        MathBox *b = mbox_new(a);
        float x = 0;
        int i;
        MathAtomClass prevAtom = MATOM_OPEN; /* start of row acts like after OPEN (so leading - is unary) */
        b->ascent = sz * 0.72f;
        b->descent = sz * 0.20f;

        for (i = 0; i < n->nkids; i++) {
            const MNode *k = n->kids[i];
            MathAtomClass curAtom = k->atom;
            MathBox *kb;

            /* TeX rule: a BIN atom after BIN, OP, REL, OPEN, PUNCT, or at end of row becomes ORD (unary) */
            if (curAtom == MATOM_BIN) {
                if (prevAtom == MATOM_BIN || prevAtom == MATOM_OP || prevAtom == MATOM_REL ||
                    prevAtom == MATOM_OPEN || prevAtom == MATOM_PUNCT || i + 1 == n->nkids) {
                    curAtom = MATOM_ORD;
                }
            }

            if (i > 0 && k->type != MN_SPACE && n->kids[i - 1]->type != MN_SPACE) {
                float sp = inter_atom_space(prevAtom, curAtom, displayMode) * sz;
                x += sp;
            }

            kb = layout_mnode(a, k, sz, displayMode, color, emitPrim);
            mbox_merge(b, kb, x, 0);
            x += kb->width;
            if (kb->ascent > b->ascent) b->ascent = kb->ascent;
            if (kb->descent > b->descent) b->descent = kb->descent;
            if (k->type != MN_SPACE) prevAtom = curAtom;
        }
        b->width = x;
        return b;
    }

    case MN_SPACE: {
        MathBox *b = mbox_new(a);
        b->width = n->spaceEm * sz;
        if (b->width < -sz * 0.5f) b->width = -sz * 0.5f;
        b->ascent = sz * 0.5f;
        b->descent = 0;
        return b;
    }

    case MN_SYM: {
        float symSz = sz;
        if (n->delimScale > 1.0f) {
            float halfH = sz * 0.5f * n->delimScale;
            float axis = sz * 0.25f;
            return mbox_stretchy_delim(a, n->cp, axis + halfH, halfH - axis, sz, color, emitPrim);
        }
        if (n->largeOp == 1 || n->largeOp == 3) {
            symSz = displayMode ? (sz * 1.48f) : (sz * 1.15f);
        }
        if (n->text && n->textlen > 0) {
            return mbox_from_text(a, n->text, n->textlen, n->style, symSz, color, emitPrim);
        }
        {
            MathBox *gb = mbox_from_glyph(a, n->cp, n->style, symSz, color, emitPrim);
            if (n->largeOp == 1 || n->largeOp == 3) {
                /* Center large symbol (\sum, \int, \prod) vertically on math axis */
                float shiftY = (symSz - sz) * 0.24f;
                MPrim *p;
                for (p = (MPrim *)gb->primHead; p; p = p->next) p->y += shiftY;
                gb->ascent = symSz * 0.75f - shiftY;
                gb->descent = symSz * 0.28f + shiftY;
            }
            return gb;
        }
    }

    case MN_SCRIPT: {
        MathBox *baseBox = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        float scSz = max(sz * 0.70f, 8.0f);
        MathBox *subBox = n->sub ? layout_mnode(a, n->sub, scSz, 0, color, emitPrim) : NULL;
        MathBox *supBox = n->sup ? layout_mnode(a, n->sup, scSz, 0, color, emitPrim) : NULL;
        int useLimits = 0;

        if (n->limits == 1) useLimits = 1;
        else if (n->limits == -1) useLimits = 0;
        else if (displayMode && n->base && (n->base->largeOp == 2 || n->base->largeOp == 3)) useLimits = 1;

        if (useLimits) {
            /* Limits centered above and below base (\sum_{i=1}^n, \lim_{x \to 0}) */
            MathBox *b = mbox_new(a);
            float maxW = baseBox->width;
            float gap = sz * 0.10f;
            if (subBox && subBox->width > maxW) maxW = subBox->width;
            if (supBox && supBox->width > maxW) maxW = supBox->width;

            mbox_merge(b, baseBox, (maxW - baseBox->width) * 0.5f, 0);
            b->ascent = baseBox->ascent;
            b->descent = baseBox->descent;

            if (supBox) {
                float supY = -(baseBox->ascent + gap + supBox->descent);
                mbox_merge(b, supBox, (maxW - supBox->width) * 0.5f, supY);
                b->ascent = -supY + supBox->ascent;
            }
            if (subBox) {
                float subY = baseBox->descent + gap + subBox->ascent;
                mbox_merge(b, subBox, (maxW - subBox->width) * 0.5f, subY);
                b->descent = subY + subBox->descent;
            }
            b->width = maxW + sz * 0.08f;
            return b;
        } else {
            /* Standard right-side superscript / subscript */
            MathBox *b = mbox_new(a);
            float scriptX = baseBox->width + sz * 0.03f;
            float subX = scriptX;
            float supY = 0, subY = 0;
            float rightW = 0;

            /* Tuck integral subscript slightly left */
            if (n->base && n->base->type == MN_SYM && n->base->largeOp == 1) {
                subX -= sz * 0.10f;
                if (subX < baseBox->width * 0.65f) subX = baseBox->width * 0.65f;
            }

            mbox_merge(b, baseBox, 0, 0);
            b->ascent = baseBox->ascent;
            b->descent = baseBox->descent;

            if (supBox) {
                supY = -max(baseBox->ascent * 0.52f, sz * 0.38f);
                if (-supY - supBox->descent < sz * 0.15f) {
                    supY = -(supBox->descent + sz * 0.15f);
                }
            }
            if (subBox) {
                subY = max(baseBox->descent * 0.55f, sz * 0.22f);
                if (subY - subBox->ascent < -sz * 0.12f) {
                    subY = subBox->ascent - sz * 0.12f;
                }
            }
            /* Prevent simultaneous sup and sub from colliding vertically */
            if (supBox && subBox) {
                float clearance = (subY - subBox->ascent) - (supY + supBox->descent);
                float minClear = sz * 0.14f;
                if (clearance < minClear) {
                    float push = (minClear - clearance) * 0.5f;
                    supY -= push;
                    subY += push;
                }
            }

            if (supBox) {
                mbox_merge(b, supBox, scriptX, supY);
                if (-supY + supBox->ascent > b->ascent) b->ascent = -supY + supBox->ascent;
                if (scriptX + supBox->width > rightW) rightW = scriptX + supBox->width;
            }
            if (subBox) {
                mbox_merge(b, subBox, subX, subY);
                if (subY + subBox->descent > b->descent) b->descent = subY + subBox->descent;
                if (subX + subBox->width > rightW) rightW = subX + subBox->width;
            }
            b->width = max(baseBox->width, rightW) + sz * 0.04f;
            return b;
        }
    }

    case MN_FRAC: {
        int subDisp = (n->fracMode == 1) ? 1 : (n->fracMode == 2 ? 0 : 0);
        float numSz = (n->fracMode == 1) ? sz : (displayMode ? sz * 0.88f : sz * 0.76f);
        MathBox *numBox = layout_mnode(a, n->base, numSz, subDisp, color, emitPrim);
        MathBox *denBox = layout_mnode(a, n->sub,  numSz, subDisp, color, emitPrim);
        MathBox *inner  = mbox_new(a);
        float axis = sz * 0.26f;
        float padX = sz * 0.14f;
        float gap  = displayMode ? (sz * 0.18f) : (sz * 0.11f);
        float lw   = max(sz * 0.06f, 1.0f);
        float barW = max(numBox->width, denBox->width) + padX * 2.0f;
        float numY = -(axis + gap + numBox->descent);
        float denY = (-axis) + gap + denBox->ascent;

        mbox_merge(inner, numBox, (barW - numBox->width) * 0.5f, numY);
        mbox_merge(inner, denBox, (barW - denBox->width) * 0.5f, denY);
        if (n->hasBar && emitPrim) {
            mbox_add_line(a, inner, sz * 0.04f, -axis, barW - sz * 0.08f, 0, lw, color);
        }
        inner->width = barW;
        inner->ascent = -numY + numBox->ascent;
        inner->descent = denY + denBox->descent;

        if (n->leftDelim || n->rightDelim) {
            MathBox *wrap = mbox_new(a);
            MathBox *ld = mbox_stretchy_delim(a, n->leftDelim, inner->ascent, inner->descent, sz, color, emitPrim);
            MathBox *rd = mbox_stretchy_delim(a, n->rightDelim, inner->ascent, inner->descent, sz, color, emitPrim);
            float x = 0;
            mbox_merge(wrap, ld, x, 0); x += ld->width;
            mbox_merge(wrap, inner, x, 0); x += inner->width;
            mbox_merge(wrap, rd, x, 0); x += rd->width;
            wrap->width = x;
            wrap->ascent = inner->ascent;
            wrap->descent = inner->descent;
            return wrap;
        }
        return inner;
    }

    case MN_SQRT: {
        MathBox *radBox = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        MathBox *degBox = n->sup ? layout_mnode(a, n->sup, max(sz * 0.60f, 7.5f), 0, color, emitPrim) : NULL;
        MathBox *b = mbox_new(a);
        float lw = max(sz * 0.06f, 1.0f);
        float gapTop = sz * 0.14f;
        float padR = sz * 0.10f;
        float topY = -(radBox->ascent + gapTop);
        float botY = max(radBox->descent, sz * 0.15f);
        float surdH = botY - topY;
        float surdW = max(sz * 0.55f, surdH * 0.36f);
        float x0 = 0;

        if (degBox) {
            float degX = sz * 0.05f;
            float degY = topY + surdH * 0.42f;
            float overlap = surdW * 0.52f;
            if (degX + degBox->width > overlap) {
                x0 = (degX + degBox->width) - overlap;
            }
            mbox_merge(b, degBox, degX, degY);
        }

        if (emitPrim) {
            /* Vector radical symbol (tick + down-stroke + up-stroke + vinculum bar) */
            float tickX = x0 + surdW * 0.08f;
            float tickY = topY + surdH * 0.60f;
            float kneeX = x0 + surdW * 0.24f;
            float kneeY = topY + surdH * 0.50f;
            float veeX  = x0 + surdW * 0.52f;
            float veeY  = botY;
            float apexX = x0 + surdW;
            float apexY = topY;
            mbox_add_line(a, b, tickX, tickY, kneeX - tickX, kneeY - tickY, lw, color);
            mbox_add_line(a, b, kneeX, kneeY, veeX - kneeX, veeY - kneeY, lw * 1.25f, color);
            mbox_add_line(a, b, veeX,  veeY,  apexX - veeX, apexY - veeY, lw, color);
            mbox_add_line(a, b, apexX, apexY, radBox->width + padR, 0, lw, color);
        }

        mbox_merge(b, radBox, x0 + surdW + sz * 0.04f, 0);
        b->width = x0 + surdW + radBox->width + padR + sz * 0.04f;
        b->ascent = -topY + lw;
        b->descent = botY;
        return b;
    }

    case MN_DELIM: {
        MathBox *inner = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        MathBox *b = mbox_new(a);
        float axis = sz * 0.25f;
        float halfSpan = max(inner->ascent - axis, inner->descent + axis);
        float asc = axis + halfSpan;
        float desc = halfSpan - axis;
        MathBox *ld = mbox_stretchy_delim(a, n->leftDelim, asc, desc, sz, color, emitPrim);
        MathBox *rd = mbox_stretchy_delim(a, n->rightDelim, asc, desc, sz, color, emitPrim);
        float x = 0;

        mbox_merge(b, ld, x, 0); x += ld->width;
        mbox_merge(b, inner, x, 0); x += inner->width;
        mbox_merge(b, rd, x, 0); x += rd->width;
        b->width = x;
        b->ascent = asc;
        b->descent = desc;
        return b;
    }

    case MN_ACCENT: {
        MathBox *baseBox = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        MathBox *b = mbox_new(a);
        float lw = max(sz * 0.06f, 1.0f);
        float w = baseBox->width;
        float topY = -baseBox->ascent;
        float botY = baseBox->descent;

        mbox_merge(b, baseBox, 0, 0);
        b->width = w;
        b->ascent = baseBox->ascent;
        b->descent = baseBox->descent;

        switch (n->accent) {
        case MACC_OVERLINE:
        case MACC_BAR: {
            float barY = topY - sz * 0.12f;
            float xPad = (n->accent == MACC_BAR) ? w * 0.12f : 0.0f;
            if (emitPrim) mbox_add_line(a, b, xPad, barY, w - xPad * 2.0f, 0, lw, color);
            b->ascent = -barY + lw;
            break;
        }
        case MACC_UNDERLINE: {
            float barY = botY + sz * 0.12f;
            if (emitPrim) mbox_add_line(a, b, 0, barY, w, 0, lw, color);
            b->descent = barY + lw;
            break;
        }
        case MACC_HAT:
        case MACC_WIDEHAT: {
            float span = (n->accent == MACC_WIDEHAT) ? max(w * 0.9f, sz * 0.45f) : min(w * 0.85f, sz * 0.45f);
            float h = sz * 0.16f;
            float cx = w * 0.5f;
            float ay = topY - sz * 0.08f;
            if (emitPrim) {
                mbox_add_line(a, b, cx - span * 0.5f, ay, span * 0.5f, -h, lw, color);
                mbox_add_line(a, b, cx, ay - h, span * 0.5f, h, lw, color);
            }
            b->ascent = -(ay - h) + lw;
            break;
        }
        case MACC_CHECK: {
            float span = min(w * 0.85f, sz * 0.45f);
            float h = sz * 0.15f;
            float cx = w * 0.5f;
            float ay = topY - sz * 0.22f;
            if (emitPrim) {
                mbox_add_line(a, b, cx - span * 0.5f, ay, span * 0.5f, h, lw, color);
                mbox_add_line(a, b, cx, ay + h, span * 0.5f, -h, lw, color);
            }
            b->ascent = -ay + lw;
            break;
        }
        case MACC_TILDE:
        case MACC_WIDETILDE: {
            float span = (n->accent == MACC_WIDETILDE) ? max(w * 0.9f, sz * 0.48f) : min(w * 0.85f, sz * 0.48f);
            float h = sz * 0.10f;
            float x0 = (w - span) * 0.5f;
            float ay = topY - sz * 0.14f;
            if (emitPrim) {
                mbox_add_line(a, b, x0, ay, span * 0.35f, -h, lw, color);
                mbox_add_line(a, b, x0 + span * 0.35f, ay - h, span * 0.30f, h, lw, color);
                mbox_add_line(a, b, x0 + span * 0.65f, ay, span * 0.35f, -h, lw, color);
            }
            b->ascent = -(ay - h) + lw;
            break;
        }
        case MACC_DOT:
        case MACC_DDOT:
        case MACC_DDDOT: {
            int ndots = (n->accent == MACC_DDDOT) ? 3 : (n->accent == MACC_DDOT ? 2 : 1);
            float ay = topY - sz * 0.08f;
            int d;
            for (d = 0; d < ndots; d++) {
                float dx = w * 0.5f + ((float)d - (float)(ndots - 1) * 0.5f) * (sz * 0.20f);
                if (emitPrim) mbox_add_line(a, b, dx - lw, ay, lw * 1.5f, 0, lw * 1.5f, color);
            }
            b->ascent = -ay + sz * 0.10f;
            break;
        }
        case MACC_VEC:
        case MACC_OVERRIGHTARROW:
        case MACC_OVERLEFTARROW:
        case MACC_OVERLEFTRIGHTARROW: {
            float span = (n->accent == MACC_VEC) ? max(w * 0.85f, sz * 0.42f) : max(w, sz * 0.55f);
            float x0 = (w - span) * 0.5f;
            float ay = topY - sz * 0.16f;
            float tip = sz * 0.14f;
            if (emitPrim) {
                mbox_add_line(a, b, x0, ay, span, 0, lw, color);
                if (n->accent != MACC_OVERLEFTARROW) {
                    mbox_add_line(a, b, x0 + span - tip, ay - tip * 0.7f, tip, tip * 0.7f, lw, color);
                    mbox_add_line(a, b, x0 + span - tip, ay + tip * 0.7f, tip, -tip * 0.7f, lw, color);
                }
                if (n->accent == MACC_OVERLEFTARROW || n->accent == MACC_OVERLEFTRIGHTARROW) {
                    mbox_add_line(a, b, x0 + tip, ay - tip * 0.7f, -tip, tip * 0.7f, lw, color);
                    mbox_add_line(a, b, x0 + tip, ay + tip * 0.7f, -tip, -tip * 0.7f, lw, color);
                }
            }
            b->ascent = -(ay - tip) + lw;
            break;
        }
        case MACC_OVERBRACE: {
            float ay = topY - sz * 0.10f;
            float tip = sz * 0.12f;
            float cx = w * 0.5f;
            if (emitPrim) {
                mbox_add_line(a, b, 0, ay, tip, -tip, lw, color);
                mbox_add_line(a, b, tip, ay - tip, cx - tip * 2.0f, 0, lw, color);
                mbox_add_line(a, b, cx - tip, ay - tip, tip, -tip, lw, color);
                mbox_add_line(a, b, cx, ay - tip * 2.0f, tip, tip, lw, color);
                mbox_add_line(a, b, cx + tip, ay - tip, (w - tip) - (cx + tip), 0, lw, color);
                mbox_add_line(a, b, w - tip, ay - tip, tip, tip, lw, color);
            }
            b->ascent = -(ay - tip * 2.0f) + lw;
            break;
        }
        case MACC_UNDERBRACE: {
            float ay = botY + sz * 0.10f;
            float tip = sz * 0.12f;
            float cx = w * 0.5f;
            if (emitPrim) {
                mbox_add_line(a, b, 0, ay, tip, tip, lw, color);
                mbox_add_line(a, b, tip, ay + tip, cx - tip * 2.0f, 0, lw, color);
                mbox_add_line(a, b, cx - tip, ay + tip, tip, tip, lw, color);
                mbox_add_line(a, b, cx, ay + tip * 2.0f, tip, -tip, lw, color);
                mbox_add_line(a, b, cx + tip, ay + tip, (w - tip) - (cx + tip), 0, lw, color);
                mbox_add_line(a, b, w - tip, ay + tip, tip, -tip, lw, color);
            }
            b->descent = ay + tip * 2.0f + lw;
            break;
        }
        default:
            break;
        }
        return b;
    }

    case MN_STACK: {
        MathBox *baseBox = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        float scSz = max(sz * 0.70f, 8.0f);
        MathBox *supBox = n->sup ? layout_mnode(a, n->sup, scSz, 0, color, emitPrim) : NULL;
        MathBox *subBox = n->sub ? layout_mnode(a, n->sub, scSz, 0, color, emitPrim) : NULL;
        MathBox *b = mbox_new(a);
        float maxW = baseBox->width;
        float gap = sz * 0.10f;
        if (supBox && supBox->width > maxW) maxW = supBox->width;
        if (subBox && subBox->width > maxW) maxW = subBox->width;

        mbox_merge(b, baseBox, (maxW - baseBox->width) * 0.5f, 0);
        b->ascent = baseBox->ascent;
        b->descent = baseBox->descent;
        if (supBox) {
            float supY = -(baseBox->ascent + gap + supBox->descent);
            mbox_merge(b, supBox, (maxW - supBox->width) * 0.5f, supY);
            b->ascent = -supY + supBox->ascent;
        }
        if (subBox) {
            float subY = baseBox->descent + gap + subBox->ascent;
            mbox_merge(b, subBox, (maxW - subBox->width) * 0.5f, subY);
            b->descent = subY + subBox->descent;
        }
        b->width = maxW;
        return b;
    }

    case MN_BOXED: {
        MathBox *inner = layout_mnode(a, n->base, sz, displayMode, color, emitPrim);
        MathBox *b = mbox_new(a);
        float pad = sz * 0.22f;
        float lw = max(sz * 0.06f, 1.0f);
        float bw = inner->width + pad * 2.0f;
        float topY = -(inner->ascent + pad);
        float botY = inner->descent + pad;
        float bh = botY - topY;

        mbox_merge(b, inner, pad, 0);
        if (emitPrim) {
            mbox_add_line(a, b, 0, topY, bw, 0, lw, color);
            mbox_add_line(a, b, 0, botY, bw, 0, lw, color);
            mbox_add_line(a, b, 0, topY, 0, bh, lw, color);
            mbox_add_line(a, b, bw, topY, 0, bh, lw, color);
        }
        b->width = bw;
        b->ascent = -topY;
        b->descent = botY;
        return b;
    }

    case MN_PHANTOM: {
        MathBox *inner = layout_mnode(a, n->base, sz, displayMode, color, 0);
        MathBox *b = mbox_new(a);
        b->width   = (n->phantomMode == 2) ? 0.0f : inner->width;
        b->ascent  = (n->phantomMode == 1) ? 0.0f : inner->ascent;
        b->descent = (n->phantomMode == 1) ? 0.0f : inner->descent;
        return b;
    }

    case MN_COLOR: {
        uint32_t c = n->color ? n->color : color;
        return layout_mnode(a, n->base, sz, displayMode, c, emitPrim);
    }

    case MN_MATRIX: {
        int rows = n->rows > 0 ? n->rows : 1;
        int cols = n->cols > 0 ? n->cols : 1;
        float cellSz = (n->envKind == 3) ? (sz * 0.75f) : sz;
        int cellDisp = (n->envKind == 2) ? displayMode : 0;
        MathBox **cboxes;
        float *colW, *rowAsc, *rowDesc, *rowY;
        float colGap = (n->envKind == 3) ? (sz * 0.35f) : (n->envKind == 2 ? sz * 0.15f : sz * 0.65f);
        float rowGap = (n->envKind == 3) ? (sz * 0.20f) : (sz * 0.35f);
        float totalW = 0, totalH = 0, curY = 0, axis = sz * 0.25f, shiftY;
        int r, c;
        MathBox *grid = mbox_new(a);
        if (rows > MAX_MATRIX_ROWS) rows = MAX_MATRIX_ROWS;
        if (cols > MAX_MATRIX_COLS) cols = MAX_MATRIX_COLS;
        if ((size_t)rows > MAX_MATRIX_CELLS / (size_t)cols) return grid;
        cboxes = (MathBox **)arena_alloc(a, (size_t)rows * (size_t)cols * sizeof(MathBox *));
        colW = (float *)arena_alloc(a, (size_t)cols * sizeof(float));
        rowAsc = (float *)arena_alloc(a, (size_t)rows * sizeof(float));
        rowDesc = (float *)arena_alloc(a, (size_t)rows * sizeof(float));
        rowY = (float *)arena_alloc(a, (size_t)rows * sizeof(float));

        for (r = 0; r < rows; r++) {
            rowAsc[r] = cellSz * 0.72f;
            rowDesc[r] = cellSz * 0.22f;
            for (c = 0; c < cols; c++) {
                const MNode *cell = n->cells ? n->cells[r * cols + c] : NULL;
                MathBox *cb = layout_mnode(a, cell, cellSz, cellDisp, color, emitPrim);
                cboxes[r * cols + c] = cb;
                if (cb->width > colW[c]) colW[c] = cb->width;
                if (cb->ascent > rowAsc[r]) rowAsc[r] = cb->ascent;
                if (cb->descent > rowDesc[r]) rowDesc[r] = cb->descent;
            }
            totalH += rowAsc[r] + rowDesc[r];
            if (r > 0) totalH += rowGap;
        }

        for (c = 0; c < cols; c++) {
            totalW += colW[c];
            if (c > 0) {
                /* In aligned/align environment, even->odd column gap is 0 (around &=), odd->even is quad */
                if (n->envKind == 2) totalW += (c % 2 == 1) ? 0.0f : sz * 0.85f;
                else totalW += colGap;
            }
        }

        /* Center the grid vertically around the math axis */
        shiftY = -totalH * 0.5f - axis;
        curY = shiftY;
        for (r = 0; r < rows; r++) {
            float baselineY = curY + rowAsc[r];
            float curX = 0;
            rowY[r] = baselineY;
            for (c = 0; c < cols; c++) {
                MathBox *cb = cboxes[r * cols + c];
                float cx = curX;
                if (n->envKind == 1) {
                    /* cases: left-aligned */
                    cx = curX;
                } else if (n->envKind == 2) {
                    /* aligned/align: right-align even cols, left-align odd cols */
                    if (c % 2 == 0) cx = curX + (colW[c] - cb->width);
                } else {
                    /* matrix: centered */
                    cx = curX + (colW[c] - cb->width) * 0.5f;
                }
                mbox_merge(grid, cb, cx, baselineY);
                curX += colW[c];
                if (c + 1 < cols) {
                    if (n->envKind == 2) curX += (c % 2 == 0) ? 0.0f : sz * 0.85f;
                    else curX += colGap;
                }
            }
            curY += rowAsc[r] + rowDesc[r] + rowGap;
        }

        grid->width = totalW;
        grid->ascent = -shiftY;
        grid->descent = shiftY + totalH;

        if (n->leftDelim || n->rightDelim) {
            MathBox *wrap = mbox_new(a);
            MathBox *ld = mbox_stretchy_delim(a, n->leftDelim, grid->ascent, grid->descent, sz, color, emitPrim);
            MathBox *rd = mbox_stretchy_delim(a, n->rightDelim, grid->ascent, grid->descent, sz, color, emitPrim);
            float x = 0;
            mbox_merge(wrap, ld, x, 0); x += ld->width;
            if (n->leftDelim) x += sz * 0.08f;
            mbox_merge(wrap, grid, x, 0); x += grid->width;
            if (n->rightDelim) x += sz * 0.08f;
            mbox_merge(wrap, rd, x, 0); x += rd->width;
            wrap->width = x;
            wrap->ascent = grid->ascent;
            wrap->descent = grid->descent;
            return wrap;
        }
        return grid;
    }
    }

    return mbox_new(a);
}

MathBox *math_layout(Arena *arena, const char *latex, int len,
                     float baseSize, int displayMode, uint32_t color)
{
    MParser p;
    MNode *root;
    if (!latex) latex = "";
    if (len < 0) len = (int)strlen(latex);
    p.arena = arena;
    p.s = latex;
    p.len = len;
    p.pos = 0;
    p.depth = 0;
    root = mp_parse_row(&p, MSTYLE_AUTO, NULL);
    return layout_mnode(arena, root, baseSize, displayMode, color, 1);
}

/* ========================================================================= */
/* 4. Display List Emitter (into Layout)                                     */
/* ========================================================================= */

static Item *layout_push_item(Layout *L, int type, int layer, int page)
{
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
    it->page = page;
    return it;
}

static int layout_append_cp(Layout *L, uint32_t cp)
{
    char u8[4];
    int n = utf8_encode(cp, u8);
    int off = L->textlen;
    if (L->textlen + n + 2 > L->textcap) {
        int nc = L->textcap ? L->textcap * 2 : 512;
        char *nt;
        while (nc < L->textlen + n + 2) {
            if (nc > INT_MAX / 2) { nc = L->textlen + n + 2; break; }
            nc *= 2;
        }
        nt = (char *)realloc(L->text, (size_t)nc);
        if (!nt) return off;
        L->text = nt;
        L->textcap = nc;
    }
    memcpy(L->text + L->textlen, u8, (size_t)n);
    L->textlen += n;
    L->text[L->textlen] = 0;
    return off;
}

void math_emit(const MathBox *box, Layout *L, int page, float x, float baselineY, int link)
{
    const MPrim *pr;
    if (!box || !L) return;

    for (pr = (const MPrim *)box->primHead; pr; ) {
        if (pr->type == MP_LINE) {
            Item *it = layout_push_item(L, IT_LINE, 1, page);
            it->x = x + pr->x;
            it->y = baselineY + pr->y;
            it->w = pr->w;
            it->h = pr->h;
            it->lw = pr->lw;
            it->color = pr->color;
            pr = pr->next;
            continue;
        }

        /* Coalesce consecutive MP_GLYPH primitives on the same baseline y, font, size, color, and contiguous x */
        if (pr->type == MP_GLYPH) {
            const MPrim *prev = pr;
            const MPrim *end = pr->next;
            int count = 1, k = 0;
            const MPrim *cur;
            Item *it;
            float baseX = x + pr->x;
            float baseY = baselineY + pr->y;

            while (end && end->type == MP_GLYPH &&
                   end->font == pr->font &&
                   fabsf(end->y - pr->y) < 0.05f &&
                   fabsf(end->h - pr->h) < 0.05f &&
                   fabsf((prev->x + prev->w) - end->x) < 0.1f &&
                   end->color == pr->color) {
                count++;
                prev = end;
                end = end->next;
            }

            it = layout_push_item(L, IT_GLYPHS, 1, page);
            it->x = baseX;
            it->y = baseY;
            it->size = pr->h;
            it->h = pr->h * 1.25f;
            it->font = pr->font;
            it->color = pr->color;
            it->link = link;
            it->n = count;
            it->gids = (uint16_t *)arena_alloc(&L->arena, (size_t)count * sizeof(uint16_t));
            it->dx   = (float *)arena_alloc(&L->arena, (size_t)count * sizeof(float));
            it->toff = (int *)arena_alloc(&L->arena, (size_t)count * sizeof(int));

            for (cur = pr; cur != end; cur = cur->next, k++) {
                it->gids[k] = cur->gid;
                it->dx[k]   = (x + cur->x) - baseX;
                it->toff[k] = layout_append_cp(L, cur->cp);
                if (k + 1 == count) {
                    it->w = it->dx[k] + cur->w;
                }
            }
            it->tend = L->textlen;
            pr = end;
            continue;
        }

        pr = pr->next;
    }

    if (link >= 0 && box->width > 0) {
        LinkArea *la;
        if (L->nlinks == L->caplinks) {
            int nc = L->caplinks ? L->caplinks * 2 : 32;
            LinkArea *na = (LinkArea *)arena_alloc(&L->arena, (size_t)nc * sizeof(LinkArea));
            if (L->nlinks) memcpy(na, L->links, (size_t)L->nlinks * sizeof(LinkArea));
            L->links = na;
            L->caplinks = nc;
        }
        la = &L->links[L->nlinks++];
        la->page = page;
        la->x = x;
        la->y = baselineY - box->ascent;
        la->w = box->width;
        la->h = box->ascent + box->descent;
        la->link = link;
    }
}

/* ========================================================================= */
/* 5. DOCX Office Math ML (OMML) Exporter                                    */
/* ========================================================================= */

static int s_ommlDepth = 0;
static void omml_emit_node_inner(Buf *xml, const MNode *n);

static void omml_emit_node(Buf *xml, const MNode *n)
{
    if (!n || s_ommlDepth >= MAX_MATH_DEPTH) return;
    s_ommlDepth++;
    omml_emit_node_inner(xml, n);
    s_ommlDepth--;
}

static void omml_put_delim_char(Buf *xml, const char *tag, uint32_t cp)
{
    char u8[5];
    int len;
    if (cp == 0) {
        buf_printf(xml, "<m:%s m:val=\"\"/>", tag);
        return;
    }
    len = utf8_encode(cp, u8);
    u8[len] = 0;
    buf_printf(xml, "<m:%s m:val=\"", tag);
    buf_put_xml(xml, u8, len);
    buf_puts(xml, "\"/>");
}

static void omml_emit_node_inner(Buf *xml, const MNode *n)
{
    if (!n) return;

    switch (n->type) {
    case MN_ROW: {
        int i;
        for (i = 0; i < n->nkids; i++) {
            const MNode *k = n->kids[i];
            /* If a large operator with limits is followed by elements in the same row,
             * emit MN_SCRIPT as <m:nary> and put the next atom inside <m:e> */
            if (k->type == MN_SCRIPT && k->base && k->base->type == MN_SYM &&
                (k->base->largeOp == 1 || k->base->largeOp == 3)) {
                char u8[5];
                int ulen = utf8_encode(k->base->cp, u8);
                u8[ulen] = 0;
                buf_puts(xml, "<m:nary><m:naryPr><m:chr m:val=\"");
                buf_put_xml(xml, u8, ulen);
                buf_printf(xml, "\"/><m:limLoc m:val=\"%s\"/>",
                           (k->limits == 1 || (k->limits == 0 && k->base->largeOp == 3)) ? "undOvr" : "subSup");
                if (!k->sub) buf_puts(xml, "<m:subHide m:val=\"1\"/>");
                if (!k->sup) buf_puts(xml, "<m:supHide m:val=\"1\"/>");
                buf_puts(xml, "</m:naryPr><m:sub>");
                if (k->sub) omml_emit_node(xml, k->sub);
                buf_puts(xml, "</m:sub><m:sup>");
                if (k->sup) omml_emit_node(xml, k->sup);
                buf_puts(xml, "</m:sup><m:e>");
                if (i + 1 < n->nkids) {
                    omml_emit_node(xml, n->kids[++i]);
                }
                buf_puts(xml, "</m:e></m:nary>");
                continue;
            }
            omml_emit_node(xml, k);
        }
        break;
    }

    case MN_SPACE:
        if (n->spaceEm > 0.05f) {
            buf_puts(xml, "<m:r><m:t xml:space=\"preserve\"> </m:t></m:r>");
        }
        break;

    case MN_SYM: {
        int upright = (n->style == MSTYLE_ROMAN || n->style == MSTYLE_TEXT || n->largeOp == 2);
        buf_puts(xml, "<m:r>");
        if (upright) {
            buf_puts(xml, "<m:rPr><m:sty m:val=\"p\"/></m:rPr>");
        } else if (n->style == MSTYLE_BOLD) {
            buf_puts(xml, "<m:rPr><m:sty m:val=\"b\"/></m:rPr>");
        } else if (n->style == MSTYLE_BOLDITALIC) {
            buf_puts(xml, "<m:rPr><m:sty m:val=\"bi\"/></m:rPr>");
        }
        buf_puts(xml, "<m:t xml:space=\"preserve\">");
        if (n->text && n->textlen > 0) {
            buf_put_xml(xml, n->text, n->textlen);
        } else if (n->cp) {
            uint32_t mapped = map_math_alphanumeric(n->cp, n->style);
            char u8[4];
            int ulen = utf8_encode(mapped, u8);
            buf_put_xml(xml, u8, ulen);
        }
        buf_puts(xml, "</m:t></m:r>");
        break;
    }

    case MN_SCRIPT: {
        int isLimOp = (n->limits == 1) || (n->limits == 0 && n->base && (n->base->largeOp == 2 || n->base->largeOp == 3));
        if (isLimOp) {
            if (n->sub && n->sup) {
                buf_puts(xml, "<m:limUpp><m:e><m:limLow><m:e>");
                omml_emit_node(xml, n->base);
                buf_puts(xml, "</m:e><m:lim>");
                omml_emit_node(xml, n->sub);
                buf_puts(xml, "</m:lim></m:limLow></m:e><m:lim>");
                omml_emit_node(xml, n->sup);
                buf_puts(xml, "</m:lim></m:limUpp>");
            } else if (n->sub) {
                buf_puts(xml, "<m:limLow><m:e>");
                omml_emit_node(xml, n->base);
                buf_puts(xml, "</m:e><m:lim>");
                omml_emit_node(xml, n->sub);
                buf_puts(xml, "</m:lim></m:limLow>");
            } else if (n->sup) {
                buf_puts(xml, "<m:limUpp><m:e>");
                omml_emit_node(xml, n->base);
                buf_puts(xml, "</m:e><m:lim>");
                omml_emit_node(xml, n->sup);
                buf_puts(xml, "</m:lim></m:limUpp>");
            } else {
                omml_emit_node(xml, n->base);
            }
        } else if (n->sub && n->sup) {
            buf_puts(xml, "<m:sSubSup><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:sub>");
            omml_emit_node(xml, n->sub);
            buf_puts(xml, "</m:sub><m:sup>");
            omml_emit_node(xml, n->sup);
            buf_puts(xml, "</m:sup></m:sSubSup>");
        } else if (n->sup) {
            buf_puts(xml, "<m:sSup><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:sup>");
            omml_emit_node(xml, n->sup);
            buf_puts(xml, "</m:sup></m:sSup>");
        } else if (n->sub) {
            buf_puts(xml, "<m:sSub><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:sub>");
            omml_emit_node(xml, n->sub);
            buf_puts(xml, "</m:sub></m:sSub>");
        } else {
            omml_emit_node(xml, n->base);
        }
        break;
    }

    case MN_FRAC: {
        if (n->leftDelim || n->rightDelim) {
            buf_puts(xml, "<m:d><m:dPr>");
            omml_put_delim_char(xml, "begChr", n->leftDelim);
            omml_put_delim_char(xml, "endChr", n->rightDelim);
            buf_puts(xml, "</m:dPr><m:e>");
        }
        buf_puts(xml, "<m:f>");
        if (!n->hasBar) {
            buf_puts(xml, "<m:fPr><m:type m:val=\"noBar\"/></m:fPr>");
        }
        buf_puts(xml, "<m:num>");
        omml_emit_node(xml, n->base);
        buf_puts(xml, "</m:num><m:den>");
        omml_emit_node(xml, n->sub);
        buf_puts(xml, "</m:den></m:f>");
        if (n->leftDelim || n->rightDelim) {
            buf_puts(xml, "</m:e></m:d>");
        }
        break;
    }

    case MN_SQRT: {
        buf_puts(xml, "<m:rad>");
        if (!n->sup) {
            buf_puts(xml, "<m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/>");
        } else {
            buf_puts(xml, "<m:deg>");
            omml_emit_node(xml, n->sup);
            buf_puts(xml, "</m:deg>");
        }
        buf_puts(xml, "<m:e>");
        omml_emit_node(xml, n->base);
        buf_puts(xml, "</m:e></m:rad>");
        break;
    }

    case MN_DELIM: {
        buf_puts(xml, "<m:d><m:dPr>");
        omml_put_delim_char(xml, "begChr", n->leftDelim);
        omml_put_delim_char(xml, "endChr", n->rightDelim);
        buf_puts(xml, "</m:dPr><m:e>");
        omml_emit_node(xml, n->base);
        buf_puts(xml, "</m:e></m:d>");
        break;
    }

    case MN_ACCENT: {
        if (n->accent == MACC_OVERLINE) {
            buf_puts(xml, "<m:bar><m:barPr><m:pos m:val=\"top\"/></m:barPr><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e></m:bar>");
        } else if (n->accent == MACC_UNDERLINE) {
            buf_puts(xml, "<m:bar><m:barPr><m:pos m:val=\"bot\"/></m:barPr><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e></m:bar>");
        } else if (n->accent == MACC_OVERBRACE || n->accent == MACC_UNDERBRACE) {
            const char *pos = (n->accent == MACC_OVERBRACE) ? "top" : "bot";
            const char *chr = (n->accent == MACC_OVERBRACE) ? "\xE2\x8F\x9E" : "\xE2\x8F\x9F";
            buf_printf(xml, "<m:groupChr><m:groupChrPr><m:chr m:val=\"%s\"/><m:pos m:val=\"%s\"/></m:groupChrPr><m:e>", chr, pos);
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e></m:groupChr>");
        } else {
            uint32_t accCp = 0x0302;
            char u8[5];
            int ulen;
            switch (n->accent) {
            case MACC_HAT: case MACC_WIDEHAT: accCp = 0x0302; break;
            case MACC_CHECK: accCp = 0x030C; break;
            case MACC_TILDE: case MACC_WIDETILDE: accCp = 0x0303; break;
            case MACC_ACUTE: accCp = 0x0301; break;
            case MACC_GRAVE: accCp = 0x0300; break;
            case MACC_DOT: accCp = 0x0307; break;
            case MACC_DDOT: accCp = 0x0308; break;
            case MACC_DDDOT: accCp = 0x20DB; break;
            case MACC_BREVE: accCp = 0x0306; break;
            case MACC_BAR: accCp = 0x0305; break;
            case MACC_VEC: case MACC_OVERRIGHTARROW: accCp = 0x20D7; break;
            case MACC_OVERLEFTARROW: accCp = 0x20D6; break;
            case MACC_OVERLEFTRIGHTARROW: accCp = 0x20E1; break;
            default: break;
            }
            ulen = utf8_encode(accCp, u8);
            u8[ulen] = 0;
            buf_printf(xml, "<m:acc><m:accPr><m:chr m:val=\"%s\"/></m:accPr><m:e>", u8);
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e></m:acc>");
        }
        break;
    }

    case MN_STACK: {
        if (n->sup && n->sub) {
            buf_puts(xml, "<m:limUpp><m:e><m:limLow><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:lim>");
            omml_emit_node(xml, n->sub);
            buf_puts(xml, "</m:lim></m:limLow></m:e><m:lim>");
            omml_emit_node(xml, n->sup);
            buf_puts(xml, "</m:lim></m:limUpp>");
        } else if (n->sup) {
            buf_puts(xml, "<m:limUpp><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:lim>");
            omml_emit_node(xml, n->sup);
            buf_puts(xml, "</m:lim></m:limUpp>");
        } else if (n->sub) {
            buf_puts(xml, "<m:limLow><m:e>");
            omml_emit_node(xml, n->base);
            buf_puts(xml, "</m:e><m:lim>");
            omml_emit_node(xml, n->sub);
            buf_puts(xml, "</m:lim></m:limLow>");
        } else {
            omml_emit_node(xml, n->base);
        }
        break;
    }

    case MN_BOXED: {
        buf_puts(xml, "<m:borderBox><m:e>");
        omml_emit_node(xml, n->base);
        buf_puts(xml, "</m:e></m:borderBox>");
        break;
    }

    case MN_PHANTOM: {
        buf_puts(xml, "<m:phant><m:e>");
        omml_emit_node(xml, n->base);
        buf_puts(xml, "</m:e></m:phant>");
        break;
    }

    case MN_COLOR: {
        omml_emit_node(xml, n->base);
        break;
    }

    case MN_MATRIX: {
        int r, c;
        if (n->leftDelim || n->rightDelim) {
            buf_puts(xml, "<m:d><m:dPr>");
            omml_put_delim_char(xml, "begChr", n->leftDelim);
            omml_put_delim_char(xml, "endChr", n->rightDelim);
            buf_puts(xml, "</m:dPr><m:e>");
        }
        buf_puts(xml, "<m:m>");
        for (r = 0; r < n->rows; r++) {
            buf_puts(xml, "<m:mr>");
            for (c = 0; c < n->cols; c++) {
                buf_puts(xml, "<m:e>");
                if (n->cells && n->cells[r * n->cols + c]) {
                    omml_emit_node(xml, n->cells[r * n->cols + c]);
                }
                buf_puts(xml, "</m:e>");
            }
            buf_puts(xml, "</m:mr>");
        }
        buf_puts(xml, "</m:m>");
        if (n->leftDelim || n->rightDelim) {
            buf_puts(xml, "</m:e></m:d>");
        }
        break;
    }
    }
}

void math_write_omml(Buf *xml, const char *latex, int len, int displayMode)
{
    Arena a = {0};
    MParser p;
    MNode *root;
    if (!xml) return;
    if (!latex) latex = "";
    if (len < 0) len = (int)strlen(latex);

    p.arena = &a;
    p.s = latex;
    p.len = len;
    p.pos = 0;
    p.depth = 0;
    root = mp_parse_row(&p, MSTYLE_AUTO, NULL);

    if (displayMode) buf_puts(xml, "<m:oMathPara>");
    buf_puts(xml, "<m:oMath>");
    omml_emit_node(xml, root);
    buf_puts(xml, "</m:oMath>");
    if (displayMode) buf_puts(xml, "</m:oMathPara>");

    arena_free(&a);
}
