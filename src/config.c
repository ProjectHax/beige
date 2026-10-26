#include "config.h"

#include "cJSON.h"

static char *cfg_path(void) {
    char *home = beige_home();
    char *path = path_join(home, "config.json");
    free(home);
    return path;
}

static char *auth_path(void) {
    char *home = beige_home();
    char *path = path_join(home, "auth.json");
    free(home);
    return path;
}

static char *jdup(cJSON *obj, const char *key, const char *fallback) {
    cJSON *it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    if (cJSON_IsString(it) && it->valuestring) {
        return xstrdup(it->valuestring);
    }
    return xstrdup(fallback ? fallback : "");
}

static void replace(char **slot, const char *value);

static void take_str(char **slot, cJSON *obj, const char *key) {
    cJSON *it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    if (cJSON_IsString(it) && it->valuestring) {
        replace(slot, it->valuestring);
    }
}

static int jint(cJSON *obj, const char *key, int fallback) {
    cJSON *it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return fallback;
}

/* A byte cap from the file. Zero, negative, or over 1 GiB keeps the default. */
static size_t cap_value(int v, size_t fallback) {
    if (v <= 0 || v > (1 << 30)) {
        return fallback;
    }
    return (size_t)v;
}

static void tok_load(OAuthTok *t, cJSON *obj) {
    cJSON *exp;
    oauth_tok_clear(t);
    if (!cJSON_IsObject(obj)) {
        return;
    }
    t->access = jdup(obj, "access_token", "");
    t->refresh = jdup(obj, "refresh_token", "");
    t->account_id = jdup(obj, "account_id", "");
    exp = cJSON_GetObjectItemCaseSensitive(obj, "expires_at");
    if (cJSON_IsNumber(exp)) {
        t->expires_at = (long long)exp->valuedouble;
    }
}

void oauth_tok_clear(OAuthTok *t) {
    free(t->access);
    free(t->refresh);
    free(t->account_id);
    t->access = NULL;
    t->refresh = NULL;
    t->account_id = NULL;
    t->expires_at = 0;
}

void oauth_tok_set(OAuthTok *t, const char *access, const char *refresh, const char *account, long long exp) {
    oauth_tok_clear(t);
    t->access = xstrdup(access ? access : "");
    t->refresh = xstrdup(refresh ? refresh : "");
    t->account_id = xstrdup(account ? account : "");
    t->expires_at = exp;
}

void config_init(Config *c) {
    memset(c, 0, sizeof *c);
    c->provider = xstrdup("anthropic");
    c->model = xstrdup("");
    c->mode = xstrdup("ask");
    c->max_tokens = 8192;
    c->max_steps = 32;
    c->context_budget = 80000;
    c->sandbox = 1;
    c->read_cap = 262144;
    c->output_cap = 32768;
    c->shell_timeout = 0;
    c->anthropic_base = xstrdup("https://api.anthropic.com");
    c->openai_base = xstrdup("https://api.openai.com/v1");
    c->openai_wire = xstrdup("responses");
    c->xai_base = xstrdup("https://api.x.ai/v1");
    c->openai_sub_base = xstrdup("https://chatgpt.com/backend-api/codex");
    c->originator = xstrdup("beige");
    c->ca_bundle = xstrdup("");
    c->glyphs = xstrdup("auto");
}

void config_free(Config *c) {
    free(c->provider);
    free(c->model);
    free(c->mode);
    free(c->anthropic_base);
    free(c->openai_base);
    free(c->openai_wire);
    free(c->xai_base);
    free(c->openai_sub_base);
    free(c->originator);
    free(c->ca_bundle);
    free(c->glyphs);
    memset(c, 0, sizeof *c);
}

static void replace(char **slot, const char *value) {
    free(*slot);
    *slot = xstrdup(value ? value : "");
}

void config_load(Config *c) {
    char *path = cfg_path();
    char *text;
    cJSON *root, *anth, *oai, *xai, *sub, *sandbox;
    text = read_file_cap(path, 1024 * 1024, NULL, NULL, NULL);
    free(path);
    if (!text) {
        return;
    }
    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return;
    }
    take_str(&c->provider, root, "provider");
    take_str(&c->model, root, "model");
    take_str(&c->mode, root, "mode");
    take_str(&c->glyphs, root, "glyphs");
    c->max_tokens = jint(root, "max_tokens", c->max_tokens);
    c->max_steps = jint(root, "max_steps", c->max_steps);
    c->context_budget = jint(root, "context_budget", c->context_budget);
    c->read_cap = cap_value(jint(root, "read_cap", (int)c->read_cap), c->read_cap);
    c->output_cap = cap_value(jint(root, "output_cap", (int)c->output_cap), c->output_cap);
    c->shell_timeout = jint(root, "shell_timeout", c->shell_timeout);
    sandbox = cJSON_GetObjectItemCaseSensitive(root, "sandbox");
    if (cJSON_IsBool(sandbox)) {
        c->sandbox = cJSON_IsTrue(sandbox);
    }
    take_str(&c->ca_bundle, root, "ca_bundle");
    anth = cJSON_GetObjectItemCaseSensitive(root, "anthropic");
    oai = cJSON_GetObjectItemCaseSensitive(root, "openai");
    xai = cJSON_GetObjectItemCaseSensitive(root, "xai");
    sub = cJSON_GetObjectItemCaseSensitive(root, "openai_sub");
    if (cJSON_IsObject(anth)) {
        take_str(&c->anthropic_base, anth, "base_url");
    }
    if (cJSON_IsObject(oai)) {
        take_str(&c->openai_base, oai, "base_url");
        take_str(&c->openai_wire, oai, "wire");
    }
    if (cJSON_IsObject(xai)) {
        take_str(&c->xai_base, xai, "base_url");
    }
    if (cJSON_IsObject(sub)) {
        take_str(&c->openai_sub_base, sub, "base_url");
        take_str(&c->originator, sub, "originator");
    }
    cJSON_Delete(root);
}

int config_save(const Config *c) {
    cJSON *root = cJSON_CreateObject();
    cJSON *anth = cJSON_CreateObject();
    cJSON *oai = cJSON_CreateObject();
    cJSON *xai = cJSON_CreateObject();
    cJSON *sub = cJSON_CreateObject();
    char *printed;
    char *path;
    int rc;
    cJSON_AddStringToObject(root, "provider", c->provider);
    cJSON_AddStringToObject(root, "model", c->model ? c->model : "");
    cJSON_AddStringToObject(root, "mode", c->mode);
    cJSON_AddStringToObject(root, "glyphs", c->glyphs ? c->glyphs : "auto");
    cJSON_AddNumberToObject(root, "max_tokens", c->max_tokens);
    cJSON_AddNumberToObject(root, "max_steps", c->max_steps);
    cJSON_AddNumberToObject(root, "context_budget", c->context_budget);
    cJSON_AddBoolToObject(root, "sandbox", c->sandbox);
    cJSON_AddNumberToObject(root, "read_cap", (double)c->read_cap);
    cJSON_AddNumberToObject(root, "output_cap", (double)c->output_cap);
    cJSON_AddNumberToObject(root, "shell_timeout", c->shell_timeout);
    cJSON_AddStringToObject(root, "ca_bundle", c->ca_bundle ? c->ca_bundle : "");
    cJSON_AddStringToObject(anth, "base_url", c->anthropic_base);
    cJSON_AddStringToObject(oai, "base_url", c->openai_base);
    cJSON_AddStringToObject(oai, "wire", c->openai_wire);
    cJSON_AddStringToObject(xai, "base_url", c->xai_base);
    cJSON_AddStringToObject(sub, "base_url", c->openai_sub_base);
    cJSON_AddStringToObject(sub, "originator", c->originator);
    cJSON_AddItemToObject(root, "anthropic", anth);
    cJSON_AddItemToObject(root, "openai", oai);
    cJSON_AddItemToObject(root, "xai", xai);
    cJSON_AddItemToObject(root, "openai_sub", sub);
    printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        return -1;
    }
    path = cfg_path();
    rc = write_file(path, printed, strlen(printed), 0644);
    free(path);
    cJSON_free(printed);
    return rc;
}

void auth_init(Auth *a) {
    memset(a, 0, sizeof *a);
}

void auth_free(Auth *a) {
    free(a->anthropic_key);
    free(a->openai_key);
    free(a->xai_key);
    oauth_tok_clear(&a->xai_sub);
    oauth_tok_clear(&a->openai_sub);
    memset(a, 0, sizeof *a);
}

void auth_load(Auth *a) {
    char *path = auth_path();
    char *text = read_file_cap(path, 1024 * 1024, NULL, NULL, NULL);
    cJSON *root, *item;
    free(path);
    if (!text) {
        return;
    }
    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "anthropic");
    if (cJSON_IsObject(item)) {
        a->anthropic_key = jdup(item, "api_key", "");
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "openai");
    if (cJSON_IsObject(item)) {
        a->openai_key = jdup(item, "api_key", "");
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "xai");
    if (cJSON_IsObject(item)) {
        a->xai_key = jdup(item, "api_key", "");
    }
    tok_load(&a->xai_sub, cJSON_GetObjectItemCaseSensitive(root, "xai_sub"));
    tok_load(&a->openai_sub, cJSON_GetObjectItemCaseSensitive(root, "openai_sub"));
    cJSON_Delete(root);
}

static void add_tok(cJSON *root, const char *key, const OAuthTok *t) {
    cJSON *o;
    if (!t->access || !t->access[0]) {
        return;
    }
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "access_token", t->access);
    cJSON_AddStringToObject(o, "refresh_token", t->refresh ? t->refresh : "");
    cJSON_AddStringToObject(o, "account_id", t->account_id ? t->account_id : "");
    cJSON_AddNumberToObject(o, "expires_at", (double)t->expires_at);
    cJSON_AddItemToObject(root, key, o);
}

int auth_save(const Auth *a) {
    cJSON *root = cJSON_CreateObject();
    cJSON *item;
    char *printed;
    char *path;
    int rc;
    if (a->anthropic_key && a->anthropic_key[0]) {
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "api_key", a->anthropic_key);
        cJSON_AddItemToObject(root, "anthropic", item);
    }
    if (a->openai_key && a->openai_key[0]) {
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "api_key", a->openai_key);
        cJSON_AddItemToObject(root, "openai", item);
    }
    if (a->xai_key && a->xai_key[0]) {
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "api_key", a->xai_key);
        cJSON_AddItemToObject(root, "xai", item);
    }
    add_tok(root, "xai_sub", &a->xai_sub);
    add_tok(root, "openai_sub", &a->openai_sub);
    printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        return -1;
    }
    path = auth_path();
    rc = write_file(path, printed, strlen(printed), 0600);
    free(path);
    cJSON_free(printed);
    return rc;
}

const char *auth_api_key(const Auth *a, const char *provider) {
    const char *env = NULL;
    const char *file = NULL;
    if (str_eq(provider, "anthropic")) {
        env = getenv("ANTHROPIC_API_KEY");
        file = a->anthropic_key;
    } else if (str_eq(provider, "openai")) {
        env = getenv("OPENAI_API_KEY");
        file = a->openai_key;
    } else if (str_eq(provider, "xai") || str_eq(provider, "xai-sub")) {
        env = getenv("XAI_API_KEY");
        file = a->xai_key;
        if (str_eq(provider, "xai-sub")) {
            env = NULL;
            file = NULL;
        }
    }
    if (env && env[0]) {
        return env;
    }
    if (file && file[0]) {
        return file;
    }
    return NULL;
}

OAuthTok *auth_oauth(Auth *a, const char *provider) {
    if (str_eq(provider, "xai-sub")) {
        return &a->xai_sub;
    }
    if (str_eq(provider, "openai-sub")) {
        return &a->openai_sub;
    }
    return NULL;
}

void auth_set_key(Auth *a, const char *provider, const char *key) {
    char **slot = NULL;
    if (str_eq(provider, "anthropic")) {
        slot = &a->anthropic_key;
    } else if (str_eq(provider, "openai")) {
        slot = &a->openai_key;
    } else if (str_eq(provider, "xai")) {
        slot = &a->xai_key;
    }
    if (!slot) {
        return;
    }
    free(*slot);
    *slot = xstrdup(key ? key : "");
}
