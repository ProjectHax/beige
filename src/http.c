#include "http.h"

#include <string.h>

static int g_ready = 0;

void http_init(void) {
    if (!g_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_ready = 1;
    }
}

void http_cleanup(void) {
    if (g_ready) {
        curl_global_cleanup();
        g_ready = 0;
    }
}

struct curl_slist *http_hdr(struct curl_slist *list, const char *line) {
    struct curl_slist *next = curl_slist_append(list, line);
    return next ? next : list;
}

static void apply_common(CURL *easy, Http *http, const char *url, struct curl_slist *headers) {
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "beige/0.1");
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    if (http && http->ca_bundle && http->ca_bundle[0]) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, http->ca_bundle);
    }
}

typedef struct {
    Buf pending;
    size_t pos;
    Buf data;
    char event[96];
    SseFn fn;
    void *ud;
    long status;
    Buf err_body;
} SseParse;

static void sse_dispatch(SseParse *s) {
    if (s->data.len == 0) {
        s->event[0] = 0;
        return;
    }
    if (s->fn) {
        s->fn(s->event, s->data.data ? s->data.data : "", s->ud);
    }
    buf_clear(&s->data);
    s->event[0] = 0;
}

static void sse_take_line(SseParse *s, char *line) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') {
        line[--n] = 0;
    }
    if (n == 0) {
        sse_dispatch(s);
        return;
    }
    if (str_starts(line, "event:")) {
        const char *v = line + 6;
        while (*v == ' ') {
            v++;
        }
        snprintf(s->event, sizeof s->event, "%s", v);
        return;
    }
    if (str_starts(line, "data:")) {
        const char *v = line + 5;
        if (*v == ' ') {
            v++;
        }
        if (s->data.len) {
            buf_putc(&s->data, '\n');
        }
        buf_puts(&s->data, v);
    }
}

static void sse_consume(SseParse *s) {
    while (s->pending.data && s->pos < s->pending.len) {
        char *start = s->pending.data + s->pos;
        char *nl = memchr(start, '\n', s->pending.len - s->pos);
        size_t linelen;
        char *line;
        if (!nl) {
            break;
        }
        linelen = (size_t)(nl - start);
        line = xstrndup(start, linelen);
        s->pos = (size_t)(nl - s->pending.data) + 1;
        sse_take_line(s, line);
        free(line);
    }
    if (s->pos > 0 && s->pending.data) {
        size_t rest = s->pending.len - s->pos;
        memmove(s->pending.data, s->pending.data + s->pos, rest);
        s->pending.len = rest;
        s->pending.data[rest] = 0;
        s->pos = 0;
    }
}

static size_t sse_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    SseParse *s = ud;
    size_t n = size * nmemb;
    if (s->status >= 400) {
        if (s->err_body.len < 8192) {
            size_t room = 8192 - s->err_body.len;
            buf_append(&s->err_body, ptr, n < room ? n : room);
        }
        return n;
    }
    buf_append(&s->pending, ptr, n);
    sse_consume(s);
    return n;
}

static size_t header_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    SseParse *s = ud;
    size_t n = size * nmemb;
    if (n >= 5 && strncmp(ptr, "HTTP/", 5) == 0) {
        char *sp = memchr(ptr, ' ', n);
        if (sp) {
            s->status = strtol(sp + 1, NULL, 10);
        }
    }
    return n;
}

static int xfer_abort(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    Http *http = ud;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    if (http && http->cancel && *http->cancel) {
        return 1;
    }
    return 0;
}

int http_post_sse(Http *http, const char *url, struct curl_slist *headers, const char *body, SseFn fn,
                  void *fn_ud, const IoHooks *hooks, long *status, char **err) {
    CURL *easy;
    CURLM *multi;
    SseParse parse;
    int still = 1;
    int failed = 0;
    CURLMcode mc;

    http_init();
    if (err) {
        *err = NULL;
    }
    if (status) {
        *status = 0;
    }
    memset(&parse, 0, sizeof parse);
    buf_init(&parse.pending);
    buf_init(&parse.data);
    buf_init(&parse.err_body);
    parse.fn = fn;
    parse.ud = fn_ud;

    easy = curl_easy_init();
    multi = curl_multi_init();
    if (!easy || !multi) {
        if (err) {
            *err = xstrdup("curl init failed");
        }
        if (easy) {
            curl_easy_cleanup(easy);
        }
        if (multi) {
            curl_multi_cleanup(multi);
        }
        buf_free(&parse.pending);
        buf_free(&parse.data);
        buf_free(&parse.err_body);
        return -1;
    }
    apply_common(easy, http, url, headers);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sse_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &parse);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_write);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &parse);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xfer_abort);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, http);

    curl_multi_add_handle(multi, easy);
    while (still) {
        fd_set r, w, e;
        int maxfd = -1;
        long timeout_ms = 1000;
        struct timeval tv;
        int rc;
        mc = curl_multi_perform(multi, &still);
        if (mc != CURLM_OK) {
            failed = 1;
            if (err) {
                *err = xstrdup(curl_multi_strerror(mc));
            }
            break;
        }
        if (!still) {
            break;
        }
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_ZERO(&e);
        curl_multi_fdset(multi, &r, &w, &e, &maxfd);
        curl_multi_timeout(multi, &timeout_ms);
        if (hooks && hooks->tick) {
            if (timeout_ms < 0 || timeout_ms > 100) {
                timeout_ms = 100;
            }
        } else if (timeout_ms < 0 || timeout_ms > 1000) {
            timeout_ms = 1000;
        }
        if (hooks && hooks->prepare) {
            hooks->prepare(hooks->ud, &r, &w, &e, &maxfd);
        }
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(maxfd + 1, &r, &w, &e, &tv);
        if (rc < 0 && errno == EINTR) {
            /* The signal handler records the request to stop. */
        } else if (rc > 0 && hooks && hooks->ready) {
            hooks->ready(hooks->ud, &r, &w, &e);
        }
        if (hooks && hooks->tick) {
            hooks->tick(hooks->ud);
        }
        if (http && http->cancel && *http->cancel) {
            failed = 1;
            if (err && !*err) {
                *err = xstrdup("cancelled");
            }
            break;
        }
    }
    {
        CURLMsg *msg;
        int left = 0;
        while ((msg = curl_multi_info_read(multi, &left)) != NULL) {
            if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK && !failed) {
                if (!(http && http->cancel && *http->cancel && msg->data.result == CURLE_ABORTED_BY_CALLBACK)) {
                    failed = 1;
                    if (err && !*err) {
                        *err = xstrdup(curl_easy_strerror(msg->data.result));
                    }
                }
            }
        }
    }
    sse_consume(&parse);
    if (parse.data.len) {
        sse_dispatch(&parse);
    }
    if (status) {
        *status = parse.status;
    }
    if (parse.status >= 400) {
        failed = 1;
        if (err && !*err) {
            if (parse.err_body.len) {
                *err = xstrdup(parse.err_body.data);
            } else {
                *err = xstrdup("HTTP error");
            }
        }
    }
    curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    buf_free(&parse.pending);
    buf_free(&parse.data);
    buf_free(&parse.err_body);
    return failed ? -1 : 0;
}

typedef struct {
    Buf body;
    size_t cap;
    int truncated;
} BodyCap;

static size_t body_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    BodyCap *b = ud;
    size_t n = size * nmemb;
    size_t room;
    if (b->body.len >= b->cap) {
        b->truncated = 1;
        return n;
    }
    room = b->cap - b->body.len;
    buf_append(&b->body, ptr, n < room ? n : room);
    if (n > room) {
        b->truncated = 1;
    }
    return n;
}

int http_exchange(Http *http, const char *method, const char *url, struct curl_slist *headers,
                  const char *body, long *status, char **resp, size_t cap, char **err) {
    CURL *easy;
    BodyCap capb;
    CURLcode rc;
    long st = 0;

    http_init();
    if (resp) {
        *resp = NULL;
    }
    if (err) {
        *err = NULL;
    }
    if (status) {
        *status = 0;
    }
    memset(&capb, 0, sizeof capb);
    buf_init(&capb.body);
    capb.cap = cap ? cap : (size_t)1024 * 1024;
    easy = curl_easy_init();
    if (!easy) {
        if (err) {
            *err = xstrdup("curl init failed");
        }
        return -1;
    }
    apply_common(easy, http, url, headers);
    if (method && str_eq(method, "POST")) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    } else {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, body_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &capb);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xfer_abort);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, http);
    rc = curl_easy_perform(easy);
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &st);
    curl_easy_cleanup(easy);
    if (status) {
        *status = st;
    }
    if (rc != CURLE_OK) {
        buf_free(&capb.body);
        if (err) {
            *err = xstrdup(curl_easy_strerror(rc));
        }
        return -1;
    }
    if (capb.truncated) {
        buf_puts(&capb.body, "\n[truncated]");
    }
    if (resp) {
        *resp = buf_detach(&capb.body);
    } else {
        buf_free(&capb.body);
    }
    return 0;
}
