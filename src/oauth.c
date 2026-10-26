#include "oauth.h"

#include "cJSON.h"
#include "signals.h"

#include <ctype.h>
#include <time.h>

static const char *XAI_CLIENT = "b1a00492-073a-47ea-816f-4c329264a828";
static const char *XAI_SCOPE = "openid profile email offline_access grok-cli:access api:access";
static const char *XAI_DEVICE = "https://auth.x.ai/oauth2/device/code";
static const char *XAI_TOKEN = "https://auth.x.ai/oauth2/token";

static const char *OAI_CLIENT = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char *OAI_USERCODE = "https://auth.openai.com/api/accounts/deviceauth/usercode";
static const char *OAI_DEVICE_TOKEN = "https://auth.openai.com/api/accounts/deviceauth/token";
static const char *OAI_TOKEN = "https://auth.openai.com/oauth/token";
static const char *OAI_REDIRECT = "https://auth.openai.com/deviceauth/callback";
static const char *OAI_VERIFY = "https://auth.openai.com/codex/device";

static int host_ok(const char *url) {
    const char *p;
    const char *slash;
    char host[128];
    size_t n;
    if (!str_starts(url, "https://")) {
        return 0;
    }
    p = url + 8;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= sizeof host) {
        return 0;
    }
    memcpy(host, p, n);
    host[n] = 0;
    {
        char *colon = strchr(host, ':');
        if (colon) {
            *colon = 0;
        }
    }
    if (str_eq(host, "x.ai")) {
        return 1;
    }
    n = strlen(host);
    return n > 5 && str_eq(host + n - 5, ".x.ai");
}

static char *form_escape(const char *s) {
    static const char *hex = "0123456789ABCDEF";
    Buf b;
    const unsigned char *p;
    buf_init(&b);
    if (!s) {
        return buf_detach(&b);
    }
    for (p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
            *p == '_' || *p == '.' || *p == '~') {
            buf_putc(&b, (char)*p);
        } else if (*p == ' ') {
            buf_putc(&b, '+');
        } else {
            char enc[4];
            enc[0] = '%';
            enc[1] = hex[(*p >> 4) & 0xf];
            enc[2] = hex[*p & 0xf];
            enc[3] = 0;
            buf_puts(&b, enc);
        }
    }
    return buf_detach(&b);
}

static char *form_pair(const char *k, const char *v) {
    char *ek = form_escape(k);
    char *ev = form_escape(v);
    char *out = xmalloc(strlen(ek) + strlen(ev) + 2);
    sprintf(out, "%s=%s", ek, ev);
    free(ek);
    free(ev);
    return out;
}

static void sleep_sec(int seconds) {
    while (seconds > 0) {
        struct timeval tv;
        if (g_sigint) {
            return;
        }
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        select(0, NULL, NULL, NULL, &tv);
        seconds--;
    }
}

static cJSON *parse_body(const char *body, long status, char **err, const char *what) {
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (!root) {
        if (err) {
            char buf[128];
            snprintf(buf, sizeof buf, "%s returned invalid JSON (HTTP %ld)", what, status);
            *err = xstrdup(buf);
        }
        return NULL;
    }
    return root;
}

static char *json_str(cJSON *o, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        return xstrdup(it->valuestring);
    }
    return NULL;
}

static long long json_num(cJSON *o, const char *key, long long fallback) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it) && it->valuedouble > 0) {
        return (long long)it->valuedouble;
    }
    return fallback;
}

static int fill_tok(OAuthTok *out, const char *access, const char *refresh, const char *account, long long expires_in,
                    const char *keep_refresh) {
    long long exp;
    char *ref;
    if (!access || !access[0]) {
        return -1;
    }
    if (expires_in <= 0) {
        expires_in = 3600;
    }
    exp = (long long)time(NULL) + expires_in;
    if (refresh && refresh[0]) {
        ref = xstrdup(refresh);
    } else if (keep_refresh && keep_refresh[0]) {
        ref = xstrdup(keep_refresh);
    } else {
        ref = xstrdup("");
    }
    oauth_tok_set(out, access, ref, account, exp);
    free(ref);
    return 0;
}

int oauth_login_xai(Http *http, OauthNote note, void *ud, OAuthTok *out, char **err) {
    struct curl_slist *hdrs = NULL;
    char *client = form_pair("client_id", XAI_CLIENT);
    char *scope = form_pair("scope", XAI_SCOPE);
    char *refer = form_pair("referrer", "beige");
    char *body;
    char *resp = NULL;
    long status = 0;
    cJSON *root;
    char *device = NULL, *user = NULL, *verify = NULL, *verify_full = NULL;
    long long interval, expires, deadline;
    int rc = -1;

    if (err) {
        *err = NULL;
    }
    body = xmalloc(strlen(client) + strlen(scope) + strlen(refer) + 3);
    sprintf(body, "%s&%s&%s", client, scope, refer);
    free(client);
    free(scope);
    free(refer);
    hdrs = http_hdr(hdrs, "content-type: application/x-www-form-urlencoded");
    hdrs = http_hdr(hdrs, "accept: application/json");
    if (http_exchange(http, "POST", XAI_DEVICE, hdrs, body, &status, &resp, 1024 * 64, err) != 0) {
        curl_slist_free_all(hdrs);
        free(body);
        return -1;
    }
    free(body);
    curl_slist_free_all(hdrs);
    root = parse_body(resp, status, err, "xAI device authorization");
    free(resp);
    if (!root) {
        return -1;
    }
    if (status >= 400) {
        char *em = json_str(root, "error");
        char *ed = json_str(root, "error_description");
        if (err) {
            char buf[256];
            snprintf(buf, sizeof buf, "xAI device authorization failed (HTTP %ld)%s%s", status, em ? ": " : "",
                     em ? em : "");
            *err = xstrdup(buf);
        }
        free(em);
        free(ed);
        cJSON_Delete(root);
        return -1;
    }
    device = json_str(root, "device_code");
    user = json_str(root, "user_code");
    verify = json_str(root, "verification_uri");
    verify_full = json_str(root, "verification_uri_complete");
    interval = json_num(root, "interval", 5);
    expires = json_num(root, "expires_in", 600);
    cJSON_Delete(root);
    if (!device || !user || !verify || !host_ok(verify) || (verify_full && !host_ok(verify_full))) {
        free(device);
        free(user);
        free(verify);
        free(verify_full);
        if (err) {
            *err = xstrdup("xAI device authorization response was rejected");
        }
        return -1;
    }
    if (note) {
        char line[512];
        snprintf(line, sizeof line, "Open %s", verify_full ? verify_full : verify);
        note(ud, line);
        snprintf(line, sizeof line, "Enter code: %s", user);
        note(ud, line);
        note(ud, "Waiting for approval. Ctrl-C cancels.");
    }
    deadline = (long long)time(NULL) + expires;
    if (interval < 1) {
        interval = 5;
    }
    sleep_sec((int)interval);
    while ((long long)time(NULL) < deadline) {
        char *dcode = form_pair("device_code", device);
        char *cid = form_pair("client_id", XAI_CLIENT);
        char *grant = form_pair("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
        char *form;
        char *access, *refresh;
        if (g_sigint) {
            free(dcode);
            free(cid);
            free(grant);
            if (err) {
                *err = xstrdup("cancelled");
            }
            break;
        }
        form = xmalloc(strlen(dcode) + strlen(cid) + strlen(grant) + 3);
        sprintf(form, "%s&%s&%s", grant, cid, dcode);
        free(dcode);
        free(cid);
        free(grant);
        hdrs = NULL;
        hdrs = http_hdr(hdrs, "content-type: application/x-www-form-urlencoded");
        hdrs = http_hdr(hdrs, "accept: application/json");
        resp = NULL;
        status = 0;
        if (http_exchange(http, "POST", XAI_TOKEN, hdrs, form, &status, &resp, 1024 * 64, err) != 0) {
            curl_slist_free_all(hdrs);
            free(form);
            break;
        }
        free(form);
        curl_slist_free_all(hdrs);
        root = parse_body(resp, status, err, "xAI token");
        free(resp);
        if (!root) {
            break;
        }
        if (status >= 400) {
            char *e = json_str(root, "error");
            if (e && (str_eq(e, "authorization_pending") || str_eq(e, "slow_down"))) {
                if (str_eq(e, "slow_down")) {
                    interval += 2;
                }
                free(e);
                cJSON_Delete(root);
                sleep_sec((int)interval);
                continue;
            }
            if (err && !*err) {
                *err = xstrdup(e ? e : "xAI device authorization failed");
            }
            free(e);
            cJSON_Delete(root);
            break;
        }
        access = json_str(root, "access_token");
        refresh = json_str(root, "refresh_token");
        if (fill_tok(out, access, refresh, "", json_num(root, "expires_in", 3600), NULL) != 0) {
            if (err) {
                *err = xstrdup("xAI token response missing access_token");
            }
            free(access);
            free(refresh);
            cJSON_Delete(root);
            break;
        }
        free(access);
        free(refresh);
        cJSON_Delete(root);
        rc = 0;
        break;
    }
    if (rc != 0 && err && !*err) {
        *err = xstrdup("xAI device code expired");
    }
    free(device);
    free(user);
    free(verify);
    free(verify_full);
    return rc;
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-' || c == '+') {
        return 62;
    }
    if (c == '_' || c == '/') {
        return 63;
    }
    return -1;
}

static char *b64url_decode(const char *in) {
    size_t n = strlen(in);
    size_t i = 0, o = 0;
    char *out = xmalloc(n + 4);
    int val = 0, valb = -8;
    for (i = 0; i < n; i++) {
        int d = b64val((unsigned char)in[i]);
        if (d < 0) {
            break;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out[o++] = (char)((val >> valb) & 0xff);
            valb -= 8;
        }
    }
    out[o] = 0;
    return out;
}

static char *account_from_jwt(const char *jwt) {
    char *copy;
    char *dot;
    char *dot2;
    char *payload;
    cJSON *root;
    cJSON *id;
    cJSON *ns;
    char *account = NULL;
    if (!jwt) {
        return NULL;
    }
    copy = xstrdup(jwt);
    dot = strchr(copy, '.');
    if (!dot) {
        free(copy);
        return NULL;
    }
    dot2 = strchr(dot + 1, '.');
    if (!dot2) {
        free(copy);
        return NULL;
    }
    *dot2 = 0;
    payload = b64url_decode(dot + 1);
    free(copy);
    root = cJSON_Parse(payload);
    free(payload);
    if (!root) {
        return NULL;
    }
    id = cJSON_GetObjectItemCaseSensitive(root, "chatgpt_account_id");
    if (cJSON_IsString(id) && id->valuestring[0]) {
        account = xstrdup(id->valuestring);
    }
    ns = cJSON_GetObjectItemCaseSensitive(root, "https://api.openai.com/auth");
    if (!account && cJSON_IsObject(ns)) {
        id = cJSON_GetObjectItemCaseSensitive(ns, "chatgpt_account_id");
        if (cJSON_IsString(id) && id->valuestring[0]) {
            account = xstrdup(id->valuestring);
        }
    }
    cJSON_Delete(root);
    return account;
}

int oauth_login_openai(Http *http, OauthNote note, void *ud, OAuthTok *out, char **err) {
    cJSON *req = cJSON_CreateObject();
    char *body;
    struct curl_slist *hdrs = NULL;
    char *resp = NULL;
    long status = 0;
    cJSON *root;
    char *device_id = NULL;
    char *user = NULL;
    long long interval = 5;
    long long deadline;
    char *auth_code = NULL;
    char *verifier = NULL;
    int rc = -1;

    if (err) {
        *err = NULL;
    }
    cJSON_AddStringToObject(req, "client_id", OAI_CLIENT);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    hdrs = http_hdr(hdrs, "content-type: application/json");
    hdrs = http_hdr(hdrs, "accept: application/json");
    if (http_exchange(http, "POST", OAI_USERCODE, hdrs, body, &status, &resp, 1024 * 64, err) != 0 || status >= 400) {
        curl_slist_free_all(hdrs);
        cJSON_free(body);
        free(resp);
        if (err && !*err) {
            *err = xstrdup("ChatGPT device authorization request failed");
        }
        return -1;
    }
    cJSON_free(body);
    curl_slist_free_all(hdrs);
    root = parse_body(resp, status, err, "ChatGPT device authorization");
    free(resp);
    if (!root) {
        return -1;
    }
    device_id = json_str(root, "device_auth_id");
    user = json_str(root, "user_code");
    if (!user) {
        user = json_str(root, "usercode");
    }
    interval = json_num(root, "interval", 5);
    cJSON_Delete(root);
    if (!device_id || !user) {
        free(device_id);
        free(user);
        if (err) {
            *err = xstrdup("ChatGPT device authorization response missing required fields");
        }
        return -1;
    }
    if (note) {
        char line[256];
        note(ud, "Open https://auth.openai.com/codex/device");
        snprintf(line, sizeof line, "Enter code: %s", user);
        note(ud, line);
        note(ud, "Waiting for approval. Ctrl-C cancels.");
    }
    (void)OAI_VERIFY;
    deadline = (long long)time(NULL) + 15 * 60;
    if (interval < 1) {
        interval = 5;
    }
    while ((long long)time(NULL) < deadline) {
        cJSON *poll;
        char *pbody;
        if (g_sigint) {
            if (err) {
                *err = xstrdup("cancelled");
            }
            break;
        }
        sleep_sec((int)interval);
        if (g_sigint) {
            if (err) {
                *err = xstrdup("cancelled");
            }
            break;
        }
        poll = cJSON_CreateObject();
        cJSON_AddStringToObject(poll, "device_auth_id", device_id);
        cJSON_AddStringToObject(poll, "user_code", user);
        pbody = cJSON_PrintUnformatted(poll);
        cJSON_Delete(poll);
        hdrs = NULL;
        hdrs = http_hdr(hdrs, "content-type: application/json");
        hdrs = http_hdr(hdrs, "accept: application/json");
        resp = NULL;
        status = 0;
        if (http_exchange(http, "POST", OAI_DEVICE_TOKEN, hdrs, pbody, &status, &resp, 1024 * 64, err) != 0) {
            curl_slist_free_all(hdrs);
            cJSON_free(pbody);
            break;
        }
        cJSON_free(pbody);
        curl_slist_free_all(hdrs);
        if (status == 403 || status == 404) {
            free(resp);
            continue;
        }
        if (status >= 400) {
            free(resp);
            if (err && !*err) {
                *err = xstrdup("ChatGPT device authorization failed");
            }
            break;
        }
        root = parse_body(resp, status, err, "ChatGPT device poll");
        free(resp);
        if (!root) {
            break;
        }
        auth_code = json_str(root, "authorization_code");
        verifier = json_str(root, "code_verifier");
        cJSON_Delete(root);
        if (!auth_code || !verifier) {
            if (err) {
                *err = xstrdup("ChatGPT device authorization response missing required fields");
            }
            break;
        }
        rc = 0;
        break;
    }
    free(device_id);
    free(user);
    if (rc != 0) {
        free(auth_code);
        free(verifier);
        if (err && !*err) {
            *err = xstrdup("ChatGPT device authorization expired");
        }
        return -1;
    }
    {
        char *pairs[5];
        char *form;
        char *access, *refresh, *id_token, *account;
        size_t len = 0;
        int i;
        pairs[0] = form_pair("grant_type", "authorization_code");
        pairs[1] = form_pair("client_id", OAI_CLIENT);
        pairs[2] = form_pair("code", auth_code);
        pairs[3] = form_pair("code_verifier", verifier);
        pairs[4] = form_pair("redirect_uri", OAI_REDIRECT);
        free(auth_code);
        free(verifier);
        for (i = 0; i < 5; i++) {
            len += strlen(pairs[i]) + 1;
        }
        form = xmalloc(len + 1);
        form[0] = 0;
        for (i = 0; i < 5; i++) {
            if (i) {
                strcat(form, "&");
            }
            strcat(form, pairs[i]);
            free(pairs[i]);
        }
        hdrs = NULL;
        hdrs = http_hdr(hdrs, "content-type: application/x-www-form-urlencoded");
        hdrs = http_hdr(hdrs, "accept: application/json");
        resp = NULL;
        status = 0;
        rc = http_exchange(http, "POST", OAI_TOKEN, hdrs, form, &status, &resp, 1024 * 128, err);
        free(form);
        curl_slist_free_all(hdrs);
        if (rc != 0 || status >= 400) {
            free(resp);
            if (err && !*err) {
                *err = xstrdup("ChatGPT token exchange failed");
            }
            return -1;
        }
        root = parse_body(resp, status, err, "ChatGPT token");
        free(resp);
        if (!root) {
            return -1;
        }
        access = json_str(root, "access_token");
        refresh = json_str(root, "refresh_token");
        id_token = json_str(root, "id_token");
        account = account_from_jwt(id_token ? id_token : access);
        rc = fill_tok(out, access, refresh, account ? account : "", json_num(root, "expires_in", 3600), NULL);
        if (rc != 0 && err) {
            *err = xstrdup("ChatGPT token response missing access_token");
        }
        free(access);
        free(refresh);
        free(id_token);
        free(account);
        cJSON_Delete(root);
        return rc;
    }
}

static int refresh_form(Http *http, const char *token_url, const char *client, OAuthTok *tok, char **err) {
    char *grant = form_pair("grant_type", "refresh_token");
    char *cid = form_pair("client_id", client);
    char *rt = form_pair("refresh_token", tok->refresh ? tok->refresh : "");
    char *form;
    struct curl_slist *hdrs = NULL;
    char *resp = NULL;
    long status = 0;
    cJSON *root;
    char *access, *refresh, *account;
    int rc;
    if (!tok->refresh || !tok->refresh[0]) {
        if (err) {
            *err = xstrdup("no refresh token; run beige login again");
        }
        free(grant);
        free(cid);
        free(rt);
        return -1;
    }
    form = xmalloc(strlen(grant) + strlen(cid) + strlen(rt) + 3);
    sprintf(form, "%s&%s&%s", grant, cid, rt);
    free(grant);
    free(cid);
    free(rt);
    hdrs = http_hdr(hdrs, "content-type: application/x-www-form-urlencoded");
    hdrs = http_hdr(hdrs, "accept: application/json");
    rc = http_exchange(http, "POST", token_url, hdrs, form, &status, &resp, 1024 * 128, err);
    free(form);
    curl_slist_free_all(hdrs);
    if (rc != 0 || status >= 400) {
        free(resp);
        if (err && !*err) {
            *err = xstrdup("token refresh failed");
        }
        return -1;
    }
    root = parse_body(resp, status, err, "token refresh");
    free(resp);
    if (!root) {
        return -1;
    }
    access = json_str(root, "access_token");
    refresh = json_str(root, "refresh_token");
    account = tok->account_id ? xstrdup(tok->account_id) : NULL;
    if (!account) {
        char *id_token = json_str(root, "id_token");
        account = account_from_jwt(id_token ? id_token : access);
        free(id_token);
    }
    rc = fill_tok(tok, access, refresh, account ? account : "", json_num(root, "expires_in", 3600), tok->refresh);
    free(access);
    free(refresh);
    free(account);
    cJSON_Delete(root);
    if (rc != 0 && err && !*err) {
        *err = xstrdup("refresh response missing access_token");
    }
    return rc;
}

int oauth_refresh_xai(Http *http, OAuthTok *tok, char **err) {
    return refresh_form(http, XAI_TOKEN, XAI_CLIENT, tok, err);
}

int oauth_refresh_openai(Http *http, OAuthTok *tok, char **err) {
    return refresh_form(http, OAI_TOKEN, OAI_CLIENT, tok, err);
}

int oauth_ensure(Http *http, const char *provider, OAuthTok *tok, char **err) {
    long long now;
    if (!tok || !tok->access || !tok->access[0]) {
        if (err) {
            *err = xstrdup("not signed in");
        }
        return -1;
    }
    now = (long long)time(NULL);
    if (tok->expires_at == 0 || tok->expires_at > now + 300) {
        return 0;
    }
    if (str_eq(provider, "xai-sub")) {
        return oauth_refresh_xai(http, tok, err);
    }
    if (str_eq(provider, "openai-sub")) {
        return oauth_refresh_openai(http, tok, err);
    }
    return 0;
}
