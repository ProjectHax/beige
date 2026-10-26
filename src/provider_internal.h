#ifndef BEIGE_PROVIDER_INTERNAL_H
#define BEIGE_PROVIDER_INTERNAL_H

#include "provider.h"

#include "cJSON.h"

char *beige_json_print(cJSON *root);

char *anthropic_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools);
void *anthropic_state_new(void);
void anthropic_state_free(void *state);
int anthropic_parse(void *state, StreamAcc *acc, const char *event, const char *data);

char *openai_responses_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools);
char *openai_chat_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools);
void *chat_state_new(void);
void chat_state_free(void *state);
int openai_responses_parse(void *state, StreamAcc *acc, const char *event, const char *data);
int openai_chat_parse(void *state, StreamAcc *acc, const char *event, const char *data);

#endif
