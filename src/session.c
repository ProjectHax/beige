#include "session.h"

#include "cJSON.h"

#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

static char *session_dir(const char *cwd) {
    char *real = realpath_alloc(cwd);
    char *home = beige_home();
    char *base;
    char *dir;
    char name[32];
    unsigned long h = str_hash(real ? real : cwd);
    snprintf(name, sizeof name, "%08lx", h);
    base = path_join(home, "sessions");
    dir = path_join(base, name);
    free(base);
    free(home);
    free(real);
    if (mkdir_p(dir, 0700) != 0) {
        free(dir);
        return NULL;
    }
    return dir;
}

static cJSON *block_encode(const Block *b) {
    cJSON *o = cJSON_CreateObject();
    if (b->type == BLOCK_TEXT) {
        cJSON_AddStringToObject(o, "type", "text");
        cJSON_AddStringToObject(o, "text", b->text ? b->text : "");
    } else if (b->type == BLOCK_TOOL_USE) {
        cJSON_AddStringToObject(o, "type", "tool_use");
        cJSON_AddStringToObject(o, "id", b->id ? b->id : "");
        cJSON_AddStringToObject(o, "name", b->name ? b->name : "");
        cJSON_AddStringToObject(o, "input", b->input_json ? b->input_json : "{}");
    } else if (b->type == BLOCK_TOOL_RESULT) {
        cJSON_AddStringToObject(o, "type", "tool_result");
        cJSON_AddStringToObject(o, "id", b->id ? b->id : "");
        cJSON_AddStringToObject(o, "text", b->text ? b->text : "");
        cJSON_AddBoolToObject(o, "error", b->is_error);
    }
    return o;
}

static char *message_line(const Message *m) {
    cJSON *o = cJSON_CreateObject();
    cJSON *blocks = cJSON_CreateArray();
    char *printed;
    char *line;
    int i;
    cJSON_AddStringToObject(o, "role", m->role == ROLE_ASSISTANT ? "assistant" : "user");
    for (i = 0; i < m->nblocks; i++) {
        cJSON_AddItemToArray(blocks, block_encode(&m->blocks[i]));
    }
    cJSON_AddItemToObject(o, "blocks", blocks);
    printed = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!printed) {
        return NULL;
    }
    line = xmalloc(strlen(printed) + 2);
    sprintf(line, "%s\n", printed);
    cJSON_free(printed);
    return line;
}

static int message_decode(Message *m, cJSON *o) {
    cJSON *role = cJSON_GetObjectItemCaseSensitive(o, "role");
    cJSON *blocks = cJSON_GetObjectItemCaseSensitive(o, "blocks");
    int i, n;
    if (!cJSON_IsString(role) || !cJSON_IsArray(blocks)) {
        return -1;
    }
    message_init(m, str_eq(role->valuestring, "assistant") ? ROLE_ASSISTANT : ROLE_USER);
    n = cJSON_GetArraySize(blocks);
    for (i = 0; i < n; i++) {
        cJSON *b = cJSON_GetArrayItem(blocks, i);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(b, "type");
        if (!cJSON_IsString(type)) {
            continue;
        }
        if (str_eq(type->valuestring, "text")) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(b, "text");
            message_add_text(m, cJSON_IsString(text) ? text->valuestring : "");
        } else if (str_eq(type->valuestring, "tool_use")) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(b, "name");
            cJSON *input = cJSON_GetObjectItemCaseSensitive(b, "input");
            message_add_tool_use(m, cJSON_IsString(id) ? id->valuestring : "",
                                 cJSON_IsString(name) ? name->valuestring : "",
                                 cJSON_IsString(input) ? input->valuestring : "{}");
        } else if (str_eq(type->valuestring, "tool_result")) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(b, "text");
            cJSON *err = cJSON_GetObjectItemCaseSensitive(b, "error");
            message_add_tool_result(m, cJSON_IsString(id) ? id->valuestring : "",
                                    cJSON_IsString(text) ? text->valuestring : "", cJSON_IsTrue(err));
        }
    }
    return 0;
}

Session *session_begin(const char *cwd) {
    char *dir = session_dir(cwd);
    char name[64];
    Session *s;
    if (!dir) {
        return NULL;
    }
    snprintf(name, sizeof name, "%lld-%d.jsonl", (long long)time(NULL), (int)getpid());
    s = xmalloc(sizeof *s);
    s->path = path_join(dir, name);
    free(dir);
    /* The file is created by the first message, so a run that says nothing leaves no empty session behind. */
    s->fp = NULL;
    return s;
}

static int read_line(FILE *fp, Buf *line) {
    int c;
    buf_clear(line);
    c = fgetc(fp);
    if (c == EOF) {
        return 0;
    }
    while (c != EOF && c != '\n') {
        char ch = (char)c;
        buf_append(line, &ch, 1);
        if (line->len > 8 * 1024 * 1024) {
            break;
        }
        c = fgetc(fp);
    }
    return 1;
}

static int load_file(const char *path, Transcript *ts) {
    FILE *fp = fopen(path, "r");
    Buf line;
    if (!fp) {
        return -1;
    }
    buf_init(&line);
    while (read_line(fp, &line)) {
        cJSON *o;
        Message m;
        if (!line.data || !line.data[0]) {
            continue;
        }
        o = cJSON_Parse(line.data);
        if (!o) {
            continue;
        }
        if (message_decode(&m, o) == 0) {
            transcript_add_message(ts, &m);
        }
        cJSON_Delete(o);
    }
    buf_free(&line);
    fclose(fp);
    return 0;
}

Session *session_resume(const char *cwd, Transcript *ts, char **err) {
    char *dir = session_dir(cwd);
    DIR *d;
    struct dirent *de;
    char *best = NULL;
    time_t best_m = 0;
    Session *s;
    if (err) {
        *err = NULL;
    }
    if (!dir) {
        if (err) {
            *err = xstrdup("cannot open session directory");
        }
        return NULL;
    }
    d = opendir(dir);
    if (!d) {
        free(dir);
        if (err) {
            *err = xstrdup("no sessions");
        }
        return NULL;
    }
    while ((de = readdir(d)) != NULL) {
        char *path;
        struct stat st;
        size_t len = strlen(de->d_name);
        if (len < 6 || !str_eq(de->d_name + len - 6, ".jsonl")) {
            continue;
        }
        path = path_join(dir, de->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_mtime >= best_m) {
            free(best);
            best = path;
            best_m = st.st_mtime;
        } else {
            free(path);
        }
    }
    closedir(d);
    if (!best) {
        free(dir);
        if (err) {
            *err = xstrdup("no session to resume");
        }
        return NULL;
    }
    if (load_file(best, ts) != 0) {
        free(best);
        free(dir);
        if (err) {
            *err = xstrdup("cannot read session");
        }
        return NULL;
    }
    s = xmalloc(sizeof *s);
    s->path = best;
    s->fp = fopen(s->path, "a");
    free(dir);
    if (!s->fp) {
        free(s->path);
        free(s);
        if (err) {
            *err = xstrdup("cannot append session");
        }
        return NULL;
    }
    return s;
}

void session_close(Session *s) {
    if (!s) {
        return;
    }
    if (s->fp) {
        fclose(s->fp);
    }
    free(s->path);
    free(s);
}

int session_write_message(Session *s, const Message *m) {
    char *line;
    if (!s) {
        return 0;
    }
    if (!s->fp) {
        s->fp = fopen(s->path, "a");
        if (!s->fp) {
            return -1;
        }
    }
    line = message_line(m);
    if (!line) {
        return -1;
    }
    if (fputs(line, s->fp) == EOF) {
        free(line);
        return -1;
    }
    fflush(s->fp);
    free(line);
    return 0;
}

int session_write_transcript(Session *s, const Transcript *ts) {
    int i;
    for (i = 0; i < ts->n; i++) {
        if (session_write_message(s, &ts->msgs[i]) != 0) {
            return -1;
        }
    }
    return 0;
}
