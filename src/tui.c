#include "tui.h"

#include "glyph.h"
#include "signals.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { LOG_CARD = 6, RK_REVERSE = 7, RK_RAW = 8 };

typedef struct LogLine {
    int kind;
    char *text;
    int id;
    int state;
    char *summary;
    char *body;
    int diff;
    int peek;
} LogLine;

typedef struct Editor {
    char *data;
    size_t len;
    size_t cap;
    size_t cursor;
    char **hist;
    int nhist;
    int hcap;
    int hpos;
    char *stash;
} Editor;

typedef struct KeyDec {
    int state;
    char param[16];
    int nparam;
    unsigned char raw[4];
    int nraw;
    int need;
    int paste;
} KeyDec;

typedef struct Key {
    int kind;
    unsigned char raw[4];
    int nraw;
} Key;

typedef struct Row {
    int kind;
    char *text;
} Row;

struct Ui {
    int mode;
    int rows;
    int cols;
    int color;
    int raw;
    int alt;
    struct termios orig;
    LogLine *log;
    int nlog;
    int logcap;
    int scroll;
    Buf stream;
    int streaming;
    const Glyphs *glyphs;
    int utf8;
    char *prompt;
    char *st_mode;
    char *st_provider;
    char *st_model;
    char *st_cwd;
    unsigned long st_tokens;
    unsigned long st_budget;
    int expand;
    int tool_seq;
    int log_base;
    const UiAsk *ask;
    int ask_sel;
    int ask_scroll;
    long long paint_ms;
    int dirty;
    int job_id;
    char *welcome_ver;
    char *welcome_cwd;
    char *welcome_warn;
    Editor ed;
    KeyDec dec;
    char *followup;
    int quit;
    Row *prev;
    int prev_n;
    UiKeyFn on_key;
    void *on_key_ud;
    const UiChoice *choices;
    int nchoices;
    int slash_sel;
    int menu_on;
    int menu_sel;
    const char *menu_title;
    const char *const *menu_items;
    int menu_n;
    int hide_input;
    int suspend_palette;
    int line_prompt;
    int line_cancel;
    const char *spin_label;
    time_t spin_start;
    int job_fd;
    pid_t job_pid;
    int job_killed;
    size_t job_shown;
    Buf job_acc;
};

static void job_shutdown(Ui *ui, int report);
static long long now_ms(void);
static const Glyphs *ui_glyphs(const Ui *ui);

static Ui *g_ui = NULL;

static void ed_init(Editor *e) {
    memset(e, 0, sizeof *e);
    e->cap = 128;
    e->data = xmalloc(e->cap);
    e->data[0] = 0;
    e->hpos = 0;
}

static void ed_free(Editor *e) {
    int i;
    free(e->data);
    free(e->stash);
    for (i = 0; i < e->nhist; i++) {
        free(e->hist[i]);
    }
    free(e->hist);
}

static void ed_clear(Editor *e) {
    e->len = 0;
    e->cursor = 0;
    if (e->data) {
        e->data[0] = 0;
    }
    e->hpos = e->nhist;
    free(e->stash);
    e->stash = NULL;
}

static void ed_set(Editor *e, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n + 1 > e->cap) {
        e->cap = n + 16;
        e->data = xrealloc(e->data, e->cap);
    }
    memcpy(e->data, s ? s : "", n + 1);
    e->len = n;
    e->cursor = n;
}

static void ed_insert(Editor *e, const char *s, size_t n) {
    if (e->len + n + 1 > e->cap) {
        size_t cap = e->cap ? e->cap : 128;
        while (cap < e->len + n + 1) {
            cap *= 2;
        }
        e->data = xrealloc(e->data, cap);
        e->cap = cap;
    }
    memmove(e->data + e->cursor + n, e->data + e->cursor, e->len - e->cursor + 1);
    memcpy(e->data + e->cursor, s, n);
    e->len += n;
    e->cursor += n;
}

static size_t utf8_next(const char *s, size_t i, size_t len) {
    return u8_next(s, i, len);
}

static size_t utf8_prev(const char *s, size_t i) {
    return u8_prev(s, i);
}

static void hist_add(Editor *e) {
    if (!e->data || !e->data[0]) {
        return;
    }
    if (e->nhist && str_eq(e->hist[e->nhist - 1], e->data)) {
        e->hpos = e->nhist;
        return;
    }
    if (e->nhist == e->hcap) {
        e->hcap = e->hcap ? e->hcap * 2 : 16;
        e->hist = xrealloc(e->hist, (size_t)e->hcap * sizeof(char *));
    }
    e->hist[e->nhist++] = xstrdup(e->data);
    if (e->nhist > 200) {
        free(e->hist[0]);
        memmove(e->hist, e->hist + 1, (size_t)(e->nhist - 1) * sizeof(char *));
        e->nhist--;
    }
    e->hpos = e->nhist;
}

static void ui_restore(Ui *ui) {
    if (!ui || !ui->raw) {
        return;
    }
    if (ui->alt) {
        fputs("\033[?1049l", stdout);
    }
    fputs("\033[?2004l\033[0m", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ui->orig);
    signals_set_tty(NULL);
    ui->raw = 0;
}

static void on_exit_ui(void) {
    if (g_ui) {
        ui_restore(g_ui);
    }
}

static void ui_size(Ui *ui) {
    struct winsize ws;
    int rows = 24, cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) {
            rows = ws.ws_row;
        }
        if (ws.ws_col > 0) {
            cols = ws.ws_col;
        }
    }
    if (rows != ui->rows || cols != ui->cols) {
        int i;
        for (i = 0; i < ui->prev_n; i++) {
            free(ui->prev[i].text);
        }
        free(ui->prev);
        ui->prev = NULL;
        ui->prev_n = 0;
    }
    ui->rows = rows;
    ui->cols = cols;
}

static int screen_possible(void) {
    const char *term;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return 0;
    }
    term = getenv("TERM");
    if (!term || str_eq(term, "dumb")) {
        return 0;
    }
    return 1;
}

Ui *ui_open(int mode) {
    Ui *ui = xmalloc(sizeof *ui);
    memset(ui, 0, sizeof *ui);
    ed_init(&ui->ed);
    buf_init(&ui->stream);
    buf_init(&ui->job_acc);
    ui->job_fd = -1;
    ui->glyphs = glyph_pick("auto", &ui->utf8);
    ui->prompt = xstrdup("> ");
    ui->rows = 24;
    ui->cols = 80;
    if (mode == UI_PRINT) {
        ui->mode = UI_PRINT;
        return ui;
    }
    if (mode == UI_LINE || !screen_possible()) {
        ui->mode = UI_LINE;
        return ui;
    }
    ui_size(ui);
    if (ui->rows < 8 || ui->cols < 40) {
        ui->mode = UI_LINE;
        return ui;
    }
    if (tcgetattr(STDIN_FILENO, &ui->orig) != 0) {
        ui->mode = UI_LINE;
        return ui;
    }
    {
        struct termios raw = ui->orig;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);
        raw.c_lflag |= ISIG;
        raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            ui->mode = UI_LINE;
            return ui;
        }
    }
    signals_set_tty(&ui->orig);
    ui->raw = 1;
    ui->mode = UI_SCREEN;
    ui->color = 1;
    ui->alt = 1;
    /* Bracketed paste lets a pasted newline stay a newline; terminals without it ignore the request. */
    fputs("\033[?1049h\033[?2004h\033[0m", stdout);
    fflush(stdout);
    g_ui = ui;
    atexit(on_exit_ui);
    return ui;
}

void ui_close(Ui *ui) {
    int i;
    if (!ui) {
        return;
    }
    job_shutdown(ui, 0);
    buf_free(&ui->job_acc);
    ui_restore(ui);
    if (g_ui == ui) {
        g_ui = NULL;
    }
    for (i = 0; i < ui->nlog; i++) {
        free(ui->log[i].text);
        free(ui->log[i].summary);
        free(ui->log[i].body);
    }
    free(ui->log);
    for (i = 0; i < ui->prev_n; i++) {
        free(ui->prev[i].text);
    }
    free(ui->prev);
    buf_free(&ui->stream);
    free(ui->prompt);
    free(ui->st_mode);
    free(ui->st_provider);
    free(ui->st_model);
    free(ui->st_cwd);
    free(ui->welcome_ver);
    free(ui->welcome_cwd);
    free(ui->welcome_warn);
    free(ui->followup);
    ed_free(&ui->ed);
    free(ui);
}

int ui_is_screen(const Ui *ui) {
    return ui && ui->mode == UI_SCREEN;
}

int ui_is_print(const Ui *ui) {
    return !ui || ui->mode == UI_PRINT;
}

static void copy_str(char **slot, const char *value) {
    free(*slot);
    *slot = xstrdup(value ? value : "");
}

void ui_set_status(Ui *ui, const UiStatus *st) {
    if (!ui || !st) {
        return;
    }
    copy_str(&ui->st_mode, st->mode);
    copy_str(&ui->st_provider, st->provider);
    copy_str(&ui->st_model, st->model);
    copy_str(&ui->st_cwd, st->cwd);
    ui->st_tokens = st->tokens;
    ui->st_budget = st->budget;
}

void ui_set_glyphs(Ui *ui, const char *pref) {
    int utf8 = 0;
    if (!ui) {
        return;
    }
    ui->glyphs = glyph_pick(pref, &utf8);
    ui->utf8 = utf8;
}

void ui_set_key_fn(Ui *ui, UiKeyFn fn, void *ud) {
    if (!ui) {
        return;
    }
    ui->on_key = fn;
    ui->on_key_ud = ud;
}

static void ui_invalidate(Ui *ui);

static char *prepare_text(const Ui *ui, const char *text) {
    char *clean;
    if (!text) {
        return xstrdup("");
    }
    clean = text_clean(text, strlen(text));
    if (ui && !ui->utf8) {
        char *asc = text_ascii(clean);
        free(clean);
        return asc;
    }
    return clean;
}

static void log_free_line(LogLine *line) {
    free(line->text);
    free(line->summary);
    free(line->body);
    line->text = NULL;
    line->summary = NULL;
    line->body = NULL;
}

static void log_add(Ui *ui, int kind, const char *text) {
    LogLine *line;
    if (ui->nlog > 4000) {
        log_free_line(&ui->log[0]);
        memmove(ui->log, ui->log + 1, (size_t)(ui->nlog - 1) * sizeof(LogLine));
        ui->nlog--;
        ui->log_base++;
    }
    if (ui->nlog == ui->logcap) {
        ui->logcap = ui->logcap ? ui->logcap * 2 : 32;
        ui->log = xrealloc(ui->log, (size_t)ui->logcap * sizeof(LogLine));
    }
    line = &ui->log[ui->nlog++];
    memset(line, 0, sizeof *line);
    line->kind = kind;
    line->text = kind == LOG_CARD ? xstrdup("") : prepare_text(ui, text);
    line->peek = 4;
    ui->scroll = 0;
}

static LogLine *log_by_id(Ui *ui, int id) {
    int i;
    if (!ui || id <= 0) {
        return NULL;
    }
    for (i = ui->nlog - 1; i >= 0; i--) {
        if (ui->log[i].id == id) {
            return &ui->log[i];
        }
    }
    return NULL;
}

static void write_gutter(FILE *fp, const char *gutter, const char *text) {
    const char *p = text ? text : "";
    if (!p[0]) {
        fprintf(fp, "%s\n", gutter);
        return;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(fp, "%s%.*s\n", gutter, n, p);
        if (!nl) {
            break;
        }
        p = nl + 1;
        if (!*p) {
            break;
        }
    }
}

void ui_log(Ui *ui, int kind, const char *text) {
    if (!ui) {
        return;
    }
    if (ui->mode == UI_PRINT) {
        if (kind == UI_ERR) {
            fprintf(stderr, "%s\n", text ? text : "");
        } else if (kind == UI_USER) {
            return;
        } else if (kind == UI_TOOL) {
            fprintf(stdout, "\n[%s]\n", text ? text : "");
        } else {
            fprintf(stdout, "%s\n", text ? text : "");
        }
        fflush(stdout);
        return;
    }
    if (ui->mode == UI_LINE) {
        if (kind == UI_ERR) {
            write_gutter(stderr, "! ", text);
        } else if (kind == UI_USER) {
            write_gutter(stdout, "> ", text);
        } else if (kind == UI_TOOL) {
            write_gutter(stdout, "* ", text);
        } else if (kind == UI_ASSIST) {
            write_gutter(stdout, "  ", text);
        } else {
            write_gutter(stdout, "  ", text);
        }
        fflush(stdout);
        return;
    }
    log_add(ui, kind, text);
    ui_present(ui);
}

int ui_log_mark(const Ui *ui) {
    if (!ui || ui->mode != UI_SCREEN) {
        return -1;
    }
    return ui->log_base + ui->nlog;
}

void ui_log_trim(Ui *ui, int mark) {
    int i;
    int idx;
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    idx = mark - ui->log_base;
    if (idx > ui->nlog) {
        return;
    }
    if (idx < 0) {
        idx = 0;
    }
    for (i = idx; i < ui->nlog; i++) {
        log_free_line(&ui->log[i]);
    }
    ui->nlog = idx;
    ui_invalidate(ui);
    ui_present(ui);
}

void ui_stream_begin(Ui *ui) {
    if (!ui) {
        return;
    }
    buf_clear(&ui->stream);
    ui->streaming = 1;
}

void ui_stream_append(Ui *ui, const char *s, size_t n) {
    if (!ui || !s || n == 0) {
        return;
    }
    if (ui->mode == UI_PRINT || ui->mode == UI_LINE) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
        return;
    }
    buf_append(&ui->stream, s, n);
    ui->scroll = 0;
    if (now_ms() - ui->paint_ms >= 50) {
        ui_present(ui);
    } else {
        ui->dirty = 1;
    }
}

void ui_stream_end(Ui *ui) {
    if (!ui) {
        return;
    }
    ui->streaming = 0;
    ui->spin_label = NULL;
    if (ui->mode == UI_PRINT || ui->mode == UI_LINE) {
        fputc('\n', stdout);
        fflush(stdout);
        buf_clear(&ui->stream);
        return;
    }
    if (ui->stream.len) {
        log_add(ui, UI_ASSIST, ui->stream.data);
    }
    buf_clear(&ui->stream);
    ui_present(ui);
}

static int key_feed(KeyDec *d, unsigned char c, Key *out) {
    memset(out, 0, sizeof *out);
    if (d->state == 4) {
        d->raw[d->nraw++] = c;
        if (d->nraw >= d->need) {
            out->kind = KEY_CHAR;
            memcpy(out->raw, d->raw, (size_t)d->nraw);
            out->nraw = d->nraw;
            d->state = 0;
            d->nraw = 0;
            return 1;
        }
        return 0;
    }
    if (d->state == 1) {
        if (c == '[') {
            d->state = 2;
            d->nparam = 0;
            return 0;
        }
        if (c == 'O') {
            d->state = 3;
            return 0;
        }
        if (c == '\r' || c == '\n') {
            d->state = 0;
            out->kind = KEY_ALT_ENTER;
            return 1;
        }
        d->state = 0;
        /* Alt+letter must not cancel. Swallow the printable. */
        if (c >= 0x20 && c < 0x7f) {
            return 0;
        }
        out->kind = KEY_ESC;
        return 1;
    }
    if (d->state == 3) {
        d->state = 0;
        if (c == 'A') {
            out->kind = KEY_UP;
        } else if (c == 'B') {
            out->kind = KEY_DOWN;
        } else if (c == 'C') {
            out->kind = KEY_RIGHT;
        } else if (c == 'D') {
            out->kind = KEY_LEFT;
        } else if (c == 'H') {
            out->kind = KEY_HOME;
        } else if (c == 'F') {
            out->kind = KEY_END;
        } else {
            return 0;
        }
        return 1;
    }
    if (d->state == 2) {
        if ((c >= '0' && c <= '9') || c == ';') {
            if (d->nparam < (int)sizeof d->param - 1) {
                d->param[d->nparam++] = (char)c;
            }
            return 0;
        }
        d->param[d->nparam] = 0;
        d->state = 0;
        if (c == 'A') {
            out->kind = KEY_UP;
        } else if (c == 'B') {
            out->kind = KEY_DOWN;
        } else if (c == 'C') {
            out->kind = KEY_RIGHT;
        } else if (c == 'D') {
            out->kind = KEY_LEFT;
        } else if (c == 'H') {
            out->kind = KEY_HOME;
        } else if (c == 'F') {
            out->kind = KEY_END;
        } else if (c == 'Z') {
            out->kind = KEY_SHIFT_TAB;
        } else if (c == 'u' && (str_eq(d->param, "13;3") || str_starts(d->param, "13;3;"))) {
            out->kind = KEY_ALT_ENTER;
        } else if (c == '~') {
            /* Bracketed paste: ESC [200~ text ESC [201~. */
            if (str_eq(d->param, "200") || str_eq(d->param, "201")) {
                d->paste = str_eq(d->param, "200");
                return 0;
            }
            if (str_eq(d->param, "27;3;13") || str_starts(d->param, "27;3;13;")) {
                out->kind = KEY_ALT_ENTER;
            } else if (str_eq(d->param, "3") || str_starts(d->param, "3;")) {
                out->kind = KEY_DELETE;
            } else if (str_eq(d->param, "5") || str_starts(d->param, "5;")) {
                out->kind = KEY_PGUP;
            } else if (str_eq(d->param, "6") || str_starts(d->param, "6;")) {
                out->kind = KEY_PGDN;
            } else if (str_eq(d->param, "1") || str_eq(d->param, "7")) {
                out->kind = KEY_HOME;
            } else if (str_eq(d->param, "4") || str_eq(d->param, "8")) {
                out->kind = KEY_END;
            } else {
                return 0;
            }
        } else {
            return 0;
        }
        return 1;
    }
    if (c == 0x1b) {
        d->state = 1;
        return 0;
    }
    if (c == '\r' || c == '\n') {
        out->kind = KEY_ENTER;
        return 1;
    }
    if (c == 0x7f || c == 0x08) {
        out->kind = KEY_BACKSPACE;
        return 1;
    }
    if (c == 0x01) {
        out->kind = KEY_CTRL_A;
        return 1;
    }
    if (c == 0x04) {
        out->kind = KEY_CTRL_D;
        return 1;
    }
    if (c == 0x05) {
        out->kind = KEY_CTRL_E;
        return 1;
    }
    if (c == 0x0b) {
        out->kind = KEY_CTRL_K;
        return 1;
    }
    if (c == 0x0c) {
        out->kind = KEY_CTRL_L;
        return 1;
    }
    if (c == 0x0f) {
        out->kind = KEY_CTRL_O;
        return 1;
    }
    if (c == 0x14) {
        out->kind = KEY_CTRL_T;
        return 1;
    }
    if (c == 0x15) {
        out->kind = KEY_CTRL_U;
        return 1;
    }
    if (c == 0x17) {
        out->kind = KEY_CTRL_W;
        return 1;
    }
    if (c == 0x18) {
        out->kind = KEY_CTRL_X;
        return 1;
    }
    if (c == '\t') {
        out->kind = KEY_TAB;
        return 1;
    }
    if (c < 0x20) {
        return 0;
    }
    if ((c & 0x80) == 0) {
        out->kind = KEY_CHAR;
        out->raw[0] = c;
        out->nraw = 1;
        return 1;
    }
    d->state = 4;
    d->raw[0] = c;
    d->nraw = 1;
    if ((c & 0xe0) == 0xc0) {
        d->need = 2;
    } else if ((c & 0xf0) == 0xe0) {
        d->need = 3;
    } else if ((c & 0xf8) == 0xf0) {
        d->need = 4;
    } else {
        d->state = 0;
        out->kind = KEY_CHAR;
        out->raw[0] = c;
        out->nraw = 1;
        return 1;
    }
    return 0;
}

/* A lone ESC is a key. Arrow keys are ESC plus more bytes in the same instant. */
static int key_flush_esc(KeyDec *d, Key *out) {
    if (!d || d->state != 1) {
        return 0;
    }
    for (;;) {
        fd_set rset;
        struct timeval tv;
        int rc;
        FD_ZERO(&rset);
        FD_SET(STDIN_FILENO, &rset);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        rc = select(STDIN_FILENO + 1, &rset, NULL, NULL, &tv);
        if (rc == 0) {
            d->state = 0;
            memset(out, 0, sizeof *out);
            out->kind = KEY_ESC;
            return 1;
        }
        if (rc > 0 || errno != EINTR) {
            return 0;
        }
    }
}

static int slash_typing(const Ui *ui) {
    const char *s;
    if (!ui || ui->suspend_palette || ui->menu_on) {
        return 0;
    }
    if (!ui->choices || ui->nchoices <= 0) {
        return 0;
    }
    s = ui->ed.data;
    if (!s || s[0] != '/' || strchr(s, ' ')) {
        return 0;
    }
    return 1;
}

static int slash_matches(const Ui *ui, int *idx, int maxn) {
    int i, n = 0;
    const char *typed = ui->ed.data ? ui->ed.data : "";
    for (i = 0; i < ui->nchoices && n < maxn; i++) {
        if (ui->choices[i].label && str_starts(ui->choices[i].label, typed)) {
            idx[n++] = i;
        }
    }
    return n;
}

static void accept_slash(Ui *ui) {
    int idx[32];
    int n = slash_matches(ui, idx, 32);
    int sel;
    if (n <= 0) {
        return;
    }
    sel = ui->slash_sel;
    if (sel < 0 || sel >= n) {
        sel = 0;
    }
    ed_set(&ui->ed, ui->choices[idx[sel]].label);
}

static void ui_invalidate(Ui *ui) {
    int i;
    for (i = 0; i < ui->prev_n; i++) {
        free(ui->prev[i].text);
        ui->prev[i].text = NULL;
        ui->prev[i].kind = -1;
    }
}

static void queue_followup(Ui *ui) {
    if (!ui->ed.len) {
        return;
    }
    if (ui->followup && ui->followup[0]) {
        size_t n = strlen(ui->followup) + ui->ed.len + 2;
        char *next = xmalloc(n);
        sprintf(next, "%s\n%s", ui->followup, ui->ed.data);
        free(ui->followup);
        ui->followup = next;
    } else {
        free(ui->followup);
        ui->followup = xstrdup(ui->ed.data);
    }
    ed_clear(&ui->ed);
}

static int handle_key(Ui *ui, const Key *key, int streaming) {
    Editor *e = &ui->ed;
    if (key->kind == KEY_CTRL_T || key->kind == KEY_SHIFT_TAB || key->kind == KEY_CTRL_L) {
        if (key->kind == KEY_CTRL_L) {
            int i;
            for (i = 0; i < ui->prev_n; i++) {
                free(ui->prev[i].text);
                ui->prev[i].text = NULL;
            }
        }
        if (ui->on_key && key->kind != KEY_CTRL_L) {
            ui->on_key(ui->on_key_ud, key->kind);
        }
        return 0;
    }
    if (key->kind == KEY_CTRL_O) {
        ui->expand = !ui->expand;
        return 0;
    }
    if (key->kind == KEY_CTRL_X) {
        job_shutdown(ui, 1);
        return 0;
    }
    if (key->kind == KEY_ALT_ENTER) {
        if (!ui->hide_input && !ui->line_prompt) {
            ed_insert(e, "\n", 1);
        }
        return 0;
    }
    if (key->kind == KEY_CTRL_D) {
        if (e->len == 0) {
            ui->quit = 1;
            if (streaming) {
                g_sigint = 1;
            }
            return 1;
        }
        if (e->cursor < e->len) {
            size_t n = utf8_next(e->data, e->cursor, e->len);
            memmove(e->data + e->cursor, e->data + n, e->len - n + 1);
            e->len -= n - e->cursor;
        }
        return 0;
    }
    if (key->kind == KEY_ESC) {
        if (ui->line_prompt) {
            ed_clear(e);
            ui->line_cancel = 1;
            return 1;
        }
        if (e->data && str_eq(e->data, "?")) {
            ed_clear(e);
            return 0;
        }
        if (e->data && e->data[0] == '/') {
            ed_clear(e);
            ui->slash_sel = 0;
            return 0;
        }
        if (streaming && e->len == 0) {
            g_sigint = 1;
        }
        return 0;
    }
    if (key->kind == KEY_TAB) {
        if (slash_typing(ui)) {
            accept_slash(ui);
        }
        return 0;
    }
    if (key->kind == KEY_ENTER) {
        if (e->data && str_eq(e->data, "?")) {
            ed_clear(e);
            return 0;
        }
        if (streaming) {
            queue_followup(ui);
            return 0;
        }
        if (slash_typing(ui)) {
            accept_slash(ui);
        }
        hist_add(e);
        return 1;
    }
    if (key->kind == KEY_BACKSPACE) {
        ui->slash_sel = 0;
        if (e->cursor > 0) {
            size_t p = utf8_prev(e->data, e->cursor);
            memmove(e->data + p, e->data + e->cursor, e->len - e->cursor + 1);
            e->len -= e->cursor - p;
            e->cursor = p;
        }
        return 0;
    }
    if (key->kind == KEY_DELETE) {
        ui->slash_sel = 0;
        if (e->cursor < e->len) {
            size_t n = utf8_next(e->data, e->cursor, e->len);
            memmove(e->data + e->cursor, e->data + n, e->len - n + 1);
            e->len -= n - e->cursor;
        }
        return 0;
    }
    if (key->kind == KEY_LEFT) {
        e->cursor = utf8_prev(e->data, e->cursor);
        return 0;
    }
    if (key->kind == KEY_RIGHT) {
        e->cursor = utf8_next(e->data, e->cursor, e->len);
        return 0;
    }
    if (key->kind == KEY_HOME || key->kind == KEY_CTRL_A) {
        e->cursor = 0;
        return 0;
    }
    if (key->kind == KEY_END || key->kind == KEY_CTRL_E) {
        e->cursor = e->len;
        return 0;
    }
    if (key->kind == KEY_CTRL_U) {
        ui->slash_sel = 0;
        memmove(e->data, e->data + e->cursor, e->len - e->cursor + 1);
        e->len -= e->cursor;
        e->cursor = 0;
        return 0;
    }
    if (key->kind == KEY_CTRL_K) {
        ui->slash_sel = 0;
        e->data[e->cursor] = 0;
        e->len = e->cursor;
        return 0;
    }
    if (key->kind == KEY_CTRL_W) {
        size_t p = e->cursor;
        ui->slash_sel = 0;
        while (p > 0 && e->data[p - 1] == ' ') {
            p--;
        }
        while (p > 0 && e->data[p - 1] != ' ') {
            p--;
        }
        memmove(e->data + p, e->data + e->cursor, e->len - e->cursor + 1);
        e->len -= e->cursor - p;
        e->cursor = p;
        return 0;
    }
    if (key->kind == KEY_UP || key->kind == KEY_DOWN) {
        if (slash_typing(ui)) {
            int idx[32];
            int n = slash_matches(ui, idx, 32);
            if (n > 0) {
                if (ui->slash_sel < 0 || ui->slash_sel >= n) {
                    ui->slash_sel = 0;
                }
                if (key->kind == KEY_UP) {
                    ui->slash_sel = (ui->slash_sel + n - 1) % n;
                } else {
                    ui->slash_sel = (ui->slash_sel + 1) % n;
                }
            }
            return 0;
        }
        if (key->kind == KEY_UP) {
            if (e->hpos == e->nhist) {
                free(e->stash);
                e->stash = xstrdup(e->data);
            }
            if (e->hpos > 0) {
                e->hpos--;
                ed_set(e, e->hist[e->hpos]);
            }
        } else if (e->hpos < e->nhist) {
            e->hpos++;
            if (e->hpos == e->nhist) {
                ed_set(e, e->stash ? e->stash : "");
            } else {
                ed_set(e, e->hist[e->hpos]);
            }
        }
        return 0;
    }
    if (key->kind == KEY_PGUP) {
        if (ui->ask) {
            ui->ask_scroll += ui->rows > 4 ? ui->rows - 4 : 1;
            return 0;
        }
        ui->scroll += ui->rows > 4 ? ui->rows - 4 : 1;
        return 0;
    }
    if (key->kind == KEY_PGDN) {
        if (ui->ask) {
            ui->ask_scroll -= ui->rows > 4 ? ui->rows - 4 : 1;
            if (ui->ask_scroll < 0) {
                ui->ask_scroll = 0;
            }
            return 0;
        }
        ui->scroll -= ui->rows > 4 ? ui->rows - 4 : 1;
        if (ui->scroll < 0) {
            ui->scroll = 0;
        }
        return 0;
    }
    if (key->kind == KEY_CHAR && key->nraw > 0) {
        ui->slash_sel = 0;
        if (e->data && str_eq(e->data, "?") && key->nraw == 1 && key->raw[0] == '/') {
            ed_clear(e);
        }
        ed_insert(e, (const char *)key->raw, (size_t)key->nraw);
    }
    return 0;
}

static int input_pending(int wait_ms) {
    fd_set rset;
    struct timeval tv;
    FD_ZERO(&rset);
    FD_SET(STDIN_FILENO, &rset);
    tv.tv_sec = 0;
    tv.tv_usec = wait_ms * 1000;
    return select(STDIN_FILENO + 1, &rset, NULL, NULL, &tv) > 0;
}

/*
 * An Enter inside a paste is a newline in the prompt, not a submit. Terminals with bracketed paste mark the
 * paste; for the rest, an Enter with more text right behind it in the same burst counts as pasted.
 */
static int pasted_newline(const Ui *ui, const unsigned char *buf, ssize_t n, ssize_t i) {
    ssize_t k;
    if (ui->hide_input || ui->line_prompt) {
        return 0;
    }
    if (ui->dec.paste) {
        return 1;
    }
    for (k = i + 1; k < n; k++) {
        if (buf[k] != '\r' && buf[k] != '\n') {
            return 1;
        }
    }
    return n > 1 && input_pending(n == 256 ? 20 : 0);
}

static int read_keys(Ui *ui, int streaming) {
    unsigned char buf[256];
    ssize_t n;
    ssize_t i;
    int submitted = 0;
    n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) {
        if (n == 0) {
            ui->quit = 1;
            submitted = 1;
        }
        return submitted;
    }
    for (i = 0; i < n; i++) {
        Key key;
        if (!key_feed(&ui->dec, buf[i], &key)) {
            continue;
        }
        if (key.kind == KEY_ENTER && pasted_newline(ui, buf, n, i)) {
            if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n') {
                i++;
            }
            ed_insert(&ui->ed, "\n", 1);
            continue;
        }
        if (handle_key(ui, &key, streaming)) {
            submitted = 1;
            if (!streaming) {
                /* Bytes after a submit belong to no line; appending them would corrupt this one. */
                break;
            }
        }
    }
    if (ui->dec.state == 1) {
        Key key;
        if (key_flush_esc(&ui->dec, &key) && handle_key(ui, &key, streaming)) {
            submitted = 1;
        }
    }
    return submitted;
}

static void push_row(Row **rows, int *n, int *cap, int kind, const char *text) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *rows = xrealloc(*rows, (size_t)(*cap) * sizeof(Row));
    }
    (*rows)[*n].kind = kind;
    (*rows)[*n].text = xstrdup(text ? text : "");
    (*n)++;
}

static void gutter_of(int kind, char *out, int *glen) {
    out[0] = 0;
    *glen = 0;
    if (kind == UI_USER) {
        out[0] = '>';
    } else if (kind == UI_TOOL) {
        out[0] = '*';
    } else if (kind == UI_ERR) {
        out[0] = '!';
    } else if (kind == UI_ASSIST || kind == UI_META) {
        out[0] = ' ';
    } else {
        return;
    }
    out[1] = ' ';
    out[2] = 0;
    *glen = 2;
}

static void push_piece(Row **rows, int *n, int *cap, int kind, const char *gutter, const char *piece) {
    size_t gl = gutter ? strlen(gutter) : 0;
    size_t pl = piece ? strlen(piece) : 0;
    char *line = xmalloc(gl + pl + 1);
    if (gl) {
        memcpy(line, gutter, gl);
    }
    if (pl) {
        memcpy(line + gl, piece, pl);
    }
    line[gl + pl] = 0;
    push_row(rows, n, cap, kind, line);
    free(line);
}

static int disp_cols_n(const char *s, size_t n) {
    return cols_n(s, n);
}

static char *clip_cols(const char *text, int width) {
    if (width < 1) {
        width = 1;
    }
    return cols_clip(text, width, "");
}

typedef struct {
    int style;
    const char *s;
    size_t n;
} Span;

enum {
    ST_PLAIN = 0,
    ST_BOLD = 1,
    ST_EM = 2,
    ST_CODE = 3,
    ST_LINK = 4,
    ST_H1 = 5,
    ST_H2 = 6,
    ST_DIM = 7,
    ST_KW = 8,
    ST_NUM = 9
};

static const char *span_ansi(int style) {
    switch (style) {
    case ST_BOLD:
        return "\033[1m";
    case ST_EM:
        return "\033[4m";
    case ST_CODE:
        return "\033[33m";
    case ST_LINK:
        return "\033[4;36m";
    case ST_H1:
        return "\033[1;36m";
    case ST_H2:
        return "\033[36m";
    case ST_DIM:
        return "\033[2m";
    case ST_KW:
        return "\033[1;36m";
    case ST_NUM:
        return "\033[35m";
    default:
        return "";
    }
}

static size_t find_mark(const char *s, size_t n, size_t from, const char *mark, size_t mlen) {
    size_t i;
    if (mlen == 0 || from > n) {
        return (size_t)-1;
    }
    for (i = from; i + mlen <= n; i++) {
        if (memcmp(s + i, mark, mlen) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static void span_add(Span *sp, int *nsp, int maxn, int style, const char *s, size_t n) {
    if (n == 0) {
        return;
    }
    if (*nsp > 0 && sp[*nsp - 1].style == style && sp[*nsp - 1].s + sp[*nsp - 1].n == s) {
        sp[*nsp - 1].n += n;
        return;
    }
    if (*nsp >= maxn) {
        return;
    }
    sp[*nsp].style = style;
    sp[*nsp].s = s;
    sp[*nsp].n = n;
    (*nsp)++;
}

static int word_byte(char c) {
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

/*
 * Closing mark for the * or _ at i, after CommonMark's flanking rules: no space just inside either mark, and
 * _ never opens or closes inside a word, so snake_case names keep their underscores.
 */
static size_t find_em_close(const char *s, size_t n, size_t i) {
    char m = s[i];
    size_t j;
    if (i + 1 >= n || isspace((unsigned char)s[i + 1])) {
        return (size_t)-1;
    }
    if (m == '_' && i > 0 && word_byte(s[i - 1])) {
        return (size_t)-1;
    }
    for (j = i + 2; j < n; j++) {
        if (s[j] != m || isspace((unsigned char)s[j - 1])) {
            continue;
        }
        if (m == '_' && j + 1 < n && word_byte(s[j + 1])) {
            continue;
        }
        return j;
    }
    return (size_t)-1;
}

static int parse_inline(const char *s, size_t n, Span *sp, int maxn) {
    size_t i = 0;
    int nsp = 0;
    while (i < n && nsp < maxn - 1) {
        if (s[i] == '`') {
            size_t j = find_mark(s, n, i + 1, "`", 1);
            if (j != (size_t)-1) {
                span_add(sp, &nsp, maxn, ST_CODE, s + i + 1, j - (i + 1));
                i = j + 1;
                continue;
            }
        }
        if (i + 1 < n && s[i] == '*' && s[i + 1] == '*') {
            size_t j = find_mark(s, n, i + 2, "**", 2);
            if (j != (size_t)-1) {
                span_add(sp, &nsp, maxn, ST_BOLD, s + i + 2, j - (i + 2));
                i = j + 2;
                continue;
            }
        }
        if (s[i] == '*' || s[i] == '_') {
            size_t j = find_em_close(s, n, i);
            if (j != (size_t)-1) {
                span_add(sp, &nsp, maxn, ST_EM, s + i + 1, j - (i + 1));
                i = j + 1;
                continue;
            }
        }
        if (s[i] == '[') {
            size_t rb = find_mark(s, n, i + 1, "](", 2);
            size_t end;
            if (rb != (size_t)-1) {
                end = find_mark(s, n, rb + 2, ")", 1);
                if (end != (size_t)-1) {
                    span_add(sp, &nsp, maxn, ST_LINK, s + i + 1, rb - (i + 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        {
            size_t start = i;
            i++;
            while (i < n && s[i] != '`' && s[i] != '*' && s[i] != '_' && s[i] != '[') {
                i++;
            }
            span_add(sp, &nsp, maxn, ST_PLAIN, s + start, i - start);
        }
    }
    if (i < n) {
        span_add(sp, &nsp, maxn, ST_PLAIN, s + i, n - i);
    }
    return nsp;
}

static void flush_vis(Row **rows, int *n, int *cap, int kind, Buf *line, int *styled) {
    if (*styled) {
        buf_puts(line, "\033[0m");
        *styled = 0;
    }
    push_row(rows, n, cap, kind, line->data ? line->data : "");
    buf_clear(line);
}

static void emit_spans(Row **rows, int *n, int *cap, int kind, const char *prefix, const char *cont,
                       const Span *sp, int nsp, int width) {
    Buf line;
    int col;
    int styled = 0;
    int cur = -1;
    int si;
    const char *pre = prefix ? prefix : "";
    const char *con = cont ? cont : "";
    if (width < 4) {
        width = 4;
    }
    buf_init(&line);
    buf_puts(&line, pre);
    col = disp_cols_n(pre, strlen(pre));
    for (si = 0; si < nsp; si++) {
        size_t off = 0;
        while (off < sp[si].n) {
            int room = width - col;
            size_t skip = 0;
            size_t take;
            const char *ansi;
            if (room <= 1) {
                flush_vis(rows, n, cap, kind, &line, &styled);
                buf_puts(&line, con);
                col = disp_cols_n(con, strlen(con));
                cur = -1;
                styled = 0;
                room = width - col;
                if (room < 1) {
                    room = 1;
                }
            }
            take = cols_break(sp[si].s + off, sp[si].n - off, room, &skip);
            if (take == 0) {
                size_t next = u8_next(sp[si].s, off, sp[si].n);
                take = next > off ? next - off : 1;
                skip = 0;
            }
            ansi = span_ansi(sp[si].style);
            if (sp[si].style != cur) {
                if (styled) {
                    buf_puts(&line, "\033[0m");
                }
                if (ansi[0]) {
                    buf_puts(&line, ansi);
                    styled = 1;
                } else {
                    styled = 0;
                }
                cur = sp[si].style;
            }
            buf_append(&line, sp[si].s + off, take);
            off += take + skip;
            col = disp_cols_n(line.data, line.len);
        }
    }
    if (line.len || nsp == 0) {
        flush_vis(rows, n, cap, kind, &line, &styled);
    }
    buf_free(&line);
}

static int is_hr(const char *s, size_t n) {
    size_t i = 0;
    int marks = 0;
    char kind = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i >= n) {
        return 0;
    }
    kind = s[i];
    if (kind != '-' && kind != '*' && kind != '_') {
        return 0;
    }
    for (; i < n; i++) {
        if (s[i] == ' ' || s[i] == '\t') {
            continue;
        }
        if (s[i] != kind) {
            return 0;
        }
        marks++;
    }
    return marks >= 3;
}

static int is_table_rule(const char *s, size_t n) {
    size_t i;
    int pipes = 0;
    int dashes = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == ':' ) {
            continue;
        }
        if (c == '|') {
            pipes++;
            continue;
        }
        if (c == '-') {
            dashes++;
            continue;
        }
        return 0;
    }
    return pipes > 0 && dashes >= 3;
}

enum { LANG_TEXT = 0, LANG_C = 1, LANG_PY = 2, LANG_SH = 3, LANG_JSON = 4 };

static int kw_hit(const char *const *list, const char *s, size_t n) {
    int i;
    for (i = 0; list[i]; i++) {
        size_t m = strlen(list[i]);
        if (m == n && memcmp(s, list[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_keyword(int lang, const char *s, size_t n) {
    static const char *const ckw[] = {
        "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default",
        "struct", "enum", "union", "typedef", "sizeof", "const", "static", "extern", "volatile", "register",
        "goto", "void", "int", "char", "long", "short", "unsigned", "signed", "float", "double", "include",
        "define", "ifdef", "ifndef", "endif", "elif", "pragma", "function", "var", "let", "new", "this",
        "async", "await", "export", "import", "class", "public", "private", "return", NULL};
    static const char *const pykw[] = {
        "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from", "as", "try",
        "except", "finally", "with", "pass", "break", "continue", "lambda", "yield", "None", "True", "False",
        "and", "or", "not", "in", "is", NULL};
    static const char *const shkw[] = {
        "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "in", "function",
        "return", "local", "export", "unset", NULL};
    static const char *const jskw[] = {"true", "false", "null", NULL};
    if (lang == LANG_C) {
        return kw_hit(ckw, s, n);
    }
    if (lang == LANG_PY) {
        return kw_hit(pykw, s, n);
    }
    if (lang == LANG_SH) {
        return kw_hit(shkw, s, n);
    }
    if (lang == LANG_JSON) {
        return kw_hit(jskw, s, n);
    }
    return 0;
}

static int ident_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ident_char(unsigned char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

static int lang_of(const char *s, size_t n) {
    char buf[16];
    size_t i;
    size_t m = 0;
    for (i = 0; i < n && m + 1 < sizeof buf; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == ',' || c == '{') {
            break;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        buf[m++] = (char)c;
    }
    buf[m] = 0;
    if (!m || str_eq(buf, "text") || str_eq(buf, "txt") || str_eq(buf, "md") || str_eq(buf, "markdown")) {
        return LANG_TEXT;
    }
    if (str_eq(buf, "py") || str_eq(buf, "python")) {
        return LANG_PY;
    }
    if (str_eq(buf, "sh") || str_eq(buf, "bash") || str_eq(buf, "zsh") || str_eq(buf, "shell")) {
        return LANG_SH;
    }
    if (str_eq(buf, "json")) {
        return LANG_JSON;
    }
    return LANG_C;
}

static size_t scan_string(const char *s, size_t n, size_t i, char q) {
    i++;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (s[i] == q) {
            return i + 1;
        }
        i++;
    }
    return n;
}

static int highlight_line(const char *s, size_t n, int lang, int *in_cmt, Span *sp, int maxn) {
    size_t i = 0;
    int nsp = 0;
    if (lang == LANG_TEXT) {
        if (n) {
            span_add(sp, &nsp, maxn, ST_PLAIN, s, n);
        }
        return nsp;
    }
    if (in_cmt && *in_cmt && lang == LANG_C) {
        size_t j = find_mark(s, n, 0, "*/", 2);
        if (j == (size_t)-1) {
            span_add(sp, &nsp, maxn, ST_DIM, s, n);
            return nsp;
        }
        span_add(sp, &nsp, maxn, ST_DIM, s, j + 2);
        i = j + 2;
        *in_cmt = 0;
    }
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (nsp >= maxn - 1) {
            span_add(sp, &nsp, maxn, ST_PLAIN, s + i, n - i);
            break;
        }
        if (c == ' ' || c == '\t') {
            size_t start = i;
            while (i < n && (s[i] == ' ' || s[i] == '\t')) {
                i++;
            }
            span_add(sp, &nsp, maxn, ST_PLAIN, s + start, i - start);
            continue;
        }
        if (lang == LANG_C && i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            span_add(sp, &nsp, maxn, ST_DIM, s + i, n - i);
            break;
        }
        if ((lang == LANG_PY || lang == LANG_SH) && c == '#') {
            span_add(sp, &nsp, maxn, ST_DIM, s + i, n - i);
            break;
        }
        if (lang == LANG_C && i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            size_t j = find_mark(s, n, i + 2, "*/", 2);
            if (j == (size_t)-1) {
                span_add(sp, &nsp, maxn, ST_DIM, s + i, n - i);
                if (in_cmt) {
                    *in_cmt = 1;
                }
                break;
            }
            span_add(sp, &nsp, maxn, ST_DIM, s + i, j + 2 - i);
            i = j + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t end = scan_string(s, n, i, (char)c);
            span_add(sp, &nsp, maxn, ST_CODE, s + i, end - i);
            i = end;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            size_t start = i;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                while (i < n && isxdigit((unsigned char)s[i])) {
                    i++;
                }
            } else {
                int dot = 0;
                while (i < n) {
                    unsigned char d = (unsigned char)s[i];
                    if (d >= '0' && d <= '9') {
                        i++;
                        continue;
                    }
                    if (d == '.' && !dot) {
                        dot = 1;
                        i++;
                        continue;
                    }
                    break;
                }
            }
            span_add(sp, &nsp, maxn, ST_NUM, s + start, i - start);
            continue;
        }
        if (ident_start(c)) {
            size_t start = i;
            int style = ST_PLAIN;
            while (i < n && ident_char((unsigned char)s[i])) {
                i++;
            }
            if (is_keyword(lang, s + start, i - start)) {
                style = ST_KW;
            }
            span_add(sp, &nsp, maxn, style, s + start, i - start);
            continue;
        }
        if (lang == LANG_C && c == '#' && i == 0) {
            size_t start = i;
            i++;
            while (i < n && ident_char((unsigned char)s[i])) {
                i++;
            }
            span_add(sp, &nsp, maxn, ST_KW, s + start, i - start);
            continue;
        }
        span_add(sp, &nsp, maxn, ST_PLAIN, s + i, 1);
        i++;
    }
    return nsp;
}

/* Reply layout follows Grok Build's scrollback (Apache-2.0). See NOTICE. */
static void cont_spaces(const char *prefix, char *out, size_t cap) {
    int pc = disp_cols_n(prefix, prefix ? strlen(prefix) : 0);
    if (pc < 0) {
        pc = 0;
    }
    if (pc > (int)cap - 1) {
        pc = (int)cap - 1;
    }
    memset(out, ' ', (size_t)pc);
    out[pc] = 0;
}

static void md_render(Row **rows, int *n, int *cap, const char *text, int width, const Glyphs *g) {
    const char *p = text ? text : "";
    int in_fence = 0;
    int fence_lang = LANG_TEXT;
    int in_cmt = 0;
    int blank = 1;
    if (!p[0]) {
        push_row(rows, n, cap, UI_ASSIST, "");
        return;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        const char *s = p;
        size_t nlen = seglen;
        while (nlen && (s[nlen - 1] == '\r')) {
            nlen--;
        }
        if (!in_fence && nlen >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
            size_t lang = 3;
            in_fence = 1;
            in_cmt = 0;
            while (lang < nlen && s[lang] == ' ') {
                lang++;
            }
            fence_lang = lang_of(s + lang, nlen - lang);
            if (lang < nlen) {
                Span one;
                one.style = ST_DIM;
                one.s = s + lang;
                one.n = nlen - lang;
                emit_spans(rows, n, cap, UI_ASSIST, "", "", &one, 1, width);
                blank = 0;
            }
        } else if (in_fence) {
            if (nlen >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
                in_fence = 0;
                in_cmt = 0;
            } else {
                Span sp[256];
                char fence[48];
                int nsp;
                snprintf(fence, sizeof fence, "\033[36m%s \033[0m", g && g->v ? g->v : "|");
                nsp = highlight_line(s, nlen, fence_lang, &in_cmt, sp, 256);
                emit_spans(rows, n, cap, UI_ASSIST, fence, fence, sp, nsp, width);
                blank = 0;
            }
        } else if (nlen == 0) {
            if (!blank) {
                push_row(rows, n, cap, 0, "");
                blank = 1;
            }
        } else if (is_hr(s, nlen) || is_table_rule(s, nlen)) {
            char rule[256];
            int cols = width < 40 ? width : 40;
            int used = 0;
            const char *hbar = g && g->h ? g->h : "-";
            int hc = disp_cols_n(hbar, strlen(hbar));
            if (hc < 1) {
                hc = 1;
                hbar = "-";
            }
            if (cols < 3) {
                cols = 3;
            }
            rule[0] = 0;
            while (used + hc <= cols && strlen(rule) + strlen(hbar) + 1 < sizeof rule) {
                strcat(rule, hbar);
                used += hc;
            }
            {
                Span one;
                one.style = ST_DIM;
                one.s = rule;
                one.n = strlen(rule);
                emit_spans(rows, n, cap, UI_ASSIST, "", "", &one, 1, width);
            }
            blank = 0;
        } else {
            size_t i = 0;
            size_t indent;
            int hashes = 0;
            int list = 0;
            char prefix[64];
            Span sp[256];
            int nsp;
            size_t h;
            prefix[0] = 0;
            while (i < nlen && (s[i] == ' ' || s[i] == '\t')) {
                i++;
            }
            indent = i;
            h = i;
            while (h < nlen && s[h] == '#' && hashes < 6) {
                hashes++;
                h++;
            }
            if (hashes && h < nlen && s[h] == ' ') {
                int k;
                int st = hashes == 1 ? ST_H1 : (hashes == 2 ? ST_H2 : ST_BOLD);
                h++;
                if (!blank) {
                    push_row(rows, n, cap, 0, "");
                }
                nsp = parse_inline(s + h, nlen - h, sp, 256);
                for (k = 0; k < nsp; k++) {
                    sp[k].style = st;
                }
                emit_spans(rows, n, cap, UI_ASSIST, "", "", sp, nsp, width);
                blank = 0;
            } else if (indent < nlen && s[indent] == '>' && (indent + 1 == nlen || s[indent + 1] == ' ')) {
                i = indent + 1;
                if (i < nlen && s[i] == ' ') {
                    i++;
                }
                char quote[48];
                snprintf(quote, sizeof quote, "\033[2m%s \033[0m", g && g->v ? g->v : "|");
                nsp = parse_inline(s + i, nlen - i, sp, 256);
                emit_spans(rows, n, cap, UI_ASSIST, quote, quote, sp, nsp, width);
                blank = 0;
            } else {
                size_t mark;
                i = indent;
                mark = i;
                if (i < nlen && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < nlen && s[i + 1] == ' ') {
                    list = 1;
                    i += 2;
                } else {
                    size_t j = i;
                    while (j < nlen && s[j] >= '0' && s[j] <= '9') {
                        j++;
                    }
                    if (j > i && j + 1 < nlen && s[j] == '.' && s[j + 1] == ' ') {
                        list = 2;
                        mark = i;
                        i = j + 2;
                    }
                }
                if (list == 1) {
                    snprintf(prefix, sizeof prefix, "\033[36m- \033[0m");
                } else if (list == 2) {
                    char num[16];
                    size_t k = 0;
                    size_t t = mark;
                    while (t < nlen && s[t] >= '0' && s[t] <= '9' && k + 1 < sizeof num) {
                        num[k++] = s[t++];
                    }
                    num[k] = 0;
                    snprintf(prefix, sizeof prefix, "\033[36m%s. \033[0m", num);
                } else {
                    prefix[0] = 0;
                    i = indent;
                }
                {
                    char contb[48];
                    cont_spaces(prefix, contb, sizeof contb);
                    nsp = parse_inline(s + i, nlen - i, sp, 256);
                    emit_spans(rows, n, cap, UI_ASSIST, prefix, contb, sp, nsp, width);
                }
                blank = 0;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void wrap_text(Row **rows, int *n, int *cap, int kind, const char *text, int width) {
    char gutter[3];
    int glen = 0;
    int inner;
    const char *p = text ? text : "";
    gutter_of(kind, gutter, &glen);
    if (width < 1) {
        width = 1;
    }
    inner = width - glen;
    if (inner < 8) {
        glen = 0;
        gutter[0] = 0;
        inner = width;
    }
    if (inner < 1) {
        inner = 1;
    }
    if (!p[0]) {
        push_piece(rows, n, cap, kind, gutter, NULL);
        return;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        size_t off = 0;
        if (seglen == 0) {
            push_piece(rows, n, cap, kind, gutter, NULL);
        }
        while (off < seglen) {
            size_t skip = 0;
            size_t take = cols_break(p + off, seglen - off, inner, &skip);
            char *piece;
            if (take == 0) {
                take = 1;
                skip = 0;
            }
            piece = xstrndup(p + off, take);
            push_piece(rows, n, cap, kind, gutter, piece);
            free(piece);
            off += take + skip;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
        if (*p == 0) {
            break;
        }
    }
}

static int role_break(int prev, int kind) {
    if (prev == 0 || kind == 0 || prev == LOG_CARD || kind == LOG_CARD) {
        return 0;
    }
    if (prev == kind) {
        return 0;
    }
    if ((prev == UI_USER || prev == UI_ASSIST || prev == UI_TOOL) &&
        (kind == UI_USER || kind == UI_ASSIST || kind == UI_TOOL)) {
        return 1;
    }
    return 0;
}

static const char *ansi_kind(const Ui *ui, int kind) {
    if (!ui->color) {
        return "";
    }
    if (kind == RK_REVERSE) {
        return "\033[7m";
    }
    if (kind == UI_USER) {
        /* Bright black background: the gray band in the 16-color palette. */
        return "\033[100m";
    }
    if (kind == UI_TOOL) {
        return "\033[36m";
    }
    if (kind == UI_META) {
        return "\033[2m";
    }
    if (kind == UI_ERR) {
        return "\033[31m";
    }
    if (kind == RK_RAW) {
        return "";
    }
    return "";
}

typedef struct {
    char *text;
    int ccol;
} InLine;

static void in_push(InLine **a, int *n, int *cap, const char *text, int ccol) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *a = xrealloc(*a, (size_t)(*cap) * sizeof(InLine));
    }
    (*a)[*n].text = xstrdup(text ? text : "");
    (*a)[*n].ccol = ccol;
    (*n)++;
}

static void layout_input(const Ui *ui, int width, InLine **out, int *nout) {
    const char *data;
    const char *prompt;
    size_t len, cursor, i;
    int prefix, room;
    int n = 0, cap = 0;
    InLine *lines = NULL;
    *out = NULL;
    *nout = 0;
    if (!ui) {
        return;
    }
    prompt = ui->prompt ? ui->prompt : "";
    prefix = disp_cols_n(prompt, strlen(prompt));
    room = width - prefix;
    if (room < 1) {
        room = 1;
    }
    data = ui->ed.data ? ui->ed.data : "";
    len = ui->ed.len;
    cursor = ui->ed.cursor;
    if (cursor > len) {
        cursor = len;
    }
    if (ui->hide_input) {
        int stars = (int)len;
        int show = stars > room ? room : stars;
        char *s = xmalloc((size_t)show + 1);
        int ccol = (int)cursor;
        memset(s, '*', (size_t)show);
        s[show] = 0;
        if (ccol > show) {
            ccol = show;
        }
        in_push(&lines, &n, &cap, s, ccol);
        free(s);
        *out = lines;
        *nout = n;
        return;
    }
    if (len == 0) {
        in_push(&lines, &n, &cap, "", 0);
        *out = lines;
        *nout = n;
        return;
    }
    i = 0;
    while (i < len) {
        size_t start = i;
        size_t take = 0;
        int ccol = -1;
        char *piece;
        while (i < len && data[i] != '\n') {
            size_t next = utf8_next(data, i, len);
            if (take > 0 && (int)take >= room) {
                break;
            }
            if (next <= i) {
                next = i + 1;
            }
            i = next;
            take++;
            if ((int)take >= room) {
                break;
            }
        }
        if (cursor >= start && cursor <= i) {
            ccol = disp_cols_n(data + start, cursor - start);
        }
        piece = xstrndup(data + start, i - start);
        in_push(&lines, &n, &cap, piece, ccol);
        free(piece);
        if (i < len && data[i] == '\n') {
            i++;
            if (i == len) {
                in_push(&lines, &n, &cap, "", cursor == i ? 0 : -1);
            }
        }
    }
    if (n == 0) {
        in_push(&lines, &n, &cap, "", 0);
    }
    *out = lines;
    *nout = n;
}

static void job_append(Ui *ui, const char *s, size_t n) {
    LogLine *ln = log_by_id(ui, ui->job_id);
    size_t cur;
    size_t add;
    char *neu;
    if (!ln || !s || n == 0) {
        return;
    }
    cur = ln->body ? strlen(ln->body) : 0;
    if (cur >= 8192) {
        return;
    }
    add = n;
    if (cur + add > 8192) {
        add = 8192 - cur;
    }
    neu = xmalloc(cur + add + 1);
    if (cur && ln->body) {
        memcpy(neu, ln->body, cur);
    }
    memcpy(neu + cur, s, add);
    neu[cur + add] = 0;
    free(ln->body);
    ln->body = neu;
    ui->dirty = 1;
}

static char *job_first_line(const char *s) {
    size_t n = 0;
    if (!s || !s[0] || s[0] == '\n') {
        return xstrdup("done");
    }
    while (s[n] && s[n] != '\n' && s[n] != '\r') {
        n++;
    }
    if (n == 0) {
        return xstrdup("done");
    }
    return xstrndup(s, n);
}

static void job_finish(Ui *ui, const char *summary, int err) {
    LogLine *ln;
    UiResult r;
    if (!ui || !ui->job_id) {
        if (summary && summary[0]) {
            ui_log(ui, UI_META, summary);
        }
        return;
    }
    ln = log_by_id(ui, ui->job_id);
    memset(&r, 0, sizeof r);
    r.state = err ? UI_TOOL_ERR : UI_TOOL_OK;
    r.summary = summary ? summary : "";
    r.body = ln && ln->body ? ln->body : NULL;
    r.peek = 4;
    ui_tool_end(ui, ui->job_id, &r);
    ui->job_id = 0;
}

static void job_emit(Ui *ui, const char *s, size_t n) {
    size_t i = 0;
    if (!ui || !s || n == 0) {
        return;
    }
    if (ui->mode == UI_SCREEN && ui->job_id) {
        job_append(ui, s, n);
        ui->job_shown += n;
        return;
    }
    while (i < n) {
        char ch = s[i++];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || ui->job_acc.len >= 240) {
            if (ch != '\n') {
                buf_putc(&ui->job_acc, ch);
            }
            ui->job_shown += ui->job_acc.len;
            ui_log(ui, UI_META, ui->job_acc.data ? ui->job_acc.data : "");
            buf_clear(&ui->job_acc);
            continue;
        }
        buf_putc(&ui->job_acc, ch);
    }
}

static void job_shutdown(Ui *ui, int report) {
    int st = 0;
    if (!ui) {
        return;
    }
    if (ui->job_pid > 0) {
        kill(-ui->job_pid, SIGTERM);
        kill(-ui->job_pid, SIGKILL);
        waitpid(ui->job_pid, &st, 0);
        ui->job_pid = 0;
        signals_set_job(0);
        if (report) {
            job_finish(ui, "killed", 1);
        }
    }
    if (ui->job_fd >= 0) {
        close(ui->job_fd);
        ui->job_fd = -1;
    }
    buf_clear(&ui->job_acc);
    ui->job_killed = 0;
    ui->job_shown = 0;
}

static void job_reap(Ui *ui) {
    int st = 0;
    pid_t w;
    char line[64];
    if (!ui || ui->job_pid <= 0) {
        return;
    }
    w = waitpid(ui->job_pid, &st, WNOHANG);
    if (w == 0) {
        return;
    }
    if (ui->mode != UI_SCREEN || !ui->job_id) {
        if (ui->job_acc.len) {
            ui->job_shown += ui->job_acc.len;
            ui_log(ui, UI_META, ui->job_acc.data ? ui->job_acc.data : "");
            buf_clear(&ui->job_acc);
        }
    }
    if (ui->job_killed) {
        job_finish(ui, "killed", 1);
    } else if (w == ui->job_pid && WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        snprintf(line, sizeof line, "exit %d", WEXITSTATUS(st));
        job_finish(ui, line, 1);
    } else if (w == ui->job_pid && WIFSIGNALED(st)) {
        snprintf(line, sizeof line, "signal %d", WTERMSIG(st));
        job_finish(ui, line, 1);
    } else if (w == ui->job_pid) {
        LogLine *ln = log_by_id(ui, ui->job_id);
        char *sum = job_first_line(ln && ln->body ? ln->body : NULL);
        job_finish(ui, sum, 0);
        free(sum);
    }
    ui->job_pid = 0;
    signals_set_job(0);
    if (ui->job_fd >= 0) {
        close(ui->job_fd);
        ui->job_fd = -1;
    }
    ui->job_killed = 0;
    ui->job_shown = 0;
}

static void job_pump(Ui *ui) {
    char tmp[1024];
    if (!ui || ui->job_fd < 0) {
        return;
    }
    for (;;) {
        ssize_t n = read(ui->job_fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            job_reap(ui);
            return;
        }
        job_emit(ui, tmp, (size_t)n);
        if (ui->job_shown > 32 * 1024) {
            if (ui->mode == UI_SCREEN && ui->job_id) {
                job_append(ui, "\ntruncated\n", 11);
                job_finish(ui, "truncated", 1);
            } else {
                ui_log(ui, UI_META, "truncated");
            }
            job_shutdown(ui, 0);
            return;
        }
    }
}

static int spawn_shell(const char *cmd, int *rd, pid_t *pid) {
    int pipefd[2];
    pid_t child;
    if (pipe(pipefd) != 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        setpgid(0, 0);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[0] > 2) {
            close(pipefd[0]);
        }
        if (pipefd[1] > 2) {
            close(pipefd[1]);
        }
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(child, child);
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    *rd = pipefd[0];
    *pid = child;
    return 0;
}

static void shell_sync(Ui *ui, const char *cmd) {
    int rd = -1;
    pid_t pid = 0;
    int killed = 0;
    char hdr[512];
    snprintf(hdr, sizeof hdr, "! %s", cmd);
    ui_log(ui, UI_TOOL, hdr);
    if (spawn_shell(cmd, &rd, &pid) != 0) {
        ui_log(ui, UI_ERR, "could not start command");
        return;
    }
    signals_set_job((int)pid);
    for (;;) {
        fd_set rset;
        struct timeval tv;
        char tmp[1024];
        ssize_t n;
        int st = 0;
        pid_t w;
        FD_ZERO(&rset);
        FD_SET(rd, &rset);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(rd + 1, &rset, NULL, NULL, &tv) < 0 && errno != EINTR) {
            break;
        }
        if (g_sigint) {
            g_sigint = 0;
            killed = 1;
            kill(-pid, SIGTERM);
            kill(-pid, SIGKILL);
        }
        if (FD_ISSET(rd, &rset)) {
            n = read(rd, tmp, sizeof tmp);
            if (n > 0) {
                size_t i;
                char line[512];
                size_t o = 0;
                for (i = 0; i < (size_t)n; i++) {
                    if (tmp[i] == '\r') {
                        continue;
                    }
                    if (tmp[i] == '\n' || o + 1 >= sizeof line) {
                        line[o] = 0;
                        ui_log(ui, UI_META, line);
                        o = 0;
                        continue;
                    }
                    line[o++] = tmp[i];
                }
                if (o) {
                    line[o] = 0;
                    ui_log(ui, UI_META, line);
                }
            } else if (n == 0) {
                break;
            }
        }
        w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            char rest[256];
            ssize_t n;
            while ((n = read(rd, rest, sizeof rest)) > 0) {
                rest[n < (ssize_t)sizeof rest ? n : (ssize_t)sizeof rest - 1] = 0;
                ui_log(ui, UI_META, rest);
            }
            if (killed) {
                ui_log(ui, UI_META, "killed");
            } else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
                char msg[32];
                snprintf(msg, sizeof msg, "exit %d", WEXITSTATUS(st));
                ui_log(ui, UI_META, msg);
            }
            pid = 0;
            break;
        }
    }
    if (pid > 0) {
        int st = 0;
        kill(-pid, SIGKILL);
        waitpid(pid, &st, 0);
        if (killed) {
            ui_log(ui, UI_META, "killed");
        }
    }
    signals_set_job(0);
    if (rd >= 0) {
        close(rd);
    }
}

static int is_question(const Ui *ui) {
    return ui && ui->ed.data && str_eq(ui->ed.data, "?") && !ui->menu_on && !ui->line_prompt && !ui->ask;
}

static void put_at(Row *rows, int idx, int n, int kind, const char *text) {
    if (!rows || idx < 0 || idx >= n) {
        return;
    }
    free(rows[idx].text);
    rows[idx].kind = kind;
    rows[idx].text = xstrdup(text ? text : "");
}

static void assist_margin(Row *rows, int from, int to, const Glyphs *g) {
    int first = 1;
    int i;
    const char *bullet = g && g->bullet ? g->bullet : "*";
    for (i = from; i < to; i++) {
        const char *t = rows[i].text ? rows[i].text : "";
        char *neu;
        if (!t[0]) {
            continue;
        }
        neu = xmalloc(strlen(bullet) + strlen(t) + 4);
        if (first) {
            sprintf(neu, "%s %s", bullet, t);
            first = 0;
        } else {
            sprintf(neu, "  %s", t);
        }
        free(rows[i].text);
        rows[i].text = neu;
    }
}

static void wrap_user(Row **rows, int *n, int *cap, const char *text, int width) {
    const char *p = text ? text : "";
    int lead = 1;
    int inner = width - 2;
    if (inner < 1) {
        inner = 1;
    }
    if (!p[0]) {
        push_row(rows, n, cap, UI_USER, "> ");
        return;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        size_t off = 0;
        if (seglen == 0) {
            push_row(rows, n, cap, UI_USER, lead ? "> " : "  ");
            lead = 0;
        }
        while (off < seglen) {
            size_t skip = 0;
            size_t take = cols_break(p + off, seglen - off, inner, &skip);
            char *piece;
            char *line;
            if (take == 0) {
                take = 1;
                skip = 0;
            }
            piece = xstrndup(p + off, take);
            line = xmalloc(strlen(piece) + 4);
            sprintf(line, "%s%s", lead ? "> " : "  ", piece);
            push_row(rows, n, cap, UI_USER, line);
            free(line);
            free(piece);
            lead = 0;
            off += take + skip;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
        if (!*p) {
            break;
        }
    }
}

static int skip_hdr(const char *s) {
    return s && (str_starts(s, "---") || str_starts(s, "+++"));
}

static void push_body_line(Row **rows, int *n, int *cap, const char *line, int diff, int width) {
    Buf b;
    char *shown;
    int kind = 0;
    (void)width;
    if (diff && skip_hdr(line)) {
        return;
    }
    buf_init(&b);
    if (diff && line && line[0] == '+') {
        buf_puts(&b, "\033[32m");
        kind = RK_RAW;
    } else if (diff && line && line[0] == '-') {
        buf_puts(&b, "\033[31m");
        kind = RK_RAW;
    } else if (diff && line && str_starts(line, "@@")) {
        buf_puts(&b, "\033[2m");
        kind = RK_RAW;
    }
    buf_puts(&b, "    ");
    buf_puts(&b, line ? line : "");
    if (kind == RK_RAW) {
        buf_puts(&b, "\033[0m");
    }
    shown = buf_detach(&b);
    push_row(rows, n, cap, kind, shown);
    free(shown);
}

static void render_tool(Ui *ui, const LogLine *ln, int width, Row **rows, int *n, int *cap) {
    const Glyphs *g = ui_glyphs(ui);
    const char *color = "\033[33m";
    Buf b;
    char *head;
    const char *body;
    int peek;
    int count = 0;
    int shown = 0;
    int cut;
    if (ln->state == UI_TOOL_OK) {
        color = "\033[32m";
    } else if (ln->state == UI_TOOL_ERR) {
        color = "\033[31m";
    }
    buf_init(&b);
    buf_printf(&b, "%s%s\033[0m %s", color, g->bullet, ln->text ? ln->text : "");
    head = buf_detach(&b);
    push_row(rows, n, cap, RK_RAW, head);
    free(head);
    if (ln->summary && ln->summary[0]) {
        buf_init(&b);
        buf_printf(&b, "\033[2m  %s %s\033[0m", g->elbow, ln->summary);
        head = buf_detach(&b);
        push_row(rows, n, cap, RK_RAW, head);
        free(head);
    }
    body = ln->body;
    if (!body || !body[0]) {
        return;
    }
    peek = ln->peek > 0 ? ln->peek : 4;
    if (!ui->expand) {
        const char *p = body;
        int kept = 0;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            char *line = xstrndup(p, seglen);
            if (!(ln->diff && skip_hdr(line)) && line[0]) {
                count++;
                if (kept < peek) {
                    push_body_line(rows, n, cap, line, ln->diff, width);
                    kept++;
                }
            }
            free(line);
            if (!nl) {
                break;
            }
            p = nl + 1;
        }
        if (count > kept) {
            char note[96];
            snprintf(note, sizeof note, "\033[2m    %s +%d lines (ctrl-o to expand)\033[0m", g->ellipsis, count - kept);
            push_row(rows, n, cap, RK_RAW, note);
        }
        return;
    }
    {
        const char *p = body;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            size_t off = 0;
            char *line = xstrndup(p, seglen);
            int inner = width - 4;
            if (inner < 1) {
                inner = 1;
            }
            if (!(ln->diff && skip_hdr(line))) {
                if (!line[0]) {
                    push_body_line(rows, n, cap, "", ln->diff, width);
                }
                while (off < seglen) {
                    size_t skip = 0;
                    size_t take = cols_break(line + off, seglen - off, inner, &skip);
                    char *piece;
                    if (take == 0) {
                        take = 1;
                        skip = 0;
                    }
                    piece = xstrndup(line + off, take);
                    push_body_line(rows, n, cap, piece, ln->diff, width);
                    free(piece);
                    shown++;
                    off += take + skip;
                }
            }
            free(line);
            if (!nl) {
                break;
            }
            p = nl + 1;
        }
    }
    (void)shown;
    (void)cut;
}

static void render_card(Ui *ui, int width, Row **rows, int *n, int *cap) {
    static const char *art[5] = {
        " _          _",
        "| |__  ___ (_) __ _  ___",
        "| '_ \\/ -_)| |/ _` |/ -_)",
        "|_.__/\\___||_|\\__, |\\___|",
        "              |___/"
    };
    static const char *hints[5] = {
        "",
        "terminal coding agent",
        "",
        "type / for commands, ? for shortcuts",
        "shift-tab cycles ask, auto, plan"
    };
    const Glyphs *g = ui_glyphs(ui);
    int artw = 0;
    int i;
    char title[64];
    Buf b;
    if (ui->rows < 16 || width < 26) {
        return;
    }
    for (i = 0; i < 5; i++) {
        int c = (int)strlen(art[i]);
        if (c > artw) {
            artw = c;
        }
    }
    snprintf(title, sizeof title, "beige %s", ui->welcome_ver ? ui->welcome_ver : "");
    buf_init(&b);
    box_top(&b, g, title, width);
    push_row(rows, n, cap, 0, b.data);
    for (i = 0; i < 5; i++) {
        const char *hint = i == 2 && ui->welcome_cwd ? ui->welcome_cwd : hints[i];
        int pad = artw - (int)strlen(art[i]);
        int k;
        Buf inner;
        buf_clear(&b);
        buf_init(&inner);
        buf_puts(&inner, "\033[33m");
        buf_puts(&inner, art[i]);
        buf_puts(&inner, "\033[0m");
        for (k = 0; k < pad + 2; k++) {
            buf_putc(&inner, ' ');
        }
        buf_puts(&inner, hint ? hint : "");
        box_row(&b, g, inner.data, width);
        buf_free(&inner);
        push_row(rows, n, cap, RK_RAW, b.data);
    }
    if (ui->welcome_warn && ui->welcome_warn[0]) {
        const char *w = ui->welcome_warn;
        size_t wn = strlen(w);
        size_t off = 0;
        int inner = width - 2;
        if (inner < 8) {
            inner = 8;
        }
        while (off < wn) {
            size_t skip = 0;
            size_t take = cols_break(w + off, wn - off, inner, &skip);
            char *piece;
            char *colored;
            if (take == 0) {
                take = 1;
                skip = 0;
            }
            piece = xstrndup(w + off, take);
            colored = xmalloc(strlen(piece) + 16);
            sprintf(colored, "\033[31m%s\033[0m", piece);
            buf_clear(&b);
            box_row(&b, g, colored, width);
            push_row(rows, n, cap, RK_RAW, b.data);
            free(colored);
            free(piece);
            off += take + skip;
            while (off < wn && (w[off] == '\n' || w[off] == '\r')) {
                off++;
            }
        }
    }
    buf_clear(&b);
    box_bottom(&b, g, width);
    push_row(rows, n, cap, 0, b.data);
    buf_free(&b);
}

static void render_text_row(Ui *ui, int kind, const char *text, int width, Row **rows, int *n, int *cap) {
    if (kind == UI_ASSIST) {
        int base = *n;
        int inner = width > 2 ? width - 2 : width;
        if (ui->color) {
            md_render(rows, n, cap, text, inner, ui_glyphs(ui));
        } else {
            wrap_text(rows, n, cap, 0, text, inner);
        }
        assist_margin(*rows, base, *n, ui_glyphs(ui));
        return;
    }
    if (kind == UI_USER) {
        wrap_user(rows, n, cap, text, width);
        return;
    }
    wrap_text(rows, n, cap, kind, text, width);
}

typedef struct Chunk {
    Row *rows;
    int n;
    int cap;
    int kind;
} Chunk;

static void render_line_chunk(Ui *ui, const LogLine *ln, int kind, const char *text, int width, Chunk *ch) {
    memset(ch, 0, sizeof *ch);
    ch->kind = kind;
    if (ln && ln->kind == LOG_CARD) {
        ch->kind = LOG_CARD;
        render_card(ui, width, &ch->rows, &ch->n, &ch->cap);
        return;
    }
    if (ln && ln->kind == UI_TOOL) {
        ch->kind = UI_TOOL;
        render_tool(ui, ln, width, &ch->rows, &ch->n, &ch->cap);
        return;
    }
    render_text_row(ui, kind, text ? text : "", width, &ch->rows, &ch->n, &ch->cap);
}

static void chunk_free(Chunk *ch) {
    int i;
    for (i = 0; i < ch->n; i++) {
        free(ch->rows[i].text);
    }
    free(ch->rows);
    ch->rows = NULL;
    ch->n = 0;
}

static const char *HELP_ROW[] = {
    "esc  interrupt the reply",
    "ctrl-o  expand tool output",
    "?  shortcuts",
    "shift-tab  cycle ask, auto, plan",
    "ctrl-c  cancel",
    "ctrl-d  exit",
    "ctrl-x  kill the shell command",
    "alt-enter  insert a newline"
};

enum { HELP_N = 8 };

static void fit_status(const Ui *ui, int width, Buf *out) {
    const char *mode = ui->st_mode && ui->st_mode[0] ? ui->st_mode : "ask";
    const char *tag = "[ ASK ]";
    const char *sgr = "\033[7;1m";
    const char *provider = ui->st_provider ? ui->st_provider : "";
    const char *model = ui->st_model ? ui->st_model : "";
    const char *cwd = ui->st_cwd ? ui->st_cwd : "";
    char left[240];
    char right[64];
    char pct[32];
    char approx[32];
    int show_cwd = 1;
    int show_prov = 1;
    int show_tok = 1;
    int pass;
    unsigned long pct_n = ui->st_budget ? ui->st_tokens * 100UL / ui->st_budget : 0UL;
    if (str_eq(mode, "auto")) {
        tag = "[ AUTO ]";
        sgr = "\033[7;33m";
    } else if (str_eq(mode, "plan")) {
        tag = "[ PLAN ]";
        sgr = "\033[7;36m";
    }
    snprintf(pct, sizeof pct, "ctx %lu%%", pct_n);
    if (ui->st_tokens >= 1000) {
        snprintf(approx, sizeof approx, "~%luk", ui->st_tokens / 1000UL);
    } else {
        snprintf(approx, sizeof approx, "~%lu", ui->st_tokens);
    }
    left[0] = 0;
    right[0] = 0;
    for (pass = 0; pass < 4; pass++) {
        int used;
        int gap;
        left[0] = 0;
        if (show_prov && provider[0] && model[0]) {
            snprintf(left, sizeof left, "%s/%s", provider, model);
        } else if (model[0]) {
            snprintf(left, sizeof left, "%s", model);
        } else if (provider[0]) {
            snprintf(left, sizeof left, "%s", provider);
        }
        if (show_cwd && cwd[0]) {
            size_t n = strlen(left);
            snprintf(left + n, sizeof left - n, "%s%s", n ? "   " : "", cwd);
        }
        snprintf(right, sizeof right, "%s", pct);
        if (show_tok) {
            size_t n = strlen(right);
            snprintf(right + n, sizeof right - n, "  %s", approx);
        }
        used = (int)strlen(tag) + (left[0] ? 1 + (int)strlen(left) : 0) + 1 + (int)strlen(right);
        gap = width - used;
        if (gap >= 0) {
            break;
        }
        if (show_cwd) {
            show_cwd = 0;
            continue;
        }
        if (show_prov) {
            show_prov = 0;
            continue;
        }
        if (show_tok) {
            show_tok = 0;
            continue;
        }
        break;
    }
    buf_puts(out, sgr);
    buf_puts(out, tag);
    buf_puts(out, "\033[7m");
    if (left[0]) {
        buf_putc(out, ' ');
        buf_puts(out, left);
    }
    {
        int used = (int)strlen(tag) + (left[0] ? 1 + (int)strlen(left) : 0);
        int pad = width - used - (int)strlen(right);
        int k;
        if (pad < 1) {
            pad = 1;
        }
        for (k = 0; k < pad; k++) {
            buf_putc(out, ' ');
        }
        buf_puts(out, right);
    }
}

static int count_body_lines(const char *s, int diff) {
    int n = 0;
    const char *p = s ? s : "";
    if (!p[0]) {
        return 0;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, seglen);
        if (!(diff && skip_hdr(line))) {
            n++;
        }
        free(line);
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

static int looks_diff(const char *s) {
    const char *p = s ? s : "";
    while (*p) {
        if (str_starts(p, "@@") || str_starts(p, "---") || str_starts(p, "+++")) {
            return 1;
        }
        p = strchr(p, '\n');
        if (!p) {
            break;
        }
        p++;
    }
    return 0;
}

static void frame_top(Buf *b, const Glyphs *g, const char *title, int cols) {
    Buf inner;
    buf_init(&inner);
    box_top(&inner, g, title, cols);
    buf_puts(b, "\033[33m");
    buf_puts(b, inner.data ? inner.data : "");
    buf_puts(b, "\033[0m");
    buf_free(&inner);
}

static void frame_bottom(Buf *b, const Glyphs *g, int cols) {
    Buf inner;
    buf_init(&inner);
    box_bottom(&inner, g, cols);
    buf_puts(b, "\033[33m");
    buf_puts(b, inner.data ? inner.data : "");
    buf_puts(b, "\033[0m");
    buf_free(&inner);
}

static void frame_row(Buf *b, const Glyphs *g, const char *text, int cols) {
    int side = disp_cols_n(g->v, g->v ? strlen(g->v) : 0);
    int inner;
    int got;
    char *clipped;
    if (side < 1) {
        side = 1;
    }
    inner = cols - side - side;
    if (inner < 0) {
        inner = 0;
    }
    clipped = cols_clip(text ? text : "", inner, "");
    buf_puts(b, "\033[33m");
    buf_puts(b, g->v);
    buf_puts(b, "\033[0m");
    buf_puts(b, clipped ? clipped : "");
    got = disp_cols_n(clipped, clipped ? strlen(clipped) : 0);
    while (got < inner) {
        buf_putc(b, ' ');
        got++;
    }
    buf_puts(b, "\033[33m");
    buf_puts(b, g->v);
    buf_puts(b, "\033[0m");
    free(clipped);
}

static void box_line(Buf *b, const Glyphs *g, const char *text, int cols, int bottom, int top, const char *title) {
    buf_clear(b);
    if (top) {
        box_top(b, g, title, cols);
    } else if (bottom) {
        box_bottom(b, g, cols);
    } else {
        box_row(b, g, text, cols);
    }
}

void ui_present(Ui *ui) {
    Row *screen;
    Chunk chunks[96];
    int nc = 0;
    int total = 0;
    Row *body = NULL;
    int nb = 0;
    int bcap = 0;
    int width;
    int i;
    int min_trans;
    int foot = 1;
    int box = 3;
    int spin = 0;
    int queue = 0;
    int trans;
    int help = 0;
    int nmatch = 0;
    int sel = 0;
    int win = 0;
    int match_idx[32];
    int y_trans, y_spin, y_queue, y_box, y_foot;
    int cursor_row = 0;
    int cursor_col = 1;
    char *stream_text = NULL;
    const Glyphs *g;
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    if (g_winch) {
        g_winch = 0;
        ui_size(ui);
    }
    width = ui->cols > 1 ? ui->cols - 1 : 1;
    g = ui_glyphs(ui);
    min_trans = ui->rows >= 16 ? 3 : 0;
    if (ui->ask) {
        int bl = count_body_lines(ui->ask->body, looks_diff(ui->ask->body));
        int want = 7 + (bl > 12 ? 12 : bl);
        int maxb = ui->rows - 1 - min_trans;
        foot = 1;
        if (maxb < 8 && ui->rows < 16) {
            foot = 0;
            maxb = ui->rows - min_trans;
        }
        if (maxb < 1) {
            maxb = ui->rows > 0 ? ui->rows : 1;
        }
        if (want > maxb) {
            want = maxb;
        }
        box = want;
    } else if (ui->menu_on && ui->menu_n > 0) {
        int want = ui->menu_n + 3;
        int maxb = ui->rows - 1 - min_trans;
        if (maxb < 3) {
            maxb = ui->rows > 1 ? ui->rows - 1 : ui->rows;
        }
        if (want > maxb) {
            want = maxb;
        }
        box = want > 0 ? want : 1;
        foot = 1;
        if (box + foot > ui->rows) {
            foot = ui->rows - box;
            if (foot < 0) {
                foot = 0;
                box = ui->rows;
            }
        }
    } else {
        InLine *inl = NULL;
        int nin = 0;
        int content;
        int max_foot;
        int inner = width > 2 ? width - 2 : width;
        layout_input(ui, inner, &inl, &nin);
        content = nin > 0 ? nin : 1;
        if (content > 6) {
            content = 6;
        }
        box = content + 2;
        for (i = 0; i < nin; i++) {
            free(inl[i].text);
        }
        free(inl);
        if (slash_typing(ui)) {
            nmatch = slash_matches(ui, match_idx, 32);
            foot = nmatch > 0 ? nmatch : 1;
            sel = ui->slash_sel;
        } else if (is_question(ui)) {
            help = 1;
            foot = HELP_N;
        } else {
            foot = 1;
        }
        max_foot = ui->rows - box - min_trans;
        if (max_foot < 0) {
            max_foot = 0;
        }
        if (foot > max_foot) {
            foot = max_foot > 0 ? max_foot : (ui->rows > box ? 1 : 0);
            if (box + foot > ui->rows) {
                foot = ui->rows - box;
                if (foot < 0) {
                    foot = 0;
                }
            }
        }
    }
    if (nmatch > 0) {
        if (sel < 0) {
            sel = 0;
        }
        if (sel >= nmatch) {
            sel = nmatch - 1;
        }
        ui->slash_sel = sel;
        if (foot > 0 && sel >= foot) {
            win = sel - foot + 1;
        }
    }
    trans = ui->rows - foot - box;
    if (ui->spin_label && trans - 1 >= min_trans) {
        spin = 1;
        trans--;
    }
    if (ui->followup && ui->followup[0] && trans - 1 >= min_trans) {
        queue = 1;
        trans--;
    }
    if (trans < 0) {
        trans = 0;
    }
    y_trans = 0;
    y_spin = trans;
    y_queue = y_spin + spin;
    y_box = y_queue + queue;
    y_foot = y_box + box;

    if (ui->stream.len && ui->stream.data) {
        stream_text = prepare_text(ui, ui->stream.data);
    }
    if (stream_text && stream_text[0]) {
        render_line_chunk(ui, NULL, UI_ASSIST, stream_text, width, &chunks[nc]);
        total += chunks[nc].n + 1;
        nc++;
    }
    for (i = ui->nlog - 1; i >= 0 && total < trans + ui->scroll && nc < 96; i--) {
        render_line_chunk(ui, &ui->log[i], ui->log[i].kind, ui->log[i].text, width, &chunks[nc]);
        total += chunks[nc].n + 1;
        nc++;
    }
    {
        int prevk = 0;
        int k;
        for (k = nc - 1; k >= 0; k--) {
            int r;
            if (nb > 0 && role_break(prevk, chunks[k].kind)) {
                push_row(&body, &nb, &bcap, 0, "");
            }
            for (r = 0; r < chunks[k].n; r++) {
                push_row(&body, &nb, &bcap, chunks[k].rows[r].kind, chunks[k].rows[r].text);
            }
            prevk = chunks[k].kind;
            chunk_free(&chunks[k]);
        }
    }
    if (ui->scroll > nb) {
        ui->scroll = nb;
    }
    screen = xmalloc((size_t)ui->rows * sizeof(Row));
    for (i = 0; i < ui->rows; i++) {
        screen[i].text = xstrdup("");
        screen[i].kind = 0;
    }
    if (trans > 0 && nb > 0) {
        int start = nb - trans - ui->scroll;
        int r;
        if (start < 0) {
            start = 0;
        }
        for (r = 0; r < trans; r++) {
            int idx = start + r;
            if (idx >= 0 && idx < nb) {
                put_at(screen, y_trans + r, ui->rows, body[idx].kind, body[idx].text);
            }
        }
    }
    if (spin) {
        char spinbuf[128];
        struct timeval tv;
        long elapsed;
        int frame, mins, secs;
        static const char rotor[] = "|/-\\";
        gettimeofday(&tv, NULL);
        frame = (int)((tv.tv_usec / 100000) % 4);
        elapsed = (long)tv.tv_sec - (long)ui->spin_start;
        if (elapsed < 0) {
            elapsed = 0;
        }
        mins = (int)(elapsed / 60);
        secs = (int)(elapsed % 60);
        snprintf(spinbuf, sizeof spinbuf, "[%c] %s%s %d:%02d  (esc to interrupt)", rotor[frame],
                 ui->spin_label ? ui->spin_label : "", g->ellipsis, mins, secs);
        put_at(screen, y_spin, ui->rows, UI_META, spinbuf);
    }
    if (queue) {
        char qbuf[512];
        snprintf(qbuf, sizeof qbuf, "queued: %s", ui->followup);
        put_at(screen, y_queue, ui->rows, UI_META, qbuf);
    }
    if (box > 0 && ui->ask) {
        Buf b;
        int slots;
        int question = 1;
        int chrome = 7;
        int diff = looks_diff(ui->ask->body);
        int nlines = count_body_lines(ui->ask->body, diff);
        int row = y_box;
        int opt;
        char store[4][192];
        const char *always = ui->ask->always ? ui->ask->always : "";
        buf_init(&b);
        if (box < chrome) {
            question = box >= 7 ? 1 : 0;
            slots = box - 2 - 4 - question;
            if (slots < 0) {
                slots = 0;
            }
        } else {
            slots = box - chrome;
        }
        frame_top(&b, g, ui->ask->title ? ui->ask->title : "", width);
        put_at(screen, row++, ui->rows, RK_RAW, b.data);
        if (nlines == 0) {
            ui->ask_scroll = 0;
        } else if (ui->ask_scroll > nlines - 1) {
            ui->ask_scroll = nlines - 1;
        }
        if (ui->ask_scroll < 0) {
            ui->ask_scroll = 0;
        }
        if (slots > 0) {
            const char *p = ui->ask->body ? ui->ask->body : "";
            int index = 0;
            int filled = 0;
            int remain;
            int show = slots;
            int cut = 0;
            remain = nlines - ui->ask_scroll;
            if (remain > slots && slots >= 2) {
                show = slots - 1;
                cut = remain - show;
            }
            while (*p && filled < show) {
                const char *nl = strchr(p, '\n');
                size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
                char *line = xstrndup(p, seglen);
                if (!(diff && skip_hdr(line))) {
                    if (index >= ui->ask_scroll && filled < show) {
                        Buf colored;
                        buf_init(&colored);
                        if (diff && line[0] == '+') {
                            buf_puts(&colored, "\033[32m");
                        } else if (diff && line[0] == '-') {
                            buf_puts(&colored, "\033[31m");
                        } else if (diff && str_starts(line, "@@")) {
                            buf_puts(&colored, "\033[2m");
                        }
                        buf_puts(&colored, line);
                        if (colored.len && (diff && (line[0] == '+' || line[0] == '-' || str_starts(line, "@@")))) {
                            buf_puts(&colored, "\033[0m");
                        }
                        buf_clear(&b);
                        frame_row(&b, g, colored.data, width);
                        put_at(screen, row++, ui->rows, RK_RAW, b.data);
                        buf_free(&colored);
                        filled++;
                    }
                    index++;
                }
                free(line);
                if (!nl) {
                    break;
                }
                p = nl + 1;
            }
            if (cut > 0 && filled < slots) {
                char note[80];
                snprintf(note, sizeof note, "%s +%d lines", g->ellipsis, cut);
                buf_clear(&b);
                frame_row(&b, g, note, width);
                put_at(screen, row++, ui->rows, UI_META, b.data);
            }
        }
        if (question && row < y_box + box - 1) {
            buf_clear(&b);
            frame_row(&b, g, "Do you want to proceed?", width);
            put_at(screen, row++, ui->rows, 0, b.data);
        }
        snprintf(store[0], sizeof store[0], "%s 1. Yes (y)", ui->ask_sel == 0 ? ">" : " ");
        if (always[0]) {
            snprintf(store[1], sizeof store[1], "%s 2. Yes, always for %s (a)", ui->ask_sel == 1 ? ">" : " ", always);
        } else {
            snprintf(store[1], sizeof store[1], "%s 2. Yes, always (a)", ui->ask_sel == 1 ? ">" : " ");
        }
        snprintf(store[2], sizeof store[2], "%s 3. No, skip it (n)", ui->ask_sel == 2 ? ">" : " ");
        snprintf(store[3], sizeof store[3], "%s 4. No, deny it (d)", ui->ask_sel == 3 ? ">" : " ");
        for (opt = 0; opt < 4 && row < y_box + box - 1; opt++) {
            buf_clear(&b);
            frame_row(&b, g, store[opt], width);
            put_at(screen, row, ui->rows, 0, b.data);
            if (opt == ui->ask_sel) {
                cursor_row = row;
                cursor_col = 2;
            }
            row++;
        }
        buf_clear(&b);
        frame_bottom(&b, g, width);
        put_at(screen, y_box + box - 1, ui->rows, RK_RAW, b.data);
        buf_free(&b);
    } else if (box > 0 && ui->menu_on && ui->menu_n > 0) {
        Buf b;
        int hint = box >= 3 ? 1 : 0;
        int slots = box - 2 - hint;
        int msel = ui->menu_sel;
        int mwin = 0;
        int row = y_box;
        int item;
        if (slots < 1) {
            slots = box > 2 ? box - 2 : 0;
            hint = 0;
        }
        if (msel < 0) {
            msel = 0;
        }
        if (msel >= ui->menu_n) {
            msel = ui->menu_n - 1;
        }
        ui->menu_sel = msel;
        if (slots > 0 && msel >= slots) {
            mwin = msel - slots + 1;
        }
        buf_init(&b);
        box_line(&b, g, NULL, width, 0, 1, NULL);
        put_at(screen, row++, ui->rows, 0, b.data);
        for (item = 0; item < slots; item++) {
            int idx = mwin + item;
            char line[512];
            const char *label = "";
            if (idx >= 0 && idx < ui->menu_n && ui->menu_items && ui->menu_items[idx]) {
                label = ui->menu_items[idx];
            }
            snprintf(line, sizeof line, "%c %d. %s", idx == msel ? '>' : ' ', idx + 1, label);
            box_line(&b, g, line, width, 0, 0, NULL);
            put_at(screen, row, ui->rows, 0, b.data);
            if (idx == msel) {
                cursor_row = row;
                cursor_col = 2;
            }
            row++;
        }
        if (hint && row < y_box + box - 1) {
            box_line(&b, g, ui->menu_title ? ui->menu_title : "", width, 0, 0, NULL);
            put_at(screen, row++, ui->rows, UI_META, b.data);
        }
        box_line(&b, g, NULL, width, 1, 0, NULL);
        put_at(screen, y_box + box - 1, ui->rows, 0, b.data);
        buf_free(&b);
    } else if (box >= 3) {
        InLine *inl = NULL;
        int nin = 0;
        int inner = width > 2 ? width - 2 : width;
        int content = box - 2;
        int cursor_line = 0;
        int ccol = 0;
        int in_win = 0;
        int prefix;
        int li;
        Buf b;
        const char *prompt = ui->prompt ? ui->prompt : "";
        layout_input(ui, inner, &inl, &nin);
        prefix = disp_cols_n(prompt, strlen(prompt));
        for (i = 0; i < nin; i++) {
            if (inl[i].ccol >= 0) {
                cursor_line = i;
                ccol = inl[i].ccol;
            }
        }
        if (content < 1) {
            content = 1;
        }
        if (nin > content && cursor_line >= content) {
            in_win = cursor_line - content + 1;
        }
        buf_init(&b);
        box_line(&b, g, NULL, width, 0, 1, NULL);
        put_at(screen, y_box, ui->rows, 0, b.data);
        for (li = 0; li < content; li++) {
            int src = in_win + li;
            const char *bit = "";
            char *rowtxt;
            int pad = 0;
            if (src >= 0 && src < nin && inl[src].text) {
                bit = inl[src].text;
            }
            rowtxt = xmalloc(strlen(prompt) + strlen(bit) + prefix + 4);
            if (li == 0) {
                sprintf(rowtxt, "%s%s", prompt, bit);
            } else {
                int k;
                rowtxt[0] = 0;
                for (k = 0; k < prefix; k++) {
                    rowtxt[pad++] = ' ';
                }
                rowtxt[pad] = 0;
                strcat(rowtxt, bit);
            }
            box_line(&b, g, rowtxt, width, 0, 0, NULL);
            put_at(screen, y_box + 1 + li, ui->rows, 0, b.data);
            free(rowtxt);
        }
        box_line(&b, g, NULL, width, 1, 0, NULL);
        put_at(screen, y_box + box - 1, ui->rows, 0, b.data);
        buf_free(&b);
        cursor_row = y_box + 1 + (cursor_line - in_win);
        cursor_col = 1 + 1 + prefix + ccol;
        for (i = 0; i < nin; i++) {
            free(inl[i].text);
        }
        free(inl);
    }
    if (foot > 0) {
        int f;
        if (help) {
            for (f = 0; f < foot && f < HELP_N; f++) {
                put_at(screen, y_foot + f, ui->rows, UI_META, HELP_ROW[f]);
            }
        } else if (nmatch > 0) {
            for (f = 0; f < foot; f++) {
                int idx = win + f;
                char line[512];
                const char *label = "";
                const char *hint = "";
                if (idx >= 0 && idx < nmatch) {
                    label = ui->choices[match_idx[idx]].label ? ui->choices[match_idx[idx]].label : "";
                    hint = ui->choices[match_idx[idx]].hint ? ui->choices[match_idx[idx]].hint : "";
                }
                snprintf(line, sizeof line, "%c %-14s \033[2m%s\033[0m", idx == sel ? '>' : ' ', label, hint);
                put_at(screen, y_foot + f, ui->rows, RK_RAW, line);
            }
        } else if (foot >= 1) {
            Buf st;
            buf_init(&st);
            fit_status(ui, width, &st);
            put_at(screen, y_foot, ui->rows, RK_REVERSE, st.data);
            buf_free(&st);
        }
    }

    if (ui->prev_n != ui->rows) {
        for (i = 0; i < ui->prev_n; i++) {
            free(ui->prev[i].text);
        }
        free(ui->prev);
        ui->prev = xmalloc((size_t)ui->rows * sizeof(Row));
        for (i = 0; i < ui->rows; i++) {
            ui->prev[i].text = NULL;
            ui->prev[i].kind = -1;
        }
        ui->prev_n = ui->rows;
    }
    for (i = 0; i < ui->rows; i++) {
        char *shown = clip_cols(screen[i].text, width);
        int kind = screen[i].kind;
        if (ui->prev[i].kind == kind && ui->prev[i].text && str_eq(ui->prev[i].text, shown)) {
            free(shown);
            free(screen[i].text);
            continue;
        }
        if (kind == RK_REVERSE || kind == UI_USER) {
            fprintf(stdout, "\033[%d;1H%s\033[K%s\033[0m", i + 1, ansi_kind(ui, kind), shown);
        } else if (kind == RK_RAW) {
            fprintf(stdout, "\033[%d;1H%s\033[0m\033[K", i + 1, shown);
        } else {
            fprintf(stdout, "\033[%d;1H%s%s\033[0m\033[K", i + 1, ansi_kind(ui, kind), shown);
        }
        free(ui->prev[i].text);
        ui->prev[i].text = shown;
        ui->prev[i].kind = kind;
        free(screen[i].text);
    }
    if (cursor_row < 0) {
        cursor_row = 0;
    }
    if (cursor_row >= ui->rows) {
        cursor_row = ui->rows - 1;
    }
    if (cursor_col < 1) {
        cursor_col = 1;
    }
    if (cursor_col > width) {
        cursor_col = width;
    }
    fprintf(stdout, "\033[%d;%dH", cursor_row + 1, cursor_col);
    fflush(stdout);
    free(screen);
    for (i = 0; i < nb; i++) {
        free(body[i].text);
    }
    free(body);
    free(stream_text);
    ui->dirty = 0;
    ui->paint_ms = now_ms();
}

static void watch_extra(Ui *ui, fd_set *r, int *maxfd) {
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    FD_SET(STDIN_FILENO, r);
    if (STDIN_FILENO > *maxfd) {
        *maxfd = STDIN_FILENO;
    }
    if (ui->job_fd >= 0) {
        FD_SET(ui->job_fd, r);
        if (ui->job_fd > *maxfd) {
            *maxfd = ui->job_fd;
        }
    }
}

static void hook_prepare(void *ud, fd_set *r, fd_set *w, fd_set *e, int *maxfd) {
    (void)w;
    (void)e;
    watch_extra(ud, r, maxfd);
}

static void hook_ready(void *ud, fd_set *r, fd_set *w, fd_set *e) {
    Ui *ui = ud;
    (void)w;
    (void)e;
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    if (ui->job_fd >= 0 && FD_ISSET(ui->job_fd, r)) {
        job_pump(ui);
    }
    if (FD_ISSET(STDIN_FILENO, r)) {
        read_keys(ui, 1);
        ui_present(ui);
    }
}

static void hook_tick(void *ud) {
    Ui *ui = ud;
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    job_pump(ui);
    if (ui->dirty || ui->spin_label) {
        ui_present(ui);
    }
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

void ui_hooks(Ui *ui, IoHooks *hooks) {
    if (!hooks) {
        return;
    }
    hooks->prepare = hook_prepare;
    hooks->ready = hook_ready;
    hooks->tick = ui && ui->mode == UI_SCREEN ? hook_tick : NULL;
    hooks->ud = ui;
}

void ui_activity(Ui *ui, const char *label) {
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    if (!label || !label[0]) {
        ui->spin_label = NULL;
        ui_present(ui);
        return;
    }
    ui->spin_label = label;
    ui->spin_start = time(NULL);
    ui_present(ui);
}

int ui_wants_quit(const Ui *ui) {
    return ui && ui->quit;
}

static const Glyphs *ui_glyphs(const Ui *ui) {
    if (ui && ui->glyphs) {
        return ui->glyphs;
    }
    return &G_ASC;
}

void ui_welcome(Ui *ui, const char *version, const char *cwd, const char *warning) {
    int i;
    if (!ui || ui->mode != UI_SCREEN) {
        return;
    }
    copy_str(&ui->welcome_ver, version);
    copy_str(&ui->welcome_cwd, cwd);
    free(ui->welcome_warn);
    ui->welcome_warn = warning && warning[0] ? xstrdup(warning) : NULL;
    for (i = 0; i < ui->nlog; i++) {
        if (ui->log[i].kind == LOG_CARD) {
            ui_present(ui);
            return;
        }
    }
    log_add(ui, LOG_CARD, "");
    ui_present(ui);
}

int ui_tool_begin(Ui *ui, const char *title) {
    LogLine *line;
    if (!ui) {
        return 0;
    }
    if (ui->mode == UI_PRINT) {
        fprintf(stdout, "\n[%s]\n", title ? title : "");
        fflush(stdout);
        return 0;
    }
    if (ui->mode == UI_LINE) {
        fprintf(stdout, "* %s\n", title ? title : "");
        fflush(stdout);
        return 0;
    }
    log_add(ui, UI_TOOL, title);
    line = &ui->log[ui->nlog - 1];
    line->id = ++ui->tool_seq;
    line->state = UI_TOOL_RUN;
    line->peek = 4;
    ui_present(ui);
    return line->id;
}

void ui_tool_end(Ui *ui, int id, const UiResult *result) {
    LogLine *ln;
    char *summary;
    char *body = NULL;
    const Glyphs *g;
    if (!ui || !result) {
        return;
    }
    if (ui->mode == UI_PRINT) {
        return;
    }
    if (ui->mode == UI_LINE) {
        g = ui_glyphs(ui);
        fprintf(stdout, "  %s %s\n", g->elbow, result->summary ? result->summary : "");
        fflush(stdout);
        return;
    }
    ln = log_by_id(ui, id);
    if (!ln) {
        return;
    }
    summary = xstrdup(result->summary ? result->summary : "");
    if (result->body && result->body[0]) {
        body = prepare_text(ui, result->body);
        if (body && strlen(body) > 8192) {
            body[8192] = 0;
        }
    }
    free(ln->summary);
    free(ln->body);
    ln->summary = summary;
    ln->body = body;
    ln->state = result->state;
    ln->diff = result->diff;
    ln->peek = result->peek > 0 ? result->peek : 4;
    ui_present(ui);
}

int ui_shell(Ui *ui, const char *cmd) {
    int rd = -1;
    pid_t pid = 0;
    int flags;
    char *hdr;
    if (!ui || !cmd || !cmd[0]) {
        return -1;
    }
    if (ui->mode != UI_SCREEN) {
        shell_sync(ui, cmd);
        return 0;
    }
    if (ui->job_fd >= 0 || ui->job_pid > 0) {
        ui_log(ui, UI_ERR, "a command is already running; ctrl-x kills it");
        return -1;
    }
    hdr = xmalloc(strlen(cmd) + 3);
    sprintf(hdr, "! %s", cmd);
    ui->job_id = ui_tool_begin(ui, hdr);
    free(hdr);
    if (spawn_shell(cmd, &rd, &pid) != 0) {
        ui_log(ui, UI_ERR, "could not start command");
        return -1;
    }
    flags = fcntl(rd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(rd, F_SETFL, flags | O_NONBLOCK);
    }
    ui->job_fd = rd;
    ui->job_pid = pid;
    ui->job_killed = 0;
    ui->job_shown = 0;
    buf_clear(&ui->job_acc);
    signals_set_job((int)pid);
    return 0;
}

char *ui_take_followup(Ui *ui) {
    char *s;
    if (!ui || !ui->followup || !ui->followup[0]) {
        return NULL;
    }
    s = ui->followup;
    ui->followup = NULL;
    return s;
}

char *ui_next_line(Ui *ui, int *quit) {
    if (quit) {
        *quit = 0;
    }
    if (!ui) {
        return NULL;
    }
    if (ui->mode == UI_LINE) {
        char buf[8192];
        fputs(ui->prompt ? ui->prompt : "> ", stdout);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {
            if (quit) {
                *quit = 1;
            }
            return NULL;
        }
        {
            size_t n = strlen(buf);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                buf[--n] = 0;
            }
        }
        if (g_sigint) {
            g_sigint = 0;
        }
        return xstrdup(buf);
    }
    while (!ui->quit) {
        fd_set r;
        int rc;
        ui_present(ui);
        if (g_sigint) {
            g_sigint = 0;
            if (ui->ed.len > 0) {
                ed_clear(&ui->ed);
            }
            continue;
        }
        FD_ZERO(&r);
        {
            int maxfd = -1;
            watch_extra(ui, &r, &maxfd);
            if (maxfd < STDIN_FILENO) {
                maxfd = STDIN_FILENO;
                FD_SET(STDIN_FILENO, &r);
            }
            rc = select(maxfd + 1, &r, NULL, NULL, NULL);
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc > 0 && ui->job_fd >= 0 && FD_ISSET(ui->job_fd, &r)) {
            job_pump(ui);
        }
        if (rc > 0 && FD_ISSET(STDIN_FILENO, &r)) {
            if (read_keys(ui, 0)) {
                char *line = xstrdup(ui->ed.data ? ui->ed.data : "");
                ed_clear(&ui->ed);
                if (ui->quit && line[0] == 0) {
                    free(line);
                    break;
                }
                return line;
            }
        }
    }
    if (quit) {
        *quit = 1;
    }
    return NULL;
}

/* The whole answer must be one of the words; a blank line or a sentence is not a yes. */
static int confirm_word(const char *line) {
    char *w = str_trim_dup(line);
    char *p;
    int answer = 0;
    for (p = w; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    if (str_eq(w, "y") || str_eq(w, "yes")) {
        answer = 'y';
    } else if (str_eq(w, "a") || str_eq(w, "always")) {
        answer = 'a';
    } else if (str_eq(w, "n") || str_eq(w, "no") || str_eq(w, "skip")) {
        answer = 'n';
    } else if (str_eq(w, "d") || str_eq(w, "deny")) {
        answer = 'd';
    }
    free(w);
    return answer;
}

int ui_confirm(Ui *ui, const UiAsk *ask) {
    if (!ui || !ask || ui->mode == UI_PRINT) {
        return 'n';
    }
    if (ui->mode != UI_SCREEN) {
        char buf[64];
        int tries;
        /* Lines typed while the reply streamed were meant for the prompt, not for this question. */
        if (isatty(STDIN_FILENO)) {
            tcflush(STDIN_FILENO, TCIFLUSH);
        }
        for (tries = 0; tries < 3; tries++) {
            int answer;
            printf("allow %s? type y (yes), n (no), a (always), or d (deny): ",
                   ask->what && ask->what[0] ? ask->what : (ask->title ? ask->title : "this"));
            fflush(stdout);
            if (!fgets(buf, sizeof buf, stdin)) {
                return 'n';
            }
            if (!strchr(buf, '\n')) {
                int c;
                while ((c = getchar()) != EOF && c != '\n') {
                }
                continue;
            }
            answer = confirm_word(buf);
            if (answer) {
                return answer;
            }
        }
        return 'n';
    }
    ui->ask = ask;
    ui->ask_sel = 0;
    ui->ask_scroll = 0;
    ui->suspend_palette = 1;
    memset(&ui->dec, 0, sizeof ui->dec);
    tcflush(STDIN_FILENO, TCIFLUSH);
    ui_invalidate(ui);
    {
        KeyDec dec;
        int answer = 0;
        static const char letters[] = "yand";
        memset(&dec, 0, sizeof dec);
        while (!answer) {
            fd_set r;
            unsigned char buf[64];
            ssize_t n;
            ssize_t i;
            int maxfd = -1;
            int rc;
            ui_present(ui);
            if (g_sigint) {
                g_sigint = 0;
                answer = 'n';
                break;
            }
            FD_ZERO(&r);
            watch_extra(ui, &r, &maxfd);
            if (maxfd < 0) {
                maxfd = STDIN_FILENO;
                FD_SET(STDIN_FILENO, &r);
            }
            rc = select(maxfd + 1, &r, NULL, NULL, NULL);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                answer = 'n';
                break;
            }
            if (ui->job_fd >= 0 && FD_ISSET(ui->job_fd, &r)) {
                job_pump(ui);
            }
            if (!FD_ISSET(STDIN_FILENO, &r)) {
                continue;
            }
            n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                answer = 'n';
                break;
            }
            for (i = 0; i < n && !answer; i++) {
                Key key;
                if (buf[i] == 0x03) {
                    answer = 'n';
                    break;
                }
                if (!key_feed(&dec, buf[i], &key)) {
                    continue;
                }
                if (key.kind == KEY_UP) {
                    ui->ask_sel = (ui->ask_sel + 3) % 4;
                } else if (key.kind == KEY_DOWN) {
                    ui->ask_sel = (ui->ask_sel + 1) % 4;
                } else if (key.kind == KEY_ENTER) {
                    answer = letters[ui->ask_sel];
                } else if (key.kind == KEY_ESC) {
                    answer = 'n';
                } else if (key.kind == KEY_CTRL_X) {
                    job_shutdown(ui, 1);
                } else if (key.kind == KEY_PGUP) {
                    ui->ask_scroll += 4;
                } else if (key.kind == KEY_PGDN) {
                    ui->ask_scroll -= 4;
                    if (ui->ask_scroll < 0) {
                        ui->ask_scroll = 0;
                    }
                } else if (key.kind == KEY_CHAR && key.nraw == 1) {
                    unsigned char c = key.raw[0];
                    if (c >= '1' && c <= '4') {
                        answer = letters[c - '1'];
                    } else if (c == 'y' || c == 'Y') {
                        answer = 'y';
                    } else if (c == 'a' || c == 'A') {
                        answer = 'a';
                    } else if (c == 'n' || c == 'N') {
                        answer = 'n';
                    } else if (c == 'd' || c == 'D') {
                        answer = 'd';
                    }
                }
            }
            if (!answer) {
                Key key;
                if (key_flush_esc(&dec, &key) && key.kind == KEY_ESC) {
                    answer = 'n';
                }
            }
        }
        ui->ask = NULL;
        ui->ask_sel = 0;
        ui->ask_scroll = 0;
        ui->suspend_palette = 0;
        ui_invalidate(ui);
        ui_present(ui);
        return answer ? answer : 'n';
    }
}

void ui_set_choices(Ui *ui, const UiChoice *choices, int n) {
    if (!ui) {
        return;
    }
    ui->choices = choices;
    ui->nchoices = n > 0 ? n : 0;
    ui->slash_sel = 0;
}

static int read_plain_line(char *buf, size_t cap) {
    size_t n;
    if (!fgets(buf, (int)cap, stdin)) {
        return -1;
    }
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    return 0;
}

static int label_matches(const char *label, const char *input) {
    size_t n;
    if (!label || !input || !input[0]) {
        return 0;
    }
    n = strlen(input);
    if (strncmp(label, input, n) != 0) {
        return 0;
    }
    return label[n] == 0 || label[n] == ' ';
}

int ui_menu(Ui *ui, const char *title, const char *const *labels, int n, int selected) {
    int i;
    if (n <= 0 || !labels) {
        return -1;
    }
    if (selected < 0 || selected >= n) {
        selected = 0;
    }
    if (!ui || ui->mode != UI_SCREEN) {
        char buf[256];
        char *end = NULL;
        long num;
        printf("%s\n", title ? title : "choose");
        puts("blank line goes back");
        for (i = 0; i < n; i++) {
            printf("  %d  %s\n", i + 1, labels[i] ? labels[i] : "");
        }
        fputs("choice: ", stdout);
        fflush(stdout);
        if (read_plain_line(buf, sizeof buf) != 0 || !buf[0]) {
            return -1;
        }
        num = strtol(buf, &end, 10);
        if (end && end != buf && *end == 0 && num >= 1 && num <= n) {
            return (int)num - 1;
        }
        for (i = 0; i < n; i++) {
            if (label_matches(labels[i], buf)) {
                return i;
            }
        }
        return -1;
    }
    ui->menu_on = 1;
    ui->menu_title = title && title[0] ? title : "choose   enter selects, esc cancels";
    ui->menu_items = labels;
    ui->menu_n = n;
    ui->menu_sel = selected;
    ui->suspend_palette = 1;
    ui_invalidate(ui);
    {
        KeyDec dec;
        int done = 0;
        int result = -1;
        memset(&dec, 0, sizeof dec);
        while (!done) {
            fd_set rset;
            unsigned char buf[64];
            ssize_t got;
            ui_present(ui);
            if (g_sigint) {
                g_sigint = 0;
                result = -1;
                break;
            }
            FD_ZERO(&rset);
            {
                int maxfd = -1;
                int rc;
                watch_extra(ui, &rset, &maxfd);
                if (maxfd < 0) {
                    maxfd = STDIN_FILENO;
                    FD_SET(STDIN_FILENO, &rset);
                }
                rc = select(maxfd + 1, &rset, NULL, NULL, NULL);
                if (rc < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    result = -1;
                    break;
                }
            }
            if (ui->job_fd >= 0 && FD_ISSET(ui->job_fd, &rset)) {
                job_pump(ui);
            }
            if (!FD_ISSET(STDIN_FILENO, &rset)) {
                continue;
            }
            got = read(STDIN_FILENO, buf, sizeof buf);
            if (got <= 0) {
                result = -1;
                break;
            }
            for (i = 0; i < got; i++) {
                Key key;
                if (!key_feed(&dec, buf[i], &key)) {
                    continue;
                }
                if (key.kind == KEY_UP) {
                    ui->menu_sel = (ui->menu_sel + n - 1) % n;
                } else if (key.kind == KEY_DOWN) {
                    ui->menu_sel = (ui->menu_sel + 1) % n;
                } else if (key.kind == KEY_PGUP) {
                    ui->menu_sel -= 5;
                    if (ui->menu_sel < 0) {
                        ui->menu_sel = 0;
                    }
                } else if (key.kind == KEY_PGDN) {
                    ui->menu_sel += 5;
                    if (ui->menu_sel >= n) {
                        ui->menu_sel = n - 1;
                    }
                } else if (key.kind == KEY_ENTER) {
                    result = ui->menu_sel;
                    done = 1;
                    break;
                } else if (key.kind == KEY_ESC) {
                    result = -1;
                    done = 1;
                    break;
                } else if (key.kind == KEY_CTRL_X) {
                    job_shutdown(ui, 1);
                } else if (key.kind == KEY_CHAR && key.nraw == 1 && key.raw[0] >= '1' && key.raw[0] <= '9') {
                    int pick = key.raw[0] - '1';
                    if (pick < n) {
                        result = pick;
                        done = 1;
                        break;
                    }
                }
            }
            if (!done) {
                Key key;
                if (key_flush_esc(&dec, &key)) {
                    result = -1;
                    done = 1;
                }
            }
        }
        ui->menu_on = 0;
        ui->menu_title = NULL;
        ui->menu_items = NULL;
        ui->menu_n = 0;
        ui->suspend_palette = 0;
        ui_invalidate(ui);
        ui_present(ui);
        return result;
    }
}

char *ui_read_line(Ui *ui, const char *prompt, int hidden) {
    if (!ui || ui->mode != UI_SCREEN) {
        struct termios orig, off;
        int tty = hidden && isatty(STDIN_FILENO);
        char buf[1024];
        fputs(prompt ? prompt : "> ", stdout);
        fflush(stdout);
        if (tty && tcgetattr(STDIN_FILENO, &orig) == 0) {
            off = orig;
            off.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &off);
        }
        if (read_plain_line(buf, sizeof buf) != 0) {
            if (tty) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            }
            return NULL;
        }
        if (tty) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            fputc('\n', stdout);
        }
        return xstrdup(buf);
    }
    {
        char *saved_prompt = xstrdup(ui->prompt ? ui->prompt : "");
        char *saved_line = xstrdup(ui->ed.data ? ui->ed.data : "");
        char *result = NULL;
        free(ui->prompt);
        ui->prompt = xstrdup(prompt ? prompt : "> ");
        ed_clear(&ui->ed);
        ui->hide_input = hidden;
        ui->suspend_palette = 1;
        ui->line_prompt = 1;
        ui->line_cancel = 0;
        ui_invalidate(ui);
        while (!ui->quit) {
            fd_set rset;
            ui_present(ui);
            if (g_sigint) {
                g_sigint = 0;
                break;
            }
            FD_ZERO(&rset);
            {
                int maxfd = -1;
                int rc;
                watch_extra(ui, &rset, &maxfd);
                if (maxfd < 0) {
                    maxfd = STDIN_FILENO;
                    FD_SET(STDIN_FILENO, &rset);
                }
                rc = select(maxfd + 1, &rset, NULL, NULL, NULL);
                if (rc < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
            }
            if (ui->job_fd >= 0 && FD_ISSET(ui->job_fd, &rset)) {
                job_pump(ui);
            }
            if (FD_ISSET(STDIN_FILENO, &rset) && read_keys(ui, 0)) {
                if (ui->line_cancel || (ui->quit && ui->ed.len == 0)) {
                    ui->quit = 0;
                    result = NULL;
                } else {
                    result = xstrdup(ui->ed.data ? ui->ed.data : "");
                }
                break;
            }
        }
        ui->hide_input = 0;
        ui->suspend_palette = 0;
        ui->line_prompt = 0;
        ui->line_cancel = 0;
        ed_set(&ui->ed, saved_line);
        free(saved_line);
        free(ui->prompt);
        ui->prompt = saved_prompt;
        ui_invalidate(ui);
        ui_present(ui);
        return result;
    }
}
