#ifndef BEIGE_CONTEXT_H
#define BEIGE_CONTEXT_H

#include "perm.h"
#include "util.h"

/* Walk parents for AGENTS.md / CLAUDE.md. global_path may be NULL. */
char *context_load(const char *cwd, const char *global_path, size_t per_file, size_t total);

char *system_prompt_build(const char *cwd, const char *instructions, PermMode mode);

#endif
