#ifndef BEIGE_APP_H
#define BEIGE_APP_H

#include "config.h"
#include "context.h"
#include "provider.h"
#include "session.h"
#include "tui.h"

typedef struct App {
    Config cfg;
    Auth auth;
    Transcript ts;
    PermState perm;
    Provider *provider;
    Http http;
    Session *session;
    Ui *ui;
    char *cwd;
    char *root;
    char *system;
    int print_mode;
    int no_context;
    int resume;
    int max_steps;
    int context_budget;
    char *base_override;
    char *key_override;
    char *model_override;
    char *provider_override;
    char *last_fp;
    int tool_streak;
    /* Token estimate right after the last compaction; the next waits for growth past it. */
    unsigned long compact_floor;
    /* config.json values that --provider, --model, and --mode stand in for until changed in the session. */
    char *file_provider;
    char *file_model;
    char *file_mode;
    int mode_from_cli;
} App;

void app_init(App *app);
void app_shutdown(App *app);
int app_prepare(App *app, char **err);
/* Save the config, keeping one-run command-line choices out of config.json. */
int app_save_config(App *app);
int app_bind_provider(App *app, char **err);
/* Open one provider without replacing the session's current provider. */
int app_open_provider(App *app, const char *id, Provider **out, char **err);
void app_rebuild_system(App *app);
void app_refresh_status(App *app);
char *app_short_cwd(const App *app);
void app_log_tool(App *app, const Block *use, const Block *result);
int app_turn(App *app, const char *user_text);
int app_compact(App *app);
int app_init_agents(App *app);

#endif
