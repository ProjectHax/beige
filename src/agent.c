#include "app.h"

#include "cJSON.h"
#include "oauth.h"
#include "signals.h"
#include "tools.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void app_init(App *app) {
    memset(app, 0, sizeof *app);
    config_init(&app->cfg);
    auth_init(&app->auth);
    transcript_init(&app->ts);
    perm_init(&app->perm, MODE_ASK);
    app->max_steps = 32;
    app->context_budget = 80000;
}

void app_shutdown(App *app) {
    session_close(app->session);
    provider_free(app->provider);
    transcript_free(&app->ts);
    perm_free(&app->perm);
    config_free(&app->cfg);
    auth_free(&app->auth);
    free(app->cwd);
    free(app->root);
    free(app->system);
    free(app->http.ca_bundle);
    free(app->base_override);
    free(app->key_override);
    free(app->model_override);
    free(app->provider_override);
    free(app->last_fp);
    free(app->file_provider);
    free(app->file_model);
    free(app->file_mode);
    ui_close(app->ui);
    memset(app, 0, sizeof *app);
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void app_rebuild_system(App *app) {
    char *instr = NULL;
    PermMode mode = app->perm.mode;
    if (!app->no_context) {
        char *home = beige_home();
        char *global = path_join(home, "AGENTS.md");
        instr = context_load(app->cwd, global, 32 * 1024, 64 * 1024);
        free(global);
        free(home);
    }
    free(app->system);
    app->system = system_prompt_build(app->cwd, instr, mode);
    free(instr);
}

char *app_short_cwd(const App *app) {
    char *home = home_dir();
    char *shown;
    if (app && app->cwd && home && str_starts(app->cwd, home) && app->cwd[strlen(home)] == '/') {
        shown = path_join("~", app->cwd + strlen(home) + 1);
    } else if (app && app->cwd && home && str_eq(app->cwd, home)) {
        shown = xstrdup("~");
    } else {
        shown = xstrdup(app && app->cwd ? app->cwd : ".");
    }
    free(home);
    return shown;
}

void app_refresh_status(App *app) {
    UiStatus st;
    char *shown;
    const char *model;
    if (!app || !app->ui) {
        return;
    }
    model = (app->provider && app->provider->model[0]) ? app->provider->model
            : (app->cfg.model && app->cfg.model[0])     ? app->cfg.model
                                                        : provider_default_model(app->cfg.provider);
    shown = app_short_cwd(app);
    memset(&st, 0, sizeof st);
    st.mode = perm_mode_name(app->perm.mode);
    st.provider = app->cfg.provider ? app->cfg.provider : "";
    st.model = model ? model : "";
    st.cwd = shown;
    st.tokens = (unsigned long)(transcript_bytes(&app->ts) / 4);
    st.budget = (unsigned long)(app->context_budget > 0 ? app->context_budget : 0);
    ui_set_status(app->ui, &st);
    free(shown);
    if (ui_is_screen(app->ui)) {
        ui_present(app->ui);
    }
}

/* The CA bundle when the config names none: ~/.beige/cacert.pem, then cacert.pem beside the executable
 * as a release tarball unpacks it, then where install.sh and the .deb and .rpm packages put it. */
static char *find_ca_bundle(void) {
    static const char *const shared[] = {"/usr/local/share/beige/cacert.pem", "/usr/share/beige/cacert.pem"};
    char exe[4096];
    char *home = beige_home();
    char *ca = path_join(home, "cacert.pem");
    char *slash;
    ssize_t n;
    size_t k;
    free(home);
    if (file_exists(ca)) {
        return ca;
    }
    free(ca);
    /* Linux names the running executable here; elsewhere the readlink fails and this step is skipped. */
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            ca = path_join(exe[0] ? exe : "/", "cacert.pem");
            if (file_exists(ca)) {
                return ca;
            }
            free(ca);
        }
    }
    for (k = 0; k < sizeof shared / sizeof shared[0]; k++) {
        if (file_exists(shared[k])) {
            return xstrdup(shared[k]);
        }
    }
    return NULL;
}

int app_prepare(App *app, char **err) {
    PermMode mode = MODE_ASK;
    if (err) {
        *err = NULL;
    }
    config_load(&app->cfg);
    auth_load(&app->auth);
    free(app->file_provider);
    free(app->file_model);
    free(app->file_mode);
    app->file_provider = xstrdup(app->cfg.provider);
    app->file_model = xstrdup(app->cfg.model);
    app->file_mode = xstrdup(app->cfg.mode);
    if (app->provider_override && app->provider_override[0]) {
        free(app->cfg.provider);
        app->cfg.provider = xstrdup(app->provider_override);
    }
    if (app->model_override && app->model_override[0]) {
        free(app->cfg.model);
        app->cfg.model = xstrdup(app->model_override);
    }
    if (!provider_is_known(app->cfg.provider)) {
        if (err) {
            *err = xstrdup("unknown provider");
        }
        return -1;
    }
    if (perm_mode_parse(app->cfg.mode, &mode) != 0) {
        mode = MODE_ASK;
    }
    perm_set_mode(&app->perm, mode);
    app->max_steps = app->cfg.max_steps > 0 ? app->cfg.max_steps : 32;
    app->context_budget = app->cfg.context_budget > 0 ? app->cfg.context_budget : 80000;
    app->cwd = getcwd_alloc();
    if (!app->cwd) {
        if (err) {
            *err = xstrdup("cannot read the working directory");
        }
        return -1;
    }
    app->root = realpath_alloc(app->cwd);
    if (!app->root) {
        app->root = xstrdup(app->cwd);
    }
    perm_set_root(&app->perm, app->root, app->cfg.sandbox);
    if (app->cfg.ca_bundle && app->cfg.ca_bundle[0]) {
        app->http.ca_bundle = xstrdup(app->cfg.ca_bundle);
    } else {
        app->http.ca_bundle = find_ca_bundle();
    }
    app->http.cancel = &g_sigint;
    http_init();
    app_rebuild_system(app);
    if (app->resume) {
        char *serr = NULL;
        app->session = session_resume(app->cwd, &app->ts, &serr);
        if (!app->session && serr && !app->print_mode) {
            ui_log(app->ui, UI_META, serr);
        }
        free(serr);
    }
    if (!app->session) {
        app->session = session_begin(app->cwd);
    }
    return 0;
}

int app_save_config(App *app) {
    Config c = app->cfg;
    if (app->provider_override && str_eq(app->cfg.provider, app->provider_override)) {
        c.provider = app->file_provider;
    }
    if (app->model_override && str_eq(app->cfg.model, app->model_override)) {
        c.model = app->file_model;
    }
    if (app->mode_from_cli) {
        c.mode = app->file_mode;
    }
    return config_save(&c);
}

int app_open_provider(App *app, const char *id, Provider **out, char **err) {
    ProviderSpec spec;
    const char *key = NULL;
    const char *base = NULL;
    const char *wire = NULL;
    OAuthTok *tok;
    memset(&spec, 0, sizeof spec);
    if (out) {
        *out = NULL;
    }
    if (!id || !provider_is_known(id)) {
        if (err) {
            *err = xstrdup("unknown provider");
        }
        return -1;
    }
    if (app->key_override && app->key_override[0] && str_eq(id, app->cfg.provider) && !str_eq(id, "openai-sub") &&
        !str_eq(id, "xai-sub")) {
        key = app->key_override;
    }
    if (str_eq(id, "anthropic")) {
        base = app->cfg.anthropic_base;
    } else if (str_eq(id, "openai")) {
        base = app->cfg.openai_base;
        wire = app->cfg.openai_wire;
    } else if (str_eq(id, "xai")) {
        base = app->cfg.xai_base;
        wire = "chat";
    } else if (str_eq(id, "xai-sub")) {
        base = app->cfg.xai_base;
        wire = "chat";
        tok = auth_oauth(&app->auth, id);
        if (oauth_ensure(&app->http, id, tok, err) != 0) {
            return -1;
        }
        auth_save(&app->auth);
        key = tok->access;
    } else if (str_eq(id, "openai-sub")) {
        base = app->cfg.openai_sub_base;
        wire = "responses";
        tok = auth_oauth(&app->auth, id);
        if (oauth_ensure(&app->http, id, tok, err) != 0) {
            return -1;
        }
        auth_save(&app->auth);
        key = tok->access;
        spec.account_id = tok->account_id;
    }
    if (!key) {
        key = auth_api_key(&app->auth, id);
    }
    if (app->base_override && app->base_override[0] && str_eq(id, app->cfg.provider)) {
        base = app->base_override;
    }
    spec.id = id;
    spec.model = app->cfg.model;
    spec.base_url = base;
    spec.api_key = key;
    spec.wire_name = wire;
    spec.originator = app->cfg.originator;
    spec.max_tokens = app->cfg.max_tokens;
    if (!out) {
        if (err) {
            *err = xstrdup("internal error");
        }
        return -1;
    }
    *out = provider_open(&spec, err);
    return *out ? 0 : -1;
}

int app_bind_provider(App *app, char **err) {
    Provider *next = NULL;
    if (app_open_provider(app, app->cfg.provider, &next, err) != 0) {
        return -1;
    }
    provider_free(app->provider);
    app->provider = next;
    return 0;
}

typedef struct {
    App *app;
    StreamAcc *acc;
    void *pst;
} StreamUD;

static void on_sse(const char *event, const char *data, void *ud) {
    StreamUD *s = ud;
    size_t before = s->acc->text.len;
    provider_parse_event(s->app->provider, s->pst, s->acc, event, data);
    if (s->acc->text.len > before && s->acc->text.data) {
        ui_stream_append(s->app->ui, s->acc->text.data + before, s->acc->text.len - before);
    }
}

static int is_cancel(const char *err) {
    if (g_sigint) {
        return 1;
    }
    if (!err) {
        return 0;
    }
    return str_eq(err, "cancelled") || strstr(err, "abort") != NULL || strstr(err, "Aborted") != NULL;
}

/* Responses keeps generating after the client disconnects unless told to stop. */
static void cancel_response(App *app, const char *id) {
    char *url;
    char *err = NULL;
    struct curl_slist *hdrs;
    sig_atomic_t saved;
    size_t n;
    if (!app || !app->provider || app->provider->wire != WIRE_RESPONSES || !id || !id[0]) {
        return;
    }
    if (!app->provider->url) {
        return;
    }
    n = strlen(app->provider->url) + strlen(id) + 16;
    url = xmalloc(n);
    sprintf(url, "%s/%s/cancel", app->provider->url, id);
    hdrs = provider_headers(app->provider);
    saved = g_sigint;
    g_sigint = 0;
    http_exchange(&app->http, "POST", url, hdrs, "{}", NULL, NULL, 4096, &err);
    g_sigint = saved;
    free(err);
    free(url);
    curl_slist_free_all(hdrs);
}

static char *trim_lines(const char *s, int max_lines);

static const char *ask_title_for(const char *name) {
    if (str_eq(name, "bash")) {
        return "Bash command";
    }
    if (str_eq(name, "edit")) {
        return "Edit file";
    }
    if (str_eq(name, "write")) {
        return "Write file";
    }
    if (str_eq(name, "fetch")) {
        return "Fetch";
    }
    if (str_eq(name, "search")) {
        return "Web search";
    }
    return name && name[0] ? name : "Tool";
}

static const char *after_first(const char *s) {
    const char *nl;
    const char *p;
    if (!s) {
        return NULL;
    }
    nl = strchr(s, '\n');
    if (!nl) {
        return NULL;
    }
    p = nl + 1;
    while (*p == '\n' || *p == '\r') {
        p++;
    }
    if (!*p) {
        return NULL;
    }
    return nl + 1;
}

static void show_result(App *app, int id, const char *name, cJSON *args, const char *result, int is_error,
                        const char *preview) {
    UiResult r;
    char *summary;
    const char *body = NULL;
    if (ui_is_print(app->ui)) {
        char *shown = trim_lines(result, is_error ? 8 : 40);
        ui_log(app->ui, is_error ? UI_ERR : UI_META, shown);
        free(shown);
        return;
    }
    summary = tool_summary(name, args, result, is_error);
    memset(&r, 0, sizeof r);
    r.state = is_error ? UI_TOOL_ERR : UI_TOOL_OK;
    r.summary = summary ? summary : "";
    r.peek = 4;
    r.diff = !is_error && preview && (str_eq(name, "edit") || str_eq(name, "write"));
    if (r.diff) {
        body = after_first(preview);
        if (!body) {
            body = preview;
        }
    } else {
        body = after_first(result);
    }
    r.body = body;
    ui_tool_end(app->ui, id, &r);
    free(summary);
}

void app_log_tool(App *app, const Block *use, const Block *result) {
    cJSON *args;
    char *title;
    int id;
    UiResult r;
    if (!app || !use) {
        return;
    }
    args = cJSON_Parse(use->input_json ? use->input_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        args = cJSON_CreateObject();
    }
    title = tool_title(use->name, args);
    id = ui_tool_begin(app->ui, title);
    free(title);
    if (!result) {
        memset(&r, 0, sizeof r);
        r.state = UI_TOOL_OK;
        r.summary = "";
        r.peek = 4;
        ui_tool_end(app->ui, id, &r);
    } else {
        show_result(app, id, use->name, args, result->text, result->is_error, NULL);
    }
    cJSON_Delete(args);
}

static char *trim_lines(const char *s, int max_lines) {
    int lines = 0;
    const char *p;
    Buf b;
    if (!s) {
        return xstrdup("");
    }
    buf_init(&b);
    p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (lines == max_lines) {
            buf_puts(&b, "\n...");
            break;
        }
        buf_append(&b, p, n);
        buf_putc(&b, '\n');
        lines++;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return buf_detach(&b);
}

static char *fingerprint(const Message *m) {
    Buf b;
    int i;
    buf_init(&b);
    for (i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type != BLOCK_TOOL_USE) {
            continue;
        }
        buf_puts(&b, m->blocks[i].name ? m->blocks[i].name : "");
        buf_putc(&b, '\n');
        buf_puts(&b, m->blocks[i].input_json ? m->blocks[i].input_json : "");
        buf_putc(&b, '\n');
    }
    return buf_detach(&b);
}

static int app_complete(App *app, const char *system, const char *user, char **out, char **err,
                        const char *activity) {
    Transcript tmp;
    char *body = NULL;
    struct curl_slist *hdrs = NULL;
    StreamAcc acc;
    void *pst = NULL;
    StreamUD ud;
    IoHooks hooks;
    long status = 0;
    char *herr = NULL;
    int rc;
    *out = NULL;
    if (app_bind_provider(app, err) != 0) {
        return -1;
    }
    transcript_init(&tmp);
    transcript_add_user_text(&tmp, user);
    body = provider_build_body(app->provider, &tmp, system, 0);
    transcript_free(&tmp);
    if (!body) {
        if (err) {
            *err = xstrdup("could not build request");
        }
        return -1;
    }
    hdrs = provider_headers(app->provider);
    stream_acc_init(&acc);
    pst = provider_parse_new(app->provider);
    ud.app = app;
    ud.acc = &acc;
    ud.pst = pst;
    memset(&hooks, 0, sizeof hooks);
    ui_hooks(app->ui, &hooks);
    ui_activity(app->ui, activity && activity[0] ? activity : "thinking");
    ui_stream_begin(app->ui);
    g_sigint = 0;
    rc = http_post_sse(&app->http, app->provider->url, hdrs, body, on_sse, &ud, &hooks, &status, &herr);
    if (is_cancel(herr)) {
        cancel_response(app, acc.response_id);
    }
    ui_stream_end(app->ui);
    if (acc.text.data && acc.text.len) {
        *out = xstrdup(acc.text.data);
    }
    if (acc.error && err && !*err) {
        *err = xstrdup(acc.error);
        rc = -1;
    } else if (rc != 0 && err && !*err) {
        *err = herr ? xstrdup(herr) : xstrdup("request failed");
    }
    free(herr);
    provider_parse_free(app->provider, pst);
    stream_acc_free(&acc);
    curl_slist_free_all(hdrs);
    free(body);
    return (*out && rc == 0) ? 0 : (rc == 0 && *out == NULL ? -1 : rc);
}

static char *dump_for_summary(const Transcript *ts, int keep) {
    Buf b;
    int i, end;
    buf_init(&b);
    end = ts->n - keep;
    if (end < 0) {
        end = 0;
    }
    for (i = 0; i < end; i++) {
        int j;
        buf_puts(&b, ts->msgs[i].role == ROLE_ASSISTANT ? "assistant:\n" : "user:\n");
        for (j = 0; j < ts->msgs[i].nblocks; j++) {
            const Block *blk = &ts->msgs[i].blocks[j];
            if (blk->type == BLOCK_TEXT && blk->text) {
                buf_append(&b, blk->text, strlen(blk->text) > 1500 ? 1500 : strlen(blk->text));
                buf_putc(&b, '\n');
            } else if (blk->type == BLOCK_TOOL_USE) {
                buf_printf(&b, "tool %s %s\n", blk->name ? blk->name : "", blk->input_json ? blk->input_json : "");
            } else if (blk->type == BLOCK_TOOL_RESULT && blk->text) {
                buf_append(&b, blk->text, strlen(blk->text) > 800 ? 800 : strlen(blk->text));
                buf_putc(&b, '\n');
            }
        }
        buf_putc(&b, '\n');
        if (b.len > 48000) {
            break;
        }
    }
    return buf_detach(&b);
}

int app_compact(App *app) {
    char *dump;
    char *user;
    char *summary = NULL;
    char *err = NULL;
    int keep = transcript_safe_keep(&app->ts, 4);
    if (app->ts.n < 6 || app->ts.n - keep < 2) {
        ui_log(app->ui, UI_META, "nothing to compact yet");
        return 0;
    }
    dump = dump_for_summary(&app->ts, keep);
    user = xmalloc(strlen(dump) + 240);
    sprintf(user,
            "Summarize this coding session so another agent can continue. Keep file paths, "
            "decisions, commands, and errors. Be concise.\n\n%s",
            dump);
    free(dump);
    if (app_complete(app, "You compress coding sessions into a briefing.", user, &summary, &err, "compacting") != 0 ||
        !summary) {
        ui_log(app->ui, UI_ERR, err ? err : "compact failed");
        free(err);
        free(user);
        free(summary);
        return -1;
    }
    free(user);
    free(err);
    transcript_replace_with_summary(&app->ts, summary, keep);
    free(summary);
    session_close(app->session);
    app->session = session_begin(app->cwd);
    if (app->session) {
        session_write_transcript(app->session, &app->ts);
    }
    app->compact_floor = (unsigned long)(transcript_bytes(&app->ts) / 4);
    app_refresh_status(app);
    ui_log(app->ui, UI_META, "compacted the session");
    return 0;
}

static char *strip_fence(const char *s) {
    const char *p = s ? s : "";
    const char *end;
    char *out;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    if (str_starts(p, "```")) {
        p = strchr(p, '\n');
        if (!p) {
            return xstrdup(s);
        }
        p++;
        end = strstr(p, "```");
        if (end) {
            out = xstrndup(p, (size_t)(end - p));
            return out;
        }
    }
    return xstrdup(s ? s : "");
}

int app_init_agents(App *app) {
    Buf brief;
    char *readme = NULL;
    char *make = NULL;
    char *prompt;
    char *draft = NULL;
    char *err = NULL;
    char *clean;
    cJSON *args;
    ToolIn in;
    char *text = NULL;
    int is_error = 0;
    char *names = NULL;
    DIR *d;
    struct dirent *de;
    buf_init(&brief);
    readme = read_file_cap("README.md", 8000, NULL, NULL, NULL);
    if (!readme) {
        readme = read_file_cap("README", 8000, NULL, NULL, NULL);
    }
    make = read_file_cap("Makefile", 4000, NULL, NULL, NULL);
    if (!make) {
        make = read_file_cap("configure", 2000, NULL, NULL, NULL);
    }
    d = opendir(".");
    if (d) {
        Buf list;
        int n = 0;
        buf_init(&list);
        while ((de = readdir(d)) != NULL && n < 80) {
            if (de->d_name[0] == '.') {
                continue;
            }
            buf_puts(&list, de->d_name);
            buf_putc(&list, '\n');
            n++;
        }
        closedir(d);
        names = buf_detach(&list);
    }
    buf_puts(&brief, "Project files:\n");
    if (names) {
        buf_puts(&brief, names);
    }
    if (readme) {
        buf_puts(&brief, "\nREADME:\n");
        buf_puts(&brief, readme);
    }
    if (make) {
        buf_puts(&brief, "\nBuild file:\n");
        buf_puts(&brief, make);
    }
    prompt = xmalloc(brief.len + 400);
    sprintf(prompt,
            "Write an AGENTS.md for this project. Include how to build, how to test, the layout, "
            "and any gotchas. Output only the markdown document.\n\n%s",
            brief.data ? brief.data : "");
    buf_free(&brief);
    free(readme);
    free(make);
    free(names);
    if (app_complete(app, "You write short project instruction files for coding agents.", prompt, &draft, &err,
                     "thinking") !=
            0 ||
        !draft) {
        ui_log(app->ui, UI_ERR, err ? err : "could not draft AGENTS.md");
        free(err);
        free(prompt);
        free(draft);
        return -1;
    }
    free(prompt);
    free(err);
    clean = strip_fence(draft);
    free(draft);
    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", "AGENTS.md");
    cJSON_AddStringToObject(args, "contents", clean);
    {
        char *subject = tool_subject("write", args, app->root, app->cfg.sandbox);
        char *preview = tool_preview("write", args, app->root, app->cfg.sandbox);
        PermDecision dec = perm_decide(&app->perm, "write", subject);
        int allow = dec == DEC_ALLOW;
        if (dec == DEC_ASK) {
            int ch;
            if (ui_is_print(app->ui)) {
                ui_log(app->ui, UI_ERR, "permission required to write AGENTS.md");
                free(subject);
                free(preview);
                free(clean);
                cJSON_Delete(args);
                return 2;
            }
            {
                UiAsk ask;
                const char *diff = preview;
                const char *nl = preview ? strchr(preview, '\n') : NULL;
                if (nl) {
                    diff = nl + 1;
                }
                memset(&ask, 0, sizeof ask);
                ask.title = "Write file";
                ask.what = "write AGENTS.md";
                ask.body = diff;
                ask.always = "AGENTS.md";
                ch = ui_confirm(app->ui, &ask);
            }
            if (ch == 'y' || ch == 'a') {
                allow = 1;
            }
        }
        free(subject);
        free(preview);
        if (!allow) {
            ui_log(app->ui, UI_ERR, "AGENTS.md was not written");
            free(clean);
            cJSON_Delete(args);
            return -1;
        }
    }
    memset(&in, 0, sizeof in);
    in.root = app->root;
    in.sandbox = app->cfg.sandbox;
    in.read_cap = app->cfg.read_cap;
    in.output_cap = app->cfg.output_cap;
    in.shell_timeout = app->cfg.shell_timeout;
    in.args = args;
    in.cancel = &g_sigint;
    in.http = &app->http;
    tool_run("write", &in, &text, &is_error);
    ui_log(app->ui, is_error ? UI_ERR : UI_META, text ? text : "wrote AGENTS.md");
    free(text);
    free(clean);
    cJSON_Delete(args);
    app_rebuild_system(app);
    return is_error ? -1 : 0;
}

/* Give id-less tool calls an id before the message reaches the session file, so results match on resume. */
static void name_tool_calls(App *app, Message *assistant) {
    int i, k = 0;
    for (i = 0; i < assistant->nblocks; i++) {
        Block *b = &assistant->blocks[i];
        if (b->type != BLOCK_TOOL_USE) {
            continue;
        }
        if (!b->id || !b->id[0]) {
            char idbuf[32];
            free(b->id);
            snprintf(idbuf, sizeof idbuf, "call_%d_%d", app->ts.n, k);
            b->id = xstrdup(idbuf);
        }
        k++;
    }
}

int app_turn(App *app, const char *user_text) {
    int step;
    if (user_text && user_text[0]) {
        transcript_add_user_text(&app->ts, user_text);
        if (app->session && app->ts.n > 0) {
            session_write_message(app->session, &app->ts.msgs[app->ts.n - 1]);
        }
        ui_log(app->ui, UI_USER, user_text);
    }
    g_sigint = 0;
    for (step = 0; step < app->max_steps; step++) {
        char *body = NULL;
        struct curl_slist *hdrs = NULL;
        StreamAcc acc;
        void *pst = NULL;
        StreamUD ud;
        IoHooks hooks;
        long status = 0;
        char *err = NULL;
        int rc;
        int cancelled;
        Message *assistant;
        int ntools = 0;
        int i, k;
        char **results = NULL;
        int *errors = NULL;
        char *fp;
        int repeated = 0;
        unsigned long est = (unsigned long)(transcript_bytes(&app->ts) / 4);
        unsigned long budget = app->context_budget > 0 ? (unsigned long)app->context_budget : 0;
        /* Compact whenever the budget is crossed, but only after half a budget of growth since the last try,
           so a session whose kept turns are still too large does not compact on every step. */
        if (budget > 0 && est > budget && est >= app->compact_floor + budget / 2 && app->ts.n >= 6) {
            if (app_compact(app) != 0) {
                app->compact_floor = est;
            }
        }
        app_refresh_status(app);
        if (app_bind_provider(app, &err) != 0) {
            ui_log(app->ui, UI_ERR, err ? err : "no credentials");
            free(err);
            return 2;
        }
        free(err);
        err = NULL;
        body = provider_build_body(app->provider, &app->ts, app->system, 1);
        if (!body) {
            ui_log(app->ui, UI_ERR, "could not build request");
            return -1;
        }
        hdrs = provider_headers(app->provider);
        stream_acc_init(&acc);
        pst = provider_parse_new(app->provider);
        ud.app = app;
        ud.acc = &acc;
        ud.pst = pst;
        memset(&hooks, 0, sizeof hooks);
        ui_hooks(app->ui, &hooks);
        ui_activity(app->ui, "thinking");
        ui_stream_begin(app->ui);
        rc = http_post_sse(&app->http, app->provider->url, hdrs, body, on_sse, &ud, &hooks, &status, &err);
        cancelled = is_cancel(err);
        if (cancelled) {
            cancel_response(app, acc.response_id);
        }
        ui_stream_end(app->ui);
        if (cancelled || acc.error || rc != 0) {
            stream_acc_drop_tools(&acc);
        }
        if (acc.text.len || acc.ntools) {
            assistant = transcript_add_assistant_from_stream(&app->ts, &acc);
            name_tool_calls(app, assistant);
            if (app->session) {
                session_write_message(app->session, assistant);
            }
        } else {
            assistant = NULL;
        }
        if (cancelled) {
            g_sigint = 0;
            ui_log(app->ui, UI_META, "cancelled");
            provider_parse_free(app->provider, pst);
            stream_acc_free(&acc);
            curl_slist_free_all(hdrs);
            free(body);
            free(err);
            return 0;
        }
        if (acc.error) {
            ui_log(app->ui, UI_ERR, acc.error);
            provider_parse_free(app->provider, pst);
            stream_acc_free(&acc);
            curl_slist_free_all(hdrs);
            free(body);
            free(err);
            return -1;
        }
        if (rc != 0) {
            ui_log(app->ui, UI_ERR, err ? err : "request failed");
            provider_parse_free(app->provider, pst);
            stream_acc_free(&acc);
            curl_slist_free_all(hdrs);
            free(body);
            free(err);
            return -1;
        }
        provider_parse_free(app->provider, pst);
        curl_slist_free_all(hdrs);
        free(body);
        free(err);
        if (!assistant) {
            stream_acc_free(&acc);
            return 0;
        }
        for (i = 0; i < assistant->nblocks; i++) {
            if (assistant->blocks[i].type == BLOCK_TOOL_USE) {
                ntools++;
            }
        }
        if (ntools == 0) {
            stream_acc_free(&acc);
            app->tool_streak = 0;
            return 0;
        }
        fp = fingerprint(assistant);
        if (app->last_fp && str_eq(app->last_fp, fp)) {
            app->tool_streak++;
        } else {
            free(app->last_fp);
            app->last_fp = xstrdup(fp);
            app->tool_streak = 1;
        }
        if (app->tool_streak >= 3) {
            repeated = 1;
        }
        results = xmalloc((size_t)ntools * sizeof(char *));
        errors = xmalloc((size_t)ntools * sizeof(int));
        k = 0;
        for (i = 0; i < assistant->nblocks; i++) {
            Block *b = &assistant->blocks[i];
            cJSON *args;
            char *subject;
            char *preview;
            PermDecision dec;
            char header[256];
            ToolIn in;
            if (b->type != BLOCK_TOOL_USE) {
                continue;
            }
            args = cJSON_Parse(b->input_json ? b->input_json : "{}");
            if (!args || !cJSON_IsObject(args)) {
                cJSON_Delete(args);
                args = cJSON_CreateObject();
            }
            subject = tool_subject(b->name, args, app->root, app->cfg.sandbox);
            preview = tool_preview(b->name, args, app->root, app->cfg.sandbox);
            snprintf(header, sizeof header, "%s %s", b->name ? b->name : "tool",
                     subject && strlen(subject) < 180 ? subject : "");
            {
                char *title = tool_title(b->name, args);
                int tid = ui_tool_begin(app->ui, title);
                int closed = 0;
                free(title);
                dec = perm_decide(&app->perm, b->name, subject);
                if (repeated) {
                    results[k] = xstrdup("Error: repeated the same tool call; stopping");
                    errors[k] = 1;
                    show_result(app, tid, b->name, args, results[k], 1, NULL);
                    closed = 1;
                } else if (dec == DEC_DENY) {
                    results[k] = xstrdup("Error: denied by plan mode");
                    errors[k] = 1;
                    show_result(app, tid, b->name, args, results[k], 1, NULL);
                    closed = 1;
                } else if (dec == DEC_ASK && ui_is_print(app->ui)) {
                    results[k] = xstrdup("Error: permission required");
                    errors[k] = 1;
                    ui_log(app->ui, UI_ERR, "permission required; print mode does not prompt");
                    show_result(app, tid, b->name, args, results[k], 1, NULL);
                    cJSON_Delete(args);
                    free(subject);
                    free(preview);
                    k++;
                    while (k < ntools) {
                        results[k] = xstrdup("Error: permission required");
                        errors[k] = 1;
                        k++;
                    }
                    transcript_add_tool_results(&app->ts, assistant, results, errors, ntools);
                    if (app->session) {
                        session_write_message(app->session, &app->ts.msgs[app->ts.n - 1]);
                    }
                    for (k = 0; k < ntools; k++) {
                        free(results[k]);
                    }
                    free(results);
                    free(errors);
                    free(fp);
                    stream_acc_free(&acc);
                    return 2;
                } else if (dec == DEC_ASK) {
                    UiAsk ask;
                    char *always = NULL;
                    const char *body = subject;
                    int ch;
                    if (str_eq(b->name, "bash")) {
                        always = perm_bash_prefix(subject);
                    } else {
                        always = xstrdup(subject ? subject : "");
                    }
                    if (str_eq(b->name, "edit") || str_eq(b->name, "write")) {
                        body = after_first(preview);
                        if (!body) {
                            body = preview;
                        }
                    }
                    memset(&ask, 0, sizeof ask);
                    ask.title = ask_title_for(b->name);
                    ask.what = header;
                    ask.body = body;
                    ask.always = always;
                    ch = ui_confirm(app->ui, &ask);
                    free(always);
                    if (ch == 'a') {
                        if (str_eq(b->name, "bash")) {
                            char *pfx = perm_bash_prefix(subject);
                            perm_allow_prefix(&app->perm, "bash", pfx);
                            free(pfx);
                        } else {
                            perm_allow_prefix(&app->perm, b->name, subject);
                        }
                        ch = 'y';
                    }
                    if (ch != 'y') {
                        results[k] = xstrdup(ch == 'd' ? "Error: denied" : "Error: skipped");
                        errors[k] = 1;
                        show_result(app, tid, b->name, args, results[k], 1, NULL);
                        closed = 1;
                        cJSON_Delete(args);
                        free(subject);
                        free(preview);
                        k++;
                        continue;
                    }
                    dec = DEC_ALLOW;
                }
                if (!closed && dec == DEC_ALLOW && !repeated) {
                    memset(&in, 0, sizeof in);
                    in.root = app->root;
                    in.sandbox = app->cfg.sandbox;
                    in.read_cap = app->cfg.read_cap ? app->cfg.read_cap : 262144;
                    in.output_cap = app->cfg.output_cap ? app->cfg.output_cap : 32768;
                    in.shell_timeout = app->cfg.shell_timeout;
                    in.args = args;
                    in.cancel = &g_sigint;
                    in.http = &app->http;
                    ui_hooks(app->ui, &hooks);
                    in.hooks = &hooks;
                    ui_activity(app->ui, "running");
                    if (tool_run(b->name, &in, &results[k], &errors[k]) != 0 || !results[k]) {
                        free(results[k]);
                        results[k] = xstrdup("Error: tool failed");
                        errors[k] = 1;
                    }
                    ui_activity(app->ui, NULL);
                    show_result(app, tid, b->name, args, results[k], errors[k], preview);
                    closed = 1;
                }
                (void)closed;
            }
            cJSON_Delete(args);
            free(subject);
            free(preview);
            k++;
            if (g_sigint) {
                break;
            }
        }
        for (; k < ntools; k++) {
            results[k] = xstrdup("Error: cancelled");
            errors[k] = 1;
        }
        transcript_add_tool_results(&app->ts, assistant, results, errors, ntools);
        if (app->session) {
            session_write_message(app->session, &app->ts.msgs[app->ts.n - 1]);
        }
        for (k = 0; k < ntools; k++) {
            free(results[k]);
        }
        free(results);
        free(errors);
        free(fp);
        stream_acc_free(&acc);
        if (g_sigint || repeated) {
            g_sigint = 0;
            if (repeated) {
                ui_log(app->ui, UI_ERR, "stopped after repeated tool calls");
            } else {
                ui_log(app->ui, UI_META, "cancelled");
            }
            return 0;
        }
    }
    ui_log(app->ui, UI_ERR, "stopped at the step limit");
    return 0;
}
