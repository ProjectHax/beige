#include "provider_internal.h"

#include "tools.h"

#include <string.h>

char *beige_json_print(cJSON *root) {
    char *printed;
    char *copy;
    printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        return NULL;
    }
    copy = xstrdup(printed);
    cJSON_free(printed);
    return copy;
}

int provider_is_known(const char *id) {
    return str_eq(id, "anthropic") || str_eq(id, "openai") || str_eq(id, "xai") || str_eq(id, "openai-sub") ||
           str_eq(id, "xai-sub");
}

const char *provider_default_model(const char *id) {
    if (str_eq(id, "anthropic")) {
        return "claude-sonnet-4-5";
    }
    if (str_eq(id, "xai") || str_eq(id, "xai-sub")) {
        return "grok-4";
    }
    if (str_eq(id, "openai-sub")) {
        return "gpt-5.4";
    }
    return "gpt-4.1";
}

static char *join_url(const char *base, const char *path) {
    size_t nb, np;
    int slash;
    char *out;
    while (path[0] == '/') {
        path++;
    }
    nb = strlen(base);
    while (nb > 0 && base[nb - 1] == '/') {
        nb--;
    }
    np = strlen(path);
    slash = 1;
    out = xmalloc(nb + np + 2);
    memcpy(out, base, nb);
    out[nb] = '/';
    memcpy(out + nb + slash, path, np + 1);
    return out;
}

static const char *default_base(const char *id) {
    if (str_eq(id, "anthropic")) {
        return "https://api.anthropic.com";
    }
    if (str_eq(id, "xai") || str_eq(id, "xai-sub")) {
        return "https://api.x.ai/v1";
    }
    if (str_eq(id, "openai-sub")) {
        return "https://chatgpt.com/backend-api/codex";
    }
    return "https://api.openai.com/v1";
}

static const char *default_path(const char *id, const char *wire_name) {
    if (str_eq(id, "anthropic")) {
        return "v1/messages";
    }
    if (str_eq(id, "xai") || str_eq(id, "xai-sub")) {
        return "chat/completions";
    }
    if (wire_name && str_eq(wire_name, "chat")) {
        return "chat/completions";
    }
    return "responses";
}

Provider *provider_open(const ProviderSpec *spec, char **err) {
    Provider *p;
    const char *id;
    const char *base;
    const char *path;
    if (err) {
        *err = NULL;
    }
    if (!spec || !spec->id || !provider_is_known(spec->id)) {
        if (err) {
            *err = xstrdup("unknown provider");
        }
        return NULL;
    }
    id = spec->id;
    if (!spec->api_key || !spec->api_key[0]) {
        if (err) {
            char buf[160];
            snprintf(buf, sizeof buf, "no credentials for %s. Run: beige login %s", id, id);
            *err = xstrdup(buf);
        }
        return NULL;
    }
    p = xmalloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->id = xstrdup(id);
    p->model = xstrdup(spec->model && spec->model[0] ? spec->model : provider_default_model(id));
    p->api_key = xstrdup(spec->api_key);
    p->account_id = spec->account_id ? xstrdup(spec->account_id) : NULL;
    p->originator = xstrdup(spec->originator && spec->originator[0] ? spec->originator : "beige");
    p->max_tokens = spec->max_tokens > 0 ? spec->max_tokens : 8192;
    p->subscription = str_eq(id, "openai-sub") || str_eq(id, "xai-sub");
    base = spec->base_url && spec->base_url[0] ? spec->base_url : default_base(id);
    path = default_path(id, spec->wire_name);
    p->url = join_url(base, path);
    if (str_eq(id, "anthropic")) {
        p->wire = WIRE_ANTHROPIC;
    } else if (str_eq(id, "xai") || str_eq(id, "xai-sub") || (spec->wire_name && str_eq(spec->wire_name, "chat"))) {
        p->wire = WIRE_CHAT;
    } else {
        p->wire = WIRE_RESPONSES;
    }
    return p;
}

void provider_free(Provider *p) {
    if (!p) {
        return;
    }
    free(p->id);
    free(p->model);
    free(p->url);
    free(p->api_key);
    free(p->account_id);
    free(p->originator);
    free(p);
}

char *provider_build_body(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools) {
    if (p->wire == WIRE_ANTHROPIC) {
        return anthropic_build(p, ts, system_prompt, with_tools);
    }
    if (p->wire == WIRE_CHAT) {
        return openai_chat_build(p, ts, system_prompt, with_tools);
    }
    return openai_responses_build(p, ts, system_prompt, with_tools);
}

struct curl_slist *provider_headers(const Provider *p) {
    struct curl_slist *h = NULL;
    char *auth;
    h = http_hdr(h, "content-type: application/json");
    h = http_hdr(h, "accept: text/event-stream");
    if (p->wire == WIRE_ANTHROPIC) {
        auth = xmalloc(strlen(p->api_key) + 16);
        sprintf(auth, "x-api-key: %s", p->api_key);
        h = http_hdr(h, auth);
        free(auth);
        h = http_hdr(h, "anthropic-version: 2023-06-01");
        return h;
    }
    auth = xmalloc(strlen(p->api_key) + 24);
    sprintf(auth, "authorization: Bearer %s", p->api_key);
    h = http_hdr(h, auth);
    free(auth);
    if (p->subscription && str_eq(p->id, "openai-sub")) {
        char *line;
        if (p->account_id && p->account_id[0]) {
            line = xmalloc(strlen(p->account_id) + 24);
            sprintf(line, "chatgpt-account-id: %s", p->account_id);
            h = http_hdr(h, line);
            free(line);
        }
        line = xmalloc(strlen(p->originator) + 16);
        sprintf(line, "originator: %s", p->originator);
        h = http_hdr(h, line);
        free(line);
        h = http_hdr(h, "OpenAI-Beta: responses=experimental");
    }
    return h;
}

void *provider_parse_new(const Provider *p) {
    if (p->wire == WIRE_ANTHROPIC) {
        return anthropic_state_new();
    }
    if (p->wire == WIRE_CHAT) {
        return chat_state_new();
    }
    return NULL;
}

void provider_parse_free(const Provider *p, void *state) {
    if (!state) {
        return;
    }
    if (p->wire == WIRE_ANTHROPIC) {
        anthropic_state_free(state);
    } else if (p->wire == WIRE_CHAT) {
        chat_state_free(state);
    }
}

int provider_parse_event(const Provider *p, void *state, StreamAcc *acc, const char *event, const char *data) {
    if (p->wire == WIRE_ANTHROPIC) {
        return anthropic_parse(state, acc, event, data);
    }
    if (p->wire == WIRE_CHAT) {
        return openai_chat_parse(state, acc, event, data);
    }
    return openai_responses_parse(state, acc, event, data);
}
