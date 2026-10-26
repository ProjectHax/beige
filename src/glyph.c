#include "glyph.h"

#include <ctype.h>

const Glyphs G_UNI = {
    "\xe2\x94\x80", /* U+2500 */
    "\xe2\x94\x82", /* U+2502 */
    "\xe2\x94\x8c", /* U+250C */
    "\xe2\x94\x90", /* U+2510 */
    "\xe2\x94\x94", /* U+2514 */
    "\xe2\x94\x98", /* U+2518 */
    "\xe2\x97\x8f", /* U+25CF */
    "\xe2\x94\x94", /* U+2514 elbow */
    "\xe2\x80\xa6"  /* U+2026 */
};

const Glyphs G_ASC = {"-", "|", "+", "+", "+", "+", "*", "`-", "..."};

static int utf8_span(unsigned char c) {
    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        return 2;
    }
    if (c >= 0xe0 && c <= 0xef) {
        return 3;
    }
    if (c >= 0xf0 && c <= 0xf4) {
        return 4;
    }
    return 0;
}

static int cont_ok(unsigned char c) {
    return (c & 0xc0) == 0x80;
}

size_t u8_next(const char *s, size_t i, size_t len) {
    unsigned char c;
    int need;
    int j;
    if (!s || i >= len) {
        return i;
    }
    c = (unsigned char)s[i];
    need = utf8_span(c);
    if (need <= 1) {
        return i + 1;
    }
    if (i + (size_t)need > len) {
        return i + 1;
    }
    for (j = 1; j < need; j++) {
        if (!cont_ok((unsigned char)s[i + (size_t)j])) {
            return i + 1;
        }
    }
    if (need == 3) {
        unsigned char c1 = (unsigned char)s[i + 1];
        if (c == 0xe0 && c1 < 0xa0) {
            return i + 1;
        }
        if (c == 0xed && c1 >= 0xa0) {
            return i + 1;
        }
    }
    if (need == 4) {
        unsigned char c1 = (unsigned char)s[i + 1];
        if (c == 0xf0 && c1 < 0x90) {
            return i + 1;
        }
        if (c == 0xf4 && c1 >= 0x90) {
            return i + 1;
        }
    }
    return i + (size_t)need;
}

size_t u8_prev(const char *s, size_t i) {
    size_t start;
    int back;
    if (!s || i == 0) {
        return 0;
    }
    start = i - 1;
    back = 0;
    while (start > 0 && back < 3 && ((unsigned char)s[start] & 0xc0) == 0x80) {
        start--;
        back++;
    }
    if (u8_next(s, start, i) == i) {
        return start;
    }
    return i - 1;
}

int esc_len(const char *s, size_t n, size_t i) {
    size_t k;
    unsigned char c;
    if (!s || i >= n || (unsigned char)s[i] != 0x1b) {
        return 0;
    }
    if (i + 1 >= n) {
        return 1;
    }
    c = (unsigned char)s[i + 1];
    if (c == '[') {
        k = i + 2;
        while (k < n && (unsigned char)s[k] >= 0x30 && (unsigned char)s[k] <= 0x3f) {
            k++;
        }
        while (k < n && (unsigned char)s[k] >= 0x20 && (unsigned char)s[k] <= 0x2f) {
            k++;
        }
        if (k < n && (unsigned char)s[k] >= 0x40 && (unsigned char)s[k] <= 0x7e) {
            return (int)(k + 1 - i);
        }
        return (int)(k - i);
    }
    k = i + 1;
    while (k < n && (unsigned char)s[k] >= 0x20 && (unsigned char)s[k] <= 0x2f) {
        k++;
    }
    if (k < n && (unsigned char)s[k] >= 0x30 && (unsigned char)s[k] <= 0x7e) {
        return (int)(k + 1 - i);
    }
    return 1;
}

int cols_n(const char *s, size_t n) {
    size_t i = 0;
    int cols = 0;
    if (!s) {
        return 0;
    }
    while (i < n) {
        int el = esc_len(s, n, i);
        size_t nx;
        if (el > 0) {
            i += (size_t)el;
            continue;
        }
        nx = u8_next(s, i, n);
        if (nx <= i) {
            break;
        }
        cols++;
        i = nx;
    }
    return cols;
}

size_t cols_fit(const char *s, size_t n, int w) {
    size_t i = 0;
    int cols = 0;
    if (!s || w <= 0) {
        return 0;
    }
    while (i < n) {
        int el = esc_len(s, n, i);
        size_t nx;
        if (el > 0) {
            i += (size_t)el;
            continue;
        }
        nx = u8_next(s, i, n);
        if (nx <= i) {
            break;
        }
        if (cols + 1 > w) {
            break;
        }
        cols++;
        i = nx;
    }
    return i;
}

char *cols_clip(const char *s, int w, const char *ellipsis) {
    size_t n;
    size_t en;
    size_t fit;
    int total;
    int elw;
    char *out;
    if (!s) {
        s = "";
    }
    if (!ellipsis) {
        ellipsis = "";
    }
    if (w < 1) {
        w = 1;
    }
    n = strlen(s);
    total = cols_n(s, n);
    if (total <= w) {
        return xstrdup(s);
    }
    en = strlen(ellipsis);
    elw = cols_n(ellipsis, en);
    if (elw >= w) {
        fit = cols_fit(ellipsis, en, w);
        return xstrndup(ellipsis, fit);
    }
    fit = cols_fit(s, n, w - elw);
    out = xmalloc(fit + en + 1);
    memcpy(out, s, fit);
    memcpy(out + fit, ellipsis, en + 1);
    return out;
}

size_t cols_break(const char *s, size_t n, int w, size_t *skip) {
    size_t i = 0;
    size_t last_space = (size_t)-1;
    int cols = 0;
    if (skip) {
        *skip = 0;
    }
    if (!s || n == 0 || w < 1) {
        return 0;
    }
    if (cols_n(s, n) <= w) {
        return n;
    }
    while (i < n && cols < w) {
        int el = esc_len(s, n, i);
        size_t nx;
        unsigned char c;
        if (el > 0) {
            i += (size_t)el;
            continue;
        }
        c = (unsigned char)s[i];
        nx = u8_next(s, i, n);
        if (nx <= i) {
            break;
        }
        if (c == ' ' || c == '\t') {
            last_space = i;
        }
        cols++;
        i = nx;
    }
    if (last_space != (size_t)-1 && last_space > 0) {
        size_t k = last_space;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) {
            k++;
        }
        if (skip) {
            *skip = k - last_space;
        }
        return last_space;
    }
    return i;
}

char *text_clean(const char *s, size_t n) {
    char *d;
    size_t i = 0;
    size_t o = 0;
    if (!s) {
        n = 0;
    }
    d = xmalloc(n * 4 + 1);
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int el;
        int need;
        size_t nx;
        if (c == '\n') {
            d[o++] = '\n';
            i++;
            continue;
        }
        if (c == '\t') {
            int k;
            for (k = 0; k < 4; k++) {
                d[o++] = ' ';
            }
            i++;
            continue;
        }
        if (c == 0x1b) {
            el = esc_len(s, n, i);
            i += el > 0 ? (size_t)el : 1;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            i++;
            continue;
        }
        need = utf8_span(c);
        if (need > 1 && i + (size_t)need > n) {
            break;
        }
        nx = u8_next(s, i, n);
        if (nx <= i) {
            break;
        }
        if (nx == i + 1 && c >= 0x80) {
            d[o++] = '?';
            i++;
            continue;
        }
        memcpy(d + o, s + i, nx - i);
        o += nx - i;
        i = nx;
    }
    d[o] = 0;
    return d;
}

static unsigned int cp_at(const char *s, size_t n, size_t i, size_t *adv) {
    unsigned char c;
    size_t nx;
    c = (unsigned char)s[i];
    nx = u8_next(s, i, n);
    if (nx <= i) {
        *adv = i + 1;
        return c;
    }
    *adv = nx;
    if (nx == i + 1) {
        return c;
    }
    if (nx == i + 2) {
        return ((unsigned int)(c & 0x1f) << 6) | ((unsigned char)s[i + 1] & 0x3f);
    }
    if (nx == i + 3) {
        return ((unsigned int)(c & 0x0f) << 12) | ((unsigned int)((unsigned char)s[i + 1] & 0x3f) << 6) |
               ((unsigned char)s[i + 2] & 0x3f);
    }
    return ((unsigned int)(c & 0x07) << 18) | ((unsigned int)((unsigned char)s[i + 1] & 0x3f) << 12) |
           ((unsigned int)((unsigned char)s[i + 2] & 0x3f) << 6) | ((unsigned char)s[i + 3] & 0x3f);
}

static void ascii_put(char **d, size_t *o, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*o + n + 1 > *cap) {
        while (*o + n + 1 > *cap) {
            *cap *= 2;
        }
        *d = xrealloc(*d, *cap);
    }
    memcpy(*d + *o, s, n);
    *o += n;
}

char *text_ascii(const char *s) {
    size_t n;
    size_t i = 0;
    size_t o = 0;
    size_t cap = 64;
    char *d;
    if (!s) {
        s = "";
    }
    n = strlen(s);
    d = xmalloc(cap);
    while (i < n) {
        size_t adv = i + 1;
        unsigned int cp = cp_at(s, n, i, &adv);
        const char *rep = NULL;
        char one[2];
        if (adv <= i) {
            break;
        }
        if (cp < 0x80) {
            one[0] = (char)cp;
            one[1] = 0;
            ascii_put(&d, &o, &cap, one);
            i = adv;
            continue;
        }
        if (cp == 0x00a0) {
            rep = " ";
        } else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || cp == 0x2500 || cp == 0x2501) {
            rep = "-";
        } else if (cp == 0x2502 || cp == 0x2503) {
            rep = "|";
        } else if (cp >= 0x250c && cp <= 0x254b) {
            rep = "+";
        } else if (cp == 0x2022 || cp == 0x00b7 || cp == 0x25cf || cp == 0x25cb) {
            rep = "*";
        } else if (cp == 0x2026) {
            rep = "...";
        } else if (cp == 0x2018 || cp == 0x2019) {
            rep = "'";
        } else if (cp == 0x201c || cp == 0x201d) {
            rep = "\"";
        } else {
            rep = "?";
        }
        ascii_put(&d, &o, &cap, rep);
        i = adv;
    }
    d[o] = 0;
    return d;
}

static int has_utf8(const char *s) {
    size_t i;
    if (!s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        char a = s[i];
        char b = s[i + 1] ? s[i + 1] : 0;
        char c = (a && b && s[i + 2]) ? s[i + 2] : 0;
        char d = (a && b && c && s[i + 3]) ? s[i + 3] : 0;
        char e = (a && b && c && d && s[i + 4]) ? s[i + 4] : 0;
        if (tolower((unsigned char)a) == 'u' && tolower((unsigned char)b) == 't' && tolower((unsigned char)c) == 'f') {
            if (d == '-' && (e == '8' || e == '8')) {
                return 1;
            }
            if (d == '8') {
                return 1;
            }
        }
    }
    return 0;
}

int glyph_locale_utf8(void) {
    const char *s = getenv("LC_ALL");
    if (!s || !s[0]) {
        s = getenv("LC_CTYPE");
    }
    if (!s || !s[0]) {
        s = getenv("LANG");
    }
    return has_utf8(s);
}

const Glyphs *glyph_pick(const char *pref, int *utf8) {
    int uni = glyph_locale_utf8();
    if (pref && str_eq(pref, "ascii")) {
        uni = 0;
    } else if (pref && str_eq(pref, "unicode")) {
        uni = 1;
    }
    if (utf8) {
        *utf8 = uni;
    }
    return uni ? &G_UNI : &G_ASC;
}

static int gcols(const char *g) {
    if (!g || !g[0]) {
        return 0;
    }
    return cols_n(g, strlen(g));
}

void box_top(Buf *b, const Glyphs *g, const char *title, int cols) {
    int col = 0;
    int right;
    int hc;
    if (!b || !g || cols < 2) {
        return;
    }
    right = gcols(g->tr);
    hc = gcols(g->h);
    if (hc < 1) {
        hc = 1;
    }
    buf_puts(b, g->tl);
    col += gcols(g->tl);
    if (title && title[0]) {
        int overhead = hc + 1 + 1 + hc + right;
        int room = cols - col - overhead;
        if (room >= 1) {
            char *clipped = cols_clip(title, room, "");
            buf_puts(b, g->h);
            col += hc;
            buf_putc(b, ' ');
            col++;
            buf_puts(b, clipped);
            col += cols_n(clipped, strlen(clipped));
            free(clipped);
            buf_putc(b, ' ');
            col++;
        }
    }
    while (col + hc + right <= cols) {
        buf_puts(b, g->h);
        col += hc;
    }
    if (col + right <= cols) {
        buf_puts(b, g->tr);
    }
}

void box_row(Buf *b, const Glyphs *g, const char *text, int cols) {
    int side;
    int inner;
    int got;
    char *clipped;
    if (!b || !g || cols < 2) {
        return;
    }
    side = gcols(g->v);
    if (side < 1) {
        side = 1;
    }
    inner = cols - side - side;
    if (inner < 0) {
        inner = 0;
    }
    clipped = cols_clip(text ? text : "", inner, "");
    buf_puts(b, g->v);
    buf_puts(b, clipped);
    got = cols_n(clipped, strlen(clipped));
    free(clipped);
    while (got < inner) {
        buf_putc(b, ' ');
        got++;
    }
    buf_puts(b, g->v);
}

void box_bottom(Buf *b, const Glyphs *g, int cols) {
    int col = 0;
    int right;
    int hc;
    if (!b || !g || cols < 2) {
        return;
    }
    right = gcols(g->br);
    hc = gcols(g->h);
    if (hc < 1) {
        hc = 1;
    }
    buf_puts(b, g->bl);
    col += gcols(g->bl);
    while (col + hc + right <= cols) {
        buf_puts(b, g->h);
        col += hc;
    }
    if (col + right <= cols) {
        buf_puts(b, g->br);
    }
}
