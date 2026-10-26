#include "check.h"
#include "glyph.h"

#include <stdlib.h>

int main(void) {
    const char *bar = "\xe2\x94\x80";
    const char *esc = "\033[1;31mab\033[0m";
    const char *raw = "a\tb\033[31mX\r\n\x01\xff";
    char *clean;
    char *asc;
    size_t skip = 99;
    size_t n;
    int utf8 = 1;
    const Glyphs *g;
    Buf box;
    char saved_all[128];
    char saved_ctype[128];
    char saved_lang[128];
    const char *env;
    int had_all = 0;
    int had_ctype = 0;
    int had_lang = 0;

    CHECK(cols_n("h\xc3\xa9llo", 6) == 5);
    CHECK(cols_n(esc, strlen(esc)) == 2);
    CHECK(cols_fit(bar, 3, 1) == 3);
    CHECK(cols_fit(bar, 3, 0) == 0);
    CHECK(cols_fit("\xe2\x94\x80" "x", 4, 1) == 3);

    n = cols_break("it does.", 8, 5, &skip);
    CHECK(n == 2);
    CHECK(skip == 1);
    CHECK(memcmp("it does.", "it", n) == 0);
    n = cols_break("abcdefgh", 8, 3, &skip);
    CHECK(n == 3);
    CHECK(skip == 0);

    clean = text_clean(raw, strlen(raw));
    CHECK(str_eq(clean, "a    bX\n?"));
    free(clean);
    clean = text_clean("h\xc3\xa9", 3);
    CHECK(str_eq(clean, "h\xc3\xa9"));
    asc = text_ascii(clean);
    CHECK(str_eq(asc, "h?"));
    free(asc);
    free(clean);
    asc = text_ascii("\xe2\x80\xa6");
    CHECK(str_eq(asc, "..."));
    free(asc);

    g = glyph_pick("ascii", &utf8);
    CHECK(g == &G_ASC);
    CHECK(utf8 == 0);
    CHECK(g->bullet[0] == '*');
    CHECK(str_eq(g->elbow, "`-"));
    g = glyph_pick("unicode", &utf8);
    CHECK(g == &G_UNI);
    CHECK(utf8 == 1);

    env = getenv("LC_ALL");
    if (env) {
        had_all = 1;
        snprintf(saved_all, sizeof saved_all, "%s", env);
    }
    env = getenv("LC_CTYPE");
    if (env) {
        had_ctype = 1;
        snprintf(saved_ctype, sizeof saved_ctype, "%s", env);
    }
    env = getenv("LANG");
    if (env) {
        had_lang = 1;
        snprintf(saved_lang, sizeof saved_lang, "%s", env);
    }
    setenv("LC_ALL", "C", 1);
    CHECK(glyph_locale_utf8() == 0);
    setenv("LC_ALL", "en_US.UTF-8", 1);
    CHECK(glyph_locale_utf8() == 1);
    unsetenv("LC_ALL");
    setenv("LC_CTYPE", "POSIX", 1);
    setenv("LANG", "en_US.UTF-8", 1);
    CHECK(glyph_locale_utf8() == 0);
    unsetenv("LC_CTYPE");
    setenv("LANG", "en_US.UTF-8", 1);
    CHECK(glyph_locale_utf8() == 1);
    if (had_all) {
        setenv("LC_ALL", saved_all, 1);
    } else {
        unsetenv("LC_ALL");
    }
    if (had_ctype) {
        setenv("LC_CTYPE", saved_ctype, 1);
    } else {
        unsetenv("LC_CTYPE");
    }
    if (had_lang) {
        setenv("LANG", saved_lang, 1);
    } else {
        unsetenv("LANG");
    }

    buf_init(&box);
    box_top(&box, &G_ASC, "hi", 20);
    CHECK(cols_n(box.data, box.len) == 20);
    CHECK(box.data[0] == '+');
    CHECK(box.data[box.len - 1] == '+');
    buf_clear(&box);
    box_row(&box, &G_ASC, "ab", 8);
    CHECK(cols_n(box.data, box.len) == 8);
    CHECK(box.data[0] == '|');
    CHECK(box.data[box.len - 1] == '|');
    buf_free(&box);

    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
