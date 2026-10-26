#include "provider_internal.h"

#include "tools.h"

#include <string.h>

typedef struct {
    int kind[64];
    int tool_at[64];
} AnthState;

void *anthropic_state_new(void) {
    AnthState *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    return s;
}

void anthropic_state_free(void *state) {
    free(state);
}

static cJSON *tool_array_anthropic(void) {
    cJSON *arr = cJSON_CreateArray();
    int n = 0, i;
    const ToolDef *defs = tool_defs(&n);
    for (i = 0; i < n; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON *schema = cJSON_Parse(defs[i].parameters_json);
        cJSON_AddStringToObject(tool, "name", defs[i].name);
        cJSON_AddStringToObject(tool, "description", defs[i].description);
        if (!schema) {
            schema = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(tool, "input_schema", schema);
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

static cJSON *block_to_anthropic(const Block *b) {
    cJSON *o = cJSON_CreateObject();
    if (b->type == BLOCK_TEXT) {
        if (!b->text || !b->text[0]) {
            cJSON_Delete(o);
            return NULL;
        }
        cJSON_AddStringToObject(o, "type", "text");
        cJSON_AddStringToObject(o, "text", b->text);
        return o;
    }
    if (b->type == BLOCK_TOOL_USE) {
        cJSON *input = cJSON_Parse(b->input_json ? b->input_json : "{}");
        cJSON_AddStringToObject(o, "type", "tool_use");
        cJSON_AddStringToObject(o, "id", b->id ? b->id : "");
        cJSON_AddStringToObject(o, "name", b->name ? b->name : "");
        if (!input || !cJSON_IsObject(input)) {
            cJSON_Delete(input);
            input = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(o, "input", input);
        return o;
    }
    if (b->type == BLOCK_TOOL_RESULT) {
        cJSON_AddStringToObject(o, "type", "tool_result");
        cJSON_AddStringToObject(o, "tool_use_id", b->id ? b->id : "");
        cJSON_AddStringToObject(o, "content", b->text ? b->text : "");
        if (b->is_error) {
            cJSON_AddBoolToObject(o, "is_error", 1);
        }
        return o;
    }
    cJSON_Delete(o);
    return NULL;
}

char *anthropic_build(const Provider *p, const Transcript *ts, const char *system_prompt, int with_tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    char *printed;
    int i;
    cJSON_AddStringToObject(root, "model", p->model);
    cJSON_AddNumberToObject(root, "max_tokens", p->max_tokens);
    cJSON_AddBoolToObject(root, "stream", 1);
    if (system_prompt && system_prompt[0]) {
        cJSON_AddStringToObject(root, "system", system_prompt);
    }
    for (i = 0; i < ts->n; i++) {
        const Message *m = &ts->msgs[i];
        cJSON *msg = cJSON_CreateObject();
        cJSON *content = cJSON_CreateArray();
        int j, any = 0;
        for (j = 0; j < m->nblocks; j++) {
            cJSON *block = block_to_anthropic(&m->blocks[j]);
            if (block) {
                cJSON_AddItemToArray(content, block);
                any = 1;
            }
        }
        if (!any) {
            cJSON_Delete(content);
            cJSON_Delete(msg);
            continue;
        }
        cJSON_AddStringToObject(msg, "role", m->role == ROLE_ASSISTANT ? "assistant" : "user");
        cJSON_AddItemToObject(msg, "content", content);
        cJSON_AddItemToArray(messages, msg);
    }
    cJSON_AddItemToObject(root, "messages", messages);
    if (with_tools) {
        cJSON_AddItemToObject(root, "tools", tool_array_anthropic());
    }
    printed = beige_json_print(root);
    cJSON_Delete(root);
    return printed;
}

static const char *event_name(const char *event, cJSON *root) {
    cJSON *type;
    if (event && event[0]) {
        return event;
    }
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        return type->valuestring;
    }
    return "";
}

static const cJSON *jobj(const cJSON *o, const char *k) {
    return cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
}

int anthropic_parse(void *state, StreamAcc *acc, const char *event, const char *data) {
    AnthState *st = state;
    cJSON *root;
    const char *name;
    if (!data || !data[0]) {
        return 0;
    }
    root = cJSON_Parse(data);
    if (!root) {
        return 0;
    }
    name = event_name(event, root);
    if (str_eq(name, "ping") || str_eq(name, "message_stop")) {
        cJSON_Delete(root);
        return 0;
    }
    if (str_eq(name, "error")) {
        const cJSON *err = jobj(root, "error");
        const cJSON *msg = err ? jobj(err, "message") : NULL;
        stream_acc_set_error(acc, cJSON_IsString(msg) ? msg->valuestring : "anthropic error");
        cJSON_Delete(root);
        return 0;
    }
    if (str_eq(name, "message_start")) {
        const cJSON *msg = jobj(root, "message");
        const cJSON *usage = msg ? jobj(msg, "usage") : NULL;
        const cJSON *in = usage ? jobj(usage, "input_tokens") : NULL;
        if (cJSON_IsNumber(in)) {
            acc->input_tokens = (long)in->valuedouble;
        }
    } else if (str_eq(name, "content_block_start")) {
        const cJSON *idx = jobj(root, "index");
        const cJSON *block = jobj(root, "content_block");
        const cJSON *btype = block ? jobj(block, "type") : NULL;
        int index = cJSON_IsNumber(idx) ? idx->valueint : -1;
        if (index >= 0 && index < 64 && cJSON_IsString(btype)) {
            if (str_eq(btype->valuestring, "tool_use")) {
                const cJSON *id = jobj(block, "id");
                const cJSON *tn = jobj(block, "name");
                ToolAcc *tool = stream_acc_add_tool(acc, cJSON_IsString(id) ? id->valuestring : "",
                                                    cJSON_IsString(tn) ? tn->valuestring : "");
                st->kind[index] = 2;
                st->tool_at[index] = (int)(tool - acc->tools);
            } else {
                st->kind[index] = 1;
            }
        }
    } else if (str_eq(name, "content_block_delta")) {
        const cJSON *idx = jobj(root, "index");
        const cJSON *delta = jobj(root, "delta");
        const cJSON *dtype = delta ? jobj(delta, "type") : NULL;
        int index = cJSON_IsNumber(idx) ? idx->valueint : -1;
        if (delta && cJSON_IsString(dtype) && str_eq(dtype->valuestring, "text_delta")) {
            const cJSON *text = jobj(delta, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                stream_acc_text(acc, text->valuestring, strlen(text->valuestring));
            }
        } else if (delta && cJSON_IsString(dtype) && str_eq(dtype->valuestring, "input_json_delta") && index >= 0 &&
                   index < 64 && st->kind[index] == 2) {
            const cJSON *part = jobj(delta, "partial_json");
            int at = st->tool_at[index];
            if (cJSON_IsString(part) && part->valuestring && at >= 0 && at < acc->ntools) {
                buf_puts(&acc->tools[at].args, part->valuestring);
            }
        }
    } else if (str_eq(name, "message_delta")) {
        const cJSON *delta = jobj(root, "delta");
        const cJSON *stop = delta ? jobj(delta, "stop_reason") : NULL;
        const cJSON *usage = jobj(root, "usage");
        const cJSON *out = usage ? jobj(usage, "output_tokens") : NULL;
        if (cJSON_IsString(stop) && stop->valuestring) {
            free(acc->stop_reason);
            acc->stop_reason = xstrdup(stop->valuestring);
        }
        if (cJSON_IsNumber(out)) {
            acc->output_tokens = (long)out->valuedouble;
        }
    }
    cJSON_Delete(root);
    return 0;
}
