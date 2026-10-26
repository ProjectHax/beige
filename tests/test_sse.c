#include "check.h"
#include "provider_internal.h"

#include <string.h>

static void feed(int (*parse)(void *, StreamAcc *, const char *, const char *), void *st, StreamAcc *acc,
                 const char *event, const char *data) {
    parse(st, acc, event, data);
}

int main(void) {
    StreamAcc acc;
    void *st = anthropic_state_new();
    stream_acc_init(&acc);
    feed(anthropic_parse, st, &acc, "content_block_start",
         "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    feed(anthropic_parse, st, &acc, "content_block_delta",
         "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}");
    feed(anthropic_parse, st, &acc, "content_block_start",
         "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\","
         "\"name\":\"bash\",\"input\":{}}}");
    feed(anthropic_parse, st, &acc, "content_block_delta",
         "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
         "\"{\\\"command\\\":\\\"echo hi\\\"}\"}}");
    feed(anthropic_parse, st, &acc, "message_delta",
         "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}");
    CHECK(acc.text.data && str_eq(acc.text.data, "Hello"));
    CHECK(acc.ntools == 1);
    CHECK(str_eq(acc.tools[0].name, "bash"));
    CHECK(acc.tools[0].args.data && strstr(acc.tools[0].args.data, "echo hi"));
    CHECK(acc.stop_reason && str_eq(acc.stop_reason, "tool_use"));
    anthropic_state_free(st);
    stream_acc_free(&acc);

    stream_acc_init(&acc);
    feed(openai_chat_parse, NULL, &acc, "",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"bash\","
         "\"arguments\":\"\"}}]}}]}");
    feed(openai_chat_parse, NULL, &acc, "",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]}}]}");
    feed(openai_chat_parse, NULL, &acc, "", "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}");
    CHECK(acc.ntools == 1);
    CHECK(str_eq(acc.tools[0].id, "c1"));
    CHECK(str_eq(acc.tools[0].name, "bash"));
    CHECK(strstr(acc.tools[0].args.data, "ls"));
    stream_acc_free(&acc);

    stream_acc_init(&acc);
    feed(openai_responses_parse, NULL, &acc, "response.output_text.delta",
         "{\"type\":\"response.output_text.delta\",\"delta\":\"Hi\"}");
    feed(openai_responses_parse, NULL, &acc, "response.output_item.added",
         "{\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\","
         "\"call_id\":\"call_1\",\"name\":\"bash\",\"arguments\":\"\"}}");
    feed(openai_responses_parse, NULL, &acc, "response.function_call_arguments.delta",
         "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"{\\\"command\\\":\\\"pwd\\\"}\"}");
    CHECK(acc.text.data && str_eq(acc.text.data, "Hi"));
    CHECK(acc.ntools == 1);
    CHECK(str_eq(acc.tools[0].id, "call_1"));
    CHECK(strstr(acc.tools[0].args.data, "pwd"));
    stream_acc_drop_tools(&acc);
    CHECK(acc.ntools == 0);
    stream_acc_free(&acc);

    stream_acc_init(&acc);
    feed(openai_responses_parse, NULL, &acc, "error",
         "{\"type\":\"error\",\"code\":\"rate_limit_exceeded\",\"message\":\"slow down\",\"sequence_number\":3}");
    CHECK(acc.error && str_eq(acc.error, "slow down"));
    stream_acc_free(&acc);

    stream_acc_init(&acc);
    feed(openai_responses_parse, NULL, &acc, "response.failed",
         "{\"type\":\"response.failed\",\"response\":{\"id\":\"resp_1\",\"error\":{\"message\":\"server busy\"}}}");
    CHECK(acc.error && str_eq(acc.error, "server busy"));
    stream_acc_free(&acc);

    stream_acc_init(&acc);
    feed(openai_chat_parse, NULL, &acc, "", "{\"error\":null,\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}");
    CHECK(acc.error == NULL);
    stream_acc_free(&acc);

    {
        Transcript ts;
        Message m;
        char *results[1];
        int errors[1] = {0};
        int i;
        transcript_init(&ts);
        /* u0 a1(tool) u2(result) a3(tool) u4(result) a5 */
        transcript_add_user_text(&ts, "go");
        for (i = 0; i < 2; i++) {
            message_init(&m, ROLE_ASSISTANT);
            message_add_tool_use(&m, i ? "t2" : "t1", "bash", "{}");
            transcript_add_message(&ts, &m);
            results[0] = (char *)"ok";
            transcript_add_tool_results(&ts, &ts.msgs[ts.n - 1], results, errors, 1);
        }
        message_init(&m, ROLE_ASSISTANT);
        message_add_text(&m, "done");
        transcript_add_message(&ts, &m);
        CHECK(ts.n == 6);
        /* Keeping 4 would start at u2, whose tool call is in a1. */
        CHECK(transcript_safe_keep(&ts, 4) == 5);
        transcript_replace_with_summary(&ts, "summary", 4);
        CHECK(ts.n == 6);
        CHECK(ts.msgs[1].role == ROLE_ASSISTANT && ts.msgs[1].blocks[0].type == BLOCK_TOOL_USE);

        transcript_add_user_text(&ts, "caf\xc3\xa9 \xe9t\xe9 \xe2\x94");
        CHECK(str_eq(ts.msgs[ts.n - 1].blocks[0].text, "caf\xc3\xa9 \xef\xbf\xbdt\xef\xbf\xbd \xef\xbf\xbd\xef\xbf\xbd"));
        transcript_free(&ts);
    }

    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
