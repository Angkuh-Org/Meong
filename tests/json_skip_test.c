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
    JDoc d;
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

    printf(fails ? "FAILED: %d mismatch(es)\n"
                 : "ALL SKIP/JGET CHECKS IDENTICAL\n",
           fails);
    return fails != 0;
}
