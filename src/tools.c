#include "tools.h"

#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char PARAMS_READ[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File to read\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"1-based line to start at\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum number of lines\"}"
    "},\"required\":[\"path\"]}";

static const char PARAMS_WRITE[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"contents\":{\"type\":\"string\",\"description\":\"Full new contents of the file\"}"
    "},\"required\":[\"path\",\"contents\"]}";

static const char PARAMS_EDIT[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to replace. Must match once.\"},"
    "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}"
    "},\"required\":[\"path\",\"old_string\",\"new_string\"]}";

static const char PARAMS_BASH[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Command passed to /bin/sh -c\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds before the command is killed\"}"
    "},\"required\":[\"command\"]}";

static const char PARAMS_GREP[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"File or directory. Defaults to the working directory.\"},"
    "\"regex\":{\"type\":\"boolean\",\"description\":\"Treat pattern as an extended regular expression\"},"
    "\"glob\":{\"type\":\"string\",\"description\":\"Shell pattern matched against each file name\"}"
    "},\"required\":[\"pattern\"]}";

static const char PARAMS_LS[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory. Defaults to the working directory.\"}"
    "}}";

static const char PARAMS_FETCH[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"http or https URL\"}"
    "},\"required\":[\"url\"]}";

static const char PARAMS_SEARCH[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Words to search for on the web\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum results, up to 8\"}"
    "},\"required\":[\"query\"]}";

static const ToolDef DEFS[] = {
    {"read", "Read a text file.", PARAMS_READ},
    {"write", "Create or overwrite a text file with the full contents.", PARAMS_WRITE},
    {"edit", "Replace one exact occurrence of old_string in a file.", PARAMS_EDIT},
    {"bash", "Run a command with /bin/sh. stdout and stderr are combined.", PARAMS_BASH},
    {"grep", "Search file contents under a directory.", PARAMS_GREP},
    {"ls", "List the names in one directory.", PARAMS_LS},
    {"fetch", "HTTP GET a URL and return the body. Waits for approval in ask mode.", PARAMS_FETCH},
    {"search", "Search the web and return titles, links, and snippets. Waits for approval in ask mode.",
     PARAMS_SEARCH},
};

const ToolDef *tool_defs(int *count) {
    if (count) {
        *count = (int)(sizeof DEFS / sizeof DEFS[0]);
    }
    return DEFS;
}

const ToolDef *tool_by_name(const char *name) {
    int i, n = 0;
    const ToolDef *d = tool_defs(&n);
    for (i = 0; i < n; i++) {
        if (str_eq(d[i].name, name)) {
            return &d[i];
        }
    }
    return NULL;
}

static const char *jstr(cJSON *args, const char *key) {
    cJSON *it;
    if (!args) {
        return NULL;
    }
    it = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!cJSON_IsString(it) || !it->valuestring) {
        return NULL;
    }
    return it->valuestring;
}

static int jint(cJSON *args, const char *key, int fallback) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!cJSON_IsNumber(it)) {
        return fallback;
    }
    return it->valueint;
}

static int jbool(cJSON *args, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    return 0;
}

static int resolve(ToolIn *in, const char *user, char **out, char **err) {
    return path_resolve(in->root, in->sandbox, user, out, err);
}

static int looks_binary(const char *data, size_t n) {
    size_t i, m = n < 8192 ? n : 8192;
    for (i = 0; i < m; i++) {
        if (data[i] == 0) {
            return 1;
        }
    }
    return 0;
}

static char *result_err(const char *msg, int *is_error) {
    *is_error = 1;
    return xstrdup(msg ? msg : "error");
}

static int run_read(ToolIn *in, char **text, int *is_error) {
    const char *user = jstr(in->args, "path");
    char *path = NULL;
    char *err = NULL;
    char *data;
    size_t n = 0;
    int truncated = 0;
    int offset, limit, line, start;
    const char *p;
    Buf out;
    if (!user) {
        *text = result_err("read requires path", is_error);
        return 0;
    }
    if (resolve(in, user, &path, &err) != 0) {
        *text = result_err(err, is_error);
        free(err);
        return 0;
    }
    data = read_file_cap(path, in->read_cap ? in->read_cap : 262144, &n, &truncated, &err);
    if (!data) {
        *text = result_err(err ? err : "cannot read file", is_error);
        free(err);
        free(path);
        return 0;
    }
    if (looks_binary(data, n)) {
        free(data);
        free(path);
        *text = result_err("binary file", is_error);
        return 0;
    }
    offset = jint(in->args, "offset", 1);
    limit = jint(in->args, "limit", 0);
    if (offset < 1) {
        offset = 1;
    }
    buf_init(&out);
    line = 1;
    p = data;
    start = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (line >= offset && (limit <= 0 || start < limit)) {
            buf_printf(&out, "%d|", line);
            buf_append(&out, p, len);
            buf_putc(&out, '\n');
            start++;
        }
        if (!nl) {
            break;
        }
        line++;
        p = nl + 1;
        if (limit > 0 && start >= limit) {
            break;
        }
    }
    if (truncated) {
        buf_puts(&out, "\n[truncated]\n");
    }
    free(data);
    free(path);
    *is_error = 0;
    *text = buf_detach(&out);
    return 0;
}

/* path_resolve follows links, so look at the name as given: write and edit refuse a symlink there. */
static int names_symlink(const char *root, const char *user) {
    char *joined = user[0] == '/' ? xstrdup(user) : path_join(root, user);
    char *norm = path_normalize(joined);
    struct stat st;
    int link = lstat(norm, &st) == 0 && S_ISLNK(st.st_mode);
    free(joined);
    free(norm);
    return link;
}

static int ensure_parent(const char *path, const char *root, int sandbox) {
    char *parent = path_dirname(path);
    char *resolved = NULL;
    char *err = NULL;
    int rc;
    if (str_eq(parent, "/") || str_eq(parent, ".")) {
        free(parent);
        return 0;
    }
    rc = path_resolve(root, sandbox, parent, &resolved, &err);
    free(err);
    if (rc != 0) {
        free(parent);
        free(resolved);
        return -1;
    }
    rc = mkdir_p(resolved, 0755);
    free(parent);
    free(resolved);
    return rc;
}

static int run_write(ToolIn *in, char **text, int *is_error) {
    const char *user = jstr(in->args, "path");
    const char *contents = jstr(in->args, "contents");
    char *path = NULL;
    char *err = NULL;
    if (!user || !contents) {
        *text = result_err("write requires path and contents", is_error);
        return 0;
    }
    if (names_symlink(in->root, user)) {
        *text = result_err("refusing to write through a symlink", is_error);
        return 0;
    }
    if (resolve(in, user, &path, &err) != 0) {
        *text = result_err(err, is_error);
        free(err);
        return 0;
    }
    if (ensure_parent(path, in->root, in->sandbox) != 0) {
        free(path);
        *text = result_err("cannot create parent directory", is_error);
        return 0;
    }
    if (write_file(path, contents, strlen(contents), 0644) != 0) {
        free(path);
        *text = result_err("write failed", is_error);
        return 0;
    }
    *is_error = 0;
    *text = xstrdup("wrote file");
    free(path);
    return 0;
}

static int count_and_replace(const char *data, const char *old_s, const char *new_s, char **out, int *count) {
    size_t oldn = strlen(old_s);
    size_t n = 0;
    const char *p = data;
    Buf b;
    *count = 0;
    if (oldn == 0) {
        return -1;
    }
    while ((p = strstr(p, old_s)) != NULL) {
        (*count)++;
        p += oldn;
    }
    if (*count != 1) {
        return 0;
    }
    buf_init(&b);
    p = data;
    while ((p = strstr(n ? p : data, old_s)) != NULL) {
        /* only one match; find it from the start once */
        break;
    }
    p = strstr(data, old_s);
    buf_append(&b, data, (size_t)(p - data));
    buf_puts(&b, new_s);
    buf_puts(&b, p + oldn);
    *out = buf_detach(&b);
    (void)n;
    return 0;
}

static int run_edit(ToolIn *in, char **text, int *is_error) {
    const char *user = jstr(in->args, "path");
    const char *old_s = jstr(in->args, "old_string");
    const char *new_s = jstr(in->args, "new_string");
    char *path = NULL;
    char *err = NULL;
    char *data;
    char *replaced = NULL;
    size_t n = 0;
    int truncated = 0;
    int count = 0;
    if (!user || !old_s || new_s == NULL) {
        *text = result_err("edit requires path, old_string, and new_string", is_error);
        return 0;
    }
    if (old_s[0] == 0) {
        *text = result_err("old_string is empty", is_error);
        return 0;
    }
    if (names_symlink(in->root, user)) {
        *text = result_err("refusing to edit a symlink", is_error);
        return 0;
    }
    if (resolve(in, user, &path, &err) != 0) {
        *text = result_err(err, is_error);
        free(err);
        return 0;
    }
    data = read_file_cap(path, in->read_cap ? in->read_cap : 262144, &n, &truncated, &err);
    if (!data) {
        *text = result_err(err ? err : "cannot read file", is_error);
        free(err);
        free(path);
        return 0;
    }
    if (truncated) {
        free(data);
        free(path);
        *text = result_err("file is larger than the read cap; refusing to edit", is_error);
        return 0;
    }
    if (count_and_replace(data, old_s, new_s, &replaced, &count) != 0 || count != 1) {
        char msg[128];
        snprintf(msg, sizeof msg, "old_string matched %d times; need exactly one", count);
        free(data);
        free(path);
        *text = result_err(msg, is_error);
        return 0;
    }
    if (write_file(path, replaced, strlen(replaced), 0644) != 0) {
        free(data);
        free(replaced);
        free(path);
        *text = result_err("write failed", is_error);
        return 0;
    }
    free(data);
    free(replaced);
    free(path);
    *is_error = 0;
    *text = xstrdup("edited file");
    return 0;
}

/*
 * Read what the shell left in the pipe after it exited. A job it put in the background (`server &`) can hold
 * the write end open forever, so stop once the pipe is quiet for a moment or after two seconds, not at EOF.
 */
static void drain_pipe(int fd, Buf *out, size_t cap) {
    time_t start = time(NULL);
    char tmp[2048];
    while ((int)(time(NULL) - start) < 2) {
        fd_set r;
        struct timeval tv;
        ssize_t got;
        int rc;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        rc = select(fd + 1, &r, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            break;
        }
        got = read(fd, tmp, sizeof tmp);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        if (out->len < cap) {
            size_t room = cap - out->len;
            buf_append(out, tmp, (size_t)got < room ? (size_t)got : room);
        }
    }
}

static int run_bash(ToolIn *in, char **text, int *is_error) {
    const char *command = jstr(in->args, "command");
    int timeout = jint(in->args, "timeout", in->shell_timeout);
    int pipefd[2];
    pid_t pid;
    Buf out;
    int status = 0;
    int reaped = 0;
    int eof = 0;
    time_t start;
    if (!command || !command[0]) {
        *text = result_err("bash requires command", is_error);
        return 0;
    }
    if (pipe(pipefd) != 0) {
        *text = result_err("pipe failed", is_error);
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        *text = result_err("fork failed", is_error);
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        setpgid(0, 0);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[0] != STDOUT_FILENO && pipefd[0] != STDERR_FILENO) {
            close(pipefd[0]);
        }
        if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO) {
            close(pipefd[1]);
        }
        /* beige ignores SIGPIPE; an ignored signal survives exec, and `yes | head` needs the default. */
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    buf_init(&out);
    start = time(NULL);
    for (;;) {
        fd_set r, w, e;
        struct timeval tv;
        int maxfd = pipefd[0];
        int rc;
        ssize_t got;
        char tmp[2048];
        int st = 0;
        pid_t wr;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_ZERO(&e);
        if (!eof) {
            FD_SET(pipefd[0], &r);
        }
        if (in->hooks && in->hooks->prepare) {
            in->hooks->prepare(in->hooks->ud, &r, &w, &e, &maxfd);
        }
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        rc = select(maxfd + 1, &r, &w, &e, &tv);
        if (rc > 0 && in->hooks && in->hooks->ready) {
            in->hooks->ready(in->hooks->ud, &r, &w, &e);
        }
        if (in->hooks && in->hooks->tick) {
            in->hooks->tick(in->hooks->ud);
        }
        if (in->cancel && *in->cancel) {
            kill(-pid, SIGTERM);
            kill(-pid, SIGKILL);
            buf_puts(&out, "\n[cancelled]");
            break;
        }
        if (timeout > 0 && (int)(time(NULL) - start) >= timeout) {
            kill(-pid, SIGKILL);
            buf_puts(&out, "\n[timed out]");
            break;
        }
        if (rc > 0 && !eof && FD_ISSET(pipefd[0], &r)) {
            size_t room;
            got = read(pipefd[0], tmp, sizeof tmp);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (got == 0) {
                /* Output is closed; wait for the shell itself rather than killing its group, which would also
                   kill a job it started with `cmd >log 2>&1 &`. */
                eof = 1;
                continue;
            }
            room = in->output_cap > out.len ? in->output_cap - out.len : 0;
            if (room == 0 || (size_t)got > room) {
                if (room > 0) {
                    buf_append(&out, tmp, room);
                }
                kill(-pid, SIGKILL);
                buf_puts(&out, "\n[truncated]");
                break;
            }
            buf_append(&out, tmp, (size_t)got);
        }
        wr = waitpid(pid, &st, WNOHANG);
        if (wr == pid) {
            status = st;
            reaped = 1;
            drain_pipe(pipefd[0], &out, in->output_cap);
            break;
        }
    }
    if (!reaped) {
        int st = 0;
        kill(-pid, SIGKILL);
        waitpid(pid, &st, 0);
        status = st;
    }
    close(pipefd[0]);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        buf_printf(&out, "\n[exit %d]", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        buf_printf(&out, "\n[signal %d]", WTERMSIG(status));
    }
    *is_error = 0;
    *text = buf_detach(&out);
    return 0;
}

typedef struct {
    int matches;
    int files;
    int regex;
    const char *pattern;
    const char *glob;
    regex_t re;
    int re_ok;
    Buf out;
    size_t cap;
    int full;
    const char *root;
    int sandbox;
} GrepCtx;

static int skip_dir_name(const char *name) {
    return str_eq(name, ".git") || str_eq(name, ".svn") || str_eq(name, ".hg") || str_eq(name, ".");
}

static void grep_file(GrepCtx *g, const char *path) {
    char *err = NULL;
    char *data;
    size_t n = 0;
    int truncated = 0;
    const char *p;
    int line = 1;
    struct stat st;
    if (g->matches >= 200 || g->files >= 4000 || g->full) {
        return;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024) {
        return;
    }
    g->files++;
    data = read_file_cap(path, 1024 * 1024, &n, &truncated, &err);
    free(err);
    if (!data) {
        return;
    }
    if (looks_binary(data, n)) {
        free(data);
        return;
    }
    p = data;
    while (*p && g->matches < 200 && !g->full) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int hit = 0;
        char *linebuf = xstrndup(p, len);
        if (g->regex) {
            hit = g->re_ok && regexec(&g->re, linebuf, 0, NULL, 0) == 0;
        } else {
            hit = strstr(linebuf, g->pattern) != NULL;
        }
        if (hit) {
            size_t room = g->cap > g->out.len ? g->cap - g->out.len : 0;
            Buf row;
            buf_init(&row);
            buf_printf(&row, "%s:%d:%s\n", path, line, linebuf);
            g->matches++;
            if (row.len > room) {
                buf_append(&g->out, row.data, room);
                buf_puts(&g->out, "\n[output cap reached]\n");
                g->full = 1;
            } else {
                buf_append(&g->out, row.data, row.len);
            }
            buf_free(&row);
        }
        free(linebuf);
        if (!nl) {
            break;
        }
        p = nl + 1;
        line++;
    }
    free(data);
    (void)truncated;
}

static void grep_walk(GrepCtx *g, const char *dir, int depth) {
    DIR *d;
    struct dirent *de;
    if (depth > 24 || g->matches >= 200 || g->files >= 4000 || g->full) {
        return;
    }
    d = opendir(dir);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char *child;
        struct stat st;
        if (str_eq(de->d_name, ".") || str_eq(de->d_name, "..")) {
            continue;
        }
        child = path_join(dir, de->d_name);
        if (lstat(child, &st) != 0) {
            free(child);
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            free(child);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!skip_dir_name(de->d_name)) {
                grep_walk(g, child, depth + 1);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!g->glob || fnmatch(g->glob, de->d_name, 0) == 0) {
                grep_file(g, child);
            }
        }
        free(child);
    }
    closedir(d);
}

static int run_grep(ToolIn *in, char **text, int *is_error) {
    const char *pattern = jstr(in->args, "pattern");
    const char *user = jstr(in->args, "path");
    const char *glob = jstr(in->args, "glob");
    char *path = NULL;
    char *err = NULL;
    struct stat st;
    GrepCtx g;
    if (!pattern) {
        *text = result_err("grep requires pattern", is_error);
        return 0;
    }
    if (!user || !user[0]) {
        user = ".";
    }
    if (resolve(in, user, &path, &err) != 0) {
        *text = result_err(err, is_error);
        free(err);
        return 0;
    }
    memset(&g, 0, sizeof g);
    g.pattern = pattern;
    g.glob = glob;
    g.regex = jbool(in->args, "regex");
    g.root = in->root;
    g.sandbox = in->sandbox;
    g.cap = in->output_cap;
    buf_init(&g.out);
    if (g.regex) {
        if (regcomp(&g.re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
            free(path);
            buf_free(&g.out);
            *text = result_err("invalid regular expression", is_error);
            return 0;
        }
        g.re_ok = 1;
    }
    if (stat(path, &st) != 0) {
        if (g.re_ok) {
            regfree(&g.re);
        }
        free(path);
        buf_free(&g.out);
        *text = result_err("path not found", is_error);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        grep_walk(&g, path, 0);
    } else {
        grep_file(&g, path);
    }
    if (g.re_ok) {
        regfree(&g.re);
    }
    if (g.matches == 0) {
        buf_puts(&g.out, "no matches");
    } else if (g.matches >= 200 && !g.full) {
        buf_puts(&g.out, "[match cap reached]\n");
    }
    free(path);
    *is_error = 0;
    *text = buf_detach(&g.out);
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int run_ls(ToolIn *in, char **text, int *is_error) {
    const char *user = jstr(in->args, "path");
    char *path = NULL;
    char *err = NULL;
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    int n = 0, cap = 0, i;
    Buf out;
    if (!user || !user[0]) {
        user = ".";
    }
    if (resolve(in, user, &path, &err) != 0) {
        *text = result_err(err, is_error);
        free(err);
        return 0;
    }
    d = opendir(path);
    if (!d) {
        free(path);
        *text = result_err("cannot open directory", is_error);
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        char *child;
        struct stat st;
        int dir;
        size_t ln;
        char *shown;
        if (str_eq(de->d_name, ".") || str_eq(de->d_name, "..")) {
            continue;
        }
        if (n >= 500) {
            break;
        }
        child = path_join(path, de->d_name);
        dir = (stat(child, &st) == 0 && S_ISDIR(st.st_mode));
        free(child);
        ln = strlen(de->d_name);
        shown = xmalloc(ln + 2);
        memcpy(shown, de->d_name, ln);
        shown[ln] = dir ? '/' : 0;
        shown[ln + (dir ? 1 : 0)] = 0;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = xrealloc(names, (size_t)cap * sizeof(char *));
        }
        names[n++] = shown;
    }
    closedir(d);
    free(path);
    if (n > 0) {
        qsort(names, (size_t)n, sizeof(char *), cmp_str);
    }
    buf_init(&out);
    for (i = 0; i < n; i++) {
        buf_puts(&out, names[i]);
        buf_putc(&out, '\n');
        free(names[i]);
    }
    free(names);
    if (n == 0) {
        buf_puts(&out, "(empty)");
    }
    *is_error = 0;
    *text = buf_detach(&out);
    return 0;
}

static int run_fetch(ToolIn *in, char **text, int *is_error) {
    const char *url = jstr(in->args, "url");
    char *body = NULL;
    char *err = NULL;
    long status = 0;
    struct curl_slist *hdrs = NULL;
    Buf out;
    if (!url || !(str_starts(url, "https://") || str_starts(url, "http://"))) {
        *text = result_err("fetch requires an http or https url", is_error);
        return 0;
    }
    if (strchr(url, ' ') || strchr(url, '\n')) {
        *text = result_err("url contains whitespace", is_error);
        return 0;
    }
    hdrs = http_hdr(hdrs, "Accept: text/plain, text/markdown, text/html, application/json, */*");
    if (http_exchange(in->http, "GET", url, hdrs, NULL, &status, &body, in->output_cap ? in->output_cap : 32768,
                      &err) != 0) {
        curl_slist_free_all(hdrs);
        *text = result_err(err ? err : "fetch failed", is_error);
        free(err);
        return 0;
    }
    curl_slist_free_all(hdrs);
    buf_init(&out);
    buf_printf(&out, "HTTP %ld\n", status);
    buf_puts(&out, body ? body : "");
    free(body);
    free(err);
    *is_error = status >= 400;
    *text = buf_detach(&out);
    return 0;
}

static char *url_encode(const char *s) {
    Buf b;
    buf_init(&b);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            buf_putc(&b, (char)c);
        } else if (c == ' ') {
            buf_putc(&b, '+');
        } else {
            buf_printf(&b, "%%%02X", c);
        }
    }
    return buf_detach(&b);
}

static void html_text(Buf *b, const char *s, size_t n) {
    size_t i = 0;
    int space = 1;
    while (i < n) {
        char ch;
        if (s[i] == '&') {
            if (i + 5 <= n && strncmp(s + i, "&amp;", 5) == 0) {
                ch = '&';
                i += 5;
            } else if (i + 4 <= n && strncmp(s + i, "&lt;", 4) == 0) {
                ch = '<';
                i += 4;
            } else if (i + 4 <= n && strncmp(s + i, "&gt;", 4) == 0) {
                ch = '>';
                i += 4;
            } else if (i + 6 <= n && strncmp(s + i, "&quot;", 6) == 0) {
                ch = '"';
                i += 6;
            } else if (i + 5 <= n && strncmp(s + i, "&#39;", 5) == 0) {
                ch = '\'';
                i += 5;
            } else {
                i++;
                continue;
            }
        } else {
            unsigned char c = (unsigned char)s[i++];
            if (c == '<') {
                break;
            }
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                ch = ' ';
            } else if (c < 0x20) {
                continue;
            } else {
                ch = (char)c;
            }
        }
        if (ch == ' ') {
            if (space) {
                continue;
            }
            space = 1;
        } else {
            space = 0;
        }
        buf_putc(b, ch);
        if (b->len > 240) {
            break;
        }
    }
}

static const char *attr_value(const char *start, const char *end, const char *key, size_t *out_n) {
    size_t klen = strlen(key);
    const char *p = start;
    *out_n = 0;
    while (p + klen + 2 < end) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char q = p[klen + 1];
            const char *v;
            const char *stop;
            if (q != '"' && q != '\'') {
                p++;
                continue;
            }
            v = p + klen + 2;
            stop = v;
            while (stop < end && *stop != q) {
                stop++;
            }
            *out_n = (size_t)(stop - v);
            return v;
        }
        p++;
    }
    return NULL;
}

static int run_search(ToolIn *in, char **text, int *is_error) {
    const char *query = jstr(in->args, "query");
    char *enc = NULL;
    char *url = NULL;
    char *body = NULL;
    char *err = NULL;
    long status = 0;
    struct curl_slist *hdrs = NULL;
    Buf out;
    const char *p;
    int limit;
    int found = 0;
    if (!query || !query[0]) {
        *text = result_err("search requires query", is_error);
        return 0;
    }
    limit = jint(in->args, "limit", 5);
    if (limit < 1) {
        limit = 1;
    }
    if (limit > 8) {
        limit = 8;
    }
    enc = url_encode(query);
    url = xmalloc(strlen(enc) + 48);
    sprintf(url, "https://html.duckduckgo.com/html/?q=%s", enc);
    free(enc);
    hdrs = http_hdr(hdrs, "Accept: text/html");
    hdrs = http_hdr(hdrs, "User-Agent: beige");
    if (http_exchange(in->http, "GET", url, hdrs, NULL, &status, &body, 512 * 1024, &err) != 0 || status >= 400 ||
        !body) {
        curl_slist_free_all(hdrs);
        free(url);
        *text = result_err(err ? err : "search failed", is_error);
        free(err);
        free(body);
        return 0;
    }
    curl_slist_free_all(hdrs);
    free(url);
    free(err);
    buf_init(&out);
    p = body;
    while (found < limit && (p = strstr(p, "result__a")) != NULL) {
        const char *back = p - 180;
        const char *href;
        size_t href_n = 0;
        const char *gt;
        const char *lt;
        Buf title;
        Buf snip;
        const char *snip_at;
        if (back < body) {
            back = body;
        }
        href = attr_value(back, p + 80, "href", &href_n);
        gt = strchr(p, '>');
        if (!gt) {
            break;
        }
        lt = strchr(gt + 1, '<');
        if (!lt) {
            break;
        }
        buf_init(&title);
        html_text(&title, gt + 1, (size_t)(lt - (gt + 1)));
        buf_init(&snip);
        snip_at = strstr(lt, "result__snippet");
        if (snip_at && snip_at < lt + 800) {
            const char *sg = strchr(snip_at, '>');
            const char *sl;
            if (sg) {
                sl = strchr(sg + 1, '<');
                if (sl) {
                    html_text(&snip, sg + 1, (size_t)(sl - (sg + 1)));
                }
            }
        }
        if (title.len) {
            found++;
            buf_printf(&out, "%d. %s\n", found, title.data);
            if (href && href_n) {
                Buf link;
                buf_init(&link);
                html_text(&link, href, href_n > 300 ? 300 : href_n);
                if (link.len) {
                    buf_printf(&out, "   %s\n", link.data);
                }
                buf_free(&link);
            }
            if (snip.len) {
                buf_printf(&out, "   %s\n", snip.data);
            }
        }
        buf_free(&title);
        buf_free(&snip);
        p = lt + 1;
    }
    free(body);
    if (found == 0) {
        buf_puts(&out, "no results");
    }
    *is_error = 0;
    *text = buf_detach(&out);
    return 0;
}

int tool_run(const char *name, ToolIn *in, char **text, int *is_error) {
    *text = NULL;
    *is_error = 0;
    if (!in->output_cap) {
        in->output_cap = 32768;
    }
    if (str_eq(name, "read")) {
        return run_read(in, text, is_error);
    }
    if (str_eq(name, "write")) {
        return run_write(in, text, is_error);
    }
    if (str_eq(name, "edit")) {
        return run_edit(in, text, is_error);
    }
    if (str_eq(name, "bash")) {
        return run_bash(in, text, is_error);
    }
    if (str_eq(name, "grep")) {
        return run_grep(in, text, is_error);
    }
    if (str_eq(name, "ls")) {
        return run_ls(in, text, is_error);
    }
    if (str_eq(name, "fetch")) {
        return run_fetch(in, text, is_error);
    }
    if (str_eq(name, "search")) {
        return run_search(in, text, is_error);
    }
    *text = result_err("unknown tool", is_error);
    return 0;
}

char *tool_subject(const char *name, cJSON *args, const char *root, int sandbox) {
    const char *s = NULL;
    char *resolved = NULL;
    char *err = NULL;
    (void)sandbox;
    if (str_eq(name, "bash")) {
        s = jstr(args, "command");
        return xstrdup(s ? s : "");
    }
    if (str_eq(name, "fetch")) {
        s = jstr(args, "url");
        return xstrdup(s ? s : "");
    }
    if (str_eq(name, "search")) {
        s = jstr(args, "query");
        return xstrdup(s ? s : "");
    }
    if (str_eq(name, "read") || str_eq(name, "write") || str_eq(name, "edit") || str_eq(name, "ls") ||
        str_eq(name, "grep")) {
        s = jstr(args, "path");
        if (!s || !s[0]) {
            s = ".";
        }
        if (path_resolve(root, 0, s, &resolved, &err) == 0) {
            free(err);
            return resolved;
        }
        free(err);
        free(resolved);
        return xstrdup(s);
    }
    return xstrdup("");
}

char *tool_preview(const char *name, cJSON *args, const char *root, int sandbox) {
    Buf b;
    buf_init(&b);
    if (str_eq(name, "bash")) {
        buf_printf(&b, "bash\n%s\n", jstr(args, "command") ? jstr(args, "command") : "");
        return buf_detach(&b);
    }
    if (str_eq(name, "fetch")) {
        buf_printf(&b, "fetch %s\n", jstr(args, "url") ? jstr(args, "url") : "");
        return buf_detach(&b);
    }
    if (str_eq(name, "search")) {
        buf_printf(&b, "search %s\n", jstr(args, "query") ? jstr(args, "query") : "");
        return buf_detach(&b);
    }
    if (str_eq(name, "write") || str_eq(name, "edit")) {
        const char *user = jstr(args, "path");
        char *path = NULL;
        char *err = NULL;
        char *old = NULL;
        size_t n = 0;
        int trunc = 0;
        char *diff;
        const char *neu = NULL;
        if (user && path_resolve(root, sandbox, user, &path, &err) == 0) {
            old = read_file_cap(path, 200000, &n, &trunc, NULL);
        }
        free(err);
        if (str_eq(name, "write")) {
            neu = jstr(args, "contents");
            diff = diff_unified(user ? user : "file", old ? old : "", neu ? neu : "");
        } else {
            const char *old_s = jstr(args, "old_string");
            const char *new_s = jstr(args, "new_string");
            diff = diff_unified(user ? user : "file", old_s ? old_s : "", new_s ? new_s : "");
        }
        buf_printf(&b, "%s %s\n%s", name, user ? user : "", diff ? diff : "");
        free(diff);
        free(old);
        free(path);
        return buf_detach(&b);
    }
    buf_printf(&b, "%s", name);
    return buf_detach(&b);
}

static const char *title_subject(const char *name, cJSON *args) {
    const char *s;
    if (str_eq(name, "bash")) {
        return jstr(args, "command");
    }
    if (str_eq(name, "fetch")) {
        return jstr(args, "url");
    }
    if (str_eq(name, "search")) {
        return jstr(args, "query");
    }
    if (str_eq(name, "grep")) {
        s = jstr(args, "path");
        if (s && s[0]) {
            return s;
        }
        return jstr(args, "pattern");
    }
    return jstr(args, "path");
}

char *tool_title(const char *name, cJSON *args) {
    const char *sub = title_subject(name, args);
    char nice[64];
    char *flat;
    char *out;
    size_t i;
    if (!name || !name[0]) {
        snprintf(nice, sizeof nice, "Tool");
    } else {
        snprintf(nice, sizeof nice, "%s", name);
        if (nice[0] >= 'a' && nice[0] <= 'z') {
            nice[0] = (char)(nice[0] - 'a' + 'A');
        }
    }
    flat = xstrdup(sub ? sub : "");
    for (i = 0; flat[i]; i++) {
        if (flat[i] == '\n' || flat[i] == '\r' || flat[i] == '\t') {
            flat[i] = ' ';
        }
    }
    out = xmalloc(strlen(nice) + strlen(flat) + 3);
    sprintf(out, "%s(%s)", nice, flat);
    free(flat);
    return out;
}

static char *first_line_dup(const char *s) {
    size_t n = 0;
    if (!s) {
        return xstrdup("");
    }
    while (s[n] && s[n] != '\n' && s[n] != '\r') {
        n++;
    }
    return xstrndup(s, n);
}

static int line_is(const char *s, const char *lit) {
    size_t n = strlen(lit);
    if (!s || strncmp(s, lit, n) != 0) {
        return 0;
    }
    return s[n] == 0 || s[n] == '\n' || s[n] == '\r';
}

static int count_nl_lines(const char *s) {
    int n = 0;
    const char *p;
    if (!s || !s[0]) {
        return 0;
    }
    n = 1;
    for (p = s; *p; p++) {
        if (*p == '\n') {
            n++;
        }
    }
    if (p > s && p[-1] == '\n') {
        n--;
    }
    return n;
}

static int count_matching(const char *s, int (*pred)(const char *line, size_t n)) {
    int hits = 0;
    const char *p = s ? s : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && pred(p, n)) {
            hits++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return hits;
}

static int pred_read_row(const char *line, size_t n) {
    size_t i = 0;
    if (n == 0 || line[0] < '0' || line[0] > '9') {
        return 0;
    }
    while (i < n && line[i] >= '0' && line[i] <= '9') {
        i++;
    }
    return i < n && line[i] == '|';
}

static int pred_grep_row(const char *line, size_t n) {
    (void)n;
    return line[0] != '[';
}

static int pred_search_row(const char *line, size_t n) {
    size_t i = 0;
    if (n == 0 || line[0] < '0' || line[0] > '9') {
        return 0;
    }
    while (i < n && line[i] >= '0' && line[i] <= '9') {
        i++;
    }
    return i + 1 < n && line[i] == '.' && line[i + 1] == ' ';
}

static int count_nonempty(const char *s) {
    int n = 0;
    const char *p = s ? s : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len) {
            n++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

char *tool_summary(const char *name, cJSON *args, const char *result, int is_error) {
    char buf[256];
    char *line;
    const char *path;
    if (is_error) {
        line = first_line_dup(result);
        if (strncmp(line, "Error: ", 7) == 0) {
            char *stripped = xstrdup(line + 7);
            free(line);
            return stripped;
        }
        return line;
    }
    if (str_eq(name, "bash")) {
        if (!result || !result[0] || result[0] == '\n') {
            return xstrdup("(no output)");
        }
        return first_line_dup(result);
    }
    if (str_eq(name, "read")) {
        snprintf(buf, sizeof buf, "Read %d lines", count_matching(result, pred_read_row));
        return xstrdup(buf);
    }
    if (str_eq(name, "write")) {
        path = jstr(args, "path");
        snprintf(buf, sizeof buf, "Wrote %d lines to %s", count_nl_lines(jstr(args, "contents")), path ? path : "");
        return xstrdup(buf);
    }
    if (str_eq(name, "edit")) {
        path = jstr(args, "path");
        snprintf(buf, sizeof buf, "Updated %s", path ? path : "");
        return xstrdup(buf);
    }
    if (str_eq(name, "grep")) {
        if (line_is(result, "no matches")) {
            return xstrdup("No matches");
        }
        snprintf(buf, sizeof buf, "%d matches", count_matching(result, pred_grep_row));
        return xstrdup(buf);
    }
    if (str_eq(name, "ls")) {
        if (line_is(result, "(empty)")) {
            return xstrdup("0 entries");
        }
        snprintf(buf, sizeof buf, "%d entries", count_nonempty(result));
        return xstrdup(buf);
    }
    if (str_eq(name, "fetch")) {
        long status = 0;
        int rest;
        line = first_line_dup(result);
        if (strncmp(line, "HTTP ", 5) == 0) {
            status = strtol(line + 5, NULL, 10);
        }
        free(line);
        rest = 0;
        if (result) {
            const char *nl = strchr(result, '\n');
            if (nl) {
                rest = count_nonempty(nl + 1);
            }
        }
        snprintf(buf, sizeof buf, "HTTP %ld, %d lines", status, rest);
        return xstrdup(buf);
    }
    if (str_eq(name, "search")) {
        snprintf(buf, sizeof buf, "%d results", count_matching(result, pred_search_row));
        return xstrdup(buf);
    }
    return first_line_dup(result);
}
