#include "check.h"
#include "tools.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    ToolIn in;
    cJSON *args = cJSON_CreateObject();
    char *text = NULL;
    int err = 0;
    char *cwd = getcwd_alloc();
    memset(&in, 0, sizeof in);
    in.root = cwd;
    in.sandbox = 1;
    in.output_cap = 8;
    in.read_cap = 1000;
    cJSON_AddStringToObject(args, "command", "printf 'abcdefghijklmnopqrstuvwxyz'");
    in.args = args;
    CHECK(tool_run("bash", &in, &text, &err) == 0);
    CHECK(text != NULL);
    CHECK(strncmp(text, "abcdefgh", 8) == 0);
    CHECK(strstr(text, "[truncated]") != NULL);
    free(text);
    cJSON_Delete(args);
    free(cwd);
    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
