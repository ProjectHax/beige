#ifndef BEIGE_CONFIG_H
#define BEIGE_CONFIG_H

#include "util.h"

typedef struct OAuthTok {
    char *access;
    char *refresh;
    char *account_id;
    long long expires_at;
} OAuthTok;

typedef struct Config {
    char *provider;
    char *model;
    char *mode;
    int max_tokens;
    int max_steps;
    int context_budget;
    int sandbox;
    size_t read_cap;
    size_t output_cap;
    int shell_timeout;
    char *anthropic_base;
    char *openai_base;
    char *openai_wire;
    char *xai_base;
    char *openai_sub_base;
    char *originator;
    char *ca_bundle;
    char *glyphs;
} Config;

typedef struct Auth {
    char *anthropic_key;
    char *openai_key;
    char *xai_key;
    OAuthTok xai_sub;
    OAuthTok openai_sub;
} Auth;

void config_init(Config *c);
void config_free(Config *c);
void config_load(Config *c);
int config_save(const Config *c);

void auth_init(Auth *a);
void auth_free(Auth *a);
void auth_load(Auth *a);
int auth_save(const Auth *a);

/* Env vars win over the file. The pointer is not owned by the caller. */
const char *auth_api_key(const Auth *a, const char *provider);
OAuthTok *auth_oauth(Auth *a, const char *provider);

void auth_set_key(Auth *a, const char *provider, const char *key);
void oauth_tok_set(OAuthTok *t, const char *access, const char *refresh, const char *account, long long exp);
void oauth_tok_clear(OAuthTok *t);

#endif
