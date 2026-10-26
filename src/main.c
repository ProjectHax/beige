#include "app.h"

#include "models.h"
#include "oauth.h"
#include "signals.h"

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BEIGE_VERSION "0.1.0"

static const char *HELP =
    "beige " BEIGE_VERSION " — terminal coding agent\n"
    "\n"
    "Usage:\n"
    "  beige                         interactive session\n"
    "  beige -p TEXT                 run one prompt and exit\n"
    "  beige --continue              resume the newest session in this directory\n"
    "  beige --mode ask|plan|auto\n"
    "  beige --provider NAME         anthropic, openai, xai, xai-sub, openai-sub\n"
    "  beige --model NAME\n"
    "  beige --base-url URL\n"
    "  beige --api-key KEY\n"
    "  beige --no-context            do not load AGENTS.md or CLAUDE.md\n"
    "  beige login PROVIDER\n"
    "  beige logout PROVIDER\n"
    "  beige status\n"
    "\n"
    "Modes: ask prompts before writes and commands, plan cannot edit source,\n"
    "auto runs tools. Catastrophic commands still ask. Ctrl-T or Shift-Tab\n"
    "cycles modes. Esc interrupts a reply. Ctrl-C cancels it. Ctrl-D exits.\n"
    "Ctrl-O expands tool output. ? shows shortcuts. Alt-Enter inserts a newline.\n"
    "!command runs a shell command in the background; Ctrl-X kills it.\n"
    "\n"
    "Slash commands: type / and choose with the arrow keys. Esc closes the\n"
    "menu in front of you, or returns to the previous menu.\n"
    "  /help /mode /plan /status /model /provider /login /logout /clear /compact /init /quit /exit\n";

static void usage(void) {
    fputs(HELP, stdout);
}

static const char *WARNING =
    "Warning: the model can do something destructive, especially on an older Linux "
    "distribution. There is zero liability and zero responsibility for what it does.";

static const char *first_run(void) {
    char *home;
    char *path;
    struct stat st;
    home = beige_home();
    path = path_join(home, "warned");
    free(home);
    if (stat(path, &st) == 0) {
        free(path);
        return NULL;
    }
    write_file(path, "1\n", 2, 0644);
    free(path);
    return WARNING;
}

static int read_secret(char **out) {
    struct termios orig, off;
    int tty = isatty(STDIN_FILENO);
    char buf[1024];
    size_t n;
    fputs("API key: ", stdout);
    fflush(stdout);
    if (tty && tcgetattr(STDIN_FILENO, &orig) == 0) {
        off = orig;
        off.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &off);
    }
    if (!fgets(buf, sizeof buf, stdin)) {
        if (tty) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        }
        return -1;
    }
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        fputc('\n', stdout);
    }
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    *out = xstrdup(buf);
    return 0;
}

static void note_err(void *ud, const char *line) {
    (void)ud;
    fprintf(stderr, "%s\n", line ? line : "");
}

static void note_ui(void *ud, const char *line) {
    ui_log(((App *)ud)->ui, UI_META, line);
}

static void tell(App *app, int is_err, const char *msg) {
    if (app && app->ui && !ui_is_print(app->ui)) {
        ui_log(app->ui, is_err ? UI_ERR : UI_META, msg);
        return;
    }
    if (is_err) {
        fprintf(stderr, "%s\n", msg ? msg : "");
    } else {
        printf("%s\n", msg ? msg : "");
    }
}

static char *prompt_secret(App *app, int back) {
    char *key = NULL;
    if (app && app->ui && !ui_is_print(app->ui)) {
        return ui_read_line(app->ui, back ? "API key (esc back): " : "API key (esc cancels): ", 1);
    }
    if (read_secret(&key) != 0) {
        free(key);
        return NULL;
    }
    return key;
}

static int do_login(App *app, const char *which, OauthNote note, void *ud, int back) {
    char *err = NULL;
    if (!which || !which[0]) {
        fprintf(stderr, "usage: beige login anthropic|openai|xai|xai-sub|openai-sub\n");
        return 2;
    }
    if (str_eq(which, "anthropic") || str_eq(which, "openai") || str_eq(which, "xai")) {
        char *key = prompt_secret(app, back);
        char saved[80];
        if (!key || !key[0]) {
            tell(app, 1, "no key entered");
            free(key);
            return 2;
        }
        auth_set_key(&app->auth, which, key);
        free(key);
        if (auth_save(&app->auth) != 0) {
            tell(app, 1, "could not save credentials");
            return 1;
        }
        snprintf(saved, sizeof saved, "saved %s API key", which);
        tell(app, 0, saved);
        return 0;
    }
    if (str_eq(which, "xai-sub")) {
        OAuthTok *tok = auth_oauth(&app->auth, which);
        if (oauth_login_xai(&app->http, note, ud, tok, &err) != 0) {
            tell(app, 1, err ? err : "login failed");
            free(err);
            return 1;
        }
        free(err);
        auth_save(&app->auth);
        tell(app, 0, "signed in to xAI");
        return 0;
    }
    if (str_eq(which, "openai-sub")) {
        OAuthTok *tok = auth_oauth(&app->auth, which);
        if (oauth_login_openai(&app->http, note, ud, tok, &err) != 0) {
            tell(app, 1, err ? err : "login failed");
            free(err);
            return 1;
        }
        free(err);
        auth_save(&app->auth);
        tell(app, 0, "signed in with ChatGPT");
        tell(app, 0, "This path is not a documented OpenAI API and can stop working.");
        return 0;
    }
    tell(app, 1, "unknown provider");
    return 2;
}

static int do_logout(App *app, const char *which) {
    if (!which) {
        fprintf(stderr, "usage: beige logout PROVIDER\n");
        return 2;
    }
    if (str_eq(which, "xai-sub") || str_eq(which, "openai-sub")) {
        OAuthTok *tok = auth_oauth(&app->auth, which);
        if (tok) {
            oauth_tok_clear(tok);
        }
    } else {
        auth_set_key(&app->auth, which, "");
    }
    auth_save(&app->auth);
    {
        char msg[80];
        snprintf(msg, sizeof msg, "removed %s credentials", which);
        tell(app, 0, msg);
    }
    return 0;
}

static int do_status(App *app) {
    const char *key = auth_api_key(&app->auth, app->cfg.provider);
    OAuthTok *tok = auth_oauth(&app->auth, app->cfg.provider);
    printf("provider: %s\n", app->cfg.provider);
    printf("model: %s\n", app->cfg.model[0] ? app->cfg.model : provider_default_model(app->cfg.provider));
    printf("mode: %s\n", app->cfg.mode);
    printf("directory: %s\n", app->cwd ? app->cwd : ".");
    if (tok && tok->access && tok->access[0]) {
        printf("auth: subscription token\n");
    } else if (key && key[0]) {
        printf("auth: api key\n");
    } else {
        printf("auth: none\n");
    }
    printf("ca bundle: %s\n", app->http.ca_bundle ? app->http.ca_bundle : "none");
    return 0;
}

static const Block *find_tool_result(App *app, int from, const char *id) {
    int i, j;
    if (!id || !id[0]) {
        return NULL;
    }
    for (i = from; i < app->ts.n; i++) {
        for (j = 0; j < app->ts.msgs[i].nblocks; j++) {
            Block *b = &app->ts.msgs[i].blocks[j];
            if (b->type == BLOCK_TOOL_RESULT && b->id && str_eq(b->id, id)) {
                return b;
            }
        }
    }
    return NULL;
}

static void replay(App *app) {
    int i, j;
    for (i = 0; i < app->ts.n; i++) {
        for (j = 0; j < app->ts.msgs[i].nblocks; j++) {
            Block *b = &app->ts.msgs[i].blocks[j];
            if (b->type == BLOCK_TEXT && b->text && b->text[0]) {
                ui_log(app->ui, app->ts.msgs[i].role == ROLE_ASSISTANT ? UI_ASSIST : UI_USER, b->text);
            } else if (b->type == BLOCK_TOOL_USE) {
                app_log_tool(app, b, find_tool_result(app, i, b->id));
            }
        }
    }
}

static int on_ui_key(void *ud, int kind) {
    App *app = ud;
    PermMode mode;
    if (kind != KEY_CTRL_T && kind != KEY_SHIFT_TAB) {
        return 0;
    }
    mode = app->perm.mode;
    if (mode == MODE_ASK) {
        mode = MODE_AUTO;
    } else if (mode == MODE_AUTO) {
        mode = MODE_PLAN;
    } else {
        mode = MODE_ASK;
    }
    perm_set_mode(&app->perm, mode);
    free(app->cfg.mode);
    app->cfg.mode = xstrdup(perm_mode_name(mode));
    app->mode_from_cli = 0;
    app_save_config(app);
    app_rebuild_system(app);
    app_refresh_status(app);
    if (!ui_is_screen(app->ui)) {
        ui_log(app->ui, UI_META, perm_mode_name(mode));
    }
    return 1;
}

static void set_mode(App *app, PermMode mode) {
    perm_set_mode(&app->perm, mode);
    free(app->cfg.mode);
    app->cfg.mode = xstrdup(perm_mode_name(mode));
    app->mode_from_cli = 0;
    app_rebuild_system(app);
    app_refresh_status(app);
}

/* A provider or model picked in the session replaces the command-line one and is saved. */
static void drop_override(char **slot) {
    free(*slot);
    *slot = NULL;
}

static const UiChoice COMMANDS[] = {
    {"/help", "list commands"},
    {"/mode", "ask, plan, or auto"},
    {"/model", "choose a model"},
    {"/provider", "choose a provider"},
    {"/login", "api key or subscription"},
    {"/logout", "forget credentials"},
    {"/clear", "fresh context"},
    {"/compact", "summarize the session"},
    {"/init", "draft AGENTS.md"},
    {"/plan", "enter plan mode"},
    {"/status", "provider, model, and auth"},
    {"/quit", "exit beige"},
    {"/exit", "alias of /quit"},
};

static const char *PROVIDER_NAMES[] = {"anthropic", "openai", "xai", "xai-sub", "openai-sub"};

enum { SLASH_DONE = 0, SLASH_QUIT = 1, SLASH_BACK = 2 };

static const char *menu_title(char *buf, size_t cap, const char *name, int back) {
    snprintf(buf, cap, "%s   enter selects, %s", name, back ? "esc back" : "esc closes");
    return buf;
}

static int slash_cancel(int nested) {
    return nested ? SLASH_BACK : SLASH_DONE;
}

static int pick_labeled(App *app, const char *title, const char *const *values, int n, const char *current) {
    const char *labels[16];
    char storage[16][96];
    int i;
    int sel = 0;
    if (n > 16) {
        n = 16;
    }
    for (i = 0; i < n; i++) {
        int here = current && values[i] && str_eq(values[i], current);
        snprintf(storage[i], sizeof storage[i], "%s%s", values[i] ? values[i] : "", here ? "  current" : "");
        labels[i] = storage[i];
        if (here) {
            sel = i;
        }
    }
    return ui_menu(app->ui, title, labels, n, sel);
}

static int pick_provider(App *app, const char *title) {
    return pick_labeled(app, title, PROVIDER_NAMES, 5, app->cfg.provider);
}

static int provider_signed_in(App *app, const char *id) {
    OAuthTok *tok = auth_oauth(&app->auth, id);
    const char *key;
    if (tok && tok->access && tok->access[0]) {
        return 1;
    }
    if (app->key_override && app->key_override[0] && str_eq(id, app->cfg.provider) && !str_eq(id, "openai-sub") &&
        !str_eq(id, "xai-sub")) {
        return 1;
    }
    key = auth_api_key(&app->auth, id);
    return key && key[0];
}

static void use_pair(App *app, const char *provider, const char *model) {
    char line[180];
    char *err = NULL;
    if (!provider || !model || !model[0]) {
        return;
    }
    free(app->cfg.provider);
    app->cfg.provider = xstrdup(provider);
    free(app->cfg.model);
    app->cfg.model = xstrdup(model);
    drop_override(&app->provider_override);
    drop_override(&app->model_override);
    app_save_config(app);
    if (app_bind_provider(app, &err) != 0) {
        ui_log(app->ui, UI_ERR, err ? err : "provider change failed");
    }
    free(err);
    app_refresh_status(app);
    snprintf(line, sizeof line, "using %s / %s", provider, model);
    ui_log(app->ui, UI_META, line);
}

static const char *const *models_for(const char *provider, int *n);

static int choose_signed_in_model(App *app, int back) {
    static const char *names[] = {"anthropic", "openai", "xai", "xai-sub", "openai-sub"};
    typedef struct {
        char *provider;
        char *model;
    } Pair;
    Pair *pairs = NULL;
    int np = 0;
    const char **labels = NULL;
    int i;
    int sel;
    int any = 0;
    for (i = 0; i < 5; i++) {
        Provider *p = NULL;
        char **ids = NULL;
        int n = 0;
        char *err = NULL;
        int k;
        if (!provider_signed_in(app, names[i])) {
            continue;
        }
        any = 1;
        if (app_open_provider(app, names[i], &p, &err) != 0) {
            ui_log(app->ui, UI_ERR, err ? err : "could not open provider");
            free(err);
            continue;
        }
        free(err);
        err = NULL;
        if (models_fetch(&app->http, p, &ids, &n, &err) != 0) {
            ui_log(app->ui, UI_ERR, err ? err : "could not list models");
            free(err);
            provider_free(p);
            continue;
        }
        provider_free(p);
        for (k = 0; k < n; k++) {
            pairs = xrealloc(pairs, (size_t)(np + 1) * sizeof(Pair));
            pairs[np].provider = xstrdup(names[i]);
            pairs[np].model = ids[k];
            ids[k] = NULL;
            np++;
        }
        models_free(ids, n);
    }
    if (np == 0) {
        int n = 0;
        const char *const *fallback = models_for(app->cfg.provider, &n);
        char title[96];
        if (!any) {
            ui_log(app->ui, UI_ERR, "sign in before choosing a model");
            return -1;
        }
        ui_log(app->ui, UI_ERR, "could not list models; choose from the built-in names");
        menu_title(title, sizeof title, "model", back);
        for (;;) {
            int pick = pick_labeled(app, title, fallback, n, app->cfg.model);
            char *typed;
            if (pick < 0) {
                return 0;
            }
            if (!str_eq(fallback[pick], "type a model name")) {
                use_pair(app, app->cfg.provider, fallback[pick]);
                return 1;
            }
            typed = ui_read_line(app->ui, "model (esc back): ", 0);
            if (typed && typed[0]) {
                use_pair(app, app->cfg.provider, typed);
                free(typed);
                return 1;
            }
            free(typed);
        }
    }
    labels = xmalloc((size_t)(np + 1) * sizeof(char *));
    for (i = 0; i < np; i++) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s  %s", pairs[i].provider, pairs[i].model);
        labels[i] = xstrdup(buf);
    }
    labels[np] = xstrdup("type a model name");
    {
        int picked = 0;
        char title[96];
        int keep = 0;
        snprintf(title, sizeof title, "model   signed-in providers, %s", back ? "esc back" : "esc closes");
        for (;;) {
            sel = ui_menu(app->ui, title, labels, np + 1, keep);
            if (sel < 0) {
                break;
            }
            keep = sel;
            if (sel < np) {
                use_pair(app, pairs[sel].provider, pairs[sel].model);
                picked = 1;
                break;
            }
            if (sel == np) {
                char *typed = ui_read_line(app->ui, "model (esc back): ", 0);
                if (typed && typed[0]) {
                    use_pair(app, app->cfg.provider, typed);
                    free(typed);
                    picked = 1;
                    break;
                }
                free(typed);
            }
        }
        for (i = 0; i < np + 1; i++) {
            free((void *)labels[i]);
        }
        free(labels);
        for (i = 0; i < np; i++) {
            free(pairs[i].provider);
            free(pairs[i].model);
        }
        free(pairs);
        return picked;
    }
}

static void apply_model(App *app, const char *name) {
    char *err = NULL;
    if (!name || !name[0] || str_eq(name, "type a model name")) {
        return;
    }
    free(app->cfg.model);
    app->cfg.model = xstrdup(name);
    drop_override(&app->model_override);
    app_save_config(app);
    if (app_bind_provider(app, &err) != 0) {
        ui_log(app->ui, UI_ERR, err ? err : "model change failed");
    }
    free(err);
    app_refresh_status(app);
    ui_log(app->ui, UI_META, app->cfg.model);
}

static void apply_provider(App *app, const char *name) {
    char *err = NULL;
    if (!name || !provider_is_known(name)) {
        ui_log(app->ui, UI_ERR, "provider is anthropic, openai, xai, xai-sub, or openai-sub");
        return;
    }
    free(app->cfg.provider);
    app->cfg.provider = xstrdup(name);
    drop_override(&app->provider_override);
    app_save_config(app);
    if (app_bind_provider(app, &err) != 0) {
        ui_log(app->ui, UI_ERR, err ? err : "provider change failed");
    }
    free(err);
    app_refresh_status(app);
    ui_log(app->ui, UI_META, app->cfg.provider);
}

static const char *const *models_for(const char *provider, int *n) {
    static const char *anthropic_models[] = {"claude-sonnet-4-5", "claude-opus-4-5", "claude-haiku-4-5",
                                              "type a model name"};
    static const char *openai_models[] = {"gpt-4.1", "gpt-5.4", "gpt-5.4-mini", "type a model name"};
    static const char *xai_models[] = {"grok-4", "grok-3", "type a model name"};
    if (str_eq(provider, "anthropic")) {
        *n = 4;
        return anthropic_models;
    }
    if (str_eq(provider, "xai") || str_eq(provider, "xai-sub")) {
        *n = 3;
        return xai_models;
    }
    *n = 4;
    return openai_models;
}

static int slash(App *app, const char *line, int nested) {
    char *copy = xstrdup(line);
    char *cmd = copy;
    char *arg;
    int quit = 0;
    int exact = -1;
    int i;
    while (*cmd == ' ') {
        cmd++;
    }
    arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = NULL;
        }
    }
    for (i = 0; i < (int)(sizeof COMMANDS / sizeof COMMANDS[0]); i++) {
        if (str_eq(cmd, COMMANDS[i].label)) {
            exact = i;
            break;
        }
    }
    if (exact < 0 && cmd[0] == '/') {
        const char *labels[16];
        char storage[16][128];
        int map[16];
        int n = 0;
        int sel;
        for (i = 0; i < (int)(sizeof COMMANDS / sizeof COMMANDS[0]) && n < 16; i++) {
            if (str_starts(COMMANDS[i].label, cmd)) {
                snprintf(storage[n], sizeof storage[n], "%s  %s", COMMANDS[i].label, COMMANDS[i].hint);
                labels[n] = storage[n];
                map[n] = i;
                n++;
            }
        }
        if (n == 0) {
            ui_log(app->ui, UI_ERR, "unknown command, try /help");
            free(copy);
            return SLASH_DONE;
        }
        {
            int keep = 0;
            for (;;) {
                char title[80];
                char *again;
                int child;
                sel = ui_menu(app->ui, menu_title(title, sizeof title, "commands", nested), labels, n, keep);
                if (sel < 0 || sel >= n) {
                    free(copy);
                    return slash_cancel(nested);
                }
                keep = sel;
                again = xstrdup(COMMANDS[map[sel]].label);
                child = slash(app, again, 1);
                free(again);
                if (child == SLASH_BACK) {
                    continue;
                }
                free(copy);
                return child;
            }
        }
    }
    if (str_eq(cmd, "/quit") || str_eq(cmd, "/exit") || str_eq(cmd, "/q")) {
        quit = 1;
    } else if (str_eq(cmd, "/help")) {
        ui_log(app->ui, UI_META, HELP);
    } else if (str_eq(cmd, "/mode")) {
        PermMode mode;
        if (!arg) {
            const char *modes[] = {"ask", "plan", "auto"};
            char title[80];
            int sel = pick_labeled(app, menu_title(title, sizeof title, "mode", nested), modes, 3,
                                   perm_mode_name(app->perm.mode));
            if (sel < 0) {
                free(copy);
                return slash_cancel(nested);
            }
            if (perm_mode_parse(modes[sel], &mode) == 0) {
                set_mode(app, mode);
                app_save_config(app);
                ui_log(app->ui, UI_META, perm_mode_name(mode));
            }
        } else if (perm_mode_parse(arg, &mode) != 0) {
            ui_log(app->ui, UI_ERR, "mode is ask, plan, or auto");
        } else {
            set_mode(app, mode);
            app_save_config(app);
            ui_log(app->ui, UI_META, perm_mode_name(mode));
        }
    } else if (str_eq(cmd, "/model")) {
        if (!arg) {
            if (choose_signed_in_model(app, nested) <= 0) {
                free(copy);
                return slash_cancel(nested);
            }
        } else {
            apply_model(app, arg);
        }
    } else if (str_eq(cmd, "/provider")) {
        if (!arg) {
            char title[80];
            int sel = pick_provider(app, menu_title(title, sizeof title, "provider", nested));
            if (sel < 0) {
                free(copy);
                return slash_cancel(nested);
            }
            apply_provider(app, PROVIDER_NAMES[sel]);
        } else {
            apply_provider(app, arg);
        }
    } else if (str_eq(cmd, "/clear")) {
        transcript_free(&app->ts);
        transcript_init(&app->ts);
        session_close(app->session);
        app->session = session_begin(app->cwd);
        free(app->last_fp);
        app->last_fp = NULL;
        app->tool_streak = 0;
        app->compact_floor = 0;
        app_refresh_status(app);
        ui_log(app->ui, UI_META, "context cleared");
    } else if (str_eq(cmd, "/compact")) {
        app_compact(app);
    } else if (str_eq(cmd, "/init")) {
        app_init_agents(app);
    } else if (str_eq(cmd, "/plan")) {
        char *path;
        char *text = NULL;
        size_t n = 0;
        int trunc = 0;
        char *rerr = NULL;
        set_mode(app, MODE_PLAN);
        app_save_config(app);
        app_refresh_status(app);
        ui_log(app->ui, UI_META, "plan");
        path = path_join(app->cwd ? app->cwd : ".", ".beige/plan.md");
        text = read_file_cap(path, 4096, &n, &trunc, &rerr);
        if (text && text[0]) {
            ui_log(app->ui, UI_META, text);
            if (trunc) {
                ui_log(app->ui, UI_META, "[plan truncated]");
            }
        } else {
            ui_log(app->ui, UI_META, "no plan file yet");
        }
        free(text);
        free(rerr);
        free(path);
    } else if (str_eq(cmd, "/status")) {
        const char *key = auth_api_key(&app->auth, app->cfg.provider);
        OAuthTok *tok = auth_oauth(&app->auth, app->cfg.provider);
        char line[256];
        snprintf(line, sizeof line, "provider: %s", app->cfg.provider ? app->cfg.provider : "");
        ui_log(app->ui, UI_META, line);
        snprintf(line, sizeof line, "model: %s",
                 app->cfg.model && app->cfg.model[0] ? app->cfg.model : provider_default_model(app->cfg.provider));
        ui_log(app->ui, UI_META, line);
        snprintf(line, sizeof line, "mode: %s", perm_mode_name(app->perm.mode));
        ui_log(app->ui, UI_META, line);
        snprintf(line, sizeof line, "directory: %s", app->cwd ? app->cwd : ".");
        ui_log(app->ui, UI_META, line);
        if (tok && tok->access && tok->access[0]) {
            ui_log(app->ui, UI_META, "auth: subscription token");
        } else if (key && key[0]) {
            ui_log(app->ui, UI_META, "auth: api key");
        } else {
            ui_log(app->ui, UI_META, "auth: none");
        }
        snprintf(line, sizeof line, "ca bundle: %s", app->http.ca_bundle ? app->http.ca_bundle : "none");
        ui_log(app->ui, UI_META, line);
    } else if (str_eq(cmd, "/login")) {
        if (arg) {
            int mark = ui_log_mark(app->ui);
            int lrc = do_login(app, arg, note_ui, app, 0);
            if (lrc == 0 && app->ui && !ui_is_print(app->ui)) {
                char signed_in[80];
                ui_log_trim(app->ui, mark);
                free(app->cfg.provider);
                app->cfg.provider = xstrdup(arg);
                drop_override(&app->provider_override);
                app_save_config(app);
                if (choose_signed_in_model(app, 0) <= 0) {
                    free(app->cfg.model);
                    app->cfg.model = xstrdup(provider_default_model(arg));
                    drop_override(&app->model_override);
                    app_save_config(app);
                    snprintf(signed_in, sizeof signed_in, "signed in to %s", arg);
                    ui_log(app->ui, UI_META, signed_in);
                }
            }
            app_refresh_status(app);
        } else {
            int pending = 0;
            const char *pending_which = NULL;
            for (;;) {
                char title[80];
                char signed_in[80];
                int sel = pick_provider(app, menu_title(title, sizeof title, "login", nested));
                int mark;
                int lrc;
                int picked;
                const char *which;
                if (sel < 0) {
                    if (pending && pending_which && app->ui && !ui_is_print(app->ui)) {
                        free(app->cfg.model);
                        app->cfg.model = xstrdup(provider_default_model(pending_which));
                        drop_override(&app->model_override);
                        app_save_config(app);
                        snprintf(signed_in, sizeof signed_in, "signed in to %s", pending_which);
                        ui_log(app->ui, UI_META, signed_in);
                    }
                    app_refresh_status(app);
                    free(copy);
                    return slash_cancel(nested);
                }
                which = PROVIDER_NAMES[sel];
                mark = ui_log_mark(app->ui);
                lrc = do_login(app, which, note_ui, app, 1);
                if (lrc != 0) {
                    continue;
                }
                if (!app->ui || ui_is_print(app->ui)) {
                    app_refresh_status(app);
                    free(copy);
                    return SLASH_DONE;
                }
                ui_log_trim(app->ui, mark);
                free(app->cfg.provider);
                app->cfg.provider = xstrdup(which);
                drop_override(&app->provider_override);
                app_save_config(app);
                picked = choose_signed_in_model(app, 1);
                app_refresh_status(app);
                if (picked > 0) {
                    free(copy);
                    return SLASH_DONE;
                }
                pending = 1;
                pending_which = which;
            }
        }
    } else if (str_eq(cmd, "/logout")) {
        const char *which = arg;
        if (!which) {
            char title[80];
            int sel = pick_provider(app, menu_title(title, sizeof title, "logout", nested));
            if (sel < 0) {
                free(copy);
                return slash_cancel(nested);
            }
            which = PROVIDER_NAMES[sel];
        }
        do_logout(app, which);
    } else {
        ui_log(app->ui, UI_ERR, "unknown command, try /help");
    }
    free(copy);
    return quit;
}

static int arg_value(int argc, char **argv, int *i, const char *flag, char **out) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", flag);
        return -1;
    }
    *out = argv[++(*i)];
    return 0;
}

int main(int argc, char **argv) {
    App app;
    int i;
    char *print_prompt = NULL;
    char *mode_arg = NULL;
    char *err = NULL;
    int rc = 0;
    int saw_command = 0;

    signals_install();
    ignore_sigpipe();
    app_init(&app);

    if (argc >= 2 && (str_eq(argv[1], "login") || str_eq(argv[1], "logout") || str_eq(argv[1], "status"))) {
        saw_command = 1;
    }
    for (i = 1; !saw_command && i < argc; i++) {
        const char *a = argv[i];
        if (str_eq(a, "-h") || str_eq(a, "--help")) {
            usage();
            app_shutdown(&app);
            return 0;
        }
        if (str_eq(a, "-v") || str_eq(a, "--version") || str_eq(a, "--version")) {
            printf("beige %s\n", BEIGE_VERSION);
            app_shutdown(&app);
            return 0;
        }
        if (str_eq(a, "-p") || str_eq(a, "--print")) {
            app.print_mode = 1;
            if (arg_value(argc, argv, &i, a, &print_prompt) != 0) {
                app_shutdown(&app);
                return 2;
            }
            continue;
        }
        if (str_eq(a, "--continue")) {
            app.resume = 1;
            continue;
        }
        if (str_eq(a, "--no-context")) {
            app.no_context = 1;
            continue;
        }
        if (str_eq(a, "--mode")) {
            if (arg_value(argc, argv, &i, a, &mode_arg) != 0) {
                app_shutdown(&app);
                return 2;
            }
            continue;
        }
        if (str_eq(a, "--model")) {
            if (arg_value(argc, argv, &i, a, &app.model_override) != 0) {
                app_shutdown(&app);
                return 2;
            }
            app.model_override = xstrdup(app.model_override);
            continue;
        }
        if (str_eq(a, "--provider")) {
            if (arg_value(argc, argv, &i, a, &app.provider_override) != 0) {
                app_shutdown(&app);
                return 2;
            }
            app.provider_override = xstrdup(app.provider_override);
            continue;
        }
        if (str_eq(a, "--base-url")) {
            if (arg_value(argc, argv, &i, a, &app.base_override) != 0) {
                app_shutdown(&app);
                return 2;
            }
            app.base_override = xstrdup(app.base_override);
            continue;
        }
        if (str_eq(a, "--api-key")) {
            if (arg_value(argc, argv, &i, a, &app.key_override) != 0) {
                app_shutdown(&app);
                return 2;
            }
            app.key_override = xstrdup(app.key_override);
            continue;
        }
        fprintf(stderr, "unknown argument %s\n", a);
        app_shutdown(&app);
        return 2;
    }

    if (saw_command) {
        if (app_prepare(&app, &err) != 0) {
            fprintf(stderr, "%s\n", err ? err : "setup failed");
            free(err);
            app_shutdown(&app);
            return 1;
        }
        if (str_eq(argv[1], "login")) {
            rc = do_login(&app, argc >= 3 ? argv[2] : NULL, note_err, NULL, 0);
        } else if (str_eq(argv[1], "logout")) {
            rc = do_logout(&app, argc >= 3 ? argv[2] : NULL);
        } else {
            rc = do_status(&app);
        }
        app_shutdown(&app);
        return rc;
    }

    app.ui = ui_open(app.print_mode ? UI_PRINT : UI_AUTO);
    if (app_prepare(&app, &err) != 0) {
        fprintf(stderr, "%s\n", err ? err : "setup failed");
        free(err);
        app_shutdown(&app);
        return 1;
    }
    if (mode_arg) {
        PermMode mode;
        if (perm_mode_parse(mode_arg, &mode) != 0) {
            fprintf(stderr, "unknown mode %s\n", mode_arg);
            app_shutdown(&app);
            return 2;
        }
        set_mode(&app, mode);
        app.mode_from_cli = 1;
    } else if (app.print_mode) {
        set_mode(&app, MODE_AUTO);
        app.mode_from_cli = 1;
    }
    ui_set_glyphs(app.ui, app.cfg.glyphs);
    if (app.print_mode) {
        const char *warn = first_run();
        if (warn) {
            ui_log(app.ui, UI_ERR, warn);
        }
        if (!print_prompt) {
            fprintf(stderr, "-p needs a prompt\n");
            app_shutdown(&app);
            return 2;
        }
        rc = app_turn(&app, print_prompt);
        {
            char *follow;
            while (rc == 0 && (follow = ui_take_followup(app.ui)) != NULL) {
                rc = app_turn(&app, follow);
                free(follow);
            }
        }
        app_shutdown(&app);
        return rc == 0 ? 0 : 1;
    }

    ui_set_key_fn(app.ui, on_ui_key, &app);
    ui_set_choices(app.ui, COMMANDS, (int)(sizeof COMMANDS / sizeof COMMANDS[0]));
    app_refresh_status(&app);
    if (app.resume && app.ts.n) {
        replay(&app);
    }
    {
        const char *warn = first_run();
        if (ui_is_screen(app.ui)) {
            char *cwd = app_short_cwd(&app);
            ui_welcome(app.ui, BEIGE_VERSION, cwd, warn);
            free(cwd);
        } else {
            if (warn) {
                ui_log(app.ui, UI_ERR, warn);
            }
            ui_log(app.ui, UI_META, "type / for commands");
        }
    }
    for (;;) {
        int quit = 0;
        char *line = ui_next_line(app.ui, &quit);
        char *follow;
        if (!line || quit) {
            free(line);
            break;
        }
        if (!line[0]) {
            free(line);
            continue;
        }
        if (line[0] == '!') {
            const char *bang = line + 1;
            while (*bang == ' ') {
                bang++;
            }
            if (!bang[0]) {
                ui_log(app.ui, UI_ERR, "usage: !command");
            } else {
                ui_shell(app.ui, bang);
            }
            free(line);
            continue;
        }
        if (line[0] == '/') {
            quit = slash(&app, line, 0);
            free(line);
            if (quit == SLASH_QUIT) {
                break;
            }
            continue;
        }
        rc = app_turn(&app, line);
        free(line);
        if (ui_wants_quit(app.ui)) {
            break;
        }
        while ((follow = ui_take_followup(app.ui)) != NULL) {
            rc = app_turn(&app, follow);
            free(follow);
            if (ui_wants_quit(app.ui)) {
                break;
            }
        }
        app_refresh_status(&app);
        (void)rc;
    }
    app_shutdown(&app);
    return 0;
}
