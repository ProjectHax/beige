#ifndef BEIGE_PERM_H
#define BEIGE_PERM_H

typedef enum {
    MODE_ASK = 0,
    MODE_PLAN = 1,
    MODE_AUTO = 2
} PermMode;

typedef enum {
    DEC_ALLOW = 0,
    DEC_DENY = 1,
    DEC_ASK = 2
} PermDecision;

typedef struct AlwaysRule {
    char *tool;
    char *prefix;
} AlwaysRule;

typedef struct PermState {
    PermMode mode;
    AlwaysRule *rules;
    int nrules;
    int cap;
    char *root;
    int sandbox;
} PermState;

void perm_init(PermState *ps, PermMode mode);
void perm_free(PermState *ps);
void perm_set_mode(PermState *ps, PermMode mode);
/* Root is the real working directory. Sandbox 0 skips the outside-cwd check. */
void perm_set_root(PermState *ps, const char *root, int sandbox);
const char *perm_mode_name(PermMode mode);
int perm_mode_parse(const char *s, PermMode *out);

/* subject is the shell command, the file path, or the URL. */
PermDecision perm_decide(PermState *ps, const char *tool, const char *subject);

/* Remember a session allow. prefix NULL allows the whole tool. */
void perm_allow_prefix(PermState *ps, const char *tool, const char *prefix);
char *perm_bash_prefix(const char *command);

int perm_is_catastrophic(const char *command);
int perm_plan_bash_allow(const char *command);

/* Plan mode may write only this relative path. */
int perm_is_plan_file(const char *path);

#endif
