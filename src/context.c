#include "context.h"

#include <sys/stat.h>

static int is_git_root(const char *dir) {
    char *marker = path_join(dir, ".git");
    struct stat st;
    int ok = stat(marker, &st) == 0;
    free(marker);
    return ok;
}

static char *read_capped(const char *path, size_t cap, int *truncated) {
    return read_file_cap(path, cap, NULL, truncated, NULL);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *context_load(const char *cwd, const char *global_path, size_t per_file, size_t total) {
    char **paths = NULL;
    int n = 0, cap = 0;
    char *dir;
    char *stop = NULL;
    Buf out;
    size_t used = 0;
    int i;
    if (per_file == 0) {
        per_file = 32 * 1024;
    }
    if (total == 0) {
        total = 64 * 1024;
    }
    buf_init(&out);
    if (global_path && file_exists(global_path)) {
        if (n == cap) {
            cap = 8;
            paths = xrealloc(paths, (size_t)cap * sizeof(char *));
        }
        paths[n++] = xstrdup(global_path);
    }
    dir = xstrdup(cwd && cwd[0] ? cwd : ".");
    for (;;) {
        char *agents = path_join(dir, "AGENTS.md");
        char *claude = path_join(dir, "CLAUDE.md");
        char *chosen = NULL;
        int git = is_git_root(dir);
        if (file_exists(agents)) {
            chosen = agents;
            free(claude);
        } else if (file_exists(claude)) {
            chosen = claude;
            free(agents);
        } else {
            free(agents);
            free(claude);
        }
        if (chosen) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                paths = xrealloc(paths, (size_t)cap * sizeof(char *));
            }
            paths[n++] = chosen;
        }
        if (git) {
            stop = xstrdup(dir);
            break;
        }
        {
            char *parent = path_dirname(dir);
            if (str_eq(parent, dir)) {
                free(parent);
                break;
            }
            free(dir);
            dir = parent;
        }
    }
    free(dir);
    free(stop);
    /* paths[0] may be global. The walk stored cwd first, then parents.
       Emit global, then farthest parent, then closer ones. */
    if (n > 0 && global_path && str_eq(paths[0], global_path)) {
        int trunc = 0;
        char *text = read_capped(paths[0], per_file, &trunc);
        if (text && used < total) {
            buf_printf(&out, "Instructions from: %s\n", paths[0]);
            if (strlen(text) > total - used) {
                text[total - used] = 0;
                trunc = 1;
            }
            buf_puts(&out, text);
            buf_puts(&out, "\n\n");
            used = out.len;
            if (trunc) {
                buf_puts(&out, "[truncated]\n\n");
            }
        }
        free(text);
        i = n - 1;
    } else {
        i = n - 1;
    }
    for (; i >= (global_path && n > 0 && str_eq(paths[0], global_path) ? 1 : 0); i--) {
        int trunc = 0;
        char *text;
        if (used >= total) {
            break;
        }
        text = read_capped(paths[i], per_file, &trunc);
        if (!text) {
            continue;
        }
        buf_printf(&out, "Instructions from: %s\n", paths[i]);
        if (strlen(text) > total - used) {
            text[total - used] = 0;
            trunc = 1;
        }
        buf_puts(&out, text);
        buf_putc(&out, '\n');
        if (trunc) {
            buf_puts(&out, "[truncated]\n");
        }
        buf_putc(&out, '\n');
        used = out.len;
        free(text);
    }
    for (i = 0; i < n; i++) {
        free(paths[i]);
    }
    free(paths);
    return buf_detach(&out);
}

/* Coding instructions adapted from OpenCode's gemini prompt (MIT). See NOTICE. */
static const char PROMPT_CODING[] =
    "You are beige, an interactive terminal agent for software engineering tasks. Help the user "
    "safely and efficiently, using the tools you have.\n"
    "\n"
    "Tools:\n"
    "- read: read a text file. Use offset and limit for long files.\n"
    "- grep: search file contents. This is how you find symbols and patterns.\n"
    "- ls: list one directory.\n"
    "- edit: replace one exact occurrence of old_string with new_string. old_string must match "
    "the file exactly once. Prefer edit over write for existing files.\n"
    "- write: create or overwrite a file with its full contents.\n"
    "- bash: run a command with /bin/sh -c. stdout and stderr come back together. The command "
    "runs in the foreground until it exits. Do not start a process that never exits unless the "
    "user asked; it will be cancelled or truncated.\n"
    "- fetch: HTTP GET. Use it for documentation and URLs the user gave you. Do not invent URLs. "
    "In ask mode this waits for approval.\n"
    "- search: web search. Use it when the answer is not in the tree. Pass a short query. "
    "In ask mode this waits for approval. Do not invent results.\n"
    "\n"
    "Paths may be relative to the working directory or absolute. Stay inside the working "
    "directory. Do not invent file contents. Read a file before you edit it.\n"
    "\n"
    "# Core mandates\n"
    "\n"
    "- Conventions: follow the project's existing style. Read surrounding code, tests, and "
    "configuration before you change anything.\n"
    "- Libraries: never assume a library is available. Check imports and the build files "
    "(Makefile, configure, package.json, Cargo.toml, requirements.txt, and neighbors) first.\n"
    "- Style: match the naming, structure, types, and architecture already in the tree.\n"
    "- Idioms: when you edit, keep the local imports and types so the change fits the file.\n"
    "- Comments: add them rarely, and only to say why. Do not narrate the change in comments, "
    "and do not touch comments you are not changing.\n"
    "- Scope: do the requested work, including the small follow-up that it implies. Do not "
    "expand into a different task without asking. If the user asks how to do something, explain "
    "first.\n"
    "- After a successful edit, do not add a long summary unless the user asked.\n"
    "- Do not revert work unless the user asks, or unless your own edit is what broke the build.\n"
    "\n"
    "# Software engineering\n"
    "\n"
    "For a bug fix, feature, refactor, or explanation:\n"
    "1. Understand. Use grep and ls, then read, until the request matches the code you have seen.\n"
    "2. Plan. Keep the plan short. If the project has tests, use them as the check that you are "
    "done.\n"
    "3. Implement with edit, write, and bash. Stay with the conventions above.\n"
    "4. Verify. Find the project's real test, build, and lint commands from the README, Makefile, "
    "or build files. Do not assume npm test, make, or a compiler flag. Run the ones this tree "
    "actually uses.\n"
    "5. If a tool fails, read the error and change the call. Keep going until the request is "
    "done or you are blocked.\n"
    "\n"
    "For a new program, state the shape and the tools you will use, then implement it and build "
    "it. On an old system the toolchain may be make, a compiler, or a shell script that is "
    "already there. Do not download a new stack unless the user asked.\n"
    "\n"
    "# How to talk\n"
    "\n"
    "- Be direct. A short answer is enough when the task is small.\n"
    "- Skip filler such as \"I will now\" and \"I have finished\".\n"
    "- Use tools to act. Use text to explain.\n"
    "- GitHub-flavored Markdown is fine. The user is on a fixed-width terminal.\n"
    "- If you cannot do something, say so in a sentence and offer the closest alternative.\n"
    "\n"
    "# Safety\n"
    "\n"
    "- Do not write code that logs, commits, or prints secrets, keys, or tokens.\n"
    "- Before a bash command that deletes, overwrites, or changes system state, say in one line "
    "what it will do.\n"
    "- Avoid interactive commands (git rebase -i, bare npm init). Use the non-interactive form.\n"
    "- If the user denies a tool call, do not repeat it unless they ask for that action again.\n"
    "\n"
    "# Examples\n"
    "\n"
    "User: list the files here.\n"
    "Action: ls on the working directory.\n"
    "\n"
    "User: refactor auth in src/auth.py to use requests instead of urllib.\n"
    "Action: grep and read the file and its tests, confirm requests is already a dependency, "
    "edit the file, then run the project's test command.\n"
    "\n"
    "User: where is the profile update handled?\n"
    "Action: grep for the symbol, then read the file you found, then answer from that text.\n";

char *system_prompt_build(const char *cwd, const char *instructions, PermMode mode) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, PROMPT_CODING);
    buf_putc(&b, '\n');
    buf_printf(&b, "Working directory: %s\n", cwd ? cwd : ".");
    if (mode == MODE_PLAN) {
        buf_puts(&b,
                 "\nPlan mode is on. You are read-only. Do not edit or write source files, and do "
                 "not use the shell to modify files. Read, search, and prefer read-only commands. "
                 "Write the plan only to .beige/plan.md, then stop for the user.\n");
    } else if (mode == MODE_ASK) {
        buf_puts(&b,
                 "\nAsk mode is on. read, grep, and ls run immediately. write, edit, bash, fetch, "
                 "and search wait for the user's approval. Do not ask for that approval in prose; "
                 "call the tool and the user will see the prompt.\n");
    } else if (mode == MODE_AUTO) {
        buf_puts(&b,
                 "\nAuto mode is on. Tools run without a prompt, except a short list of "
                 "destructive commands that still ask. Still explain a destructive bash command "
                 "in one line before you call it.\n");
    }
    if (instructions && instructions[0]) {
        buf_puts(&b, "\n# Project instructions\n\n");
        buf_puts(&b, instructions);
    }
    return buf_detach(&b);
}
