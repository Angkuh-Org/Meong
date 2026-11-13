#include "api/api.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json/json.h"

int extract_api_error(const char *json, size_t len, char *out,
                             size_t cap) {
    JDoc d = {0};
    char *msg = NULL;
    int parsed = jdoc_parse(&d, json, len) == 0;
    if (parsed) {
        int root = jdoc_root(&d);
        int er = jget(&d, root, "error");
        if (er >= 0) {
            if (jtype(&d, er) == J_STRING) {
                msg = jstring_alloc(&d, er);
            } else {
                int m = jget(&d, er, "message");
                if (m < 0) {
                    m = jget(&d, er, "detail");
                }
                if (m < 0) {
                    m = jget(&d, er, "msg");
                }
                if (m >= 0) {
                    msg = jstring_alloc(&d, m);
                }
            }
        }
        if (!msg) {
            int m = jget(&d, root, "message");
            if (m < 0) {
                m = jget(&d, root, "detail");
            }
            if (m < 0) {
                m = jget(&d, root, "msg");
            }
            if (m >= 0) {
                msg = jstring_alloc(&d, m);
            }
        }
        if (!msg) {
            int t = jget(&d, root, "type");
            if (t >= 0 && jtype(&d, t) == J_STRING) {
                char *tv = jstring_alloc(&d, t);
                if (tv && str_ieq(tv, "error")) {
                    msg = tv;
                } else {
                    free(tv);
                }
            }
        }
        jdoc_free(&d);
        if (!msg) {
            return 0;
        }
    } else {
        size_t n = len;
        if (n > cap - 1) {
            n = cap - 1;
        }
        if (n == 0) {
            return 0;
        }
        memcpy(out, json, n);
        out[n] = '\0';
        for (char *p = out; *p; p++) {
            if (*p == '\n' || *p == '\r' || *p == '\t') {
                *p = ' ';
            }
        }
        return 1;
    }
    snprintf(out, cap, "%s", msg);
    free(msg);
    return 1;
}

int extract_chat_text(const char *json, size_t len, Buf *out,
                             long *prompt_tokens, long *completion_tokens) {
    JDoc d = {0};
    if (jdoc_parse(&d, json, len) != 0) {
        return 0;
    }
    int root = jdoc_root(&d);
    int choices = jget(&d, root, "choices");
    int c0 = jgeti(&d, choices, 0);
    int message = c0 >= 0 ? jget(&d, c0, "message") : -1;
    int content = message >= 0 ? jget(&d, message, "content") : -1;
    if (content < 0) {
        content = c0 >= 0 ? jget(&d, c0, "text") : -1;
    }
    int ok = 0;
    if (content >= 0 && jtype(&d, content) == J_STRING &&
        jstring(&d, content, out) == 0 && out->len > 0) {
        ok = 1;
    }
    if (ok && prompt_tokens && completion_tokens) {
        int usage = jget(&d, root, "usage");
        if (usage >= 0) {
            long long v = jint(&d, jget(&d, usage, "prompt_tokens"), -1);
            if (v >= 0) {
                *prompt_tokens = (long)v;
            }
            v = jint(&d, jget(&d, usage, "completion_tokens"), -1);
            if (v >= 0) {
                *completion_tokens = (long)v;
            }
        }
    }
    jdoc_free(&d);
    return ok;
}

void debug_dump_body(const Buf *b) {
    if (!getenv("MEONG_WIRE_DEBUG") || !b || !b->p || !b->len) {
        return;
    }
    size_t n = b->len > 400 ? 400 : b->len;
    fprintf(stderr, "wire: body tail: %.*s\n", (int)n, b->p + b->len - n);
}

void format_http_error(int status, const Buf *body, char *out,
                              size_t cap) {
    char msg[1024];
    msg[0] = '\0';
    if (body && body->p && body->len) {
        extract_api_error(body->p, body->len, msg, sizeof msg);
    }
    if (msg[0]) {
        snprintf(out, cap, "HTTP %d: %s", status, msg);
    } else if (status) {
        snprintf(out, cap, "HTTP %d", status);
    } else {
        snprintf(out, cap, "request failed");
    }
}

int drive_stream(Stream *s, double total_timeout, size_t *progress) {
    double t0 = now_sec();
    for (;;) {
        int r = stream_tick(s, progress);
        if (r != 0) {
            return r;
        }
        if (now_sec() - t0 > total_timeout) {
            return -2;
        }
        usleep(4000);
    }
}

