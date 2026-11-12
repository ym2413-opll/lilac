/*
 * math.h - LaTeX math parser, 2D box layout engine, display-list emitter,
 * and DOCX Office Math ML (OMML) exporter.
 *
 * Pure C, zero external dependencies. All inputs are UTF-8.
 */
#ifndef MV_MATH_H
#define MV_MATH_H

#include "util.h"
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MathBox {
    float width;    /* total advance width in DIP */
    float ascent;   /* distance above baseline in DIP */
    float descent;  /* distance below baseline in DIP */
    void *primHead; /* internal linked list of positioned primitives */
    void *primTail;
} MathBox;

/* Parse and lay out a UTF-8 LaTeX math string into a 2D MathBox allocated in arena.
 * displayMode: 0 = inline ($...$ / \(...\)), 1 = display ($$...$$ / \[...\] / ```math). */
MathBox *math_layout(Arena *arena, const char *latex, int len,
                     float baseSize, int displayMode, uint32_t color);

/* Emit a positioned MathBox into Layout L at (x, baselineY) on the given page. */
void math_emit(const MathBox *box, Layout *L, int page, float x, float baselineY, int link);

/* Convert a UTF-8 LaTeX math string to Word Office Math ML (<m:oMath> / <m:oMathPara>)
 * and append to xml buffer. */
void math_write_omml(Buf *xml, const char *latex, int len, int displayMode);

#ifdef __cplusplus
}
#endif
#endif
