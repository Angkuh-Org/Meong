#include "ui/md.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

/* A line-start reader that needs a newline must not hold an unbounded line
 * and rescan it on every delta; past this cap it falls back to raw text. */
#define MD_PEND_MAX 8192

typedef struct MdState {
    Buf pend;
    Buf label;
    Buf url;
    size_t src;
    unsigned mask;
    unsigned char at_start;
    unsigned char in_fence;
    unsigned char fence_ch;
    unsigned char fence_run;
    unsigned char raw;
    unsigned char esc;
    unsigned char star;
    unsigned char under;
    unsigned char tilde;
    unsigned char link;
    unsigned char table;
    unsigned char prev;
    unsigned char unicode;
    unsigned char flushed;
} MdState;

static void dadd(Msg *m, const void *s, size_t n) {
    if (n == 0) {
        return;
    }
    if (m->dlen + n + 1 > m->dcap) {
        size_t cap = m->dcap ? m->dcap : 256;
        while (cap < m->dlen + n + 1) {
            cap += cap / 2;
        }
        m->disp = xrealloc(m->disp, cap);
        m->dcap = cap;
    }
    memcpy(m->disp + m->dlen, s, n);
    m->dlen += n;
    m->disp[m->dlen] = '\0';
}

static void daddc(Msg *m, char c) {
    dadd(m, &c, 1);
}

/* Raw markdown also reaches display verbatim; 0x01 is our control byte. */
static void dadd_raw(Msg *m, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && (unsigned char)s[j] != MD_CMD) {
            j++;
        }
        if (j > i) {
            dadd(m, s + i, j - i);
        }
        if (j < n) {
            j++;
        }
        i = j;
    }
}

static void dmeta(Msg *m, int kind, int hang) {
    unsigned char b[3] = {(unsigned char)MD_CMD,
                          (unsigned char)(MD_META | kind),
                          (unsigned char)(hang < 0 ? 0 : hang > 255 ? 255
                                                                  : hang)};
    dadd(m, b, sizeof b);
}

static void dstyle(Msg *m, MdState *st, unsigned mask) {
    if (st->mask == mask) {
        return;
    }
    unsigned char b[2] = {(unsigned char)MD_CMD, (unsigned char)mask};
    dadd(m, b, sizeof b);
    st->mask = mask;
}

static int md_special(unsigned char c) {
    return c == '\n' || c == '\\' || c == '`' || c == '*' || c == '_' ||
           c == '~' || c == '[' || c == '|' || c == MD_CMD;
}

static void link_clear(MdState *st) {
    st->link = 0;
    buf_clear(&st->label);
    buf_clear(&st->url);
}

static void link_literal(Msg *m, MdState *st) {
    daddc(m, '[');
    if (st->label.len > 0) {
        dadd_raw(m, st->label.p, st->label.len);
    }
    if (st->link >= 2) {
        daddc(m, ']');
    }
    if (st->link >= 3) {
        daddc(m, '(');
        if (st->url.len > 0) {
            dadd_raw(m, st->url.p, st->url.len);
        }
    }
    link_clear(st);
}

static void link_emit(Msg *m, MdState *st) {
    const char *txt = st->label.len > 0 ? st->label.p : st->url.p;
    size_t n = st->label.len > 0 ? st->label.len : st->url.len;
    if (n == 0) {
        link_clear(st);
        return;
    }
    unsigned base = st->mask;
    dstyle(m, st, base | MD_LINK);
    dadd_raw(m, txt, n);
    dstyle(m, st, base);
    link_clear(st);
}

static void inline_flush(Msg *m, MdState *st) {
    if (st->esc) {
        daddc(m, '\\');
        st->esc = 0;
    }
    if (st->star) {
        daddc(m, '*');
        st->star = 0;
    }
    if (st->under) {
        daddc(m, '_');
        st->under = 0;
    }
    if (st->tilde) {
        daddc(m, '~');
        st->tilde = 0;
    }
    if (st->link) {
        link_literal(m, st);
    }
}

static void line_end(Msg *m, MdState *st) {
    inline_flush(m, st);
    dstyle(m, st, 0);
    st->table = 0;
    st->prev = 0;
}

/*
 * Inline pass.  Markers are consumed only once the next byte resolves them,
 * so display bytes are never rewritten.  Returns 1 after a newline so the
 * caller re-enters the line-start classifier.
 *
 * TODO(perf): the loop reloads six state bytes (mask, esc, star, under,
 * tilde, link) per input byte and md_special() is an eight-compare chain.
 * Pack the flags into one local word and use a 256-byte lookup table; the
 * 2026-11-18 register audit shows x19-x28 already all live, so this is a
 * fewer-loads/fewer-compares lever, not a spill fix.  Baseline: 583 insn,
 * 15.7% of a 100 KB markdown stream at delta 256 B.
 */
static int md_inline(Msg *m, MdState *st, const char *p, size_t n,
                     size_t *pi) {
    size_t i = *pi;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];

        if (st->mask & MD_CODE) {
            size_t j = i;
            while (j < n) {
                unsigned char b = (unsigned char)p[j];
                if (b == '`' || b == '\n' || b == MD_CMD) {
                    break;
                }
                j++;
            }
            if (j > i) {
                dadd_raw(m, p + i, j - i);
                st->prev = (unsigned char)p[j - 1];
                i = j;
            }
            if (i >= n) {
                break;
            }
            c = (unsigned char)p[i];
            if (c == '`') {
                dstyle(m, st, st->mask & ~MD_CODE);
                st->prev = '`';
                i++;
                continue;
            }
            if (c == MD_CMD) {
                i++;
                continue;
            }
        }

        if (st->esc) {
            st->esc = 0;
            if (c != '\n') {
                dadd_raw(m, p + i, 1);
                st->prev = c;
                i++;
            }
            continue;
        }

        if (st->star) {
            st->star = 0;
            if (c == '*') {
                dstyle(m, st, st->mask ^ MD_BOLD);
                st->prev = '*';
                i++;
                continue;
            }
            if (st->mask & MD_ITALIC) {
                dstyle(m, st, st->mask & ~MD_ITALIC);
                st->prev = '*';
                continue;
            }
            if (c == '\n' || isspace(c)) {
                dadd_raw(m, "*", 1);
                st->prev = '*';
                continue;
            }
            dstyle(m, st, st->mask | MD_ITALIC);
            st->prev = '*';
            continue;
        }

        if (st->under) {
            st->under = 0;
            int prev_word = st->prev != 0 && isalnum(st->prev) != 0;
            if (c == '_') {
                if (st->mask & MD_BOLD) {
                    dstyle(m, st, st->mask & ~MD_BOLD);
                } else if (prev_word) {
                    dadd_raw(m, "__", 2);
                } else {
                    dstyle(m, st, st->mask | MD_BOLD);
                }
                st->prev = '_';
                i++;
                continue;
            }
            if (c == '\n' || isspace(c)) {
                if (st->mask & MD_ITALIC) {
                    dstyle(m, st, st->mask & ~MD_ITALIC);
                    st->prev = '_';
                    continue;
                }
                dadd_raw(m, "_", 1);
                st->prev = '_';
                continue;
            }
            if (st->mask & MD_ITALIC) {
                dstyle(m, st, st->mask & ~MD_ITALIC);
            } else if (prev_word && isalnum(c)) {
                dadd_raw(m, "_", 1);
                st->prev = '_';
                continue;
            } else {
                dstyle(m, st, st->mask | MD_ITALIC);
            }
            st->prev = '_';
            continue;
        }

        if (st->tilde) {
            st->tilde = 0;
            if (c == '~') {
                dstyle(m, st, st->mask ^ MD_STRIKE);
                st->prev = '~';
                i++;
                continue;
            }
            dadd_raw(m, "~", 1);
            st->prev = '~';
            continue;
        }

        if (st->link == 1) {
            if (c == ']') {
                st->link = 2;
                st->prev = ']';
                i++;
                continue;
            }
            if (c == '\n') {
                link_literal(m, st);
                continue;
            }
            if (c == MD_CMD) {
                i++;
                continue;
            }
            if (st->label.len < 512) {
                buf_addc(&st->label, (char)c);
            } else {
                link_literal(m, st);
                continue;
            }
            st->prev = c;
            i++;
            continue;
        }
        if (st->link == 2) {
            if (c == '(') {
                st->link = 3;
                buf_clear(&st->url);
                st->prev = '(';
                i++;
                continue;
            }
            if (c == MD_CMD) {
                i++;
                continue;
            }
            link_literal(m, st);
            continue;
        }
        if (st->link == 3) {
            if (c == ')') {
                link_emit(m, st);
                st->prev = ')';
                i++;
                continue;
            }
            if (c == '\n') {
                link_literal(m, st);
                continue;
            }
            if (c == MD_CMD) {
                i++;
                continue;
            }
            if (st->url.len < 2048) {
                buf_addc(&st->url, (char)c);
            } else {
                link_literal(m, st);
                continue;
            }
            st->prev = c;
            i++;
            continue;
        }

        switch (c) {
        case '\n':
            line_end(m, st);
            daddc(m, '\n');
            i++;
            *pi = i;
            st->mask = 0;
            st->at_start = 1;
            return 1;
        case MD_CMD:
            i++;
            continue;
        case '\\':
            st->esc = 1;
            i++;
            continue;
        case '`':
            dstyle(m, st, st->mask ^ MD_CODE);
            st->prev = '`';
            i++;
            continue;
        case '*':
            st->star = 1;
            i++;
            continue;
        case '_':
            st->under = 1;
            i++;
            continue;
        case '~':
            st->tilde = 1;
            i++;
            continue;
        case '[':
            st->link = 1;
            buf_clear(&st->label);
            buf_clear(&st->url);
            st->prev = '[';
            i++;
            continue;
        case '|':
            if (st->table) {
                unsigned base = st->mask;
                dstyle(m, st, base | MD_DIM);
                if (st->unicode) {
                    dadd(m, "\xE2\x94\x82", 3);
                } else {
                    daddc(m, '|');
                }
                dstyle(m, st, base);
            } else {
                dadd_raw(m, "|", 1);
            }
            st->prev = '|';
            i++;
            continue;
        default: {
            size_t j = i + 1;
            while (j < n && !md_special((unsigned char)p[j])) {
                j++;
            }
            dadd_raw(m, p + i, j - i);
            st->prev = (unsigned char)p[j - 1];
            i = j;
            continue;
        }
        }
    }
    *pi = i;
    return 0;
}

static int table_sep(const char *p, size_t start, size_t n, size_t *nl_out) {
    int dash = 0;
    size_t i = start;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '\n') {
            *nl_out = i;
            return dash;
        }
        if (c == '-') {
            dash = 1;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return 0;
        }
    }
    return -1;
}

static int md_line_start(Msg *m, MdState *st, const char *p, size_t n,
                         size_t *pi) {
    size_t i = *pi;
    if (i >= n) {
        return 0;
    }

    if (st->in_fence) {
        size_t sp = i;
        while (sp < n && sp - i < 4 && p[sp] == ' ') {
            sp++;
        }
        if (sp < n && p[sp] == (char)st->fence_ch) {
            size_t run = 0;
            while (sp + run < n && p[sp + run] == (char)st->fence_ch) {
                run++;
            }
            if (run >= st->fence_run) {
                size_t k = sp + run;
                while (k < n && (p[k] == ' ' || p[k] == '\t')) {
                    k++;
                }
                if (k < n && p[k] == '\n') {
                    dmeta(m, MDL_FENCE_CLOSE, 0);
                    daddc(m, '\n');
                    st->in_fence = 0;
                    st->fence_ch = 0;
                    st->fence_run = 0;
                    st->mask = 0;
                    st->at_start = 1;
                    *pi = k + 1;
                    return 1;
                }
                if (k == n) {
                    return 0;
                }
            } else if (sp + run >= n) {
                return 0;
            }
        }
        if (p[i] == '\n') {
            dmeta(m, MDL_CODE, 0);
            daddc(m, '\n');
            st->mask = 0;
            st->at_start = 1;
            *pi = i + 1;
            return 1;
        }
        dmeta(m, MDL_CODE, 0);
        st->raw = 1;
        st->at_start = 0;
        return 1;
    }

    if (p[i] == '\n') {
        daddc(m, '\n');
        st->mask = 0;
        st->at_start = 1;
        *pi = i + 1;
        return 1;
    }

    size_t sp = i;
    while (sp < n && sp - i < 16 && p[sp] == ' ') {
        sp++;
    }
    size_t lead = sp - i;
    if (sp >= n) {
        return 0;
    }
    char c = p[sp];

    if (c == '\n') {
        dadd_raw(m, p + i, lead);
        daddc(m, '\n');
        st->mask = 0;
        st->at_start = 1;
        *pi = sp + 1;
        return 1;
    }

    if (c == '#' && lead <= 3) {
        size_t run = 0;
        while (sp + run < n && run < 7 && p[sp + run] == '#') {
            run++;
        }
        if (run > 0 && run <= 6) {
            size_t k = sp + run;
            if (k >= n) {
                return 0;
            }
            if (p[k] == ' ' || p[k] == '\n') {
                dmeta(m, MDL_HEAD, 0);
                dstyle(m, st, MD_ACCENT);
                st->at_start = 0;
                *pi = p[k] == ' ' ? k + 1 : k;
                return 1;
            }
        }
    }

    if (c == '>' && lead <= 3) {
        size_t k = i;
        int levels = 0;
        while (k < n && p[k] == '>') {
            levels++;
            k++;
            if (k >= n) {
                return 0;
            }
            if (p[k] == ' ') {
                k++;
            }
        }
        if (levels > 0) {
            dmeta(m, MDL_QUOTE, levels * 2);
            dstyle(m, st, MD_DIM);
            for (int l = 0; l < levels; l++) {
                if (st->unicode) {
                    dadd(m, "\xE2\x94\x82 ", 4);
                } else {
                    dadd(m, "| ", 2);
                }
            }
            dstyle(m, st, 0);
            st->at_start = 0;
            *pi = k;
            return 1;
        }
    }

    if ((c == '`' || c == '~') && lead <= 3) {
        size_t run = 0;
        while (sp + run < n && p[sp + run] == c) {
            run++;
        }
        if (run >= 3) {
            dmeta(m, MDL_FENCE_OPEN, 0);
            st->in_fence = 1;
            st->fence_ch = (unsigned char)c;
            st->fence_run = (unsigned char)(run > 255 ? 255 : run);
            st->raw = 1;
            st->at_start = 0;
            *pi = sp + run;
            return 1;
        }
        if (sp + run >= n) {
            return 0;
        }
    }

    if (c == '|') {
        int sep = table_sep(p, sp, n, &i);
        if (sep < 0) {
            return 0;
        }
        if (sep > 0) {
            dmeta(m, MDL_RULE, 0);
            daddc(m, '\n');
            st->mask = 0;
            st->at_start = 1;
            *pi = i + 1;
            return 1;
        }
        dmeta(m, MDL_TABLE, 0);
        st->table = 1;
        st->at_start = 0;
        *pi = sp;
        return 1;
    }

    if (c == '-' || c == '*' || c == '+' || c == '_') {
        size_t run = 0;
        while (sp + run < n && p[sp + run] == c) {
            run++;
        }
        if (sp + run >= n) {
            return 0;
        }
        if (run >= 3 && (c == '-' || c == '*' || c == '_')) {
            size_t k = sp + run;
            while (k < n && p[k] == ' ') {
                k++;
            }
            if (k >= n) {
                return 0;
            }
            if (p[k] == '\n') {
                dmeta(m, MDL_RULE, 0);
                daddc(m, '\n');
                st->mask = 0;
                st->at_start = 1;
                *pi = k + 1;
                return 1;
            }
        }
        if (c != '_' && sp + 1 < n && p[sp + 1] == ' ') {
            size_t k = sp + 1;
            while (k < n && p[k] == ' ') {
                k++;
            }
            if (k >= n) {
                return 0;
            }
            int task = 0;
            char mark = 0;
            size_t after = k;
            if (c == '-' && k + 3 < n && p[k] == '[' && p[k + 2] == ']' &&
                (p[k + 1] == ' ' || p[k + 1] == 'x' || p[k + 1] == 'X') &&
                (p[k + 3] == ' ' || p[k + 3] == '\n')) {
                task = 1;
                mark = p[k + 1];
                after = k + 3;
                if (after < n && p[after] == ' ') {
                    after++;
                }
            }
            int glyph = 2;
            if (task) {
                glyph = st->unicode ? 2 : 4;
            }
            dmeta(m, MDL_LIST, (int)(lead + (size_t)glyph));
            if (lead > 0) {
                dadd(m, p + i, lead);
            }
            dstyle(m, st, MD_ACCENT);
            if (task) {
                if (st->unicode) {
                    dadd(m, mark == ' ' ? "\xE2\x98\x90 " : "\xE2\x98\x91 ", 4);
                } else if (mark == ' ') {
                    dadd(m, "[ ] ", 4);
                } else {
                    dadd(m, "[x] ", 4);
                }
                *pi = after;
            } else {
                if (st->unicode) {
                    dadd(m, "\xE2\x80\xA2 ", 4);
                } else {
                    dadd(m, "- ", 2);
                }
                *pi = k;
            }
            dstyle(m, st, 0);
            st->at_start = 0;
            return 1;
        }
    }

    if (c >= '0' && c <= '9' && lead <= 3) {
        size_t k = sp;
        while (k < n && k - sp < 9 && p[k] >= '0' && p[k] <= '9') {
            k++;
        }
        if (k >= n) {
            return 0;
        }
        if ((p[k] == '.' || p[k] == ')') && k + 1 < n && p[k + 1] == ' ') {
            size_t after = k + 1;
            while (after < n && p[after] == ' ') {
                after++;
            }
            if (after >= n) {
                return 0;
            }
            size_t digits = k - sp;
            dmeta(m, MDL_LIST, (int)(lead + digits + 2));
            if (lead > 0) {
                dadd(m, p + i, lead);
            }
            dstyle(m, st, MD_ACCENT);
            dadd(m, p + sp, digits + 1);
            daddc(m, ' ');
            dstyle(m, st, 0);
            st->at_start = 0;
            *pi = after;
            return 1;
        }
    }

    st->at_start = 0;
    return 1;
}

static void md_process(Msg *m, MdState *st) {
    char *p = st->pend.p;
    size_t n = st->pend.len;
    size_t i = 0;

    while (i < n) {
        if (st->raw) {
            size_t j = i;
            while (j < n && p[j] != '\n') {
                j++;
            }
            if (j > i) {
                dadd_raw(m, p + i, j - i);
            }
            if (j >= n) {
                i = j;
                break;
            }
            daddc(m, '\n');
            st->mask = 0;
            st->raw = 0;
            st->at_start = 1;
            i = j + 1;
            continue;
        }
        if (st->at_start) {
            size_t before = i;
            int r = md_line_start(m, st, p, n, &i);
            if (r == 0) {
                if (n - i > MD_PEND_MAX) {
                    if (st->in_fence) {
                        dmeta(m, MDL_CODE, 0);
                        st->raw = 1;
                    }
                    st->at_start = 0;
                    continue;
                }
                break;
            }
            if (i == before && !st->at_start && !st->raw) {
                int nl = md_inline(m, st, p, n, &i);
                if (nl) {
                    continue;
                }
                if (i == before) {
                    break;
                }
            }
            continue;
        }
        size_t before = i;
        int nl = md_inline(m, st, p, n, &i);
        if (nl) {
            continue;
        }
        if (i == before) {
            break;
        }
    }

    if (i > 0) {
        memmove(p, p + i, n - i);
        st->pend.len = n - i;
    }
    if (!m->streaming && !st->flushed) {
        inline_flush(m, st);
        dstyle(m, st, 0);
        if (st->in_fence && st->at_start) {
            dmeta(m, MDL_FENCE_CLOSE, 0);
            daddc(m, '\n');
            st->in_fence = 0;
            st->fence_ch = 0;
            st->fence_run = 0;
        }
        st->flushed = 1;
    }
}

static void md_clear(Msg *m, MdState *st) {
    free(m->disp);
    m->disp = NULL;
    m->dlen = 0;
    m->dcap = 0;
    if (st) {
        buf_clear(&st->pend);
        buf_clear(&st->label);
        buf_clear(&st->url);
        st->src = 0;
        st->mask = 0;
        st->at_start = 1;
        st->in_fence = 0;
        st->fence_ch = 0;
        st->fence_run = 0;
        st->raw = 0;
        st->esc = 0;
        st->star = 0;
        st->under = 0;
        st->tilde = 0;
        st->link = 0;
        st->table = 0;
        st->prev = 0;
        st->flushed = 0;
    }
    m->lay_valid = 0;
}

size_t md_sync(Msg *m, int unicode) {
    if (!m || m->role != ROLE_ASSISTANT) {
        return 0;
    }
    MdState *st = m->md;
    if (!st) {
        st = xmalloc(sizeof *st);
        memset(st, 0, sizeof *st);
        buf_init(&st->pend);
        buf_init(&st->label);
        buf_init(&st->url);
        st->at_start = 1;
        st->unicode = (unsigned char)(unicode != 0);
        m->md = st;
    }
    unsigned want = unicode ? 1 : 0;
    if (m->dlen > 0 && st->unicode != want) {
        md_clear(m, st);
    }
    st->unicode = (unsigned char)want;

    /* A finished message gained text: its display ended with a synthetic
     * newline, so rebuild from raw instead of splitting the line. */
    if (st->flushed && m->len > st->src) {
        md_clear(m, st);
    }
    if (m->len < st->src) {
        md_clear(m, st);
    }
    if (m->len > st->src) {
        buf_add(&st->pend, m->text + st->src, m->len - st->src);
        st->src = m->len;
        st->flushed = 0;
    }
    /* A finished reply may end mid-line.  The synthetic newline lets the
     * line-start reader finish a fence, rule or table row; layout still
     * sees no extra row because a final newline ends the last row. */
    if (!m->streaming && !st->flushed) {
        int need = 0;
        if (st->pend.len > 0) {
            need = st->pend.p[st->pend.len - 1] != '\n';
        } else if (m->dlen > 0) {
            need = m->disp[m->dlen - 1] != '\n';
        }
        if (need) {
            buf_addc(&st->pend, '\n');
        }
    }
    md_process(m, st);
    return m->dlen;
}

void md_reset(Msg *m) {
    if (!m) {
        return;
    }
    md_clear(m, m->md);
}

void md_release(Msg *m) {
    if (!m) {
        return;
    }
    free(m->disp);
    m->disp = NULL;
    m->dlen = 0;
    m->dcap = 0;
    MdState *st = m->md;
    if (st) {
        buf_free(&st->pend);
        buf_free(&st->label);
        buf_free(&st->url);
        free(st);
        m->md = NULL;
    }
}

/* TODO(perf): the kind/hang/skip out-params force layout_message to keep
 * &hang and &skip on the stack for every line.  Return one packed value
 * instead and re-measure layout_message (272 B frame / 140 stack refs at
 * -O2 on 2026-11-18).  This is an address-taken-local cost, not a spill. */
int md_line_meta(const char *s, size_t len, int *hang, size_t *skip) {
    if (!s || len < 3) {
        return -1;
    }
    unsigned char a = (unsigned char)s[0];
    unsigned char b = (unsigned char)s[1];
    if (a != MD_CMD || !(b & MD_META)) {
        return -1;
    }
    int kind = b & 0x7F;
    if (kind > MDL_TABLE) {
        return -1;
    }
    if (hang) {
        *hang = (unsigned char)s[2];
    }
    if (skip) {
        *skip = 3;
    }
    return kind;
}
