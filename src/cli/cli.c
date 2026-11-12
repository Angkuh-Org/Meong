#include "cli/cli.h"

#include <stdlib.h>
#include <string.h>

#include "api/api.h"
#include "app/app.h"
#include "json/json.h"
#include "net/net.h"

char *resolve_url(const char *cli_url) {
    if (cli_url && *cli_url) {
        return xstrdup(cli_url);
    }
    const char *e = getenv("MEONG_URL");
    if (e && *e) {
        return xstrdup(e);
    }
    e = getenv("PAIG_URL");
    if (e && *e) {
        return xstrdup(e);
    }
    const char *host = getenv("PAIG_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    const char *port = getenv("PAIG_PORT");
    if (!port || !*port) {
        port = "8899";
    }
    Buf b;
    buf_init(&b);
    if (strchr(host, ':') && host[0] != '[') {
        buf_printf(&b, "http://[%s]:%s", host, port);
    } else {
        buf_printf(&b, "http://%s:%s", host, port);
    }
    return buf_take(&b);
}

void usage(FILE *out) {
    fprintf(out,
            "Meong " APP_VERSION " - a terminal cat for PAIG\n"
            "\n"
            "usage:\n"
            "  meong [options]\n"
            "\n"
            "options:\n"
            "  --url URL          gateway base (default: $MEONG_URL, $PAIG_URL,\n"
            "                     $PAIG_HOST/$PAIG_PORT, or http://127.0.0.1:8899)\n"
            "  --model ID         initial model id (PAIG prefix required)\n"
            "  --system TEXT      system prompt\n"
            "  --oneshot TEXT     print one reply and exit\n"
            "  --stream           stream tokens with --oneshot\n"
            "  --list-models      print model ids and exit\n"
            "  --ascii            ASCII-only chrome\n"
            "  --no-mouse         disable mouse wheel scrolling\n"
            "  --no-color         disable color\n"
            "  -h, --help         this text\n"
            "  -V, --version      version\n"
            "\n"
            "keys:\n"
            "  enter send   tab models   pgup/pgdn scroll   ctrl-c cancel/quit\n"
            "  ctrl-g bottom   ctrl-t top   ctrl-l redraw   /help commands\n");
}

void version(void) {
    printf("%s %s\n", APP_NAME, APP_VERSION);
}

int parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof *o);
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--url") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Meong: --url needs a value\n");
                return -1;
            }
            o->url = argv[++i];
        } else if (strncmp(arg, "--url=", 6) == 0) {
            o->url = arg + 6;
        } else if (strcmp(arg, "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Meong: --model needs a value\n");
                return -1;
            }
            o->model = argv[++i];
        } else if (strncmp(arg, "--model=", 8) == 0) {
            o->model = arg + 8;
        } else if (strcmp(arg, "--system") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Meong: --system needs a value\n");
                return -1;
            }
            o->system = argv[++i];
        } else if (strncmp(arg, "--system=", 9) == 0) {
            o->system = arg + 9;
        } else if (strcmp(arg, "--oneshot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Meong: --oneshot needs a prompt\n");
                return -1;
            }
            o->oneshot = argv[++i];
        } else if (strncmp(arg, "--oneshot=", 10) == 0) {
            o->oneshot = arg + 10;
        } else if (strcmp(arg, "--stream") == 0) {
            o->stream = 1;
        } else if (strcmp(arg, "--list-models") == 0) {
            o->list_models = 1;
        } else if (strcmp(arg, "--ascii") == 0) {
            o->ascii = 1;
        } else if (strcmp(arg, "--no-mouse") == 0) {
            o->no_mouse = 1;
        } else if (strcmp(arg, "--no-color") == 0) {
            o->no_color = 1;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            o->help = 1;
        } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            o->version = 1;
        } else {
            fprintf(stderr, "Meong: unknown option %s\n", arg);
            return -1;
        }
    }
    if (o->stream && !o->oneshot) {
        fprintf(stderr, "Meong: --stream only works with --oneshot\n");
        return -1;
    }
    if (o->stream && o->list_models) {
        fprintf(stderr, "Meong: --stream and --list-models conflict\n");
        return -1;
    }
    if (o->oneshot && o->list_models) {
        fprintf(stderr, "Meong: --oneshot and --list-models conflict\n");
        return -1;
    }
    return 0;
}

static void build_oneshot_body(Buf *b, const char *model, const char *system,
                               const char *prompt, int stream) {
    buf_adds(b, "{\"model\":");
    buf_json_str(b, model, strlen(model));
    buf_adds(b, stream ? ",\"stream\":true,\"messages\":[" 
                       : ",\"stream\":false,\"messages\":[");
    int first = 1;
    if (system && *system) {
        buf_adds(b, "{\"role\":\"system\",\"content\":");
        buf_json_str(b, system, strlen(system));
        buf_adds(b, "}");
        first = 0;
    }
    if (!first) {
        buf_addc(b, ',');
    }
    buf_adds(b, "{\"role\":\"user\",\"content\":");
    buf_json_str(b, prompt, strlen(prompt));
    buf_adds(b, "}]}");
}

typedef struct {
    int wrote;
    int failed;
    char err[1024];
} CliStream;

static int cli_on_event(void *ud, const char *data, size_t len) {
    CliStream *c = ud;
    /* TODO(perf): reuse a JDoc and Buf pair on CliStream instead of
     * allocating per event (see the allocator note above jdoc_parse). */
    JDoc d;
    if (jdoc_parse(&d, data, len) != 0) {
        return 0;
    }
    int root = jdoc_root(&d);
    int er = jget(&d, root, "error");
    if (er >= 0 && !c->failed) {
        extract_api_error(data, len, c->err, sizeof c->err);
        c->failed = 1;
    }
    int choices = jget(&d, root, "choices");
    int c0 = jgeti(&d, choices, 0);
    if (c0 >= 0) {
        int delta = jget(&d, c0, "delta");
        int content = delta >= 0 ? jget(&d, delta, "content") : -1;
        if (content >= 0 && jtype(&d, content) == J_STRING) {
            Buf b;
            buf_init(&b);
            if (jstring(&d, content, &b) == 0 && b.len > 0) {
                fwrite(b.p, 1, b.len, stdout);
                fflush(stdout);
                c->wrote = 1;
            }
            buf_free(&b);
        }
    }
    jdoc_free(&d);
    return 0;
}

int cli_oneshot(const Options *o) {
    char *url = resolve_url(o->url);
    Endpoint ep;
    char err[512];
    if (endpoint_parse(url, &ep, err, sizeof err) != 0) {
        fprintf(stderr, "Meong: %s\n", err);
        free(url);
        return 1;
    }
    const char *model = o->model;
    const char *env_model = getenv("MEONG_MODEL");
    if (!model || !*model) {
        model = env_model && *env_model ? env_model : DEFAULT_MODEL;
    }
    Buf body;
    buf_init(&body);
    build_oneshot_body(&body, model, o->system, o->oneshot, o->stream);
    CliStream cli;
    memset(&cli, 0, sizeof cli);
    Stream *s = NULL;
    if (stream_start(&s, &ep, "POST", "/v1/chat/completions", body.p, body.len,
                     o->stream, o->stream ? cli_on_event : NULL,
                     &cli, 240000, err, sizeof err) != 0) {
        fprintf(stderr, "Meong: %s\n", err);
        buf_free(&body);
        free(url);
        return 1;
    }
    int r = drive_stream(s, 600.0, NULL);
    if (r == -2) {
        fprintf(stderr, "Meong: timed out\n");
        stream_free(s);
        buf_free(&body);
        free(url);
        return 1;
    }
    int status = stream_status(s);
    const Buf *resp = stream_body(s);
    if (status != 200) {
        char emsg[1200];
        format_http_error(status, resp, emsg, sizeof emsg);
        fprintf(stderr, "Meong: %s\n", emsg);
        stream_free(s);
        buf_free(&body);
        free(url);
        return 1;
    }
    if (r < 0) {
        fprintf(stderr, "Meong: %s\n",
                stream_error(s)[0] ? stream_error(s) : "request failed");
        stream_free(s);
        buf_free(&body);
        free(url);
        return 1;
    }
    if (o->stream) {
        Buf text;
        buf_init(&text);
        if (!cli.wrote && resp && resp->p && resp->len &&
            extract_chat_text(resp->p, resp->len, &text, NULL, NULL)) {
            fwrite(text.p, 1, text.len, stdout);
            cli.wrote = 1;
        }
        if (cli.failed) {
            if (cli.wrote) {
                fputc('\n', stdout);
            }
            fprintf(stderr, "Meong: %s\n", cli.err);
            buf_free(&text);
            stream_free(s);
            buf_free(&body);
            free(url);
            return 1;
        }
        if (!cli.wrote && resp && resp->len) {
            char emsg[1200];
            if (!extract_api_error(resp->p, resp->len, emsg, sizeof emsg)) {
                snprintf(emsg, sizeof emsg, "the model returned no text");
            }
            debug_dump_body(resp);
            fprintf(stderr, "Meong: %s\n", emsg);
            buf_free(&text);
            stream_free(s);
            buf_free(&body);
            free(url);
            return 1;
        }
        if (!cli.wrote) {
            fprintf(stderr, "Meong: the model returned no text\n");
            buf_free(&text);
            stream_free(s);
            buf_free(&body);
            free(url);
            return 1;
        }
        fputc('\n', stdout);
        buf_free(&text);
    } else {
        Buf text;
        buf_init(&text);
        int ok = resp && resp->p &&
                 extract_chat_text(resp->p, resp->len, &text, NULL, NULL);
        if (!ok || text.len == 0) {
            char emsg[1200];
            if (resp && resp->len &&
                extract_api_error(resp->p, resp->len, emsg, sizeof emsg)) {
                fprintf(stderr, "Meong: %s\n", emsg);
            } else {
                debug_dump_body(resp);
                fprintf(stderr, "Meong: the model returned no text\n");
            }
            buf_free(&text);
            stream_free(s);
            buf_free(&body);
            free(url);
            return 1;
        }
        fwrite(text.p, 1, text.len, stdout);
        if (text.p[text.len - 1] != '\n') {
            fputc('\n', stdout);
        }
        buf_free(&text);
    }
    stream_free(s);
    buf_free(&body);
    free(url);
    return 0;
}

int cli_list_models(const Options *o) {
    char *url = resolve_url(o->url);
    Endpoint ep;
    char err[512];
    if (endpoint_parse(url, &ep, err, sizeof err) != 0) {
        fprintf(stderr, "Meong: %s\n", err);
        free(url);
        return 1;
    }
    char **ids = NULL;
    int n = 0;
    if (models_fetch_live(&ep, &ids, &n, err, sizeof err) != 0) {
        fprintf(stderr, "Meong: %s\n", err);
        free(url);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        printf("%s\n", ids[i]);
    }
    models_free(ids, n);
    free(url);
    return 0;
}
