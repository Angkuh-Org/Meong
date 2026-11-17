/*
 * Differential oracle for jdelta_scan().  Every frame the fast path accepts
 * must produce the same content, reasoning, token counts and error verdict as
 * the jsmn path in chat.c.  Frames the fast path rejects are allowed: the
 * caller falls back, which is always correct but slower.
 */
#include "chat/tool_accum.h"
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
    int has_cached;
    long long ptok;
    long long ctok;
    long long cached;
    Buf content;
    Buf reason;
    ToolAccum tools;
    int tool_failed;
} Slow;

typedef struct {
    int accepted;
    int has_content;
    int has_reason;
    int has_ptok;
    int has_ctok;
    int has_cached;
    long long ptok;
    long long ctok;
    long long cached;
    Buf content;
    Buf reason;
    ToolAccum tools;
    int tool_failed;
} Fast;

static int fails;
static int tool_fast_frames;

static void slow_tools(const JDoc *d, int delta, ToolAccum *ta, int *failed) {
    int tcs = jget(d, delta, "tool_calls");
    if (tcs < 0 || jtype(d, tcs) != J_ARRAY) {
        return;
    }
    for (int i = 0;; i++) {
        int item = jgeti(d, tcs, i);
        if (item < 0) {
            break;
        }
        int idx = jint(d, jget(d, item, "index"), -1);
        Buf id;
        Buf name;
        Buf args;
        buf_init(&id);
        buf_init(&name);
        buf_init(&args);
        int tok = jget(d, item, "id");
        if (tok >= 0 && jtype(d, tok) == J_STRING) {
            jstring(d, tok, &id);
        }
        int fn = jget(d, item, "function");
        int nt = jget(d, fn, "name");
        if (nt >= 0 && jtype(d, nt) == J_STRING) {
            jstring(d, nt, &name);
        }
        int at = jget(d, fn, "arguments");
        if (at >= 0 && jtype(d, at) == J_STRING) {
            jstring(d, at, &args);
        }
        if (idx >= 0 || id.len > 0 || name.len > 0 || args.len > 0) {
            int r = tool_accum_add(ta, idx >= 0, idx, id.p, id.len, name.p,
                                   name.len, args.p, args.len);
            if (r != 0) {
                *failed = 1;
                buf_free(&id);
                buf_free(&name);
                buf_free(&args);
                return;
            }
        }
        buf_free(&id);
        buf_free(&name);
        buf_free(&args);
    }
}

static void slow_parse(const char *text, size_t len, Slow *s, int whole) {
    memset(s, 0, sizeof *s);
    buf_init(&s->content);
    buf_init(&s->reason);
    tool_accum_reset(&s->tools, whole);
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
        int details = jget(&d, usage, "prompt_tokens_details");
        if (details >= 0) {
            v = jint(&d, jget(&d, details, "cached_tokens"), -1);
            if (v >= 0) {
                s->has_cached = 1;
                s->cached = v;
            }
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
            slow_tools(&d, delta, &s->tools, &s->tool_failed);
        }
    }
    jdoc_free(&d);
}

static void fast_parse(const char *text, size_t len, Fast *f, int whole) {
    memset(f, 0, sizeof *f);
    buf_init(&f->content);
    buf_init(&f->reason);
    tool_accum_reset(&f->tools, whole);
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
    if (fd.has_tool_index || fd.tool_args_len > 0) {
        tool_fast_frames++;
        Buf args;
        buf_init(&args);
        if (fd.tool_args_len > 0) {
            json_unescape(fd.tool_args, fd.tool_args_len, &args);
        }
        if (tool_accum_add(&f->tools, fd.has_tool_index, fd.tool_index,
                           NULL, 0, NULL, 0, args.p, args.len) != 0) {
            f->tool_failed = 1;
        }
        buf_free(&args);
    }
    if (fd.has_ptok && fd.prompt_tokens >= 0) {
        f->has_ptok = 1;
        f->ptok = fd.prompt_tokens;
    }
    if (fd.has_ctok && fd.completion_tokens >= 0) {
        f->has_ctok = 1;
        f->ctok = fd.completion_tokens;
    }
    if (fd.has_cached && fd.cached_tokens >= 0) {
        f->has_cached = 1;
        f->cached = fd.cached_tokens;
    }
}

static int buf_eq(const Buf *a, const Buf *b) {
    return a->len == b->len &&
           (a->len == 0 || memcmp(a->p, b->p, a->len) == 0);
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

static int tools_eq(const ToolAccum *a, const ToolAccum *b) {
    if (a->ncalls != b->ncalls || a->whole != b->whole) {
        return 0;
    }
    for (int i = 0; i < TOOL_MAX_CALLS; i++) {
        if (a->used[i] != b->used[i]) {
            return 0;
        }
        if (!a->used[i]) {
            continue;
        }
        if (!str_eq(a->calls[i].id, b->calls[i].id) ||
            !str_eq(a->calls[i].name, b->calls[i].name)) {
            return 0;
        }
        if (a->calls[i].args_len != b->calls[i].args_len) {
            return 0;
        }
        if (a->calls[i].args_len &&
            (!a->calls[i].args || !b->calls[i].args ||
             memcmp(a->calls[i].args, b->calls[i].args,
                    a->calls[i].args_len) != 0)) {
            return 0;
        }
    }
    return 1;
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
    if (f->has_cached != s->has_cached ||
        (f->has_cached && f->cached != s->cached)) {
        printf("FAIL %s: cached_tokens mismatch\n", name);
        fails++;
    }
    if (f->tool_failed != s->tool_failed) {
        printf("FAIL %s: tool failure mismatch (fast=%d slow=%d)\n", name,
               f->tool_failed, s->tool_failed);
        fails++;
    }
    if (!tools_eq(&f->tools, &s->tools)) {
        printf("FAIL %s: tool accumulator mismatch (fast ncalls=%d, "
               "slow ncalls=%d)\n", name, f->tools.ncalls, s->tools.ncalls);
        fails++;
    }
}

static void check_frame_mode(const char *name, const char *text,
                             int expect_fast, int whole) {
    Slow s;
    Fast f;
    slow_parse(text, strlen(text), &s, whole);
    fast_parse(text, strlen(text), &f, whole);
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
    tool_accum_clear(&s.tools);
    tool_accum_clear(&f.tools);
}

static void check_frame(const char *name, const char *text, int expect_fast) {
    check_frame_mode(name, text, expect_fast, 0);
}

static void check_bytes(const char *name, const char *text, size_t len) {
    Slow s;
    Fast f;
    slow_parse(text, len, &s, 0);
    fast_parse(text, len, &f, 0);
    compare_ok(name, &s, &f);
    buf_free(&s.content);
    buf_free(&s.reason);
    buf_free(&f.content);
    buf_free(&f.reason);
    tool_accum_clear(&s.tools);
    tool_accum_clear(&f.tools);
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

static void gen_tool_frame(Buf *b) {
    buf_adds(b, "{\"choices\":[{\"delta\":{");
    if (rnd() % 3 == 0) {
        buf_adds(b, "\"content\":");
        gen_string(b);
        buf_adds(b, ",");
    }
    buf_adds(b, "\"tool_calls\":");
    if (rnd() % 8 == 0) {
        buf_adds(b, "null");
    } else {
        int items = rnd() % 10 == 0 ? 2 : 1;
        buf_addc(b, '[');
        for (int item = 0; item < items; item++) {
            if (item) {
                buf_addc(b, ',');
            }
            buf_addc(b, '{');
            int first = 1;
            if (rnd() % 3) {
                if (!first) {
                    buf_addc(b, ',');
                }
                first = 0;
                buf_adds(b, "\"index\":");
                switch (rnd() % 6) {
                case 0: buf_adds(b, "0"); break;
                case 1: buf_adds(b, "1"); break;
                case 2: buf_adds(b, "-1"); break;
                case 3: buf_adds(b, "\"0\""); break;
                case 4: buf_adds(b, "null"); break;
                default: buf_adds(b, "8"); break;
                }
            }
            if (rnd() % 4 == 0) {
                if (!first) {
                    buf_addc(b, ',');
                }
                first = 0;
                buf_adds(b, "\"id\":");
                buf_adds(b, rnd() % 2 ? "\"\"" : "\"call_1\"");
            }
            if (rnd() % 5) {
                if (!first) {
                    buf_addc(b, ',');
                }
                buf_adds(b, "\"function\":");
                if (rnd() % 10 == 0) {
                    buf_adds(b, "null");
                } else {
                    buf_addc(b, '{');
                    int ff = 1;
                    if (rnd() % 3 == 0) {
                        buf_adds(b, "\"name\":");
                        buf_adds(b, rnd() % 2 ? "\"read_file\"" : "\"\"");
                        ff = 0;
                    }
                    if (rnd() % 5) {
                        if (!ff) {
                            buf_addc(b, ',');
                        }
                        buf_adds(b, "\"arguments\":");
                        switch (rnd() % 6) {
                        case 0: buf_adds(b, "null"); break;
                        case 1: buf_adds(b, "{}"); break;
                        default: gen_string(b); break;
                        }
                    }
                    buf_addc(b, '}');
                }
            }
            buf_addc(b, '}');
        }
        buf_addc(b, ']');
    }
    buf_adds(b, "}}]}");
}

static int slow_tools_frame(const char *text, ToolAccum *ta) {
    JDoc d = {0};
    if (jdoc_parse(&d, text, strlen(text)) != 0) {
        jdoc_free(&d);
        return -1;
    }
    int root = jdoc_root(&d);
    int c0 = jgeti(&d, jget(&d, root, "choices"), 0);
    int delta = c0 >= 0 ? jget(&d, c0, "delta") : -1;
    int failed = 0;
    if (delta >= 0) {
        slow_tools(&d, delta, ta, &failed);
    }
    jdoc_free(&d);
    return failed ? -1 : 0;
}

static int fast_tools_frame(const char *text, ToolAccum *ta) {
    JDelta fd;
    if (jdelta_scan(text, strlen(text), &fd) != 0) {
        return slow_tools_frame(text, ta);
    }
    if (fd.has_tool_index || fd.tool_args_len > 0) {
        Buf args;
        buf_init(&args);
        if (fd.tool_args_len > 0) {
            json_unescape(fd.tool_args, fd.tool_args_len, &args);
        }
        int r = tool_accum_add(ta, fd.has_tool_index, fd.tool_index, NULL, 0,
                               NULL, 0, args.p, args.len);
        buf_free(&args);
        if (r != 0) {
            return -1;
        }
    }
    return 0;
}

static void check_sequence(const char *name, const char *const *frames, int n,
                           int whole) {
    ToolAccum slow;
    ToolAccum fast;
    memset(&slow, 0, sizeof slow);
    memset(&fast, 0, sizeof fast);
    tool_accum_reset(&slow, whole);
    tool_accum_reset(&fast, whole);
    for (int i = 0; i < n; i++) {
        int rs = slow_tools_frame(frames[i], &slow);
        int rf = fast_tools_frame(frames[i], &fast);
        char step[64];
        snprintf(step, sizeof step, "%s[%d]", name, i);
        if (rs != rf) {
            printf("FAIL %s: failure mismatch (fast=%d slow=%d)\n", step, rf,
                   rs);
            fails++;
            break;
        }
        if (!tools_eq(&slow, &fast)) {
            printf("FAIL %s: sequence accumulator mismatch (fast=%d, "
                   "slow=%d)\n", step, fast.ncalls, slow.ncalls);
            fails++;
            break;
        }
        if (rs != 0) {
            break;
        }
    }
    tool_accum_clear(&slow);
    tool_accum_clear(&fast);
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
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":42,"
        "\"completion_tokens\":7,\"prompt_tokens_details\":"
        "{\"cached_tokens\":30}}}",
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":42,"
        "\"completion_tokens\":7,\"prompt_tokens_details\":null}}",
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":42,"
        "\"completion_tokens\":7,\"prompt_tokens_details\":"
        "{\"cached_tokens\":0}}}",
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":42,"
        "\"completion_tokens\":7,\"prompt_tokens_details\":"
        "{\"audio_tokens\":1,\"cached_tokens\":30,"
        "\"reasoning_tokens\":2}}}",
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

    static const struct {
        const char *name;
        const char *text;
        int fast;
    } tool_frames[] = {
        {"tool-frag-index",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"part1\"}}]}}]}",
         1},
        {"tool-frag-noindex",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":"
         "{\"arguments\":\"part2\"}}]}}]}",
         1},
        {"tool-claim",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"\"}}]}}]}",
         1},
        {"tool-empty-noindex",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":"
         "{\"arguments\":\"\"}}]}}]}",
         1},
        {"tool-empty-array",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[]}}]}", 1},
        {"tool-nonarray",
         "{\"choices\":[{\"delta\":{\"tool_calls\":null}}]}", 1},
        {"tool-first",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
         "\"name\":\"read_file\",\"arguments\":\"\"}}]}}]}",
         0},
        {"tool-empty-id-name",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"id\":\"\",\"function\":{\"name\":\"\",\"arguments\":\"x\"}}]}}]}",
         1},
        {"tool-multi",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"a\"}},{\"index\":1,"
         "\"function\":{\"arguments\":\"b\"}}]}}]}",
         0},
        {"tool-escapes",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"a\\\"b\\\\c\\nd\\u00e9"
         "\\ud83c\\udf0a\"}}]}}]}",
         1},
        {"tool-content-calls",
         "{\"choices\":[{\"delta\":{\"content\":\"hi\",\"tool_calls\":["
         "{\"index\":0,\"function\":{\"arguments\":\"x\"}}]}}]}",
         1},
        {"tool-calls-before-content",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"x\"}}],\"content\":\"hi\"}}]}",
         1},
        {"tool-index-string",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":\"0\","
         "\"function\":{\"arguments\":\"{}\"}}]}}]}",
         1},
        {"tool-index-neg",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":-1,"
         "\"function\":{\"arguments\":\"{}\"}}]}}]}",
         1},
        {"tool-index-float",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1.5,"
         "\"function\":{\"arguments\":\"{}\"}}]}}]}",
         1},
        {"tool-index-big",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":2000000,"
         "\"function\":{\"arguments\":\"{}\"}}]}}]}",
         0},
        {"tool-index-oob",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":8,"
         "\"function\":{\"arguments\":\"{}\"}}]}}]}",
         1},
        {"tool-args-null",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":null}}]}}]}",
         1},
        {"tool-args-object",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":{}}}]}}]}",
         1},
        {"tool-args-true",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":true}}]}}]}",
         1},
        {"tool-dup-calls",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"one\"}}],\"tool_calls\":["
         "{\"function\":{\"arguments\":\"two\"}}]}}]}",
         1},
        {"tool-dup-index",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"index\":3,\"function\":{\"arguments\":\"x\"}}]}}]}",
         1},
        {"tool-name-nonempty",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"name\":\"read_file\",\"arguments\":\"x\"}}]}}]}",
         0},
        {"tool-function-missing",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0}]}}]}", 1},
        {"tool-function-null",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":null}]}}]}",
         1},
        {"tool-id-nonempty",
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_1\","
         "\"function\":{\"arguments\":\"\"}}]}}]}",
         0},
    };
    for (unsigned i = 0; i < sizeof tool_frames / sizeof tool_frames[0];
         i++) {
        check_frame_mode(tool_frames[i].name, tool_frames[i].text,
                         tool_frames[i].fast, 0);
    }
    check_frame_mode("tool-whole-empty-name",
                     "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                     "\"function\":{\"arguments\":\"{}\"}}]}}]}",
                     1, 1);
    check_frame_mode("tool-whole-named",
                     "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                     "\"id\":\"call_1\",\"function\":{\"name\":\"read_file\","
                     "\"arguments\":\"{}\"}}]}}]}",
                     0, 1);

    static const char *seq_index[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"read_file\",\"arguments\":\"\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\\\"/proj/x\\\"}\"}}]}}]}",
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}",
    };
    check_sequence("seq-index", seq_index,
                   (int)(sizeof seq_index / sizeof seq_index[0]), 0);

    static const char *seq_noindex[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_1\","
        "\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"pa\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{"
        "\"arguments\":\"th\\\":1}\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{"
        "\"arguments\":\"x\"}}]}}]}",
    };
    check_sequence("seq-noindex", seq_noindex,
                   (int)(sizeof seq_noindex / sizeof seq_noindex[0]), 0);

    static const char *seq_two[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c0\",\"function\":{\"name\":\"read_file\","
        "\"arguments\":\"\"}},{\"index\":1,\"id\":\"c1\",\"function\":{"
        "\"name\":\"list_dir\",\"arguments\":\"\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"function\":{\"arguments\":\"B\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"A\"}}]}}]}",
    };
    check_sequence("seq-two", seq_two,
                   (int)(sizeof seq_two / sizeof seq_two[0]), 0);

    static const char *seq_claim[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{}\"}}]}}]}",
    };
    check_sequence("seq-claim", seq_claim,
                   (int)(sizeof seq_claim / sizeof seq_claim[0]), 0);

    static const char *seq_whole[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c1\",\"function\":{\"name\":\"read_file\","
        "\"arguments\":\"{}\"}}]}}]}",
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c1\",\"function\":{\"name\":\"read_file\","
        "\"arguments\":\"{\\\"a\\\":1}\"}}]}}]}",
    };
    check_sequence("seq-whole", seq_whole,
                   (int)(sizeof seq_whole / sizeof seq_whole[0]), 1);


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
    for (int i = 0; i < 800; i++) {
        Buf b;
        buf_init(&b);
        gen_tool_frame(&b);
        char name[32];
        snprintf(name, sizeof name, "toolrand%d", i);
        check_frame(name, b.p, -1);
        buf_free(&b);
    }
    for (int i = 0; i < 100; i++) {
        Buf b;
        buf_init(&b);
        gen_tool_frame(&b);
        char name[32];
        snprintf(name, sizeof name, "wholerand%d", i);
        check_frame_mode(name, b.p, -1, 1);
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

    if (tool_fast_frames < 300) {
        printf("FAIL: only %d tool fragments reached the fast path\n",
               tool_fast_frames);
        fails++;
    }
    printf(fails ? "FAILED: %d jdelta check(s)\n"
                 : "JDELTA CHECKS OK (%d tool fragments fast)\n",
           fails ? fails : tool_fast_frames);
    return fails != 0;
}
