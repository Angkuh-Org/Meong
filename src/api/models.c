#include "api/api.h"

#include <stdlib.h>
#include <string.h>

#include "json/json.h"

static int model_cmp(const void *pa, const void *pb) {
    const char *a = *(const char *const *)pa;
    const char *b = *(const char *const *)pb;
    return strcmp(a, b);
}

void models_free(char **ids, int n) {
    if (!ids) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(ids[i]);
    }
    free(ids);
}

static int parse_models(const Buf *body, char ***out, int *out_n, char *err,
                        size_t errlen) {
    *out = NULL;
    *out_n = 0;
    if (!body || !body->p || body->len == 0) {
        snprintf(err, errlen, "empty model list");
        return -1;
    }
    JDoc d = {0};
    if (jdoc_parse(&d, body->p, body->len) != 0) {
        snprintf(err, errlen, "model list is not valid JSON");
        return -1;
    }
    int root = jdoc_root(&d);
    int data = jget(&d, root, "data");
    if (data < 0) {
        jdoc_free(&d);
        snprintf(err, errlen, "model list has no data array");
        return -1;
    }
    int n = jlen(&d, data);
    char **ids = xcalloc((size_t)(n > 0 ? n : 1), sizeof *ids);
    int count = 0;
    for (int i = 0; i < n; i++) {
        int obj = jgeti(&d, data, i);
        if (obj < 0) {
            continue;
        }
        int id = jget(&d, obj, "id");
        if (id < 0) {
            continue;
        }
        char *s = jstring_alloc(&d, id);
        if (!s || !*s) {
            free(s);
            continue;
        }
        ids[count++] = s;
    }
    jdoc_free(&d);
    qsort(ids, (size_t)count, sizeof *ids, model_cmp);
    int uniq = 0;
    for (int i = 0; i < count; i++) {
        if (uniq > 0 && strcmp(ids[uniq - 1], ids[i]) == 0) {
            free(ids[i]);
            continue;
        }
        ids[uniq++] = ids[i];
    }
    if (uniq == 0) {
        free(ids);
        snprintf(err, errlen, "the gateway returned no models");
        return -1;
    }
    *out = ids;
    *out_n = uniq;
    return 0;
}

int models_fetch_live(const Endpoint *ep, char ***out, int *out_n,
                             char *err, size_t errlen) {
    Stream *s = NULL;
    if (stream_start(&s, ep, "GET", "/v1/models", NULL, 0, 0, NULL, NULL,
                     15000, err, errlen) != 0) {
        return -1;
    }
    int r = drive_stream(s, 20.0, NULL);
    if (r == -2) {
        snprintf(err, errlen, "timed out fetching models");
        stream_free(s);
        return -1;
    }
    if (r < 0) {
        snprintf(err, errlen, "%s", stream_error(s));
        stream_free(s);
        return -1;
    }
    int status = stream_status(s);
    const Buf *body = stream_body(s);
    if (status != 200) {
        format_http_error(status, body, err, errlen);
        stream_free(s);
        return -1;
    }
    int rc = parse_models(body, out, out_n, err, errlen);
    stream_free(s);
    return rc;
}

int models_load_cache(App *a) {
    char *dir = cache_dir();
    char *path = join_path(dir, "models");
    free(dir);
    size_t n = 0;
    char *data = read_file(path, &n);
    free(path);
    if (!data) {
        return 0;
    }
    char **ids = NULL;
    int count = 0;
    int cap = 0;
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *line = trim_ws(p);
        if (*line) {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                ids = xrealloc(ids, (size_t)cap * sizeof *ids);
            }
            ids[count++] = xstrdup(line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(data);
    if (count == 0) {
        free(ids);
        return 0;
    }
    qsort(ids, (size_t)count, sizeof *ids, model_cmp);
    for (int i = 0; i < a->nmodels; i++) {
        free(a->models[i]);
    }
    free(a->models);
    a->models = ids;
    a->nmodels = count;
    return 0;
}

static void models_save_cache(App *a) {
    if (a->nmodels <= 0) {
        return;
    }
    Buf b;
    buf_init(&b);
    for (int i = 0; i < a->nmodels; i++) {
        buf_adds(&b, a->models[i]);
        buf_addc(&b, '\n');
    }
    char *dir = cache_dir();
    char *path = join_path(dir, "models");
    free(dir);
    write_file_atomic(path, b.p ? b.p : "", b.len);
    free(path);
    buf_free(&b);
}

void models_replace(App *a, char **ids, int n) {
    for (int i = 0; i < a->nmodels; i++) {
        free(a->models[i]);
    }
    free(a->models);
    a->models = ids;
    a->nmodels = n;
    a->models_warn[0] = '\0';
    models_save_cache(a);
}

const char *choose_default_model(App *a) {
    for (int i = 0; i < a->nmodels; i++) {
        if (strcmp(a->models[i], DEFAULT_MODEL) == 0) {
            return a->models[i];
        }
    }
    for (int i = 0; i < a->nmodels; i++) {
        if (strncmp(a->models[i], "cc/", 3) == 0) {
            return a->models[i];
        }
    }
    if (a->nmodels > 0) {
        return a->models[0];
    }
    return DEFAULT_MODEL;
}

