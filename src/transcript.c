#include "transcript.h"

static void block_clear(Block *b) {
    free(b->text);
    free(b->id);
    free(b->name);
    free(b->input_json);
    memset(b, 0, sizeof(*b));
}

void message_init(Message *m, MsgRole role) {
    m->role = role;
    m->blocks = NULL;
    m->nblocks = 0;
    m->cap = 0;
}

void message_free(Message *m) {
    int i;
    for (i = 0; i < m->nblocks; i++) {
        block_clear(&m->blocks[i]);
    }
    free(m->blocks);
    m->blocks = NULL;
    m->nblocks = 0;
    m->cap = 0;
}

static Block *message_push(Message *m) {
    Block *b;
    if (m->nblocks == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 4;
        m->blocks = xrealloc(m->blocks, (size_t)m->cap * sizeof(Block));
    }
    b = &m->blocks[m->nblocks++];
    memset(b, 0, sizeof(*b));
    return b;
}

void message_add_text(Message *m, const char *text) {
    Block *b = message_push(m);
    b->type = BLOCK_TEXT;
    b->text = str_utf8_dup(text);
}

void message_add_tool_use(Message *m, const char *id, const char *name, const char *input_json) {
    Block *b = message_push(m);
    b->type = BLOCK_TOOL_USE;
    b->id = xstrdup(id ? id : "");
    b->name = xstrdup(name ? name : "");
    b->input_json = xstrdup(input_json && input_json[0] ? input_json : "{}");
}

void message_add_tool_result(Message *m, const char *id, const char *text, int is_error) {
    Block *b = message_push(m);
    b->type = BLOCK_TOOL_RESULT;
    b->id = xstrdup(id ? id : "");
    /* Tool output is cut at byte caps and may be any encoding; a request must be valid UTF-8. */
    b->text = str_utf8_dup(text);
    b->is_error = is_error;
}

void transcript_init(Transcript *ts) {
    ts->msgs = NULL;
    ts->n = 0;
    ts->cap = 0;
}

void transcript_free(Transcript *ts) {
    int i;
    for (i = 0; i < ts->n; i++) {
        message_free(&ts->msgs[i]);
    }
    free(ts->msgs);
    ts->msgs = NULL;
    ts->n = 0;
    ts->cap = 0;
}

void transcript_add_message(Transcript *ts, Message *m) {
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 8;
        ts->msgs = xrealloc(ts->msgs, (size_t)ts->cap * sizeof(Message));
    }
    ts->msgs[ts->n++] = *m;
    m->blocks = NULL;
    m->nblocks = 0;
    m->cap = 0;
}

void transcript_add_user_text(Transcript *ts, const char *text) {
    Message m;
    message_init(&m, ROLE_USER);
    message_add_text(&m, text);
    transcript_add_message(ts, &m);
}

void transcript_add_tool_results(Transcript *ts, Message *assistant, char **results, int *errors, int n) {
    Message m;
    int i, k;
    message_init(&m, ROLE_USER);
    k = 0;
    for (i = 0; i < assistant->nblocks && k < n; i++) {
        if (assistant->blocks[i].type != BLOCK_TOOL_USE) {
            continue;
        }
        message_add_tool_result(&m, assistant->blocks[i].id, results[k], errors ? errors[k] : 0);
        k++;
    }
    transcript_add_message(ts, &m);
}

Message *transcript_add_assistant_from_stream(Transcript *ts, const StreamAcc *acc) {
    Message m;
    int i;
    message_init(&m, ROLE_ASSISTANT);
    if (acc->text.len > 0) {
        message_add_text(&m, acc->text.data);
    }
    for (i = 0; i < acc->ntools; i++) {
        const char *args = acc->tools[i].args.data;
        const char *id = acc->tools[i].id && acc->tools[i].id[0] ? acc->tools[i].id : acc->tools[i].item_id;
        message_add_tool_use(&m, id, acc->tools[i].name, args);
    }
    if (m.nblocks == 0) {
        message_add_text(&m, "");
    }
    transcript_add_message(ts, &m);
    return &ts->msgs[ts->n - 1];
}

static int has_tool_result(const Message *m) {
    int i;
    for (i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type == BLOCK_TOOL_RESULT) {
            return 1;
        }
    }
    return 0;
}

int transcript_safe_keep(const Transcript *ts, int keep) {
    if (keep < 0) {
        keep = 0;
    }
    if (keep > ts->n) {
        keep = ts->n;
    }
    while (keep > 0 && keep < ts->n && has_tool_result(&ts->msgs[ts->n - keep])) {
        keep++;
    }
    return keep;
}

void transcript_replace_with_summary(Transcript *ts, const char *summary, int keep) {
    Message *fresh;
    int n = ts->n;
    int start;
    int i, nkeep;
    keep = transcript_safe_keep(ts, keep);
    start = n - keep;
    nkeep = keep;
    fresh = xmalloc((size_t)(nkeep + 1) * sizeof(Message));
    message_init(&fresh[0], ROLE_USER);
    message_add_text(&fresh[0], summary ? summary : "");
    for (i = 0; i < nkeep; i++) {
        fresh[i + 1] = ts->msgs[start + i];
        ts->msgs[start + i].blocks = NULL;
        ts->msgs[start + i].nblocks = 0;
    }
    for (i = 0; i < start; i++) {
        message_free(&ts->msgs[i]);
    }
    free(ts->msgs);
    ts->msgs = fresh;
    ts->n = nkeep + 1;
    ts->cap = nkeep + 1;
}

size_t transcript_bytes(const Transcript *ts) {
    size_t n = 0;
    int i, j;
    for (i = 0; i < ts->n; i++) {
        for (j = 0; j < ts->msgs[i].nblocks; j++) {
            const Block *b = &ts->msgs[i].blocks[j];
            if (b->text) {
                n += strlen(b->text);
            }
            if (b->input_json) {
                n += strlen(b->input_json);
            }
            if (b->name) {
                n += strlen(b->name);
            }
        }
    }
    return n;
}

void stream_acc_init(StreamAcc *a) {
    memset(a, 0, sizeof(*a));
    buf_init(&a->text);
}

static void tool_acc_free(ToolAcc *t) {
    free(t->id);
    free(t->item_id);
    free(t->name);
    buf_free(&t->args);
}

void stream_acc_free(StreamAcc *a) {
    int i;
    buf_free(&a->text);
    for (i = 0; i < a->ntools; i++) {
        tool_acc_free(&a->tools[i]);
    }
    free(a->tools);
    free(a->stop_reason);
    free(a->error);
    free(a->response_id);
    memset(a, 0, sizeof(*a));
}

void stream_acc_drop_tools(StreamAcc *a) {
    int i;
    for (i = 0; i < a->ntools; i++) {
        tool_acc_free(&a->tools[i]);
    }
    a->ntools = 0;
}

void stream_acc_text(StreamAcc *a, const char *s, size_t n) {
    buf_append(&a->text, s, n);
}

ToolAcc *stream_acc_add_tool(StreamAcc *a, const char *id, const char *name) {
    ToolAcc *t;
    if (a->ntools == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->tools = xrealloc(a->tools, (size_t)a->cap * sizeof(ToolAcc));
    }
    t = &a->tools[a->ntools++];
    memset(t, 0, sizeof(*t));
    buf_init(&t->args);
    t->id = id ? xstrdup(id) : NULL;
    t->name = name ? xstrdup(name) : xstrdup("");
    t->index = -1;
    return t;
}

ToolAcc *stream_acc_find(StreamAcc *a, const char *id) {
    int i;
    if (!id) {
        return NULL;
    }
    for (i = 0; i < a->ntools; i++) {
        if (a->tools[i].id && str_eq(a->tools[i].id, id)) {
            return &a->tools[i];
        }
    }
    return NULL;
}

ToolAcc *stream_acc_find_item(StreamAcc *a, const char *item_id) {
    int i;
    if (!item_id) {
        return NULL;
    }
    for (i = 0; i < a->ntools; i++) {
        if (a->tools[i].item_id && str_eq(a->tools[i].item_id, item_id)) {
            return &a->tools[i];
        }
    }
    return NULL;
}

ToolAcc *stream_acc_find_index(StreamAcc *a, int index) {
    int i;
    for (i = 0; i < a->ntools; i++) {
        if (a->tools[i].index == index) {
            return &a->tools[i];
        }
    }
    return NULL;
}

void stream_acc_set_error(StreamAcc *a, const char *msg) {
    free(a->error);
    a->error = xstrdup(msg ? msg : "request failed");
}
