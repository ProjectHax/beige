#include "check.h"
#include "tools.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static cJSON *args_of(const char *path, const char *old_s, const char *new_s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", path);
    if (old_s) {
        cJSON_AddStringToObject(o, "old_string", old_s);
    }
    if (new_s) {
        cJSON_AddStringToObject(o, "new_string", new_s);
    }
    return o;
}

int main(void) {
    char dir[64];
    char *root;
    char *file;
    char *text = NULL;
    int err = 0;
    ToolIn in;
    cJSON *args;
    FILE *fp;
    snprintf(dir, sizeof dir, "/tmp/beige-edit-%d", (int)getpid());
    mkdir(dir, 0700);
    root = realpath_alloc(dir);
    file = path_join(root, "note.txt");
    fp = fopen(file, "w");
    fputs("hello world\n", fp);
    fclose(fp);

    memset(&in, 0, sizeof in);
    in.root = root;
    in.sandbox = 1;
    in.read_cap = 100000;
    in.output_cap = 10000;
    args = args_of(file, "world", "beige");
    in.args = args;
    CHECK(tool_run("edit", &in, &text, &err) == 0);
    CHECK(err == 0);
    CHECK(text && str_eq(text, "edited file"));
    free(text);
    cJSON_Delete(args);

    {
        char *data = read_file_cap(file, 1000, NULL, NULL, NULL);
        CHECK(data && str_eq(data, "hello beige\n"));
        free(data);
    }

    args = args_of(file, "hello", "nope");
    fp = fopen(file, "w");
    fputs("hello hello\n", fp);
    fclose(fp);
    in.args = args;
    text = NULL;
    err = 0;
    CHECK(tool_run("edit", &in, &text, &err) == 0);
    CHECK(err == 1);
    CHECK(text && strstr(text, "matched"));
    free(text);
    cJSON_Delete(args);

    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", "../outside.txt");
    cJSON_AddStringToObject(args, "contents", "no");
    in.args = args;
    text = NULL;
    err = 0;
    CHECK(tool_run("write", &in, &text, &err) == 0);
    CHECK(err == 1);
    free(text);
    cJSON_Delete(args);
    unlink(file);
    free(file);
    free(root);
    rmdir(dir);
    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
