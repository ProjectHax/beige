#ifndef BEIGE_TUI_H
#define BEIGE_TUI_H

#include "http.h"

typedef struct Ui Ui;

typedef enum {
    UI_AUTO = 0,
    UI_PRINT = 1,
    UI_LINE = 2,
    UI_SCREEN = 3
} UiMode;

typedef enum {
    UI_USER = 1,
    UI_ASSIST = 2,
    UI_TOOL = 3,
    UI_META = 4,
    UI_ERR = 5
} UiKind;

enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_CTRL_D,
    KEY_CTRL_L,
    KEY_CTRL_T,
    KEY_CTRL_U,
    KEY_CTRL_K,
    KEY_CTRL_A,
    KEY_CTRL_E,
    KEY_CTRL_W,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_DELETE,
    KEY_ESC,
    KEY_TAB,
    KEY_CTRL_X,
    KEY_SHIFT_TAB,
    KEY_ALT_ENTER,
    KEY_CTRL_O
};

enum { UI_TOOL_OK = 0, UI_TOOL_RUN = 1, UI_TOOL_ERR = 2 };

typedef struct UiStatus {
    const char *mode;
    const char *provider;
    const char *model;
    const char *cwd;
    unsigned long tokens;
    unsigned long budget;
} UiStatus;

typedef struct UiAsk {
    const char *title;
    const char *what;
    const char *body;
    const char *always;
} UiAsk;

typedef struct UiResult {
    int state;
    const char *summary;
    const char *body;
    int diff;
    int peek;
} UiResult;

typedef int (*UiKeyFn)(void *ud, int key_kind);

typedef struct UiChoice {
    const char *label;
    const char *hint;
} UiChoice;

Ui *ui_open(int mode);
void ui_close(Ui *ui);
int ui_is_screen(const Ui *ui);
int ui_is_print(const Ui *ui);

void ui_set_status(Ui *ui, const UiStatus *st);
void ui_set_glyphs(Ui *ui, const char *pref);
void ui_welcome(Ui *ui, const char *version, const char *cwd, const char *warning);
void ui_set_key_fn(Ui *ui, UiKeyFn fn, void *ud);

void ui_log(Ui *ui, int kind, const char *text);
/* Screen log only. mark is the current length; trim drops lines from mark on. */
int ui_log_mark(const Ui *ui);
void ui_log_trim(Ui *ui, int mark);
void ui_stream_begin(Ui *ui);
void ui_stream_append(Ui *ui, const char *s, size_t n);
void ui_stream_end(Ui *ui);

/* Screen only. label is "thinking" or "compacting"; NULL clears. */
void ui_activity(Ui *ui, const char *label);

/* Run a shell command. On a screen it stays in the background. */
int ui_shell(Ui *ui, const char *cmd);
int ui_wants_quit(const Ui *ui);

int ui_tool_begin(Ui *ui, const char *title);
void ui_tool_end(Ui *ui, int id, const UiResult *result);
int ui_confirm(Ui *ui, const UiAsk *ask);

/* Commands offered while the input starts with /. labels must outlive the UI. */
void ui_set_choices(Ui *ui, const UiChoice *choices, int n);

/*
 * Blocking list. Returns the selected index, or -1 if cancelled.
 * selected is the initial highlight, or 0 when negative.
 */
int ui_menu(Ui *ui, const char *title, const char *const *labels, int n, int selected);

/* One line. hidden prints stars instead of the characters. NULL on cancel. */
char *ui_read_line(Ui *ui, const char *prompt, int hidden);
void ui_hooks(Ui *ui, IoHooks *hooks);
char *ui_take_followup(Ui *ui);
char *ui_next_line(Ui *ui, int *quit);
void ui_present(Ui *ui);

#endif
