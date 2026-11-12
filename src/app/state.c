#include "app/app.h"

#include <stdlib.h>
#include <string.h>

char *state_file(void) {
    char *dir = config_dir();
    char *path = join_path(dir, "state");
    free(dir);
    return path;
}

char *state_load_model(void) {
    char *path = state_file();
    size_t n = 0;
    char *data = read_file(path, &n);
    free(path);
    if (!data) {
        return NULL;
    }
    char *result = NULL;
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (strncmp(p, "model=", 6) == 0) {
            char *v = trim_ws(p + 6);
            if (*v) {
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

void state_save_model(const char *model) {
    char *path = state_file();
    Buf b;
    buf_init(&b);
    buf_printf(&b, "model=%s\n", model);
    write_file_atomic(path, b.p ? b.p : "", b.len);
    buf_free(&b);
    free(path);
}

