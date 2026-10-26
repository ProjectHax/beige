#ifndef BEIGE_SESSION_H
#define BEIGE_SESSION_H

#include "transcript.h"

typedef struct Session {
    char *path;
    FILE *fp;
} Session;

Session *session_begin(const char *cwd);
/* Loads the newest session for this directory into ts. */
Session *session_resume(const char *cwd, Transcript *ts, char **err);
void session_close(Session *s);
int session_write_message(Session *s, const Message *m);
int session_write_transcript(Session *s, const Transcript *ts);

#endif
