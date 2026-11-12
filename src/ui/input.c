#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

void history_free(App *a) {
    for (int i = 0; i < a->nhist; i++) {
        free(a->hist[i]);
    }
    a->nhist = 0;
    free(a->draft);
    a->draft = NULL;
    a->hist_pos = -1;
}

static wchar_t *wcs_dup_n(const wchar_t *s, size_t n) {
    wchar_t *p = xmalloc((n + 1) * sizeof *p);
    memcpy(p, s, n * sizeof *p);
    p[n] = L'\0';
    return p;
}

void history_add(App *a, const wchar_t *s, size_t n) {
    if (n == 0) {
        return;
    }
    if (a->nhist == MAX_HISTORY) {
        free(a->hist[0]);
        memmove(a->hist, a->hist + 1,
                (MAX_HISTORY - 1) * sizeof *a->hist);
        a->nhist--;
    }
    a->hist[a->nhist++] = wcs_dup_n(s, n);
    a->hist_pos = -1;
    free(a->draft);
    a->draft = NULL;
}

static void input_set(App *a, const wchar_t *s, size_t n) {
    if (n >= MEONG_INPUT_MAX) {
        n = MEONG_INPUT_MAX - 1;
    }
    memcpy(a->input, s, n * sizeof *s);
    a->ilen = n;
    a->icur = n;
    a->dirty = 1;
}

void history_move(App *a, int dir) {
    if (a->nhist == 0) {
        return;
    }
    if (a->hist_pos < 0) {
        free(a->draft);
        a->draft = wcs_dup_n(a->input, a->ilen);
        a->hist_pos = a->nhist;
    }
    if (dir < 0) {
        if (a->hist_pos <= 0) {
            return;
        }
        a->hist_pos--;
    } else {
        if (a->hist_pos >= a->nhist) {
            return;
        }
        a->hist_pos++;
    }
    if (a->hist_pos == a->nhist) {
        if (a->draft) {
            input_set(a, a->draft, wcslen(a->draft));
        } else {
            a->ilen = 0;
            a->icur = 0;
        }
    } else {
        wchar_t *h = a->hist[a->hist_pos];
        input_set(a, h, wcslen(h));
    }
}

void input_insert(App *a, wchar_t wc) {
    if (a->ilen + 1 >= MEONG_INPUT_MAX) {
        return;
    }
    memmove(&a->input[a->icur + 1], &a->input[a->icur],
            (a->ilen - a->icur) * sizeof *a->input);
    a->input[a->icur++] = wc;
    a->ilen++;
    a->dirty = 1;
}

void input_backspace(App *a) {
    if (a->icur == 0) {
        return;
    }
    memmove(&a->input[a->icur - 1], &a->input[a->icur],
            (a->ilen - a->icur) * sizeof *a->input);
    a->icur--;
    a->ilen--;
    a->dirty = 1;
}

void input_del(App *a) {
    if (a->icur >= a->ilen) {
        return;
    }
    memmove(&a->input[a->icur], &a->input[a->icur + 1],
            (a->ilen - a->icur - 1) * sizeof *a->input);
    a->ilen--;
    a->dirty = 1;
}

void input_kill_word(App *a) {
    size_t end = a->icur;
    size_t start = a->icur;
    while (start > 0 && iswspace(a->input[start - 1])) {
        start--;
    }
    while (start > 0 && !iswspace(a->input[start - 1])) {
        start--;
    }
    memmove(&a->input[start], &a->input[end],
            (a->ilen - end) * sizeof *a->input);
    a->ilen -= end - start;
    a->icur = start;
    a->dirty = 1;
}

void input_reset(App *a) {
    a->ilen = 0;
    a->icur = 0;
    a->hist_pos = -1;
    free(a->draft);
    a->draft = NULL;
    a->dirty = 1;
}

void chat_scroll(App *a, int lines) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    int view_h = chat_height(a, rows);
    if (view_h < 1) {
        return;
    }
    int max_top = a->nlines > view_h ? a->nlines - view_h : 0;
    if (lines < 0) {
        if (a->follow) {
            return;
        }
        a->top += -lines;
        if (a->top >= max_top) {
            a->top = max_top;
            a->follow = 1;
            a->new_below = 0;
        }
    } else {
        if (a->follow) {
            if (max_top == 0) {
                return;
            }
            a->follow = 0;
            a->top = max_top;
        }
        a->top -= lines;
        if (a->top < 0) {
            a->top = 0;
        }
        if (a->top >= max_top) {
            a->top = max_top;
        }
    }
    a->dirty = 1;
}

void scroll_bottom(App *a) {
    a->follow = 1;
    a->new_below = 0;
    a->dirty = 1;
}

