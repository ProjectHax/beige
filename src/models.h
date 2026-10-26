#ifndef BEIGE_MODELS_H
#define BEIGE_MODELS_H

#include "http.h"
#include "provider.h"

/* Append chat model ids from a list payload. Skips embeddings, speech, and images. */
int models_parse(const char *json, char ***ids, int *n, int *has_more, char **last_id);

void models_free(char **ids, int n);

/* GET the provider's model list. ids is malloc'd; caller frees with models_free. */
int models_fetch(Http *http, const Provider *provider, char ***ids, int *n, char **err);

#endif
