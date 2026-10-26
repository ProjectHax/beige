#include "models.h"

#include "cJSON.h"

static int model_skip(const char *id) {
    static const char *skip[] = {"whisper",    "embedding", "moderation", "dall-e",   "tts",
                                 "transcribe", "realtime",  "sora",       "audio",    "image",
                                 "davinci",    "babbage",   NULL};
    int i;
    if (!id || !id[0]) {
        return 1;
    }
    for (i = 0; skip[i]; i++) {
        if (strstr(id, skip[i])) {
            return 1;
        }
    }
    return 0;
}

static int have_id(char **ids, int n, const char *id) {
    int i;
    for (i = 0; i < n; i++) {
        if (str_eq(ids[i], id)) {
            return 1;
        }
    }
    return 0;
}

static void add_id(char ***ids, int *n, const char *id) {
    if (model_skip(id) || have_id(*ids, *n, id) || *n >= 80) {
        return;
    }
    *ids = xrealloc(*ids, (size_t)(*n + 1) * sizeof(char *));
    (*ids)[*n] = xstrdup(id);
    (*n)++;
}

static void add_item(char ***ids, int *n, cJSON *item) {
    cJSON *id;
    cJSON *slug;
    if (cJSON_IsString(item) && item->valuestring) {
        add_id(ids, n, item->valuestring);
        return;
    }
    if (!cJSON_IsObject(item)) {
        return;
    }
    id = cJSON_GetObjectItemCaseSensitive(item, "id");
    if (cJSON_IsString(id)) {
        add_id(ids, n, id->valuestring);
        return;
    }
    slug = cJSON_GetObjectItemCaseSensitive(item, "slug");
    if (cJSON_IsString(slug)) {
        add_id(ids, n, slug->valuestring);
    }
}

int models_parse(const char *json, char ***ids, int *n, int *has_more, char **last_id) {
    cJSON *root;
    cJSON *data;
    cJSON *more;
    cJSON *tail;
    int i;
    int count;
    if (has_more) {
        *has_more = 0;
    }
    if (last_id) {
        *last_id = NULL;
    }
    if (!json || !ids || !n) {
        return -1;
    }
    root = cJSON_Parse(json);
    if (!root) {
        return -1;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        data = cJSON_GetObjectItemCaseSensitive(root, "models");
    }
    if (cJSON_IsArray(data)) {
        count = cJSON_GetArraySize(data);
        for (i = 0; i < count; i++) {
            add_item(ids, n, cJSON_GetArrayItem(data, i));
        }
    }
    more = cJSON_GetObjectItemCaseSensitive(root, "has_more");
    if (has_more && cJSON_IsTrue(more)) {
        *has_more = 1;
    }
    tail = cJSON_GetObjectItemCaseSensitive(root, "last_id");
    if (last_id && cJSON_IsString(tail) && tail->valuestring[0]) {
        *last_id = xstrdup(tail->valuestring);
    }
    cJSON_Delete(root);
    return 0;
}

void models_free(char **ids, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free(ids[i]);
    }
    free(ids);
}

static int str_ends_check(const char *s, const char *suffix) {
    size_t n;
    size_t m;
    if (!s || !suffix) {
        return 0;
    }
    n = strlen(s);
    m = strlen(suffix);
    if (n < m) {
        return 0;
    }
    return strcmp(s + n - m, suffix) == 0;
}

static char *models_url(const Provider *provider, const char *after_id) {
    const char *url = provider && provider->url ? provider->url : "";
    const char *suffix = NULL;
    const char *repl = "/models";
    size_t n;
    size_t s;
    char *base;
    char *out;
    if (str_ends_check(url, "/chat/completions")) {
        suffix = "/chat/completions";
    } else if (str_ends_check(url, "/messages")) {
        suffix = "/messages";
    } else if (str_ends_check(url, "/responses")) {
        suffix = "/responses";
    }
    if (!suffix) {
        return NULL;
    }
    n = strlen(url);
    s = strlen(suffix);
    base = xmalloc(n - s + strlen(repl) + 1);
    memcpy(base, url, n - s);
    memcpy(base + (n - s), repl, strlen(repl) + 1);
    if (provider->wire == WIRE_ANTHROPIC) {
        if (after_id && after_id[0]) {
            out = xmalloc(strlen(base) + strlen(after_id) + 48);
            sprintf(out, "%s?limit=100&after_id=%s", base, after_id);
        } else {
            out = xmalloc(strlen(base) + 16);
            sprintf(out, "%s?limit=100", base);
        }
        free(base);
        return out;
    }
    return base;
}

int models_fetch(Http *http, const Provider *provider, char ***ids, int *n, char **err) {
    char *after = NULL;
    int page;
    if (err) {
        *err = NULL;
    }
    if (ids) {
        *ids = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!provider || !provider->api_key || !provider->api_key[0]) {
        if (err) {
            *err = xstrdup("no credentials for model list");
        }
        return -1;
    }
    for (page = 0; page < 5; page++) {
        char *url = models_url(provider, after);
        struct curl_slist *hdrs;
        char *body = NULL;
        long status = 0;
        char *herr = NULL;
        int more = 0;
        char *tail = NULL;
        int rc;
        if (!url) {
            free(after);
            models_free(*ids, *n);
            *ids = NULL;
            *n = 0;
            if (err) {
                *err = xstrdup("cannot build model list URL");
            }
            return -1;
        }
        hdrs = provider_headers(provider);
        rc = http_exchange(http, "GET", url, hdrs, NULL, &status, &body, 512 * 1024, &herr);
        curl_slist_free_all(hdrs);
        free(url);
        if (rc != 0 || status >= 400) {
            free(after);
            models_free(*ids, *n);
            *ids = NULL;
            *n = 0;
            if (err && !*err) {
                if (body && body[0] && status >= 400) {
                    *err = body;
                    body = NULL;
                } else {
                    *err = herr ? xstrdup(herr) : xstrdup("model list failed");
                }
            }
            free(herr);
            free(body);
            return -1;
        }
        free(herr);
        if (models_parse(body, ids, n, &more, &tail) != 0) {
            free(body);
            free(after);
            free(tail);
            models_free(*ids, *n);
            *ids = NULL;
            *n = 0;
            if (err) {
                *err = xstrdup("model list was not valid JSON");
            }
            return -1;
        }
        free(body);
        free(after);
        after = NULL;
        if (!more || !tail || provider->wire != WIRE_ANTHROPIC) {
            free(tail);
            break;
        }
        after = tail;
    }
    free(after);
    if (*n <= 0) {
        if (err) {
            *err = xstrdup("model list was empty");
        }
        return -1;
    }
    return 0;
}
