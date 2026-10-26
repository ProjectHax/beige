#ifndef BEIGE_TRANSCRIPT_H
#define BEIGE_TRANSCRIPT_H

#include "util.h"

typedef enum {
    BLOCK_TEXT = 0,
    BLOCK_TOOL_USE = 1,
    BLOCK_TOOL_RESULT = 2
} BlockType;

typedef enum {
    ROLE_USER = 0,
    ROLE_ASSISTANT = 1
} MsgRole;

typedef struct Block {
    BlockType type;
    char *text;
    char *id;
    char *name;
    char *input_json;
    int is_error;
} Block;

typedef struct Message {
    MsgRole role;
    Block *blocks;
    int nblocks;
    int cap;
} Message;

typedef struct Transcript {
    Message *msgs;
    int n;
    int cap;
} Transcript;

typedef struct ToolAcc {
    char *id;
    char *item_id;
    char *name;
    Buf args;
    int index;
} ToolAcc;

typedef struct StreamAcc {
    Buf text;
    ToolAcc *tools;
    int ntools;
    int cap;
    char *stop_reason;
    char *error;
    char *response_id;
    long input_tokens;
    long output_tokens;
} StreamAcc;

void transcript_init(Transcript *ts);
void transcript_free(Transcript *ts);
void message_init(Message *m, MsgRole role);
void message_free(Message *m);
void message_add_text(Message *m, const char *text);
void message_add_tool_use(Message *m, const char *id, const char *name, const char *input_json);
void message_add_tool_result(Message *m, const char *id, const char *text, int is_error);

void transcript_add_message(Transcript *ts, Message *m); /* takes ownership of blocks */
void transcript_add_user_text(Transcript *ts, const char *text);
void transcript_add_tool_results(Transcript *ts, Message *assistant_with_uses,
                                 char **results, int *errors, int n);
Message *transcript_add_assistant_from_stream(Transcript *ts, const StreamAcc *acc);

/* keep, grown until the first kept message is not a tool result whose tool call would be dropped. */
int transcript_safe_keep(const Transcript *ts, int keep);
/* Drop everything except the last keep messages (see transcript_safe_keep) and insert a summary user turn. */
void transcript_replace_with_summary(Transcript *ts, const char *summary, int keep);

size_t transcript_bytes(const Transcript *ts);

void stream_acc_init(StreamAcc *a);
void stream_acc_free(StreamAcc *a);
void stream_acc_text(StreamAcc *a, const char *s, size_t n);
/* Forget the tool calls of a reply that was cut off; they would have no results. */
void stream_acc_drop_tools(StreamAcc *a);
ToolAcc *stream_acc_add_tool(StreamAcc *a, const char *id, const char *name);
ToolAcc *stream_acc_find(StreamAcc *a, const char *id);
ToolAcc *stream_acc_find_item(StreamAcc *a, const char *item_id);
ToolAcc *stream_acc_find_index(StreamAcc *a, int index);
void stream_acc_set_error(StreamAcc *a, const char *msg);

#endif
