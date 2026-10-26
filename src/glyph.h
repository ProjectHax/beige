#ifndef BEIGE_GLYPH_H
#define BEIGE_GLYPH_H

#include "util.h"

typedef struct Glyphs {
    const char *h;
    const char *v;
    const char *tl;
    const char *tr;
    const char *bl;
    const char *br;
    const char *bullet;
    const char *elbow;
    const char *ellipsis;
} Glyphs;

extern const Glyphs G_UNI;
extern const Glyphs G_ASC;

int glyph_locale_utf8(void);
/* pref is "auto", "ascii", or "unicode". *utf8 is set when non-NULL. */
const Glyphs *glyph_pick(const char *pref, int *utf8);

size_t u8_next(const char *s, size_t i, size_t len);
size_t u8_prev(const char *s, size_t i);

/* Byte length of a CSI (or other ESC) sequence at i, or 0 when s[i] is not ESC. */
int esc_len(const char *s, size_t n, size_t i);
int cols_n(const char *s, size_t n);
size_t cols_fit(const char *s, size_t n, int w);
char *cols_clip(const char *s, int w, const char *ellipsis);
size_t cols_break(const char *s, size_t n, int w, size_t *skip);

char *text_clean(const char *s, size_t n);
char *text_ascii(const char *s);

/* Write at most cols columns. cols is already the screen width minus one. */
void box_top(Buf *b, const Glyphs *g, const char *title, int cols);
void box_row(Buf *b, const Glyphs *g, const char *text, int cols);
void box_bottom(Buf *b, const Glyphs *g, int cols);

#endif
