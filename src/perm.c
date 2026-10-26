#include "perm.h"

#include "util.h"

#include <ctype.h>
#include <string.h>

void perm_init(PermState *ps, PermMode mode) {
    ps->mode = mode;
    ps->rules = NULL;
    ps->nrules = 0;
    ps->cap = 0;
    ps->root = NULL;
    ps->sandbox = 1;
}

void perm_free(PermState *ps) {
    int i;
    for (i = 0; i < ps->nrules; i++) {
        free(ps->rules[i].tool);
        free(ps->rules[i].prefix);
    }
    free(ps->rules);
    ps->rules = NULL;
    ps->nrules = 0;
    ps->cap = 0;
    free(ps->root);
    ps->root = NULL;
}

void perm_set_mode(PermState *ps, PermMode mode) {
    ps->mode = mode;
}

void perm_set_root(PermState *ps, const char *root, int sandbox) {
    if (!ps) {
        return;
    }
    free(ps->root);
    ps->root = root && root[0] ? xstrdup(root) : NULL;
    ps->sandbox = sandbox ? 1 : 0;
}

const char *perm_mode_name(PermMode mode) {
    if (mode == MODE_PLAN) {
        return "plan";
    }
    if (mode == MODE_AUTO) {
        return "auto";
    }
    return "ask";
}

int perm_mode_parse(const char *s, PermMode *out) {
    if (!s || !out) {
        return -1;
    }
    if (str_eq(s, "ask") || str_eq(s, "default")) {
        *out = MODE_ASK;
        return 0;
    }
    if (str_eq(s, "plan")) {
        *out = MODE_PLAN;
        return 0;
    }
    if (str_eq(s, "auto")) {
        *out = MODE_AUTO;
        return 0;
    }
    return -1;
}

int perm_is_plan_file(const char *path) {
    size_t n;
    const char *suffix = ".beige/plan.md";
    size_t sn;
    if (!path) {
        return 0;
    }
    n = strlen(path);
    sn = strlen(suffix);
    if (n < sn) {
        return 0;
    }
    if (strcmp(path + (n - sn), suffix) != 0) {
        return 0;
    }
    if (n == sn) {
        return 1;
    }
    return path[n - sn - 1] == '/';
}

static int is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static size_t copy_word(const char *s, char *buf, size_t cap);
static const char *base_cmd(const char *word);

/* True when word appears as a command token inside s. */
static int has_command(const char *s, const char *word) {
    size_t n = strlen(word);
    const char *p = s;
    while ((p = strstr(p, word)) != NULL) {
        int before_ok = (p == s) || !is_word_char(p[-1]);
        int after_ok = p[n] == 0 || !is_word_char(p[n]);
        if (before_ok && after_ok) {
            return 1;
        }
        p += n;
    }
    return 0;
}

static int rootish_word(const char *w) {
    return w[0] == '/' || w[0] == '~' || str_starts(w, "$HOME") || str_starts(w, "${HOME}") || strstr(w, "/*");
}

/* rm with any recursive flag (-r, -R, -Rf, -r -f, --recursive) and a target at /, ~, or another absolute path. */
static int rm_dangerous(const char *s) {
    char word[1024];
    int in_rm = 0;
    int opts = 1;
    int recursive = 0;
    int rootish = 0;
    while (*s) {
        size_t used;
        const char *w;
        if (isspace((unsigned char)*s)) {
            s++;
            continue;
        }
        used = copy_word(s, word, sizeof word);
        if (used == 0) {
            s++;
            continue;
        }
        s += used;
        w = word[0] == '\\' ? word + 1 : word;
        if (!in_rm && str_eq(base_cmd(w), "rm")) {
            in_rm = 1;
            continue;
        }
        if (in_rm && opts && str_eq(w, "--")) {
            opts = 0;
            continue;
        }
        if (in_rm && opts && w[0] == '-' && w[1]) {
            if (str_eq(w, "--recursive") || str_eq(w, "--no-preserve-root") ||
                (w[1] != '-' && (strchr(w, 'r') || strchr(w, 'R')))) {
                recursive = 1;
            }
            continue;
        }
        if (rootish_word(w)) {
            rootish = 1;
        }
    }
    return in_rm && recursive && rootish;
}

static int segment_dangerous(const char *seg) {
    const char *s = skip_ws(seg);
    if (has_command(s, "mkfs")) {
        return 1;
    }
    if (has_command(s, "shutdown") || has_command(s, "reboot") || has_command(s, "poweroff") ||
        has_command(s, "halt")) {
        return 1;
    }
    if (strstr(s, ":(){") || strstr(s, ":() {")) {
        return 1;
    }
    if (strstr(s, ">/dev/") || strstr(s, "> /dev/")) {
        return 1;
    }
    if (has_command(s, "dd") && strstr(s, "of=/dev/")) {
        return 1;
    }
    if (rm_dangerous(s)) {
        return 1;
    }
    if (has_command(s, "chmod") && strstr(s, "-R") && strstr(s, "777") && strstr(s, " /")) {
        return 1;
    }
    return 0;
}

int perm_is_catastrophic(const char *command) {
    char *copy;
    char *p;
    int bad = 0;
    if (!command) {
        return 0;
    }
    copy = xstrdup(command);
    p = copy;
    while (*p) {
        char *start = p;
        while (*p && *p != '\n' && *p != ';' && *p != '|' && *p != '&') {
            p++;
        }
        if (*p) {
            *p = 0;
            p++;
        }
        if (segment_dangerous(start)) {
            bad = 1;
            break;
        }
    }
    free(copy);
    return bad;
}

static int has_meta(const char *s) {
    if (strstr(s, ";") || strstr(s, "|") || strstr(s, "&") || strstr(s, ">") || strstr(s, "<") ||
        strstr(s, "`") || strstr(s, "$(") || strchr(s, '\n')) {
        return 1;
    }
    return 0;
}

static int starts_token(const char *s, const char *pfx) {
    size_t n = strlen(pfx);
    s = skip_ws(s);
    if (strncmp(s, pfx, n) != 0) {
        return 0;
    }
    return s[n] == 0 || isspace((unsigned char)s[n]);
}

/* find actions that delete, run, or write, and git's --output=<file>. */
static int plan_word_writes(const char *word) {
    static const char *acts[] = {
        "-delete", "-exec", "-execdir", "-ok", "-okdir", "-fls", "-fprint", "-fprint0", "-fprintf", NULL
    };
    int i;
    for (i = 0; acts[i]; i++) {
        if (str_eq(word, acts[i])) {
            return 1;
        }
    }
    return str_starts(word, "--output");
}

static int any_plan_write(const char *s) {
    char word[1024];
    while (*s) {
        size_t used;
        if (isspace((unsigned char)*s)) {
            s++;
            continue;
        }
        used = copy_word(s, word, sizeof word);
        if (used == 0) {
            s++;
            continue;
        }
        s += used;
        if (plan_word_writes(word)) {
            return 1;
        }
    }
    return 0;
}

int perm_plan_bash_allow(const char *command) {
    static const char *prefixes[] = {
        "pwd", "ls", "cat", "grep", "find", "uname", "head", "tail", "wc", "true", "echo",
        "git status", "git diff", "git log", "git show", "make -n", NULL
    };
    int i;
    if (!command || has_meta(command)) {
        return 0;
    }
    if (perm_is_catastrophic(command)) {
        return 0;
    }
    for (i = 0; prefixes[i]; i++) {
        if (starts_token(command, prefixes[i])) {
            return !any_plan_write(command);
        }
    }
    return 0;
}

/* prefix matches whole, or ends at a path or URL boundary, so https://a.com does not cover https://a.com.evil. */
static int starts_bounded(const char *subject, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(subject, prefix, n) != 0) {
        return 0;
    }
    if (subject[n] == 0 || prefix[n - 1] == '/') {
        return 1;
    }
    return subject[n] == '/' || subject[n] == '?' || subject[n] == '#';
}

static int rule_matches(const AlwaysRule *r, const char *tool, const char *subject) {
    if (!str_eq(r->tool, tool)) {
        return 0;
    }
    if (!r->prefix || !r->prefix[0]) {
        return 1;
    }
    if (!subject) {
        return 0;
    }
    if (str_eq(tool, "bash")) {
        /* A rule covers one simple command, never a pipeline or a list that starts with it. */
        return !has_meta(subject) && starts_token(subject, r->prefix);
    }
    return starts_bounded(subject, r->prefix);
}

static int allowed_by_rule(const PermState *ps, const char *tool, const char *subject) {
    int i;
    for (i = 0; i < ps->nrules; i++) {
        if (rule_matches(&ps->rules[i], tool, subject)) {
            return 1;
        }
    }
    return 0;
}

static int tool_is_read(const char *tool) {
    return str_eq(tool, "read") || str_eq(tool, "grep") || str_eq(tool, "ls");
}

static int path_under(const char *root, const char *path) {
    size_t n;
    if (!root || !root[0] || !path) {
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

static int has_dotdot(const char *path) {
    const char *p;
    if (!path) {
        return 0;
    }
    for (p = path; *p; p++) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0) && (p == path || p[-1] == '/')) {
            return 1;
        }
    }
    return 0;
}

/* System directories. A write here still asks, even with sandbox off. */
static int is_sys_path(const char *path) {
    static const char *prefs[] = {
        "/etc", "/usr", "/bin", "/sbin", "/boot", "/lib", "/lib64",
        "/opt", "/var", "/sys", "/proc", "/dev", "/root", "/run", NULL
    };
    int i;
    for (i = 0; prefs[i]; i++) {
        if (path_under(prefs[i], path)) {
            return 1;
        }
    }
    return 0;
}

/* host:/abs and not a URL scheme. */
static const char *dest_path(const char *word) {
    const char *colon;
    if (!word) {
        return "";
    }
    colon = strrchr(word, ':');
    if (!colon || colon[1] != '/') {
        return word;
    }
    if (colon[2] == '/') {
        return word;
    }
    return colon + 1;
}

static int write_guarded(const char *word, const PermState *ps) {
    const char *path;
    if (!ps || !word || !word[0]) {
        return 0;
    }
    path = dest_path(word);
    if (!path[0] || path[0] == '&') {
        return 0;
    }
    if (has_dotdot(path)) {
        return ps->sandbox ? 1 : 0;
    }
    if (ps->root && path[0] == '/' && path_under(ps->root, path)) {
        return 0;
    }
    if (is_sys_path(path)) {
        return 1;
    }
    if (!ps->sandbox) {
        return 0;
    }
    if (path[0] == '~' || str_starts(path, "$HOME") || str_starts(path, "${HOME}")) {
        return 1;
    }
    return path[0] == '/';
}

/* Copies one shell word. Returns how many source bytes were consumed, including quotes. */
static size_t copy_word(const char *s, char *buf, size_t cap) {
    size_t n = 0;
    size_t used = 0;
    if (cap == 0) {
        return 0;
    }
    buf[0] = 0;
    if (*s == '\'' || *s == '"') {
        char quote = *s;
        used = 1;
        s++;
        while (*s && *s != quote) {
            if (n + 1 < cap) {
                buf[n++] = *s;
            }
            s++;
            used++;
        }
        if (*s == quote) {
            used++;
        }
        buf[n] = 0;
        return used;
    }
    while (*s && !isspace((unsigned char)*s) && *s != ';' && *s != '|' && *s != '&' && *s != '<' && *s != '>' &&
           *s != '\n') {
        if (n + 1 < cap) {
            buf[n++] = *s;
        }
        s++;
        used++;
    }
    buf[n] = 0;
    return used;
}

static int redirect_target(const char *s, const char **target);

/* Consume a redirect starting at '>'. dest is the path when is_path is set. */
static size_t take_redirect(const char *s, char *dest, size_t cap, int *is_path) {
    const char *target = NULL;
    const char *start = s;
    *is_path = 0;
    if (dest && cap) {
        dest[0] = 0;
    }
    if (!redirect_target(s, &target)) {
        s++;
        if (*s == '>' || *s == '|') {
            s++;
        }
        while (*s && isspace((unsigned char)*s)) {
            s++;
        }
        if (*s == '&') {
            s++;
            while (isdigit((unsigned char)*s)) {
                s++;
            }
            if (*s == '-') {
                s++;
            }
        }
        return (size_t)(s - start);
    }
    *is_path = 1;
    s = target + copy_word(target, dest, cap);
    return (size_t)(s - start);
}

/* s points at '>'. Returns 1 and the target word when this writes a path. */
static int redirect_target(const char *s, const char **target) {
    if (!s || *s != '>') {
        return 0;
    }
    s++;
    if (*s == '>') {
        s++;
    } else if (*s == '|') {
        s++;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '&') {
        s++;
        while (*s && isspace((unsigned char)*s)) {
            s++;
        }
        if (*s == 0 || isdigit((unsigned char)*s) || *s == '-') {
            return 0;
        }
    }
    if (*s == 0 || *s == ';' || *s == '|' || *s == '&' || *s == '<' || *s == '\n') {
        return 0;
    }
    *target = s;
    return 1;
}

static const char *base_cmd(const char *word) {
    const char *slash = word ? strrchr(word, '/') : NULL;
    if (slash && slash[1]) {
        return slash + 1;
    }
    return word ? word : "";
}

static int is_writer(const char *cmd) {
    return str_eq(cmd, "cp") || str_eq(cmd, "mv") || str_eq(cmd, "ln") || str_eq(cmd, "install") ||
           str_eq(cmd, "rsync") || str_eq(cmd, "truncate");
}

static int segment_write_guarded(const char *seg, const PermState *ps) {
    const char *p = seg ? seg : "";
    char word[1024];
    char cmd[64];
    char last[1024];
    int saw = 0;
    cmd[0] = 0;
    last[0] = 0;
    while (*p) {
        size_t used;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == 0 || *p == ';' || *p == '|' || *p == '&' || *p == '\n') {
            break;
        }
        if (*p == '>') {
            char dest[1024];
            int is_path = 0;
            used = take_redirect(p, dest, sizeof dest, &is_path);
            if (used == 0) {
                break;
            }
            if (is_path && write_guarded(dest, ps)) {
                return 1;
            }
            p += used;
            continue;
        }
        if (*p == '<') {
            p++;
            if (*p == '<') {
                p++;
            }
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            p += copy_word(p, word, sizeof word);
            continue;
        }
        used = copy_word(p, word, sizeof word);
        if (used == 0 || word[0] == 0) {
            break;
        }
        p += used;
        if (!saw) {
            if (word[0] != '=' && strchr(word, '=') && !str_starts(word, "of=")) {
                continue;
            }
            saw = 1;
            snprintf(cmd, sizeof cmd, "%s", base_cmd(word));
            continue;
        }
        if (str_eq(cmd, "dd") && str_starts(word, "of=") && write_guarded(word + 3, ps)) {
            return 1;
        }
        if (str_eq(cmd, "tee") && word[0] != '-' && write_guarded(word, ps)) {
            return 1;
        }
        if (str_eq(word, "-t") || str_eq(word, "--target-directory")) {
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            used = copy_word(p, word, sizeof word);
            p += used;
            if (word[0] && write_guarded(word, ps)) {
                return 1;
            }
            continue;
        }
        if (str_starts(word, "--target-directory=") && write_guarded(word + strlen("--target-directory="), ps)) {
            return 1;
        }
        if (word[0] != '-') {
            snprintf(last, sizeof last, "%s", word);
        }
    }
    if (saw && is_writer(cmd) && last[0] && write_guarded(last, ps)) {
        return 1;
    }
    return 0;
}

static int bash_write_guarded(const PermState *ps, const char *command) {
    const char *p = command ? command : "";
    int sq = 0;
    int dq = 0;
    const char *start = p;
    if (!command || !command[0]) {
        return 0;
    }
    while (*p) {
        if (!dq && *p == '\'') {
            sq = !sq;
        } else if (!sq && *p == '"') {
            dq = !dq;
        } else if (!sq && !dq && (*p == ';' || *p == '|' || *p == '&' || *p == '\n')) {
            char *seg = xstrndup(start, (size_t)(p - start));
            int bad = segment_write_guarded(seg, ps);
            free(seg);
            if (bad) {
                return 1;
            }
            start = p + 1;
        }
        p++;
    }
    return segment_write_guarded(start, ps);
}

PermDecision perm_decide(PermState *ps, const char *tool, const char *subject) {
    if (!tool) {
        return DEC_DENY;
    }
    if (tool_is_read(tool)) {
        return DEC_ALLOW;
    }
    if (str_eq(tool, "bash") && (perm_is_catastrophic(subject) || bash_write_guarded(ps, subject))) {
        return DEC_ASK;
    }
    if (ps->mode == MODE_PLAN) {
        if (str_eq(tool, "write") || str_eq(tool, "edit")) {
            if (perm_is_plan_file(subject)) {
                return DEC_ALLOW;
            }
            return DEC_DENY;
        }
        if (str_eq(tool, "bash")) {
            if (perm_plan_bash_allow(subject)) {
                return DEC_ALLOW;
            }
            return DEC_ASK;
        }
        if (str_eq(tool, "fetch")) {
            return DEC_ASK;
        }
        return DEC_ASK;
    }
    if (ps->mode == MODE_AUTO) {
        return DEC_ALLOW;
    }
    if (allowed_by_rule(ps, tool, subject)) {
        return DEC_ALLOW;
    }
    return DEC_ASK;
}

void perm_allow_prefix(PermState *ps, const char *tool, const char *prefix) {
    AlwaysRule *r;
    if (ps->nrules == ps->cap) {
        ps->cap = ps->cap ? ps->cap * 2 : 4;
        ps->rules = xrealloc(ps->rules, (size_t)ps->cap * sizeof(AlwaysRule));
    }
    r = &ps->rules[ps->nrules++];
    r->tool = xstrdup(tool ? tool : "");
    r->prefix = prefix ? xstrdup(prefix) : NULL;
}

/* First token of a shell command, used as the "always" prefix. */
char *perm_bash_prefix(const char *command) {
    const char *s;
    const char *e;
    if (!command) {
        return xstrdup("");
    }
    s = skip_ws(command);
    e = s;
    while (*e && !isspace((unsigned char)*e)) {
        e++;
    }
    return xstrndup(s, (size_t)(e - s));
}
