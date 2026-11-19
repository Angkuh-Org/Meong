#include "app/app.h"

#include <stdlib.h>
#include <string.h>

char *state_file(void) {
    char *dir = config_dir();
    char *path = join_path(dir, "state");
    free(dir);
    return path;
}

/* TODO(perf): take the key length from the caller; strlen(key) plus keeping
 * klen live costs two callee-saved regs and 424 B against 348 B for the old
 * single-key body (startup only). */
static char *state_load_key(const char *key) {
    char *path = state_file();
    size_t n = 0;
    char *data = read_file(path, &n);
    free(path);
    if (!data) {
        return NULL;
    }
    char *result = NULL;
    size_t klen = strlen(key);
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char *v = trim_ws(p + klen + 1);
            if (*v) {
                free(result);
                result = xstrdup(v);
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(data);
    return result;
}

char *state_load_model(void) {
    return state_load_key("model");
}

char *state_load_effort(void) {
    return state_load_key("effort");
}

void state_save_state(const char *model, const char *effort) {
    char *path = state_file();
    Buf b;
    buf_init(&b);
    if (model && *model) {
        buf_printf(&b, "model=%s\n", model);
    }
    if (effort && *effort) {
        buf_printf(&b, "effort=%s\n", effort);
    }
    write_file_atomic(path, b.p ? b.p : "", b.len);
    buf_free(&b);
    free(path);
}
