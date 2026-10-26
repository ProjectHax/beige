#ifndef BEIGE_TOOLS_H
#define BEIGE_TOOLS_H

#include "http.h"

#include "cJSON.h"

typedef struct ToolIn {
    const char *root;
    int sandbox;
    size_t read_cap;
    size_t output_cap;
    int shell_timeout;
    cJSON *args;
    volatile sig_atomic_t *cancel;
    Http *http;
    const IoHooks *hooks;
} ToolIn;

typedef struct ToolDef {
    const char *name;
    const char *description;
    const char *parameters_json;
} ToolDef;

const ToolDef *tool_defs(int *count);
const ToolDef *tool_by_name(const char *name);

/* Subject string for the permission check (command, path, or URL). */
char *tool_subject(const char *name, cJSON *args, const char *root, int sandbox);

/* Human-readable preview shown before an ask prompt. */
char *tool_preview(const char *name, cJSON *args, const char *root, int sandbox);

/* "Bash(echo hi)" for the tool bullet. Caller frees. */
char *tool_title(const char *name, cJSON *args);

/* One-line elbow summary. Caller frees. */
char *tool_summary(const char *name, cJSON *args, const char *result, int is_error);

/*
 * Run a tool. On return *text is the model-facing result.
 * *is_error is 1 when the tool failed. Returns -1 only when *text could not
 * be produced.
 */
int tool_run(const char *name, ToolIn *in, char **text, int *is_error);

#endif
