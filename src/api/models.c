#include "api/api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"

#define MODEL_CACHE_MAGIC "#meong-models-v3"
#define MODEL_CACHE_MAGIC_V2 "#meong-models-v2"

typedef struct {
    char *id;
    ModelCaps caps;
} ModelEntry;

static void caps_reset(ModelCaps *c) {
    memset(c, 0, sizeof *c);
    c->context_window = -1;
    c->max_output_tokens = -1;
    c->in_cost = -1;
    c->out_cost = -1;
    c->vision = -1;
    c->function_calling = -1;
    c->reasoning = -1;
    c->prompt_caching = -1;
    c->estimated = 0;
}

static void caps_read(const JDoc *d, int obj, ModelCaps *c) {
    caps_reset(c);
    int caps = jget(d, obj, "capabilities");
    if (caps < 0) {
        return;
    }
    c->context_window = jint(d, jget(d, caps, "context_window"), -1);
    c->max_output_tokens = jint(d, jget(d, caps, "max_output_tokens"), -1);
    c->in_cost = jdouble(d, jget(d, caps, "input_cost_per_token"), -1);
    c->out_cost = jdouble(d, jget(d, caps, "output_cost_per_token"), -1);
    c->vision = (signed char)jbool(d, jget(d, caps, "supports_vision"), -1);
    c->function_calling =
        (signed char)jbool(d, jget(d, caps, "supports_function_calling"), -1);
    c->reasoning =
        (signed char)jbool(d, jget(d, caps, "supports_reasoning"), -1);
    c->prompt_caching =
        (signed char)jbool(d, jget(d, caps, "supports_prompt_caching"), -1);
    c->estimated = (signed char)jbool(d, jget(d, caps, "estimated"), 0);
}

int caps_reported(const ModelCaps *c) {
    return c->context_window >= 0 || c->max_output_tokens >= 0 ||
           c->in_cost >= 0 || c->out_cost >= 0 || c->vision >= 0 ||
           c->function_calling >= 0 || c->reasoning >= 0 ||
           c->prompt_caching >= 0;
}

const ModelCaps *model_caps_for(const App *a, const char *id) {
    for (int i = 0; i < a->nmodels; i++) {
        if (strcmp(a->models[i], id) == 0) {
            return &a->model_caps[i];
        }
    }
    return NULL;
}

static int entry_cmp(const void *pa, const void *pb) {
    const ModelEntry *a = pa;
    const ModelEntry *b = pb;
    return strcmp(a->id, b->id);
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

static int parse_models(const Buf *body, char ***out, ModelCaps **caps_out,
                        int *out_n, char *err, size_t errlen) {
    *out = NULL;
    *caps_out = NULL;
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
    ModelEntry *entries = xcalloc((size_t)(n > 0 ? n : 1), sizeof *entries);
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
        entries[count].id = s;
        caps_read(&d, obj, &entries[count].caps);
        count++;
    }
    jdoc_free(&d);
    qsort(entries, (size_t)count, sizeof *entries, entry_cmp);
    int uniq = 0;
    for (int i = 0; i < count; i++) {
        if (uniq > 0 && strcmp(entries[uniq - 1].id, entries[i].id) == 0) {
            free(entries[i].id);
            continue;
        }
        entries[uniq++] = entries[i];
    }
    if (uniq == 0) {
        free(entries);
        snprintf(err, errlen, "the gateway returned no models");
        return -1;
    }
    char **ids = xcalloc((size_t)uniq, sizeof *ids);
    ModelCaps *caps = xcalloc((size_t)uniq, sizeof *caps);
    for (int i = 0; i < uniq; i++) {
        ids[i] = entries[i].id;
        caps[i] = entries[i].caps;
    }
    free(entries);
    *out = ids;
    *caps_out = caps;
    *out_n = uniq;
    return 0;
}

int models_fetch_live(const Endpoint *ep, char ***out, ModelCaps **caps_out,
                      int *out_n, char *err, size_t errlen) {
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
    int rc = parse_models(body, out, caps_out, out_n, err, errlen);
    stream_free(s);
    return rc;
}

void models_clear(App *a) {
    for (int i = 0; i < a->nmodels; i++) {
        free(a->models[i]);
    }
    free(a->models);
    free(a->model_caps);
    a->models = NULL;
    a->model_caps = NULL;
    a->nmodels = 0;
}

static long long cache_ll(const char *s) {
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (!end || end == s) {
        return -1;
    }
    return v;
}

static double cache_double(const char *s) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) {
        return -1;
    }
    return v;
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
    size_t magic_len = strlen(MODEL_CACHE_MAGIC);
    size_t v2_len = strlen(MODEL_CACHE_MAGIC_V2);
    int version = 0;
    if (n >= magic_len && memcmp(data, MODEL_CACHE_MAGIC, magic_len) == 0) {
        version = 3;
    } else if (n >= v2_len &&
               memcmp(data, MODEL_CACHE_MAGIC_V2, v2_len) == 0) {
        version = 2;
    }
    char *p = data;
    if (version) {
        p += version == 3 ? magic_len : v2_len;
        if (*p == '\n') {
            p++;
        }
    }
    ModelEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *line = trim_ws(p);
        if (*line) {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                entries = xrealloc(entries, (size_t)cap * sizeof *entries);
            }
            ModelEntry *e = &entries[count];
            caps_reset(&e->caps);
            if (version >= 2) {
                int max_fields = version >= 3 ? 9 : 8;
                char *tab = strchr(line, '\t');
                if (tab) {
                    *tab++ = '\0';
                }
                e->id = xstrdup(line);
                if (tab) {
                    char *fields[9];
                    int nf = 0;
                    while (nf < max_fields) {
                        fields[nf++] = tab;
                        char *next = strchr(tab, '\t');
                        if (!next) {
                            break;
                        }
                        *next = '\0';
                        tab = next + 1;
                    }
                    for (int i = nf; i < max_fields; i++) {
                        fields[i] = "";
                    }
                    e->caps.context_window = cache_ll(fields[0]);
                    e->caps.max_output_tokens = cache_ll(fields[1]);
                    e->caps.vision = (signed char)cache_ll(fields[2]);
                    e->caps.function_calling =
                        (signed char)cache_ll(fields[3]);
                    e->caps.reasoning = (signed char)cache_ll(fields[4]);
                    e->caps.prompt_caching = (signed char)cache_ll(fields[5]);
                    e->caps.in_cost = cache_double(fields[6]);
                    e->caps.out_cost = cache_double(fields[7]);
                    if (max_fields >= 9) {
                        e->caps.estimated =
                            cache_ll(fields[8]) > 0 ? 1 : 0;
                    }
                }
            } else {
                e->id = xstrdup(line);
            }
            count++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(data);
    if (count == 0) {
        free(entries);
        return 0;
    }
    qsort(entries, (size_t)count, sizeof *entries, entry_cmp);
    models_clear(a);
    a->models = xcalloc((size_t)count, sizeof *a->models);
    a->model_caps = xcalloc((size_t)count, sizeof *a->model_caps);
    for (int i = 0; i < count; i++) {
        a->models[i] = entries[i].id;
        a->model_caps[i] = entries[i].caps;
    }
    a->nmodels = count;
    free(entries);
    return version == 3 ? 0 : 1;
}

static void models_save_cache(App *a) {
    if (a->nmodels <= 0) {
        return;
    }
    Buf b;
    buf_init(&b);
    buf_adds(&b, MODEL_CACHE_MAGIC "\n");
    for (int i = 0; i < a->nmodels; i++) {
        const ModelCaps *c = &a->model_caps[i];
        buf_printf(&b, "%s\t%lld\t%lld\t%d\t%d\t%d\t%d\t%.17g\t%.17g\t%d\n",
                   a->models[i], c->context_window, c->max_output_tokens,
                   (int)c->vision, (int)c->function_calling, (int)c->reasoning,
                   (int)c->prompt_caching, c->in_cost, c->out_cost,
                   (int)c->estimated);
    }
    char *dir = cache_dir();
    char *path = join_path(dir, "models");
    free(dir);
    write_file_atomic(path, b.p ? b.p : "", b.len);
    free(path);
    buf_free(&b);
}

void models_replace(App *a, char **ids, ModelCaps *caps, int n) {
    models_clear(a);
    a->models = ids;
    a->model_caps = caps;
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
