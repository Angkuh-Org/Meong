/*
 * Locks the cached skip table against the recursive walk it replaced.
 * The table is an optimization, so a divergence here is silent on real
 * gateway data; the reference implementation below is the oracle.
 */
#include "json/json.h"
#define JSMN_HEADER
#include "json/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jsmntok_t *T(const JDoc *d) {
    return (jsmntok_t *)d->tok;
}

static int jskip_old(const JDoc *d, int i) {
    jsmntok_t *t = T(d);
    if (i < 0 || i >= d->ntok) {
        return d->ntok;
    }
    int j = i + 1;
    for (int k = 0; k < t[i].size; k++) {
        j = jskip_old(d, j);
        if (j >= d->ntok) {
            return d->ntok;
        }
    }
    return j;
}

static int jget_old(const JDoc *d, int obj, const char *key) {
    jsmntok_t *t = T(d);
    if (obj < 0 || obj >= d->ntok || t[obj].type != JSMN_OBJECT) {
        return -1;
    }
    int end = jskip_old(d, obj);
    int i = obj + 1;
    while (i < end && i < d->ntok) {
        int value = i + 1;
        if (value >= d->ntok) {
            break;
        }
        if (t[i].type == JSMN_STRING &&
            (size_t)(t[i].end - t[i].start) == strlen(key) &&
            memcmp(d->text + t[i].start, key, strlen(key)) == 0) {
            return value;
        }
        i = jskip_old(d, value);
    }
    return -1;
}

static int jgeti_old(const JDoc *d, int arr, int index) {
    jsmntok_t *t = T(d);
    if (arr < 0 || arr >= d->ntok || t[arr].type != JSMN_ARRAY ||
        index < 0 || index >= t[arr].size) {
        return -1;
    }
    int end = jskip_old(d, arr);
    int i = arr + 1;
    int n = 0;
    while (i < end && i < d->ntok) {
        if (n == index) {
            return i;
        }
        i = jskip_old(d, i);
        n++;
    }
    return -1;
}

static int fails;

static void check(const char *name, const char *text) {
    JDoc d = {0};
    if (jdoc_parse(&d, text, strlen(text)) != 0) {
        printf("FAIL %s: parse\n", name);
        fails++;
        return;
    }
    for (int i = 0; i < d.ntok; i++) {
        int a = jskip_old(&d, i);
        int b = d.skip[i];
        if (a != b) {
            printf("FAIL %s: skip[%d] old=%d new=%d\n", name, i, a, b);
            fails++;
            break;
        }
    }
    for (int i = 0; i < d.ntok; i++) {
        jsmntok_t *t = &T(&d)[i];
        if (t->type == JSMN_OBJECT) {
            static const char *keys[] = {"a", "b", "c", "d", "id", "object",
                                         "created", "owned_by", "choices",
                                         "delta", "content", "usage",
                                         "prompt_tokens", "nope", ""};
            for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
                int x = jget_old(&d, i, keys[k]);
                int y = jget(&d, i, keys[k]);
                if (x != y) {
                    printf("FAIL %s: jget(%d,%s) old=%d new=%d\n", name, i,
                           keys[k], x, y);
                    fails++;
                }
            }
        }
        if (t->type == JSMN_ARRAY) {
            for (int idx = -1; idx <= t->size + 1; idx++) {
                int x = jgeti_old(&d, i, idx);
                int y = jgeti(&d, i, idx);
                if (x != y) {
                    printf("FAIL %s: jgeti(%d,%d) old=%d new=%d\n", name, i,
                           idx, x, y);
                    fails++;
                }
            }
            if (jlen(&d, i) != t->size) {
                printf("FAIL %s: jlen(%d)\n", name, i);
                fails++;
            }
        }
    }
    jdoc_free(&d);
}

static void check_reuse(void) {
    static const char *a =
        "{\"choices\":[{\"delta\":{\"content\":\"hi \\\"q\\\"\"}}]}";
    static const char *b =
        "{\"data\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"nested\":{\"x\":[1,2]}}";
    static const char *bad = "{\"choices\":[";
    JDoc d = {0};
    if (jdoc_parse(&d, a, strlen(a)) != 0) {
        printf("FAIL reuse: first parse\n");
        fails++;
        return;
    }
    int root = jdoc_root(&d);
    int choices = jget(&d, root, "choices");
    int c0 = jgeti(&d, choices, 0);
    int delta = jget(&d, c0, "delta");
    int content = jget(&d, delta, "content");
    Buf out;
    buf_init(&out);
    if (content < 0 || jstring(&d, content, &out) != 0 ||
        out.len != strlen("hi \"q\"") ||
        memcmp(out.p, "hi \"q\"", out.len) != 0) {
        printf("FAIL reuse: first content\n");
        fails++;
    }
    buf_free(&out);

    int cap = d.cap;
    for (int i = 0; i < 100; i++) {
        if (jdoc_parse(&d, a, strlen(a)) != 0) {
            printf("FAIL reuse: repeated parse\n");
            fails++;
            break;
        }
    }
    if (d.cap != cap) {
        printf("FAIL reuse: capacity grew on equal documents\n");
        fails++;
    }

    Buf big;
    buf_init(&big);
    buf_adds(&big, "{\"a\":[");
    for (int i = 0; i < 300; i++) {
        char num[16];
        snprintf(num, sizeof num, i ? ",%d" : "%d", i);
        buf_adds(&big, num);
    }
    buf_adds(&big, "]}");
    if (jdoc_parse(&d, big.p, big.len) != 0) {
        printf("FAIL reuse: growth document\n");
        fails++;
    } else {
        if (d.cap <= 256) {
            printf("FAIL reuse: token capacity did not grow\n");
            fails++;
        }
        root = jdoc_root(&d);
        int arr = jget(&d, root, "a");
        if (jlen(&d, arr) != 300) {
            printf("FAIL reuse: grown array length\n");
            fails++;
        }
        int last = jgeti(&d, arr, 299);
        if (last < 0 || jint(&d, last, -1) != 299) {
            printf("FAIL reuse: grown array tail\n");
            fails++;
        }
    }
    buf_free(&big);
    if (jdoc_parse(&d, a, strlen(a)) != 0) {
        printf("FAIL reuse: parse after growth\n");
        fails++;
    }

    if (jdoc_parse(&d, b, strlen(b)) != 0) {
        printf("FAIL reuse: second document\n");
        fails++;
    } else {
        root = jdoc_root(&d);
        int data = jget(&d, root, "data");
        int b1 = jgeti(&d, data, 1);
        int id = jget(&d, b1, "id");
        if (id < 0 || jtype(&d, id) != J_STRING) {
            printf("FAIL reuse: second document lookup\n");
            fails++;
        }
    }

    if (jdoc_parse(&d, bad, strlen(bad)) == 0) {
        printf("FAIL reuse: malformed document parsed\n");
        fails++;
    }
    if (d.ntok != 0) {
        printf("FAIL reuse: stale token count after a failure\n");
        fails++;
    }
    if (jdoc_parse(&d, a, strlen(a)) != 0) {
        printf("FAIL reuse: parse after a failure\n");
        fails++;
    }
    jdoc_free(&d);
    if (d.tok || d.skip || d.cap || d.ntok || d.text) {
        printf("FAIL reuse: jdoc_free left state behind\n");
        fails++;
    }
}

static size_t gen(char *buf, size_t n, size_t cap, int depth, unsigned *seed) {
    *seed = *seed * 1103515245u + 12345u;
    int kind = (int)((*seed >> 16) % 4);
    if (depth <= 0 || kind < 2) {
        if (kind == 0) {
            return n + (size_t)snprintf(buf + n, cap - n, "%u",
                                        *seed % 1000);
        }
        return n + (size_t)snprintf(buf + n, cap - n, "\"v%u\"",
                                    *seed % 100);
    }
    if (kind == 2) {
        int kids = (int)((*seed >> 8) % 4);
        buf[n++] = '{';
        for (int i = 0; i < kids && n < cap - 64; i++) {
            if (i) {
                buf[n++] = ',';
            }
            n += (size_t)snprintf(buf + n, cap - n, "\"k%d\":", i);
            n = gen(buf, n, cap, depth - 1, seed);
        }
        buf[n++] = '}';
        return n;
    }
    int kids = (int)((*seed >> 8) % 4);
    buf[n++] = '[';
    for (int i = 0; i < kids && n < cap - 64; i++) {
        if (i) {
            buf[n++] = ',';
        }
        n = gen(buf, n, cap, depth - 1, seed);
    }
    buf[n++] = ']';
    return n;
}

int main(void) {
    static const char *cases[] = {
        "{\"a\":1,\"b\":2}",
        "{\"a\":{\"b\":1},\"c\":[1,2],\"d\":3}",
        "[]",
        "{}",
        "[[],[[]],[[[1]]],{\"a\":[]}]",
        "[{\"id\":\"mock/plain\",\"object\":\"model\",\"created\":1,"
        "\"owned_by\":\"org\"},{\"id\":\"mock/split\",\"object\":\"model\","
        "\"created\":2,\"owned_by\":\"org\"}]",
        "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi \\\"q\\\" "
        "\\\\ \\u00e9 \\ud83c\\udf0a\"},\"finish_reason\":null}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}}",
        "{\"data\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"}],"
        "\"nested\":{\"x\":[{\"y\":1},{\"y\":2}]}}",
        "{\"empty\":{},\"earr\":[],\"s\":\"\",\"n\":null,\"t\":true}",
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "case%u", i);
        check(name, cases[i]);
    }

    /* Deterministic random JSON: valid objects/arrays with 0..3 children. */
    char buf[16384];
    unsigned seed = 12345;
    for (int r = 0; r < 500; r++) {
        size_t n = gen(buf, 0, sizeof buf, 5, &seed);
        buf[n] = '\0';
        char name[32];
        snprintf(name, sizeof name, "rand%d", r);
        check(name, buf);
    }

    check_reuse();

    printf(fails ? "FAILED: %d mismatch(es)\n"
                 : "ALL SKIP/JGET CHECKS IDENTICAL\n",
           fails);
    return fails != 0;
}
