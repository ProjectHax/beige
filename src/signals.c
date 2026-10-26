#include "signals.h"

#include <string.h>
#include <unistd.h>

volatile sig_atomic_t g_sigint = 0;
volatile sig_atomic_t g_winch = 0;
static volatile sig_atomic_t g_job_pgid = 0;
static struct termios g_tty;
static volatile sig_atomic_t g_tty_saved = 0;

void signals_set_job(int pid) {
    g_job_pgid = pid > 1 ? pid : 0;
}

void signals_set_tty(const struct termios *t) {
    g_tty_saved = 0;
    if (t) {
        g_tty = *t;
        g_tty_saved = 1;
    }
}

static void kill_job(void) {
    if (g_job_pgid > 1) {
        kill(-g_job_pgid, SIGKILL);
    }
}

/* tcsetattr is async-signal-safe, so a hard exit can still hand back a cooked, echoing terminal. */
static void restore_tty(void) {
    if (g_tty_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty);
    }
}

static void on_int(int sig) {
    const char reset[] = "\033[0m\033[?2004l\033[?1049l\r\n";
    (void)sig;
    if (g_sigint) {
        kill_job();
        restore_tty();
        if (write(STDOUT_FILENO, reset, sizeof reset - 1) < 0) {
            /* best effort */
        }
        _exit(130);
    }
    g_sigint = 1;
}

#ifdef SIGWINCH
static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}
#endif

static void on_term(int sig) {
    const char reset[] = "\033[0m\033[?2004l\033[?1049l\r\n";
    kill_job();
    restore_tty();
    if (write(STDOUT_FILENO, reset, sizeof reset - 1) < 0) {
        /* The terminal is already going away. */
    }
    _exit(128 + sig);
}

void signals_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_int;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#ifdef SIGWINCH
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
#endif
}
