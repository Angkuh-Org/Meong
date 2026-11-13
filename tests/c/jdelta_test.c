/*
 * Differential oracle for jdelta_scan().  Every frame the fast path accepts
 * must produce the same content, reasoning, token counts and error verdict as
 * the jsmn path in chat.c.  Frames the fast path rejects are allowed: the
 * caller falls back, which is always correct but slower.
 */
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int parsed;
    int has_error;
    int has_content;
    int has_reason;
    int has_ptok;
    int has_ctok;
    long long ptok;
    long long ctok;
    Buf content;
    Buf reason;
} Slow;

typedef struct {
    int accepted;
    int has_content;
    int has_reason;
    int has_ptok;
    int has_ctok;
    long long ptok;
    long long ctok;
    Buf content;
    Buf reason;
} Fast;

static int fails;

static void slow_parse(const char *text, size_t len, Slow *s) {
    memset(s, 0, sizeof *s);
    buf_init(&s->content);
    buf_init(&s->reason);
    JDoc d = {0};
    if (jdoc_parse(&d, text, len) != 0) {
        return;
    }
    s->parsed = 1;
    int root = jdoc_root(&d);
    s->has_error = jget(&d, root, "error") >= 0;
    int usage = jget(&d, root, "usage");
    if (usage >= 0) {
        long long v = jint(&d, jget(&d, usage, "prompt_tokens"), -1);
        if (v >= 0) {
            s->has_ptok = 1;
            s->ptok = v;
        }
        v = jint(&d, jget(&d, usage, "completion_tokens"), -1);
        if (v >= 0) {
            s->has_ctok = 1;
            s->ctok = v;
        }
    }
    int choices = jget(&d, root, "choices");
    int c0 = jgeti(&d, choices, 0);
    if (c0 >= 0) {
        int delta = jget(&d, c0, "delta");
        if (delta >= 0) {
            int content = jget(&d, delta, "content");
            if (content >= 0 && jtype(&d, content) == J_STRING) {
                s->has_content = 1;
                jstring(&d, content, &s->content);
            }
            int reason = jget(&d, delta, "reasoning_content");
            if (reason < 0) {
                reason = jget(&d, delta, "reasoning");
            }
            if (reason >= 0 && jtype(&d, reason) == J_STRING) {
                s->has_reason = 1;
                jstring(&d, reason, &s->reason);
            }
        }
    }
    jdoc_free(&d);
}

static void fast_parse(const char *text, size_t len, Fast *f) {
    memset(f, 0, sizeof *f);
    buf_init(&f->content);
    buf_init(&f->reason);
    JDelta fd;
    if (jdelta_scan(text, len, &fd) != 0) {
        return;
    }
    f->accepted = 1;
    if (fd.has_content) {
        f->has_content = 1;
        json_unescape(fd.content, fd.content_len, &f->content);
    }
    if (fd.has_reason) {
        f->has_reason = 1;
        json_unescape(fd.reason, fd.reason_len, &f->reason);
    }
    if (fd.has_ptok && fd.prompt_tokens >= 0) {
        f->has_ptok = 1;
        f->ptok = fd.prompt_tokens;
    }
    if (fd.has_ctok && fd.completion_tokens >= 0) {
        f->has_ctok = 1;
        f->ctok = fd.completion_tokens;
    }
}

static int buf_eq(const Buf *a, const Buf *b) {
    return a->len == b->len &&
           (a->len == 0 || memcmp(a->p, b->p, a->len) == 0);
}

static void compare_ok(const char *name, const Slow *s, const Fast *f) {
    if (!f->accepted) {
        return;
    }
    if (!s->parsed) {
        printf("FAIL %s: fast accepted but jsmn rejected\n", name);
        fails++;
        return;
    }
    if (s->has_error) {
        printf("FAIL %s: fast accepted a frame with a top-level error\n", name);
        fails++;
    }
    if (f->has_content != s->has_content ||
        !buf_eq(&f->content, &s->content)) {
        printf("FAIL %s: content mismatch (fast=%d/%zu slow=%d/%zu)\n", name,
               f->has_content, f->content.len, s->has_content,
               s->content.len);
        fails++;
    }
    if (f->has_reason != s->has_reason ||
        !buf_eq(&f->reason, &s->reason)) {
        printf("FAIL %s: reason mismatch (fast=%d/%zu slow=%d/%zu)\n", name,
               f->has_reason, f->reason.len, s->has_reason, s->reason.len);
        fails++;
    }
    if (f->has_ptok != s->has_ptok ||
        (f->has_ptok && f->ptok != s->ptok)) {
        printf("FAIL %s: prompt_tokens mismatch\n", name);
        fails++;
    }
    if (f->has_ctok != s->has_ctok ||
        (f->has_ctok && f->ctok != s->ctok)) {
        printf("FAIL %s: completion_tokens mismatch\n", name);
        fails++;
    }
}

static void check_frame(const char *name, const char *text, int expect_fast) {
    Slow s;
    Fast f;
    slow_parse(text, strlen(text), &s);
    fast_parse(text, strlen(text), &f);
    if (expect_fast >= 0 && f.accepted != expect_fast) {
        printf("FAIL %s: fast accepted=%d expected=%d\n", name, f.accepted,
               expect_fast);
        fails++;
    }
    compare_ok(name, &s, &f);
    buf_free(&s.content);
    buf_free(&s.reason);
    buf_free(&f.content);
    buf_free(&f.reason);
}

static void check_bytes(const char *name, const char *text, size_t len) {
    Slow s;
    Fast f;
    slow_parse(text, len, &s);
    fast_parse(text, len, &f);
    compare_ok(name, &s, &f);
    buf_free(&s.content);
    buf_free(&s.reason);
    buf_free(&f.content);
    buf_free(&f.reason);
}

static unsigned seed;

static unsigned rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

static void gen_string(Buf *b) {
    buf_addc(b, '"');
    int n = 1 + (int)(rnd() % 40);
    for (int i = 0; i < n; i++) {
        switch (rnd() % 9) {
        case 0: buf_adds(b, "\\\""); break;
        case 1: buf_adds(b, "\\\\"); break;
        case 2: buf_adds(b, "\\n"); break;
        case 3: buf_adds(b, "\\u00e9"); break;
        case 4: buf_adds(b, "\\ud83c\\udf0a"); break;
        case 5: buf_adds(b, "\\t"); break;
        case 6: buf_adds(b, "\\/"); break;
        default: buf_addc(b, (char)('a' + rnd() % 26)); break;
        }
    }
    buf_addc(b, '"');
}

static void fuzz_bytes(int n) {
    static const unsigned char alphabet[] = {
        '{', '}', '[', ']', '"', ':', ',', '0', '1', '2', '3', '4',
        '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
        ' ', '\\', '/', 't', 'f', 'n', 'u', 'r', 0x00, 0x80, 0xFF,
    };
    for (int c = 0; c < n; c++) {
        unsigned char buf[512];
        size_t len = rnd() % sizeof buf;
        for (size_t i = 0; i < len; i++) {
            buf[i] = alphabet[rnd() % (sizeof alphabet)];
        }
        char name[32];
        snprintf(name, sizeof name, "fuzz%d", c);
        check_bytes(name, (const char *)buf, len);
    }
}

static void gen_frame(Buf *b, int with_usage, int with_extra, int reason) {
    buf_adds(b, "{\"id\":\"x\",\"object\":\"chat.completion.chunk\"");
    if (with_extra) {
        buf_adds(b,
                 ",\"note\":\"choices delta content error\","
                 "\"nested\":{\"a\":[1,2,{\"b\":true},null]}");
    }
    buf_adds(b, ",\"choices\":[{\"index\":0,\"delta\":{");
    if (reason) {
        buf_adds(b, "\"reasoning_content\":");
        gen_string(b);
        buf_adds(b, ",\"content\":");
        gen_string(b);
    } else {
        buf_adds(b, "\"content\":");
        gen_string(b);
    }
    buf_adds(b, "}}]");
    if (with_usage) {
        buf_adds(b,
                 ",\"usage\":{\"prompt_tokens\":7,"
                 "\"completion_tokens\":9}");
    }
    buf_adds(b, "}");
}

int main(void) {
    static const char *frames[] = {
        "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}",
        "{\"id\":\"1\",\"object\":\"chat.completion.chunk\",\"created\":1,"
        "\"model\":\"m\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"hi \\\"q\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u00e9 "
        "\\ud83c\\udf0a\"},\"finish_reason\":null}]}",
        "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}",
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}",
        "{\"choices\":[{\"delta\":{\"reasoning\":\"think\"}}]}",
        "{\"choices\":[{\"delta\":{\"reasoning_content\":null,"
        "\"reasoning\":\"ignored\"}}]}",
        "{\"choices\":[{\"delta\":{\"reasoning\":\"ignored\","
        "\"reasoning_content\":\"won\"}}]}",
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":3,"
        "\"completion_tokens\":4}}",
        "{\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4},"
        "\"choices\":[]}",
        "{\"note\":\"choices delta content error\",\"choices\":[{\"delta\":"
        "{\"content\":\"safe\"}}]}",
        "{\"choices\":[{\"delta\":{\"content\":null}}]}",
        "{\"choices\":[{\"delta\":{\"content\":\"a\"}},"
        "{\"delta\":{\"content\":\"b\"}}]}",
        "{\"choices\":{\"delta\":{\"content\":\"no\"}}}",
        "{\"choices\":[1,{\"delta\":{\"content\":\"no\"}}]}",
        "{\"choices\":[{\"delta\":\"no\"}]}",
        "{\"usage\":\"no\",\"choices\":[{\"delta\":{\"content\":\"yes\"}}]}",
        "{\"usage\":{\"prompt_tokens\":-1,\"completion_tokens\":3.5}}",
        "{\"usage\":{\"prompt_tokens\":\"7\",\"completion_tokens\":9}}",
        "{\"choices\":[{\"delta\":{\"content\":\"a\"},"
        "\"content\":\"dup\"}],\"choices\":[{\"delta\":{\"content\":\"b\"}}]}",
        "{}",
        "  {\"choices\" : [ {\"delta\" : {\"content\" : \"spaced\" } } ] }  ",
    };
    for (unsigned i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "frame%u", i);
        check_frame(name, frames[i], 1);
    }

    check_frame("error", "{\"error\":{\"message\":\"no\"}}", 0);
    check_frame("error-after",
                "{\"choices\":[{\"delta\":{\"content\":\"x\"}}],"
                "\"error\":{\"message\":\"no\"}}", 0);
    check_frame("truncated", "{\"choices\":[", 0);
    check_frame("missing-brace",
                "{\"choices\":[{\"delta\":{\"content\":\"x\"}}]", 0);
    check_frame("bad-escape",
                "{\"choices\":[{\"delta\":{\"content\":\"\\q\"}}]}", 0);
    check_frame("array", "[]", 0);
    check_frame("trailing", "{} x", 0);
    check_frame("trailing-comma", "{\"choices\":[],}", 0);
    check_frame("lone-backslash",
                "{\"choices\":[{\"delta\":{\"content\":\"abc\\", 0);

    {
        static const char nul_frame[] =
            "{\"choices\":[{\"delta\":{\"content\":\"a\0b\"}}]}";
        check_bytes("raw-nul", nul_frame, sizeof nul_frame - 1);
    }
    {
        Buf deep;
        buf_init(&deep);
        buf_adds(&deep, "{\"choices\":[{\"delta\":{\"content\":\"x\"}}],"
                        "\"n\":[");
        for (int i = 0; i < 100; i++) {
            buf_addc(&deep, '[');
        }
        buf_adds(&deep, "0");
        for (int i = 0; i < 100; i++) {
            buf_addc(&deep, ']');
        }
        buf_adds(&deep, "]}");
        check_frame("deep", deep.p, 0);
        buf_free(&deep);
    }

    seed = 20261113u;
    for (int i = 0; i < 500; i++) {
        Buf b;
        buf_init(&b);
        gen_frame(&b, (int)(rnd() % 2), (int)(rnd() % 2), (int)(rnd() % 2));
        char name[32];
        snprintf(name, sizeof name, "rand%d", i);
        check_frame(name, b.p, 1);
        buf_free(&b);
    }
    fuzz_bytes(10000);

    /* The benchmark frame from tests/python/bench_sse.py must stay fast. */
    Buf bench;
    buf_init(&bench);
    buf_adds(&bench,
             "{\"choices\":[{\"delta\":{\"content\":\"");
    for (int i = 0; i < 64; i++) {
        buf_addc(&bench, 'x');
    }
    buf_adds(&bench, "\"}}]}");
    check_frame("bench", bench.p, 1);
    buf_free(&bench);

    printf(fails ? "FAILED: %d jdelta check(s)\n" : "JDELTA CHECKS OK\n",
           fails);
    return fails != 0;
}
