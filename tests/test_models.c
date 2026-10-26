#include "check.h"
#include "models.h"

#include <string.h>

int main(void) {
    char **ids = NULL;
    int n = 0;
    int more = 0;
    char *last = NULL;
    const char *openai =
        "{\"object\":\"list\",\"data\":["
        "{\"id\":\"gpt-4.1\",\"object\":\"model\"},"
        "{\"id\":\"whisper-1\",\"object\":\"model\"},"
        "{\"id\":\"text-embedding-3-small\"},"
        "{\"id\":\"gpt-4.1\"}"
        "]}";
    const char *anthropic =
        "{\"data\":[{\"id\":\"claude-sonnet-4-5\"},{\"id\":\"claude-opus-4-5\"}],"
        "\"has_more\":true,\"last_id\":\"claude-opus-4-5\"}";
    const char *codex = "{\"models\":[{\"slug\":\"gpt-5.4\"},{\"id\":\"gpt-5.4-mini\"}]}";

    CHECK(models_parse(openai, &ids, &n, &more, &last) == 0);
    CHECK(n == 1);
    CHECK(str_eq(ids[0], "gpt-4.1"));
    CHECK(more == 0);
    models_free(ids, n);
    ids = NULL;
    n = 0;

    CHECK(models_parse(anthropic, &ids, &n, &more, &last) == 0);
    CHECK(n == 2);
    CHECK(str_eq(ids[0], "claude-sonnet-4-5"));
    CHECK(str_eq(ids[1], "claude-opus-4-5"));
    CHECK(more == 1);
    CHECK(last && str_eq(last, "claude-opus-4-5"));
    free(last);
    models_free(ids, n);
    ids = NULL;
    n = 0;
    last = NULL;

    CHECK(models_parse(codex, &ids, &n, NULL, NULL) == 0);
    CHECK(n == 2);
    CHECK(str_eq(ids[0], "gpt-5.4"));
    CHECK(str_eq(ids[1], "gpt-5.4-mini"));
    models_free(ids, n);

    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
