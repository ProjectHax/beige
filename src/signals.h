#ifndef BEIGE_SIGNALS_H
#define BEIGE_SIGNALS_H

#include <signal.h>
#include <termios.h>

extern volatile sig_atomic_t g_sigint;
extern volatile sig_atomic_t g_winch;

void signals_install(void);
/* Remember a background process group so a hard exit can kill it. 0 clears. */
void signals_set_job(int pid);
/* Terminal settings a hard exit (second Ctrl-C, SIGTERM, SIGHUP) puts back. NULL clears. */
void signals_set_tty(const struct termios *t);

#endif
