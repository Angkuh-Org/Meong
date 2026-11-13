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
static void build_skip(const jsmntok_t *t, int n, int *skip) {
    skip[n] = n;
    for (int i = n - 1; i >= 0; i--) {
        int j = i + 1;
        for (int k = 0; k < t[i].size && j < n; k++) {
            j = skip[j];
        }
        skip[i] = j;
    }
}

_Static_assert(sizeof(jsmntok_t) % sizeof(int) == 0,
               "skip storage must stay int-aligned after the token array");

/*
 * The fast delta path below handles the common SSE frame; this full parser
 * remains the fallback for errors, odd shapes and malformed input.  Keep
 * tests/c/jdelta_test.c green before changing either side of that contract.
 */
int jdoc_parse(JDoc *d, const char *text, size_t len) {
    int cap = d->cap > 0 ? d->cap : 256;
    for (;;) {
        if (!d->tok || cap > d->cap) {
            size_t need = (size_t)cap * sizeof(jsmntok_t) +
                          ((size_t)cap + 1) * sizeof(int);
            d->tok = d->tok ? xrealloc(d->tok, need) : xmalloc(need);
            d->cap = cap;
            d->skip = (int *)((jsmntok_t *)d->tok + cap);
        }
        jsmn_parser p;
        jsmn_init(&p);
        jsmntok_t *t = d->tok;
        int r = jsmn_parse(&p, text, len, t, (unsigned int)cap);
        if (r == JSMN_ERROR_NOMEM) {
            if (cap >= (1 << 20)) {
                d->ntok = 0;
                d->text = NULL;
                return -1;
            }
            cap *= 2;
            continue;
        }
        if (r < 0) {
            d->ntok = 0;
            d->text = NULL;
            return -1;
        }
        build_skip(t, r, d->skip);
        d->ntok = r;
        d->text = text;
        return 0;
    }
}

void jdoc_free(JDoc *d) {
    free(d->tok);
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

void json_unescape(const char *s, int n, Buf *out) {
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
}

int jstring(const JDoc *d, int tok, Buf *out) {
    if (tok < 0 || tok >= d->ntok || jtype(d, tok) != JSMN_STRING) {
        return -1;
    }
    jsmntok_t *t = &toks(d)[tok];
    json_unescape(d->text + t->start, t->end - t->start, out);
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

/*
 * The fast path below reimplements just enough of the OpenAI delta shape to
 * avoid jsmn on the common SSE frame.  It never guesses: every construct it
 * cannot prove (top-level error, odd types, invalid escapes, deep nesting,
 * malformed structure) returns -1 and the caller reruns the full parser.
 */
typedef struct {
    const char *p;
    size_t len;
    size_t i;
} JScan;

static inline void jscan_ws(JScan *s) {
    while (s->i < s->len) {
        char c = s->p[s->i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        s->i++;
    }
}

static inline int jscan_key_eq(const char *k, int n, const char *want) {
    size_t w = strlen(want);
    return (size_t)n == w && memcmp(k, want, w) == 0;
}

static inline const char *jscan_find(const char *p, size_t n, char c) {
    if (n <= 16) {
        for (size_t i = 0; i < n; i++) {
            if (p[i] == c) {
                return p + i;
            }
        }
        return NULL;
    }
    return memchr(p, c, n);
}

static int jscan_string(JScan *s, const char **sp, int *sn) {
    if (s->i >= s->len || s->p[s->i] != '"') {
        return -1;
    }
    const char *base = s->p;
    size_t start = ++s->i;
    for (;;) {
        const char *q = jscan_find(base + s->i, s->len - s->i, '"');
        if (!q) {
            return -1;
        }
        size_t qi = (size_t)(q - base);
        const char *bs = jscan_find(base + s->i, qi - s->i, '\\');
        if (!bs) {
            *sp = base + start;
            *sn = (int)(qi - start);
            s->i = qi + 1;
            return 0;
        }
        size_t bi = (size_t)(bs - base);
        if (bi + 1 >= s->len) {
            return -1;
        }
        switch (base[bi + 1]) {
        case '"':
        case '/':
        case '\\':
        case 'b':
        case 'f':
        case 'r':
        case 'n':
        case 't':
            s->i = bi + 2;
            break;
        case 'u': {
            size_t hi = bi + 2;
            if (hi + 4 > s->len) {
                return -1;
            }
            for (int k = 0; k < 4; k++) {
                char h = base[hi + k];
                if (!((h >= '0' && h <= '9') ||
                      (h >= 'a' && h <= 'f') ||
                      (h >= 'A' && h <= 'F'))) {
                    return -1;
                }
            }
            s->i = hi + 4;
            break;
        }
        default:
            return -1;
        }
    }
}

static int jscan_primitive(JScan *s, const char **sp, int *sn) {
    size_t start = s->i;
    while (s->i < s->len) {
        char c = s->p[s->i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == ',' || c == ']' || c == '}') {
            break;
        }
        s->i++;
    }
    if (s->i == start) {
        return -1;
    }
    *sp = s->p + start;
    *sn = (int)(s->i - start);
    return 0;
}

static int jscan_skip(JScan *s, int depth) {
    if (depth > 64) {
        return -1;
    }
    jscan_ws(s);
    if (s->i >= s->len) {
        return -1;
    }
    char c = s->p[s->i];
    if (c == '"') {
        const char *sp;
        int sn;
        return jscan_string(s, &sp, &sn);
    }
    if (c == '{') {
        s->i++;
        jscan_ws(s);
        if (s->i < s->len && s->p[s->i] == '}') {
            s->i++;
            return 0;
        }
        for (;;) {
            const char *k;
            int kn;
            if (jscan_string(s, &k, &kn) != 0) {
                return -1;
            }
            jscan_ws(s);
            if (s->i >= s->len || s->p[s->i] != ':') {
                return -1;
            }
            s->i++;
            if (jscan_skip(s, depth + 1) != 0) {
                return -1;
            }
            jscan_ws(s);
            if (s->i >= s->len) {
                return -1;
            }
            if (s->p[s->i] == ',') {
                s->i++;
                continue;
            }
            if (s->p[s->i] == '}') {
                s->i++;
                return 0;
            }
            return -1;
        }
    }
    if (c == '[') {
        s->i++;
        jscan_ws(s);
        if (s->i < s->len && s->p[s->i] == ']') {
            s->i++;
            return 0;
        }
        for (;;) {
            if (jscan_skip(s, depth + 1) != 0) {
                return -1;
            }
            jscan_ws(s);
            if (s->i >= s->len) {
                return -1;
            }
            if (s->p[s->i] == ',') {
                s->i++;
                continue;
            }
            if (s->p[s->i] == ']') {
                s->i++;
                return 0;
            }
            return -1;
        }
    }
    const char *sp;
    int sn;
    return jscan_primitive(s, &sp, &sn);
}

static int jscan_delta(JScan *s, JDelta *o) {
    jscan_ws(s);
    if (s->i >= s->len || s->p[s->i] != '{') {
        return jscan_skip(s, 0);
    }
    s->i++;
    jscan_ws(s);
    if (s->i < s->len && s->p[s->i] == '}') {
        s->i++;
        return 0;
    }
    int seen_content = 0;
    int seen_rc = 0;
    int seen_r = 0;
    const char *rc = NULL;
    const char *r = NULL;
    int rcn = 0;
    int rn = 0;
    for (;;) {
        const char *k;
        int kn;
        if (jscan_string(s, &k, &kn) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len || s->p[s->i] != ':') {
            return -1;
        }
        s->i++;
        jscan_ws(s);
        if (jscan_key_eq(k, kn, "content") && !seen_content) {
            seen_content = 1;
            if (s->i < s->len && s->p[s->i] == '"') {
                if (jscan_string(s, &o->content, &o->content_len) != 0) {
                    return -1;
                }
                o->has_content = 1;
            } else if (jscan_skip(s, 0) != 0) {
                return -1;
            }
        } else if (jscan_key_eq(k, kn, "reasoning_content") && !seen_rc) {
            seen_rc = 1;
            if (s->i < s->len && s->p[s->i] == '"') {
                if (jscan_string(s, &rc, &rcn) != 0) {
                    return -1;
                }
            } else if (jscan_skip(s, 0) != 0) {
                return -1;
            }
        } else if (jscan_key_eq(k, kn, "reasoning") && !seen_r) {
            seen_r = 1;
            if (s->i < s->len && s->p[s->i] == '"') {
                if (jscan_string(s, &r, &rn) != 0) {
                    return -1;
                }
            } else if (jscan_skip(s, 0) != 0) {
                return -1;
            }
        } else if (jscan_skip(s, 0) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len) {
            return -1;
        }
        if (s->p[s->i] == ',') {
            s->i++;
            continue;
        }
        if (s->p[s->i] == '}') {
            s->i++;
            break;
        }
        return -1;
    }
    if (seen_rc) {
        if (rc) {
            o->reason = rc;
            o->reason_len = rcn;
            o->has_reason = 1;
        }
    } else if (seen_r && r) {
        o->reason = r;
        o->reason_len = rn;
        o->has_reason = 1;
    }
    return 0;
}

static int jscan_choice(JScan *s, JDelta *o) {
    jscan_ws(s);
    if (s->i >= s->len || s->p[s->i] != '{') {
        return jscan_skip(s, 0);
    }
    s->i++;
    jscan_ws(s);
    if (s->i < s->len && s->p[s->i] == '}') {
        s->i++;
        return 0;
    }
    int seen_delta = 0;
    for (;;) {
        const char *k;
        int kn;
        if (jscan_string(s, &k, &kn) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len || s->p[s->i] != ':') {
            return -1;
        }
        s->i++;
        jscan_ws(s);
        if (jscan_key_eq(k, kn, "delta") && !seen_delta) {
            seen_delta = 1;
            if (jscan_delta(s, o) != 0) {
                return -1;
            }
        } else if (jscan_skip(s, 0) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len) {
            return -1;
        }
        if (s->p[s->i] == ',') {
            s->i++;
            continue;
        }
        if (s->p[s->i] == '}') {
            s->i++;
            break;
        }
        return -1;
    }
    return 0;
}

static int jscan_choices(JScan *s, JDelta *o) {
    jscan_ws(s);
    if (s->i >= s->len || s->p[s->i] != '[') {
        return jscan_skip(s, 0);
    }
    s->i++;
    jscan_ws(s);
    if (s->i < s->len && s->p[s->i] == ']') {
        s->i++;
        return 0;
    }
    if (jscan_choice(s, o) != 0) {
        return -1;
    }
    jscan_ws(s);
    while (s->i < s->len && s->p[s->i] == ',') {
        s->i++;
        if (jscan_skip(s, 0) != 0) {
            return -1;
        }
        jscan_ws(s);
    }
    if (s->i >= s->len || s->p[s->i] != ']') {
        return -1;
    }
    s->i++;
    return 0;
}

static int jscan_int_value(JScan *s, long long *out, unsigned char *has) {
    jscan_ws(s);
    if (s->i >= s->len) {
        return -1;
    }
    char c = s->p[s->i];
    if (c == '"' || c == '{' || c == '[') {
        return jscan_skip(s, 0);
    }
    const char *sp;
    int sn;
    if (jscan_primitive(s, &sp, &sn) != 0) {
        return -1;
    }
    if (sn > 0 && sn <= 31) {
        char tmp[32];
        memcpy(tmp, sp, (size_t)sn);
        tmp[sn] = '\0';
        char *endp = NULL;
        long long v = strtoll(tmp, &endp, 10);
        if (endp && *endp == '\0') {
            *out = v;
            *has = 1;
        }
    }
    return 0;
}

static int jscan_usage(JScan *s, JDelta *o) {
    jscan_ws(s);
    if (s->i >= s->len || s->p[s->i] != '{') {
        return jscan_skip(s, 0);
    }
    s->i++;
    jscan_ws(s);
    if (s->i < s->len && s->p[s->i] == '}') {
        s->i++;
        return 0;
    }
    int seen_p = 0;
    int seen_c = 0;
    for (;;) {
        const char *k;
        int kn;
        if (jscan_string(s, &k, &kn) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len || s->p[s->i] != ':') {
            return -1;
        }
        s->i++;
        jscan_ws(s);
        if (jscan_key_eq(k, kn, "prompt_tokens") && !seen_p) {
            seen_p = 1;
            if (jscan_int_value(s, &o->prompt_tokens, &o->has_ptok) != 0) {
                return -1;
            }
        } else if (jscan_key_eq(k, kn, "completion_tokens") && !seen_c) {
            seen_c = 1;
            if (jscan_int_value(s, &o->completion_tokens, &o->has_ctok) != 0) {
                return -1;
            }
        } else if (jscan_skip(s, 0) != 0) {
            return -1;
        }
        jscan_ws(s);
        if (s->i >= s->len) {
            return -1;
        }
        if (s->p[s->i] == ',') {
            s->i++;
            continue;
        }
        if (s->p[s->i] == '}') {
            s->i++;
            break;
        }
        return -1;
    }
    return 0;
}

/*
 * Fast path contract: every field stream_apply() reads (content, reason,
 * prompt_tokens, completion_tokens) must be mirrored here; a field this
 * scanner ignores becomes silently absent on the fast path.
 */
int jdelta_scan(const char *text, size_t len, JDelta *out) {
    memset(out, 0, sizeof *out);
    if (len > 0 && memchr(text, '\0', len)) {
        return -1;
    }
    JScan s = {text, len, 0};
    jscan_ws(&s);
    if (s.i >= s.len || s.p[s.i] != '{') {
        return -1;
    }
    s.i++;
    jscan_ws(&s);
    if (s.i < s.len && s.p[s.i] == '}') {
        s.i++;
    } else {
        int seen_choices = 0;
        int seen_usage = 0;
        for (;;) {
            const char *k;
            int kn;
            if (jscan_string(&s, &k, &kn) != 0) {
                return -1;
            }
            jscan_ws(&s);
            if (s.i >= s.len || s.p[s.i] != ':') {
                return -1;
            }
            s.i++;
            jscan_ws(&s);
            if (jscan_key_eq(k, kn, "error")) {
                return -1;
            }
            if (jscan_key_eq(k, kn, "choices") && !seen_choices) {
                seen_choices = 1;
                if (jscan_choices(&s, out) != 0) {
                    return -1;
                }
            } else if (jscan_key_eq(k, kn, "usage") && !seen_usage) {
                seen_usage = 1;
                if (jscan_usage(&s, out) != 0) {
                    return -1;
                }
            } else if (jscan_skip(&s, 0) != 0) {
                return -1;
            }
            jscan_ws(&s);
            if (s.i >= s.len) {
                return -1;
            }
            if (s.p[s.i] == ',') {
                s.i++;
                continue;
            }
            if (s.p[s.i] == '}') {
                s.i++;
                break;
            }
            return -1;
        }
    }
    jscan_ws(&s);
    if (s.i != s.len) {
        return -1;
    }
    return 0;
}
