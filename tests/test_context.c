#include "check.h"
#include "context.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char root[64];
    char sub[96];
    char path[128];
    char *text;
    FILE *fp;
    snprintf(root, sizeof root, "/tmp/beige-ctx-%d", (int)getpid());
    snprintf(sub, sizeof sub, "%s/src", root);
    mkdir(root, 0700);
    mkdir(sub, 0700);
    snprintf(path, sizeof path, "%s/.git", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/AGENTS.md", root);
    fp = fopen(path, "w");
    fputs("root rules", fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/CLAUDE.md", sub);
    fp = fopen(path, "w");
    fputs("closer rules", fp);
    fclose(fp);
    text = context_load(sub, NULL, 1000, 8000);
    CHECK(text != NULL);
    CHECK(strstr(text, "root rules") != NULL);
    CHECK(strstr(text, "closer rules") != NULL);
    CHECK(strstr(text, "root rules") < strstr(text, "closer rules"));
    free(text);

    snprintf(path, sizeof path, "%s/AGENTS.md", root);
    fp = fopen(path, "w");
    fputs("0123456789abcdef", fp);
    fclose(fp);
    text = context_load(root, NULL, 4, 4);
    CHECK(text && strstr(text, "[truncated]"));
    free(text);

    text = system_prompt_build(root, "root rules", MODE_ASK);
    CHECK(text && strstr(text, "Core mandates"));
    CHECK(strstr(text, "old_string"));
    CHECK(strstr(text, "root rules"));
    CHECK(strstr(text, "Ask mode"));
    free(text);
    text = system_prompt_build(root, NULL, MODE_PLAN);
    CHECK(text && strstr(text, ".beige/plan.md"));
    free(text);

    snprintf(path, sizeof path, "%s/CLAUDE.md", sub);
    unlink(path);
    snprintf(path, sizeof path, "%s/AGENTS.md", root);
    unlink(path);
    snprintf(path, sizeof path, "%s/.git", root);
    rmdir(path);
    rmdir(sub);
    rmdir(root);
    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
