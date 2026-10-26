#ifndef BEIGE_HTTP_H
#define BEIGE_HTTP_H

#include "util.h"

#include <curl/curl.h>
#include <signal.h>
#include <sys/select.h>

typedef struct Http {
    char *ca_bundle;
    volatile sig_atomic_t *cancel;
} Http;

typedef struct IoHooks {
    void (*prepare)(void *ud, fd_set *r, fd_set *w, fd_set *e, int *maxfd);
    void (*ready)(void *ud, fd_set *r, fd_set *w, fd_set *e);
    /* Called on every wait, including a timeout, so the screen can animate. */
    void (*tick)(void *ud);
    void *ud;
} IoHooks;

typedef void (*SseFn)(const char *event, const char *data, void *ud);

void http_init(void);
void http_cleanup(void);

struct curl_slist *http_hdr(struct curl_slist *list, const char *line);

/*
 * POST body and read a server-sent event stream.
 * Returns 0 on a finished request (check *status). Returns -1 on transport
 * failure or cancel; *err is set.
 */
int http_post_sse(Http *http, const char *url, struct curl_slist *headers, const char *body,
                  SseFn fn, void *fn_ud, const IoHooks *hooks, long *status, char **err);

/* method is "GET" or "POST". *resp is the body, truncated to cap. */
int http_exchange(Http *http, const char *method, const char *url, struct curl_slist *headers,
                  const char *body, long *status, char **resp, size_t cap, char **err);

#endif
