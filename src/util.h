#ifndef BEIGE_UTIL_H
#define BEIGE_UTIL_H

#include "compat.h"
#include <stdarg.h>

void beige_die(const char *fmt, ...);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

typedef struct Buf {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_grow(Buf *b, size_t need);
void buf_append(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...);
char *buf_detach(Buf *b);

int str_eq(const char *a, const char *b);
int str_starts(const char *s, const char *pfx);
char *str_trim_dup(const char *s);
/* Copy with every byte that is not well-formed UTF-8 replaced by U+FFFD, so it is safe in a JSON request. */
char *str_utf8_dup(const char *s);
unsigned long str_hash(const char *s);

char *path_join(const char *a, const char *b);
char *path_dirname(const char *path);
char *path_basename(const char *path);
/* Lexical cleanup of an absolute path. "." and ".." are removed. */
char *path_normalize(const char *abs_path);
char *getcwd_alloc(void);
char *realpath_alloc(const char *path);

/*
 * Resolve userpath against root. When sandbox is set, the result must stay
 * inside root after symlink resolution. *out is malloc'd on success.
 * *err is malloc'd on failure.
 */
int path_resolve(const char *root, int sandbox, const char *userpath, char **out, char **err);

char *home_dir(void);
/* ~/.beige, or $BEIGE_HOME when that is set. Created mode 0700. */
char *beige_home(void);
int mkdir_p(const char *path, int mode);
int write_file(const char *path, const char *data, size_t n, int mode);
/* Reads at most cap bytes. *truncated is set when the file is longer. */
char *read_file_cap(const char *path, size_t cap, size_t *out_n, int *truncated, char **err);

char *diff_unified(const char *path, const char *old_text, const char *new_text);

void ignore_sigpipe(void);

#endif
