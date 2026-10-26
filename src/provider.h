#ifndef BEIGE_PROVIDER_H
#define BEIGE_PROVIDER_H

#include "http.h"
#include "transcript.h"

typedef enum {
    WIRE_ANTHROPIC = 1,
    WIRE_RESPONSES = 2,
    WIRE_CHAT = 3
} WireKind;

typedef struct Provider {
    char *id;
    char *model;
    char *url;
    char *api_key;
    char *account_id;
    char *originator;
    WireKind wire;
    int max_tokens;
    int subscription;
} Provider;

typedef struct ProviderSpec {
    const char *id;
    const char *model;
    const char *base_url;
    const char *api_key;
    const char *account_id;
    const char *originator;
    const char *wire_name;
    int max_tokens;
} ProviderSpec;

int provider_is_known(const char *id);
const char *provider_default_model(const char *id);

Provider *provider_open(const ProviderSpec *spec, char **err);
void provider_free(Provider *p);

char *provider_build_body(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools);
struct curl_slist *provider_headers(const Provider *p);

void *provider_parse_new(const Provider *p);
void provider_parse_free(const Provider *p, void *state);
int provider_parse_event(const Provider *p, void *state, StreamAcc *acc, const char *event, const char *data);

#endif
