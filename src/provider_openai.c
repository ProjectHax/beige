#include "provider_internal.h"

#include "tools.h"

#include <string.h>

void *chat_state_new(void) {
    return NULL;
}

void chat_state_free(void *state) {
    (void)state;
}

static cJSON *schema_of(const ToolDef *def) {
    cJSON *schema = cJSON_Parse(def->parameters_json);
    if (!schema) {
        schema = cJSON_CreateObject();
    }
    return schema;
}

static cJSON *tools_responses(void) {
    cJSON *arr = cJSON_CreateArray();
    int n = 0, i;
    const ToolDef *defs = tool_defs(&n);
    for (i = 0; i < n; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(tool, "name", defs[i].name);
        cJSON_AddStringToObject(tool, "description", defs[i].description);
        cJSON_AddItemToObject(tool, "parameters", schema_of(&defs[i]));
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

static cJSON *tools_chat(void) {
    cJSON *arr = cJSON_CreateArray();
    int n = 0, i;
    const ToolDef *defs = tool_defs(&n);
    for (i = 0; i < n; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", defs[i].name);
        cJSON_AddStringToObject(fn, "description", defs[i].description);
        cJSON_AddItemToObject(fn, "parameters", schema_of(&defs[i]));
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

static void add_user_text_parts(cJSON *content, const Message *m) {
    int j;
    for (j = 0; j < m->nblocks; j++) {
        if (m->blocks[j].type == BLOCK_TEXT && m->blocks[j].text && m->blocks[j].text[0]) {
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "type", "input_text");
            cJSON_AddStringToObject(part, "text", m->blocks[j].text);
            cJSON_AddItemToArray(content, part);
        }
    }
}

char *openai_responses_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    char *printed;
    int i;
    cJSON_AddStringToObject(root, "model", p->model);
    cJSON_AddBoolToObject(root, "stream", 1);
    cJSON_AddNumberToObject(root, "max_output_tokens", p->max_tokens);
    if (system_prompt && system_prompt[0]) {
        cJSON_AddStringToObject(root, "instructions", system_prompt);
    }
    for (i = 0; i < ts->n; i++) {
        const Message *m = &ts->msgs[i];
        int j;
        if (m->role == ROLE_USER) {
            cJSON *content = cJSON_CreateArray();
            add_user_text_parts(content, m);
            if (cJSON_GetArraySize(content) > 0) {
                cJSON *msg = cJSON_CreateObject();
                cJSON_AddStringToObject(msg, "role", "user");
                cJSON_AddItemToObject(msg, "content", content);
                cJSON_AddItemToArray(input, msg);
            } else {
                cJSON_Delete(content);
            }
            for (j = 0; j < m->nblocks; j++) {
                const Block *b = &m->blocks[j];
                if (b->type == BLOCK_TOOL_RESULT) {
                    cJSON *out = cJSON_CreateObject();
                    cJSON_AddStringToObject(out, "type", "function_call_output");
                    cJSON_AddStringToObject(out, "call_id", b->id ? b->id : "");
                    cJSON_AddStringToObject(out, "output", b->text ? b->text : "");
                    cJSON_AddItemToArray(input, out);
                }
            }
        } else {
            Buf text;
            int any_text = 0;
            buf_init(&text);
            for (j = 0; j < m->nblocks; j++) {
                if (m->blocks[j].type == BLOCK_TEXT && m->blocks[j].text && m->blocks[j].text[0]) {
                    if (any_text) {
                        buf_putc(&text, '\n');
                    }
                    buf_puts(&text, m->blocks[j].text);
                    any_text = 1;
                }
            }
            if (any_text) {
                cJSON *msg = cJSON_CreateObject();
                cJSON *content = cJSON_CreateArray();
                cJSON *part = cJSON_CreateObject();
                cJSON_AddStringToObject(msg, "type", "message");
                cJSON_AddStringToObject(msg, "role", "assistant");
                cJSON_AddStringToObject(part, "type", "output_text");
                cJSON_AddStringToObject(part, "text", text.data);
                cJSON_AddItemToArray(content, part);
                cJSON_AddItemToObject(msg, "content", content);
                cJSON_AddItemToArray(input, msg);
            }
            buf_free(&text);
            for (j = 0; j < m->nblocks; j++) {
                const Block *b = &m->blocks[j];
                if (b->type == BLOCK_TOOL_USE) {
                    cJSON *call = cJSON_CreateObject();
                    cJSON_AddStringToObject(call, "type", "function_call");
                    cJSON_AddStringToObject(call, "call_id", b->id ? b->id : "");
                    cJSON_AddStringToObject(call, "name", b->name ? b->name : "");
                    cJSON_AddStringToObject(call, "arguments", b->input_json ? b->input_json : "{}");
                    cJSON_AddItemToArray(input, call);
                }
            }
        }
    }
    cJSON_AddItemToObject(root, "input", input);
    if (with_tools) {
        cJSON_AddItemToObject(root, "tools", tools_responses());
    }
    printed = beige_json_print(root);
    cJSON_Delete(root);
    return printed;
}

static char *joined_text(const Message *m) {
    Buf b;
    int j, any = 0;
    buf_init(&b);
    for (j = 0; j < m->nblocks; j++) {
        if (m->blocks[j].type == BLOCK_TEXT && m->blocks[j].text) {
            if (any) {
                buf_putc(&b, '\n');
            }
            buf_puts(&b, m->blocks[j].text);
            any = 1;
        }
    }
    return buf_detach(&b);
}

char *openai_chat_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    char *printed;
    int i;
    cJSON_AddStringToObject(root, "model", p->model);
    cJSON_AddBoolToObject(root, "stream", 1);
    cJSON_AddNumberToObject(root, "max_tokens", p->max_tokens);
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(messages, sys);
    }
    for (i = 0; i < ts->n; i++) {
        const Message *m = &ts->msgs[i];
        int j;
        if (m->role == ROLE_USER) {
            char *text = joined_text(m);
            if (text[0]) {
                cJSON *msg = cJSON_CreateObject();
                cJSON_AddStringToObject(msg, "role", "user");
                cJSON_AddStringToObject(msg, "content", text);
                cJSON_AddItemToArray(messages, msg);
            }
            free(text);
            for (j = 0; j < m->nblocks; j++) {
                const Block *b = &m->blocks[j];
                if (b->type == BLOCK_TOOL_RESULT) {
                    cJSON *msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(msg, "role", "tool");
                    cJSON_AddStringToObject(msg, "tool_call_id", b->id ? b->id : "");
                    cJSON_AddStringToObject(msg, "content", b->text ? b->text : "");
                    cJSON_AddItemToArray(messages, msg);
                }
            }
        } else {
            cJSON *msg = cJSON_CreateObject();
            cJSON *calls = cJSON_CreateArray();
            char *text = joined_text(m);
            int ncalls = 0;
            cJSON_AddStringToObject(msg, "role", "assistant");
            if (text[0]) {
                cJSON_AddStringToObject(msg, "content", text);
            } else {
                cJSON_AddNullToObject(msg, "content");
            }
            free(text);
            for (j = 0; j < m->nblocks; j++) {
                const Block *b = &m->blocks[j];
                if (b->type == BLOCK_TOOL_USE) {
                    cJSON *call = cJSON_CreateObject();
                    cJSON *fn = cJSON_CreateObject();
                    cJSON_AddStringToObject(call, "id", b->id ? b->id : "");
                    cJSON_AddStringToObject(call, "type", "function");
                    cJSON_AddStringToObject(fn, "name", b->name ? b->name : "");
                    cJSON_AddStringToObject(fn, "arguments", b->input_json ? b->input_json : "{}");
                    cJSON_AddItemToObject(call, "function", fn);
                    cJSON_AddItemToArray(calls, call);
                    ncalls++;
                }
            }
            if (ncalls) {
                cJSON_AddItemToObject(msg, "tool_calls", calls);
            } else {
                cJSON_Delete(calls);
            }
            cJSON_AddItemToArray(messages, msg);
        }
    }
    cJSON_AddItemToObject(root, "messages", messages);
    if (with_tools) {
        cJSON_AddItemToObject(root, "tools", tools_chat());
    }
    printed = beige_json_print(root);
    cJSON_Delete(root);
    return printed;
}

static const char *json_type(const char *event, cJSON *root) {
    cJSON *type;
    if (event && event[0] && !str_eq(event, "message")) {
        return event;
    }
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        return type->valuestring;
    }
    return event ? event : "";
}

static void note_error(StreamAcc *acc, cJSON *root) {
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *msg;
    if (!err || cJSON_IsNull(err)) {
        return;
    }
    msg = cJSON_GetObjectItemCaseSensitive(err, "message");
    if (cJSON_IsString(msg) && msg->valuestring) {
        stream_acc_set_error(acc, msg->valuestring);
    } else if (cJSON_IsString(err) && err->valuestring) {
        stream_acc_set_error(acc, err->valuestring);
    } else {
        stream_acc_set_error(acc, "provider error");
    }
}

static void take_text(StreamAcc *acc, cJSON *node, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(node, key);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        stream_acc_text(acc, v->valuestring, strlen(v->valuestring));
    }
}

static ToolAcc *ensure_call(StreamAcc *acc, const char *call_id, const char *item_id, const char *name) {
    ToolAcc *t = NULL;
    if (item_id && item_id[0]) {
        t = stream_acc_find_item(acc, item_id);
    }
    if (!t && call_id && call_id[0]) {
        t = stream_acc_find(acc, call_id);
    }
    if (!t) {
        t = stream_acc_add_tool(acc, call_id && call_id[0] ? call_id : item_id, name ? name : "");
        if (item_id && item_id[0]) {
            t->item_id = xstrdup(item_id);
        }
        if ((!t->id || !t->id[0]) && call_id && call_id[0]) {
            free(t->id);
            t->id = xstrdup(call_id);
        }
    } else if (name && name[0] && (!t->name || !t->name[0])) {
        free(t->name);
        t->name = xstrdup(name);
    }
    return t;
}

static void absorb_function_item(StreamAcc *acc, cJSON *item, int replace_args) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    cJSON *name, *call_id, *item_id, *args;
    ToolAcc *t;
    if (!cJSON_IsString(type) || !str_eq(type->valuestring, "function_call")) {
        return;
    }
    name = cJSON_GetObjectItemCaseSensitive(item, "name");
    call_id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
    item_id = cJSON_GetObjectItemCaseSensitive(item, "id");
    args = cJSON_GetObjectItemCaseSensitive(item, "arguments");
    t = ensure_call(acc, cJSON_IsString(call_id) ? call_id->valuestring : NULL,
                    cJSON_IsString(item_id) ? item_id->valuestring : NULL,
                    cJSON_IsString(name) ? name->valuestring : NULL);
    if (cJSON_IsString(args) && args->valuestring && args->valuestring[0] && (replace_args || t->args.len == 0)) {
        buf_clear(&t->args);
        buf_puts(&t->args, args->valuestring);
    }
}

static int response_id_ok(const char *s) {
    const char *p;
    if (!s || !s[0] || strlen(s) > 128) {
        return 0;
    }
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static void note_response_id(StreamAcc *acc, cJSON *root) {
    cJSON *resp;
    cJSON *id;
    if (!acc || acc->response_id) {
        return;
    }
    resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    id = cJSON_IsObject(resp) ? cJSON_GetObjectItemCaseSensitive(resp, "id") : NULL;
    if (cJSON_IsString(id) && response_id_ok(id->valuestring)) {
        acc->response_id = xstrdup(id->valuestring);
    }
}

int openai_responses_parse(void *state, StreamAcc *acc, const char *event, const char *data) {
    cJSON *root;
    const char *type;
    (void)state;
    if (!data || !data[0] || str_eq(data, "[DONE]")) {
        if (data && str_eq(data, "[DONE]")) {
            free(acc->stop_reason);
            acc->stop_reason = xstrdup("stop");
        }
        return 0;
    }
    root = cJSON_Parse(data);
    if (!root) {
        return 0;
    }
    note_error(acc, root);
    note_response_id(acc, root);
    type = json_type(event, root);
    if (str_eq(type, "response.output_text.delta")) {
        take_text(acc, root, "delta");
    } else if (str_eq(type, "response.output_item.added") || str_eq(type, "response.output_item.done")) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
        if (cJSON_IsObject(item)) {
            absorb_function_item(acc, item, str_eq(type, "response.output_item.done"));
        }
    } else if (str_eq(type, "response.function_call_arguments.delta")) {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        cJSON *item_id = cJSON_GetObjectItemCaseSensitive(root, "item_id");
        cJSON *call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        ToolAcc *t = ensure_call(acc, cJSON_IsString(call_id) ? call_id->valuestring : NULL,
                                 cJSON_IsString(item_id) ? item_id->valuestring : NULL, NULL);
        if (cJSON_IsString(delta) && delta->valuestring) {
            buf_puts(&t->args, delta->valuestring);
        }
    } else if (str_eq(type, "response.completed") || str_eq(type, "response.incomplete")) {
        cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
        cJSON *usage = resp ? cJSON_GetObjectItemCaseSensitive(resp, "usage") : NULL;
        cJSON *in_tok = usage ? cJSON_GetObjectItemCaseSensitive(usage, "input_tokens") : NULL;
        cJSON *out_tok = usage ? cJSON_GetObjectItemCaseSensitive(usage, "output_tokens") : NULL;
        free(acc->stop_reason);
        acc->stop_reason = xstrdup(str_eq(type, "response.incomplete") ? "incomplete" : "end_turn");
        if (cJSON_IsNumber(in_tok)) {
            acc->input_tokens = (long)in_tok->valuedouble;
        }
        if (cJSON_IsNumber(out_tok)) {
            acc->output_tokens = (long)out_tok->valuedouble;
        }
    } else if (str_eq(type, "error")) {
        /* The stream-level error event carries its message at the top, not under "error". */
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (!acc->error) {
            stream_acc_set_error(acc, cJSON_IsString(msg) ? msg->valuestring : "provider error");
        }
    } else if (str_eq(type, "response.failed")) {
        cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
        cJSON *err = cJSON_IsObject(resp) ? cJSON_GetObjectItemCaseSensitive(resp, "error") : NULL;
        cJSON *msg = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
        if (!acc->error) {
            stream_acc_set_error(acc, cJSON_IsString(msg) ? msg->valuestring : "response failed");
        }
    }
    cJSON_Delete(root);
    return 0;
}

int openai_chat_parse(void *state, StreamAcc *acc, const char *event, const char *data) {
    cJSON *root;
    cJSON *choices, *choice, *delta, *calls, *finish, *usage;
    int i, ncalls;
    (void)state;
    (void)event;
    if (!data || !data[0]) {
        return 0;
    }
    if (str_eq(data, "[DONE]")) {
        free(acc->stop_reason);
        acc->stop_reason = xstrdup("stop");
        return 0;
    }
    root = cJSON_Parse(data);
    if (!root) {
        return 0;
    }
    note_error(acc, root);
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    delta = choice ? cJSON_GetObjectItemCaseSensitive(choice, "delta") : NULL;
    if (delta) {
        take_text(acc, delta, "content");
        calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        ncalls = cJSON_IsArray(calls) ? cJSON_GetArraySize(calls) : 0;
        for (i = 0; i < ncalls; i++) {
            cJSON *call = cJSON_GetArrayItem(calls, i);
            cJSON *idx = cJSON_GetObjectItemCaseSensitive(call, "index");
            cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            int index = cJSON_IsNumber(idx) ? idx->valueint : i;
            ToolAcc *t = stream_acc_find_index(acc, index);
            if (!t) {
                t = stream_acc_add_tool(acc, cJSON_IsString(id) ? id->valuestring : "",
                                        cJSON_IsString(name) ? name->valuestring : "");
                t->index = index;
            } else {
                if (cJSON_IsString(id) && id->valuestring[0] && (!t->id || !t->id[0])) {
                    free(t->id);
                    t->id = xstrdup(id->valuestring);
                }
                if (cJSON_IsString(name) && name->valuestring[0] && (!t->name || !t->name[0])) {
                    free(t->name);
                    t->name = xstrdup(name->valuestring);
                }
            }
            if (cJSON_IsString(args) && args->valuestring) {
                buf_puts(&t->args, args->valuestring);
            }
        }
    }
    finish = choice ? cJSON_GetObjectItemCaseSensitive(choice, "finish_reason") : NULL;
    if (cJSON_IsString(finish) && finish->valuestring && !str_eq(finish->valuestring, "null")) {
        free(acc->stop_reason);
        acc->stop_reason = xstrdup(finish->valuestring);
    }
    usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *in_tok = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON *out_tok = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (cJSON_IsNumber(in_tok)) {
            acc->input_tokens = (long)in_tok->valuedouble;
        }
        if (cJSON_IsNumber(out_tok)) {
            acc->output_tokens = (long)out_tok->valuedouble;
        }
    }
    cJSON_Delete(root);
    return 0;
}
