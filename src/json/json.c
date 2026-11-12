#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "jsmn.h"

_Static_assert((int)J_UNDEFINED == (int)JSMN_UNDEFINED,
               "jsmn token types changed");
_Static_assert((int)J_OBJECT == (int)JSMN_OBJECT, "jsmn token types changed");
_Static_assert((int)J_ARRAY == (int)JSMN_ARRAY, "jsmn token types changed");
_Static_assert((int)J_STRING == (int)JSMN_STRING, "jsmn token types changed");
_Static_assert((int)J_PRIMITIVE == (int)JSMN_PRIMITIVE,
               "jsmn token types changed");

static jsmntok_t *toks(const JDoc *d) {
    return (jsmntok_t *)d->tok;
}

/*
 * skip[i] is the first token after i's subtree.  jsmn sizes already describe
 * the recursion: a container counts its direct children, and an object key
 * counts the one value that follows it, so skipping size children whose own
 * skip is known covers exactly what the old recursive walk did.
 */
static int *build_skip(const jsmntok_t *t, int n) {
    int *skip = xmalloc(((size_t)n + 1) * sizeof *skip);
    skip[n] = n;
    for (int i = n - 1; i >= 0; i--) {
        int j = i + 1;
        for (int k = 0; k < t[i].size && j < n; k++) {
            j = skip[j];
        }
        skip[i] = j;
    }
    return skip;
}

/*
 * TODO(perf): every SSE event still costs two allocations (tokens + skip)
 * and a full jsmn pass.  jsmn_parse dominates the streaming profile and the
 * allocator is the next cheap win: one allocation for tok+skip and a
 * reusable JDoc across events.  A "content":" fast scanner is the bigger
 * win but needs escape/null guards plus a partial-parse fallback -- land
 * the allocator change first.
 */
int jdoc_parse(JDoc *d, const char *text, size_t len) {
    memset(d, 0, sizeof *d);
    int cap = 256;
    for (;;) {
        jsmn_parser p;
        jsmn_init(&p);
        jsmntok_t *t = xmalloc((size_t)cap * sizeof *t);
        int r = jsmn_parse(&p, text, len, t, (unsigned int)cap);
        if (r == JSMN_ERROR_NOMEM) {
            free(t);
            if (cap >= (1 << 20)) {
                return -1;
            }
            cap *= 2;
            continue;
        }
        if (r < 0) {
            free(t);
            return -1;
        }
        d->tok = t;
        d->skip = build_skip(t, r);
        d->ntok = r;
        d->cap = cap;
        d->text = text;
        return 0;
    }
}

void jdoc_free(JDoc *d) {
    free(d->tok);
    free(d->skip);
    memset(d, 0, sizeof *d);
}

static int jskip(const JDoc *d, int i) {
    if (i < 0 || i >= d->ntok) {
        return d->ntok;
    }
    return d->skip[i];
}

int jdoc_root(const JDoc *d) {
    return d->ntok > 0 ? 0 : -1;
}

int jtype(const JDoc *d, int tok) {
    if (tok < 0 || tok >= d->ntok) {
        return JSMN_UNDEFINED;
    }
    return toks(d)[tok].type;
}

static int key_eq(const JDoc *d, int tok, const char *key) {
    jsmntok_t *t = toks(d);
    if (t[tok].type != JSMN_STRING) {
        return 0;
    }
    size_t n = strlen(key);
    if ((size_t)(t[tok].end - t[tok].start) != n) {
        return 0;
    }
    return memcmp(d->text + t[tok].start, key, n) == 0;
}

int jget(const JDoc *d, int obj, const char *key) {
    jsmntok_t *t = toks(d);
    if (obj < 0 || obj >= d->ntok || t[obj].type != JSMN_OBJECT) {
        return -1;
    }
    int end = jskip(d, obj);
    int i = obj + 1;
    while (i < end && i < d->ntok) {
        int value = i + 1;
        if (value >= d->ntok) {
            break;
        }
        if (key_eq(d, i, key)) {
            return value;
        }
        i = jskip(d, value);
    }
    return -1;
}

int jlen(const JDoc *d, int arr) {
    if (arr < 0 || arr >= d->ntok || toks(d)[arr].type != JSMN_ARRAY) {
        return 0;
    }
    return toks(d)[arr].size;
}

int jgeti(const JDoc *d, int arr, int index) {
    if (index < 0 || index >= jlen(d, arr)) {
        return -1;
    }
    int end = jskip(d, arr);
    int i = arr + 1;
    int n = 0;
    while (i < end && i < d->ntok) {
        if (n == index) {
            return i;
        }
        i = jskip(d, i);
        n++;
    }
    return -1;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return 0xFFFD;
        }
        v = v * 16 + d;
    }
    return v;
}

int jstring(const JDoc *d, int tok, Buf *out) {
    if (tok < 0 || tok >= d->ntok || jtype(d, tok) != JSMN_STRING) {
        return -1;
    }
    jsmntok_t *t = &toks(d)[tok];
    const char *s = d->text + t->start;
    int n = t->end - t->start;
    int i = 0;
    while (i < n) {
        const char *bs = memchr(s + i, '\\', (size_t)(n - i));
        int plain = bs ? (int)(bs - (s + i)) : n - i;
        if (plain > 0) {
            buf_add(out, s + i, (size_t)plain);
            i += plain;
        }
        if (!bs) {
            break;
        }
        if (i + 1 >= n) {
            break;
        }
        char e = s[i + 1];
        if (e == 'u') {
            if (i + 5 >= n) {
                wchar_t repl = 0xFFFD;
                char tmp[4];
                int k = utf8_encode(repl, tmp);
                buf_add(out, tmp, (size_t)k);
                i += 2;
                continue;
            }
            unsigned cp = (unsigned)hex4(s + i + 2);
            i += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < n && s[i] == '\\' &&
                s[i + 1] == 'u') {
                unsigned lo = (unsigned)hex4(s + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            char tmp[4];
            int k = utf8_encode((wchar_t)cp, tmp);
            buf_add(out, tmp, (size_t)k);
            continue;
        }
        switch (e) {
        case '"': buf_addc(out, '"'); break;
        case '\\': buf_addc(out, '\\'); break;
        case '/': buf_addc(out, '/'); break;
        case 'b': buf_addc(out, '\b'); break;
        case 'f': buf_addc(out, '\f'); break;
        case 'n': buf_addc(out, '\n'); break;
        case 'r': buf_addc(out, '\r'); break;
        case 't': buf_addc(out, '\t'); break;
        default: buf_addc(out, e); break;
        }
        i += 2;
    }
    return 0;
}

char *jstring_alloc(const JDoc *d, int tok) {
    Buf b;
    buf_init(&b);
    if (jstring(d, tok, &b) != 0) {
        buf_free(&b);
        return NULL;
    }
    return buf_take(&b);
}

long long jint(const JDoc *d, int tok, long long def) {
    if (tok < 0 || tok >= d->ntok) {
        return def;
    }
    jsmntok_t *t = &toks(d)[tok];
    if (t->type != JSMN_PRIMITIVE) {
        return def;
    }
    int n = t->end - t->start;
    if (n <= 0 || n > 31) {
        return def;
    }
    char tmp[32];
    memcpy(tmp, d->text + t->start, (size_t)n);
    tmp[n] = '\0';
    char *endp = NULL;
    long long v = strtoll(tmp, &endp, 10);
    if (!endp || *endp != '\0') {
        return def;
    }
    return v;
}
