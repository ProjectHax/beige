#include "util.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

void beige_die(const char *fmt, ...) {
    va_list ap;
    fputs("beige: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n) {
    void *p;
    if (n == 0) {
        n = 1;
    }
    p = malloc(n);
    if (!p) {
        beige_die("out of memory");
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q;
    if (n == 0) {
        n = 1;
    }
    q = realloc(p, n);
    if (!q) {
        beige_die("out of memory");
    }
    return q;
}

char *xstrdup(const char *s) {
    size_t n;
    char *d;
    if (!s) {
        s = "";
    }
    n = strlen(s);
    d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

char *xstrndup(const char *s, size_t n) {
    char *d;
    size_t i;
    if (!s) {
        return xstrdup("");
    }
    for (i = 0; i < n && s[i]; i++) {
    }
    d = xmalloc(i + 1);
    memcpy(d, s, i);
    d[i] = 0;
    return d;
}

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_clear(Buf *b) {
    b->len = 0;
    if (b->data) {
        b->data[0] = 0;
    }
}

void buf_grow(Buf *b, size_t need) {
    size_t cap;
    if (need <= b->cap) {
        return;
    }
    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) {
            beige_die("out of memory");
        }
        cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
    if (b->len == 0) {
        b->data[0] = 0;
    }
}

void buf_append(Buf *b, const char *s, size_t n) {
    if (!b->data) {
        buf_grow(b, 1);
        b->data[0] = 0;
    }
    if (!s || n == 0) {
        return;
    }
    buf_grow(b, b->len + n + 1);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) {
    if (!s) {
        return;
    }
    buf_append(b, s, strlen(s));
}

void buf_putc(Buf *b, char c) {
    buf_append(b, &c, 1);
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap, aq;
    int n;
    va_start(ap, fmt);
    va_copy(aq, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(aq);
        return;
    }
    buf_grow(b, b->len + (size_t)n + 1);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, aq);
    va_end(aq);
    b->len += (size_t)n;
}

char *buf_detach(Buf *b) {
    char *s = b->data;
    if (!s) {
        s = xstrdup("");
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return s;
}

int str_eq(const char *a, const char *b) {
    if (!a || !b) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

int str_starts(const char *s, const char *pfx) {
    size_t n;
    if (!s || !pfx) {
        return 0;
    }
    n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

char *str_trim_dup(const char *s) {
    const char *a;
    const char *b;
    if (!s) {
        return xstrdup("");
    }
    a = s;
    while (*a && isspace((unsigned char)*a)) {
        a++;
    }
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) {
        b--;
    }
    return xstrndup(a, (size_t)(b - a));
}

/* Length of the well-formed UTF-8 sequence at s, or 0. */
static size_t utf8_seq(const unsigned char *s) {
    unsigned char c = s[0];
    size_t need, i;
    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        need = 2;
    } else if (c >= 0xe0 && c <= 0xef) {
        need = 3;
    } else if (c >= 0xf0 && c <= 0xf4) {
        need = 4;
    } else {
        return 0;
    }
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) || (c == 0xf0 && s[1] < 0x90) ||
        (c == 0xf4 && s[1] >= 0x90)) {
        return 0;
    }
    return need;
}

char *str_utf8_dup(const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    const unsigned char *q;
    Buf b;
    for (q = p; *q;) {
        size_t n = utf8_seq(q);
        if (n == 0) {
            break;
        }
        q += n;
    }
    if (!*q) {
        return xstrdup((const char *)p);
    }
    buf_init(&b);
    buf_append(&b, (const char *)p, (size_t)(q - p));
    while (*q) {
        size_t n = utf8_seq(q);
        if (n == 0) {
            buf_puts(&b, "\xef\xbf\xbd");
            q++;
        } else {
            buf_append(&b, (const char *)q, n);
            q += n;
        }
    }
    return buf_detach(&b);
}

unsigned long str_hash(const char *s) {
    unsigned long h = 5381;
    unsigned char c;
    if (!s) {
        return h;
    }
    while ((c = (unsigned char)*s++) != 0) {
        h = ((h << 5) + h) + c;
    }
    return h;
}

char *path_join(const char *a, const char *b) {
    size_t na, nb;
    int slash;
    char *out;
    if (!a || !a[0]) {
        return xstrdup(b ? b : "");
    }
    if (!b || !b[0]) {
        return xstrdup(a);
    }
    if (b[0] == '/') {
        return xstrdup(b);
    }
    na = strlen(a);
    nb = strlen(b);
    slash = a[na - 1] != '/';
    out = xmalloc(na + nb + (slash ? 2 : 1));
    memcpy(out, a, na);
    if (slash) {
        out[na] = '/';
        memcpy(out + na + 1, b, nb + 1);
    } else {
        memcpy(out + na, b, nb + 1);
    }
    return out;
}

char *path_dirname(const char *path) {
    const char *slash;
    size_t n;
    if (!path || !path[0]) {
        return xstrdup(".");
    }
    slash = strrchr(path, '/');
    if (!slash) {
        return xstrdup(".");
    }
    if (slash == path) {
        return xstrdup("/");
    }
    n = (size_t)(slash - path);
    return xstrndup(path, n);
}

char *path_basename(const char *path) {
    const char *slash;
    if (!path || !path[0]) {
        return xstrdup("");
    }
    slash = strrchr(path, '/');
    if (!slash) {
        return xstrdup(path);
    }
    if (slash[1] == 0) {
        return xstrdup("/");
    }
    return xstrdup(slash + 1);
}

char *path_normalize(const char *abs_path) {
    char *copy;
    char **parts = NULL;
    int np = 0;
    int cap = 0;
    char *save = NULL;
    char *tok;
    Buf out;
    int abs;
    int i;

    if (!abs_path || !abs_path[0]) {
        return xstrdup("");
    }
    copy = xstrdup(abs_path);
    abs = abs_path[0] == '/';
    tok = strtok_r(copy, "/", &save);
    while (tok) {
        if (tok[0] == 0 || str_eq(tok, ".")) {
            /* skip */
        } else if (str_eq(tok, "..")) {
            if (np > 0) {
                free(parts[--np]);
            }
        } else {
            if (np == cap) {
                cap = cap ? cap * 2 : 8;
                parts = xrealloc(parts, (size_t)cap * sizeof(char *));
            }
            parts[np++] = xstrdup(tok);
        }
        tok = strtok_r(NULL, "/", &save);
    }
    free(copy);
    buf_init(&out);
    if (abs) {
        buf_putc(&out, '/');
    }
    if (np == 0 && !abs) {
        buf_puts(&out, ".");
    }
    for (i = 0; i < np; i++) {
        if (!(abs && i == 0 && out.len == 1)) {
            if (out.len > 0 && out.data[out.len - 1] != '/') {
                buf_putc(&out, '/');
            }
        }
        buf_puts(&out, parts[i]);
        free(parts[i]);
    }
    free(parts);
    if (abs && np == 0) {
        buf_clear(&out);
        buf_puts(&out, "/");
    }
    return buf_detach(&out);
}

char *getcwd_alloc(void) {
    size_t n = 256;
    for (;;) {
        char *b = xmalloc(n);
        if (getcwd(b, n)) {
            return b;
        }
        free(b);
        if (errno != ERANGE) {
            return NULL;
        }
        if (n > 1024 * 1024) {
            return NULL;
        }
        n *= 2;
    }
}

char *realpath_alloc(const char *path) {
#ifdef PATH_MAX
    char buf[PATH_MAX];
#else
    char buf[4096];
#endif
    if (!path || !realpath(path, buf)) {
        return NULL;
    }
    return xstrdup(buf);
}

static int is_under(const char *root, const char *path) {
    size_t n;
    if (!root || !path) {
        return 0;
    }
    if (str_eq(root, "/")) {
        return path[0] == '/';
    }
    n = strlen(root);
    if (strncmp(path, root, n) != 0) {
        return 0;
    }
    return path[n] == 0 || path[n] == '/';
}

int path_resolve(const char *root, int sandbox, const char *userpath, char **out, char **err) {
    char *joined = NULL;
    char *norm = NULL;
    char *cursor = NULL;
    char *rest = NULL;
    char *real_prefix = NULL;
    char *combined = NULL;
    char *final = NULL;
    char *root_real = NULL;

    *out = NULL;
    if (err) {
        *err = NULL;
    }
    if (!userpath || !userpath[0]) {
        if (err) {
            *err = xstrdup("empty path");
        }
        return -1;
    }
    if (userpath[0] == '/') {
        joined = xstrdup(userpath);
    } else if (root && root[0]) {
        joined = path_join(root, userpath);
    } else {
        joined = xstrdup(userpath);
    }
    norm = path_normalize(joined);
    free(joined);
    if (!norm || norm[0] != '/') {
        free(norm);
        if (err) {
            *err = xstrdup("path is not absolute");
        }
        return -1;
    }

    cursor = xstrdup(norm);
    rest = xstrdup("");
    for (;;) {
        struct stat st;
        if (lstat(cursor, &st) == 0) {
            break;
        }
        if (str_eq(cursor, "/")) {
            break;
        }
        {
            char *base = path_basename(cursor);
            char *parent = path_dirname(cursor);
            char *new_rest;
            if (rest[0]) {
                new_rest = path_join(base, rest);
            } else {
                new_rest = xstrdup(base);
            }
            free(base);
            free(rest);
            rest = new_rest;
            free(cursor);
            cursor = parent;
        }
    }
    real_prefix = realpath_alloc(cursor);
    if (!real_prefix) {
        free(cursor);
        free(rest);
        free(norm);
        if (err) {
            *err = xstrdup("cannot resolve path");
        }
        return -1;
    }
    if (rest[0]) {
        combined = path_join(real_prefix, rest);
        final = path_normalize(combined);
        free(combined);
    } else {
        final = xstrdup(real_prefix);
    }
    free(cursor);
    free(rest);
    free(real_prefix);
    free(norm);

    if (sandbox) {
        root_real = root ? realpath_alloc(root) : NULL;
        if (!root_real) {
            root_real = root ? xstrdup(root) : xstrdup("/");
        }
        if (!is_under(root_real, final)) {
            if (err) {
                *err = xstrdup("path escapes the working directory");
            }
            free(root_real);
            free(final);
            return -1;
        }
        free(root_real);
    }
    *out = final;
    return 0;
}

char *home_dir(void) {
    const char *h = getenv("HOME");
    struct passwd *pw;
    if (h && h[0]) {
        return xstrdup(h);
    }
    pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return xstrdup(pw->pw_dir);
    }
    return xstrdup("/");
}

char *beige_home(void) {
    const char *env = getenv("BEIGE_HOME");
    char *home;
    char *dir;
    if (env && env[0]) {
        dir = xstrdup(env);
    } else {
        home = home_dir();
        dir = path_join(home, ".beige");
        free(home);
    }
    if (mkdir_p(dir, 0700) != 0) {
        beige_die("cannot create %s", dir);
    }
    return dir;
}

int mkdir_p(const char *path, int mode) {
    char *copy;
    char *p;
    size_t len;
    if (!path || !path[0]) {
        return -1;
    }
    copy = xstrdup(path);
    len = strlen(copy);
    if (len > 1 && copy[len - 1] == '/') {
        copy[len - 1] = 0;
    }
    for (p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

static int write_all(int fd, const char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* A file with other hard links is rewritten in place so every name sees the change. */
static int write_in_place(const char *path, const char *data, size_t n) {
    int flags = O_WRONLY | O_TRUNC;
    int fd;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, data, n) != 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int write_file(const char *path, const char *data, size_t n, int mode) {
    char *tmp = NULL;
    int fd;
    size_t plen;
    struct stat st;
    int exists;
    mode_t want;
    if (!path) {
        return -1;
    }
    exists = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (exists && st.st_nlink > 1 && mode != 0600) {
        return write_in_place(path, data, n);
    }
    if (mode == 0600) {
        want = 0600;
    } else if (exists) {
        want = st.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        want = (mode_t)mode & ~mask;
    }
    plen = strlen(path);
    tmp = xmalloc(plen + 16);
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".XXXXXX", 8);
    /* mkstemp uses O_EXCL, so a planted symlink or an existing sibling is never opened. */
    fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    if (exists && mode != 0600 && fchown(fd, st.st_uid, st.st_gid) != 0) {
        /* Not the owner: the replacement belongs to us, as a fresh file would. */
    }
    if (fchmod(fd, want) != 0 || write_all(fd, data, n) != 0) {
        close(fd);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

char *read_file_cap(const char *path, size_t cap, size_t *out_n, int *truncated, char **err) {
    FILE *fp;
    char *buf;
    size_t n = 0;
    struct stat st;
    if (out_n) {
        *out_n = 0;
    }
    if (truncated) {
        *truncated = 0;
    }
    if (err) {
        *err = NULL;
    }
    if (lstat(path, &st) != 0) {
        if (err) {
            *err = xstrdup(strerror(errno));
        }
        return NULL;
    }
    if (S_ISLNK(st.st_mode)) {
        if (err) {
            *err = xstrdup("refusing to read a symlink");
        }
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        if (err) {
            *err = xstrdup("not a regular file");
        }
        return NULL;
    }
    if (cap == 0 || cap > (size_t)1 << 30) {
        if (err) {
            *err = xstrdup("read cap must be between 1 byte and 1 GiB");
        }
        return NULL;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        if (err) {
            *err = xstrdup(strerror(errno));
        }
        return NULL;
    }
    buf = xmalloc(cap + 1);
    while (n < cap) {
        size_t got = fread(buf + n, 1, cap - n, fp);
        n += got;
        if (got == 0) {
            break;
        }
    }
    if (n == cap) {
        int c = fgetc(fp);
        if (c != EOF) {
            if (truncated) {
                *truncated = 1;
            }
        }
    }
    if (ferror(fp)) {
        fclose(fp);
        free(buf);
        if (err) {
            *err = xstrdup("read error");
        }
        return NULL;
    }
    fclose(fp);
    buf[n] = 0;
    if (out_n) {
        *out_n = n;
    }
    return buf;
}

static char **split_lines(const char *text, int *out_n) {
    const char *p;
    char **lines;
    int n = 0;
    int cap = 16;
    lines = xmalloc((size_t)cap * sizeof(char *));
    if (!text) {
        text = "";
    }
    p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (n == cap) {
            cap *= 2;
            lines = xrealloc(lines, (size_t)cap * sizeof(char *));
        }
        lines[n++] = xstrndup(p, len);
        if (!nl) {
            break;
        }
        p = nl + 1;
        if (*p == 0 && nl) {
            /* trailing newline does not add an extra empty line beyond a final empty */
            break;
        }
    }
    *out_n = n;
    return lines;
}

static void free_lines(char **lines, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free(lines[i]);
    }
    free(lines);
}

char *diff_unified(const char *path, const char *old_text, const char *new_text) {
    int na = 0, nb = 0;
    char **a;
    char **b;
    int i, j;
    int cap_lines = 160;
    Buf out;
    const char *label = path && path[0] ? path : "file";

    buf_init(&out);
    a = split_lines(old_text ? old_text : "", &na);
    b = split_lines(new_text ? new_text : "", &nb);
    buf_printf(&out, "--- %s\n+++ %s\n", label, label);
    if (na > cap_lines || nb > cap_lines) {
        buf_printf(&out, "(diff omitted, %d -> %d lines)\n", na, nb);
        free_lines(a, na);
        free_lines(b, nb);
        return buf_detach(&out);
    }
    {
        /* LCS length table, rolled into a pair of rows would be tighter.
           Full table is fine at this cap: 161*161 ints. */
        int rows = na + 1;
        int cols = nb + 1;
        unsigned short *dp = xmalloc((size_t)rows * (size_t)cols * sizeof(unsigned short));
        int *keep_a = xmalloc((size_t)na * sizeof(int));
        int *keep_b = xmalloc((size_t)nb * sizeof(int));
        for (i = 0; i < na; i++) {
            keep_a[i] = 0;
        }
        for (j = 0; j < nb; j++) {
            keep_b[j] = 0;
        }
        for (i = rows - 1; i >= 0; i--) {
            for (j = cols - 1; j >= 0; j--) {
                unsigned short v = 0;
                if (i == rows - 1 || j == cols - 1) {
                    v = 0;
                } else if (str_eq(a[i], b[j])) {
                    v = (unsigned short)(1 + dp[(i + 1) * cols + (j + 1)]);
                } else {
                    unsigned short down = dp[(i + 1) * cols + j];
                    unsigned short right = dp[i * cols + (j + 1)];
                    v = down > right ? down : right;
                }
                dp[i * cols + j] = v;
            }
        }
        i = 0;
        j = 0;
        while (i < na && j < nb) {
            if (str_eq(a[i], b[j])) {
                keep_a[i] = 1;
                keep_b[j] = 1;
                i++;
                j++;
            } else if (dp[(i + 1) * cols + j] >= dp[i * cols + (j + 1)]) {
                i++;
            } else {
                j++;
            }
        }
        i = 0;
        j = 0;
        while (i < na || j < nb) {
            if (i < na && j < nb && keep_a[i] && keep_b[j]) {
                buf_printf(&out, " %s\n", a[i]);
                i++;
                j++;
            } else {
                int is = i, js = j;
                while (i < na && !keep_a[i]) {
                    i++;
                }
                while (j < nb && !keep_b[j]) {
                    j++;
                }
                if (i == is && j == js) {
                    break;
                }
                buf_printf(&out, "@@ -%d,%d +%d,%d @@\n", is + 1, i - is, js + 1, j - js);
                for (; is < i; is++) {
                    buf_printf(&out, "-%s\n", a[is]);
                }
                for (; js < j; js++) {
                    buf_printf(&out, "+%s\n", b[js]);
                }
            }
        }
        free(dp);
        free(keep_a);
        free(keep_b);
    }
    free_lines(a, na);
    free_lines(b, nb);
    return buf_detach(&out);
}

void ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}
