#include "check.h"
#include "util.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[64];
    char *root;
    char *out = NULL;
    char *err = NULL;
    char *norm;
    snprintf(dir, sizeof dir, "/tmp/beige-path-%d", (int)getpid());
    mkdir(dir, 0700);
    root = realpath_alloc(dir);
    CHECK(root != NULL);
    CHECK(path_resolve(root, 1, "a/b", &out, &err) == 0);
    CHECK(out != NULL);
    CHECK(str_starts(out, root));
    free(out);
    free(err);
    err = NULL;
    out = NULL;
    CHECK(path_resolve(root, 1, "../etc/passwd", &out, &err) != 0);
    CHECK(err != NULL);
    free(out);
    free(err);
    norm = path_normalize("/a/./b/../c");
    CHECK(str_eq(norm, "/a/c"));
    free(norm);
    norm = path_normalize("/");
    CHECK(str_eq(norm, "/"));
    free(norm);
    rmdir(dir);
    free(root);
    if (g_checks_failed) {
        fprintf(stderr, "%d checks failed\n", g_checks_failed);
        return 1;
    }
    return 0;
}
