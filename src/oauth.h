#ifndef BEIGE_OAUTH_H
#define BEIGE_OAUTH_H

#include "config.h"
#include "http.h"

typedef void (*OauthNote)(void *ud, const char *line);

int oauth_login_xai(Http *http, OauthNote note, void *ud, OAuthTok *out, char **err);
int oauth_login_openai(Http *http, OauthNote note, void *ud, OAuthTok *out, char **err);
int oauth_refresh_xai(Http *http, OAuthTok *tok, char **err);
int oauth_refresh_openai(Http *http, OAuthTok *tok, char **err);

/* Refresh when the access token is missing or inside five minutes of expiry. */
int oauth_ensure(Http *http, const char *provider, OAuthTok *tok, char **err);

#endif
