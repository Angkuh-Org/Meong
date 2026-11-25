/* One-shot CLI harness for the property/differential test.
 * usage: prop_harness <tool> <cwd> <args-json-file>
 * Prints: line1 "rc=<n>", line2 "err=<text>", line3 "out=<text>".
 * The authorizer always allows; the permission layer is not under test here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/perm.h"
#include "tools/tools.h"
#include "util/util.h"

static enum PermDecision allow_all(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ALLOW;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <tool> <cwd> <args-file>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[3], "rb");
    if (!f) {
        perror("args");
        return 2;
    }
    char *args = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            args = realloc(args, cap);
            if (!args) {
                return 2;
            }
        }
        size_t n = fread(args + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            break;
        }
    }
    fclose(f);

    Buf out;
    buf_init(&out);
    char err[1024] = {0};
    ToolAsk ask;
    perm_ask_clear(&ask);
    int rc = tool_execute(argv[1], args, len, argv[2], NULL, allow_all, &ask,
                          &out, err, sizeof err);
    printf("rc=%d\n", rc);
    printf("err=%s\n", err);
    printf("out=%s\n", out.p ? out.p : "");
    buf_free(&out);
    free(args);
    return 0;
}
