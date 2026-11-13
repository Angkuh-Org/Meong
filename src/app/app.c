#include "app/app.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void *xcalloc(size_t n, size_t sz) {
    void *p = xmalloc(n * sz);
    memset(p, 0, n * sz);
    return p;
}

char *join_path(const char *dir, const char *name) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s/%s", dir, name);
    return buf_take(&b);
}

char *config_dir(void) {
    const char *d = getenv("MEONG_CONFIG_DIR");
    if (d && *d) {
        return xstrdup(d);
    }
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) {
        char *p = join_path(x, "meong");
        return p;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        char *p = join_path(home, ".config/meong");
        return p;
    }
    return xstrdup("/tmp/meong");
}

char *cache_dir(void) {
    const char *d = getenv("MEONG_CACHE_DIR");
    if (d && *d) {
        return xstrdup(d);
    }
    const char *x = getenv("XDG_CACHE_HOME");
    if (x && *x) {
        return join_path(x, "meong");
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        return join_path(home, ".cache/meong");
    }
    return xstrdup("/tmp/meong-cache");
}

void msgs_clear(App *a) {
    for (int i = 0; i < a->nmsg; i++) {
        free(a->msgs[i].text);
        free(a->msgs[i].reason);
    }
    a->nmsg = 0;
    a->have_gen_stats = 0;
    a->last_trimmed = 0;
    a->last_ptok = 0;
    a->last_ctok = 0;
    a->last_elapsed = 0;
    a->top = 0;
    a->follow = 1;
    a->new_below = 0;
    a->layout_from = 0;
    a->layout_dirty = 1;
    a->dirty = 1;
}

int msg_add(App *a, int role, const char *text) {
    if (a->nmsg == a->msgcap) {
        a->msgcap = a->msgcap ? a->msgcap * 2 : 32;
        a->msgs = xrealloc(a->msgs, (size_t)a->msgcap * sizeof *a->msgs);
    }
    int idx = a->nmsg++;
    Msg *m = &a->msgs[idx];
    memset(m, 0, sizeof *m);
    m->role = role;
    if (text && *text) {
        size_t n = strlen(text);
        m->text = xmalloc(n + 1);
        memcpy(m->text, text, n + 1);
        m->len = n;
        m->cap = n + 1;
        for (size_t i = 0; i < n;) {
            wchar_t wc;
            int k = utf8_next(text + i, n - i, &wc);
            if (k <= 0) {
                break;
            }
            if (wc != '\n' && wc != '\r') {
                m->chars++;
            }
            i += (size_t)k;
        }
    }
    a->layout_dirty = 1;
    a->dirty = 1;
    return idx;
}

void queue_push(App *a, const char *text) {
    if (a->nqueued == a->queued_cap) {
        a->queued_cap = a->queued_cap ? a->queued_cap * 2 : 4;
        a->queued = xrealloc(a->queued,
                             (size_t)a->queued_cap * sizeof *a->queued);
    }
    a->queued[a->nqueued++] = xstrdup(text);
    a->dirty = 1;
}

char *queue_pop(App *a) {
    if (a->nqueued == 0) {
        return NULL;
    }
    char *text = a->queued[0];
    memmove(a->queued, a->queued + 1,
            (size_t)(a->nqueued - 1) * sizeof *a->queued);
    a->nqueued--;
    a->dirty = 1;
    return text;
}

void queue_clear(App *a) {
    for (int i = 0; i < a->nqueued; i++) {
        free(a->queued[i]);
    }
    free(a->queued);
    a->queued = NULL;
    a->nqueued = 0;
    a->queued_cap = 0;
}

void msg_set(Msg *m, const char *text) {
    free(m->text);
    m->text = NULL;
    m->len = 0;
    m->cap = 0;
    m->chars = 0;
    m->lay_valid = 0;
    if (text && *text) {
        size_t n = strlen(text);
        m->text = xmalloc(n + 1);
        memcpy(m->text, text, n + 1);
        m->len = n;
        m->cap = n + 1;
        for (size_t i = 0; i < n;) {
            wchar_t wc;
            int k = utf8_next(text + i, n - i, &wc);
            if (k <= 0) {
                break;
            }
            if (wc != '\n' && wc != '\r') {
                m->chars++;
            }
            i += (size_t)k;
        }
    }
}

static int msg_append_buf(char **buf, size_t *len, size_t *cap, size_t *chars,
                          const char *s, size_t n) {
    if (*len + n > MAX_TEXT) {
        return -1;
    }
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *len + n + 1) {
            nc += nc / 2;
        }
        *buf = xrealloc(*buf, nc);
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    if (chars) {
        for (size_t i = 0; i < n;) {
            wchar_t wc;
            int k = utf8_next(s + i, n - i, &wc);
            if (k <= 0) {
                break;
            }
            if (wc != '\n' && wc != '\r') {
                (*chars)++;
            }
            i += (size_t)k;
        }
    }
    return 0;
}

int msg_append(Msg *m, const char *s, size_t n) {
    return msg_append_buf(&m->text, &m->len, &m->cap, &m->chars, s, n);
}

int msg_append_reason(Msg *m, const char *s, size_t n) {
    m->lay_valid = 0;
    return msg_append_buf(&m->reason, &m->rlen, &m->rcap, NULL, s, n);
}

void trim_trailing_ws(Msg *m) {
    m->lay_valid = 0;
    if (m->text) {
        while (m->len > 0 && isspace((unsigned char)m->text[m->len - 1])) {
            m->len--;
        }
        m->text[m->len] = '\0';
    }
    if (m->reason) {
        while (m->rlen > 0 &&
               isspace((unsigned char)m->reason[m->rlen - 1])) {
            m->rlen--;
        }
        m->reason[m->rlen] = '\0';
    }
}

