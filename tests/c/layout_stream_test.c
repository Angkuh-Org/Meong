/*
 * Differential oracle for the streaming layout cursor.  One App is rebuilt
 * incrementally after every append; the other is rebuilt from scratch with
 * the same text.  Every RLine field must match, including cursor and head,
 * because both are stateful and a stale tail row is invisible on screen.
 */
#include "app/app.h"
#include "ui/ui.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "view.c"

static int fails;
static int step;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL step %d: %s\n", step, what);
        fails++;
    }
}

static void rows_match(App *inc, App *full) {
    if (inc->nlines != full->nlines) {
        printf("FAIL step %d: rows inc=%d full=%d\n", step, inc->nlines,
               full->nlines);
        fails++;
        return;
    }
    for (int i = 0; i < inc->nlines; i++) {
        RLine *a = &inc->lines[i];
        RLine *b = &full->lines[i];
        if (a->type != b->type || a->msg != b->msg || a->off != b->off ||
            a->len != b->len || a->cont != b->cont ||
            a->cursor != b->cursor || a->head != b->head) {
            printf("FAIL step %d row %d: inc[t=%d m=%d off=%d len=%d c=%d "
                   "cur=%d h=%d] full[t=%d m=%d off=%d len=%d c=%d cur=%d "
                   "h=%d]\n",
                   step, i, a->type, a->msg, a->off, a->len, a->cont,
                   a->cursor, a->head, b->type, b->msg, b->off, b->len,
                   b->cont, b->cursor, b->head);
            fails++;
            return;
        }
    }
}

static void new_app(App *a, const char *reason) {
    memset(a, 0, sizeof *a);
    a->layout_width = 42;
    a->gen_idx = msg_add(a, ROLE_ASSISTANT, "");
    a->msgs[a->gen_idx].streaming = 1;
    if (reason && *reason) {
        msg_append_reason(&a->msgs[a->gen_idx], reason, strlen(reason));
    }
}

static void layout_inc(App *a, int first) {
    a->layout_from = first ? 0 : a->gen_idx;
    a->layout_dirty = 1;
    layout_build(a);
}

static void layout_full(App *a) {
    a->msgs[a->gen_idx].lay_valid = 0;
    a->layout_from = 0;
    a->layout_dirty = 1;
    layout_build(a);
}

static void sync_layout(App *inc, App *full, int first) {
    step++;
    layout_inc(inc, first);
    layout_full(full);
    rows_match(inc, full);
}

static void append_both(App *inc, App *full, const char *s, int reason) {
    size_t n = strlen(s);
    if (reason) {
        msg_append_reason(&inc->msgs[inc->gen_idx], s, n);
        msg_append_reason(&full->msgs[full->gen_idx], s, n);
    } else {
        msg_append(&inc->msgs[inc->gen_idx], s, n);
        msg_append(&full->msgs[full->gen_idx], s, n);
    }
    sync_layout(inc, full, 0);
}

static void finish_both(App *inc, App *full) {
    trim_trailing_ws(&inc->msgs[inc->gen_idx]);
    trim_trailing_ws(&full->msgs[full->gen_idx]);
    inc->msgs[inc->gen_idx].streaming = 0;
    full->msgs[full->gen_idx].streaming = 0;
    sync_layout(inc, full, 1);
}

static void free_app(App *a) {
    free(a->lines);
    free(a->msgs[0].text);
    free(a->msgs[0].reason);
    free(a->msgs);
}

static void case_text(void) {
    static const char *chunks[] = {
        "hello", " world", "\n", "second line with spaces", "\n",
        "```\n", "int x = 1;\n", "```\n", "tail\twith tab", "\n\n",
        "wide \xF0\x9F\x8C\x8A and \xC3\xA9", "\n", "end",
    };
    App inc;
    App full;
    new_app(&inc, NULL);
    new_app(&full, NULL);
    sync_layout(&inc, &full, 1);
    for (unsigned i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        append_both(&inc, &full, chunks[i], 0);
    }
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

static void case_long_line(void) {
    App inc;
    App full;
    new_app(&inc, NULL);
    new_app(&full, NULL);
    sync_layout(&inc, &full, 1);
    for (int i = 0; i < 1500; i++) {
        append_both(&inc, &full, "abc def ghij", 0);
    }
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

static void case_reason_then_text(void) {
    App inc;
    App full;
    new_app(&inc, "thinking a little ");
    new_app(&full, "thinking a little ");
    sync_layout(&inc, &full, 1);
    append_both(&inc, &full, "more reasoning\n", 1);
    append_both(&inc, &full, "answer starts", 0);
    append_both(&inc, &full, " here", 0);
    append_both(&inc, &full, "\nnext line", 0);
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

static void case_interleaved(void) {
    App inc;
    App full;
    new_app(&inc, NULL);
    new_app(&full, NULL);
    sync_layout(&inc, &full, 1);
    append_both(&inc, &full, "first answer ", 0);
    append_both(&inc, &full, "a thought\n", 1);
    append_both(&inc, &full, "more answer", 0);
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

static void case_empty_then_text(void) {
    App inc;
    App full;
    new_app(&inc, NULL);
    new_app(&full, NULL);
    sync_layout(&inc, &full, 1);
    sync_layout(&inc, &full, 0);
    append_both(&inc, &full, "x", 0);
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

static void case_fence_growth(void) {
    App inc;
    App full;
    new_app(&inc, NULL);
    new_app(&full, NULL);
    sync_layout(&inc, &full, 1);
    append_both(&inc, &full, "``", 0);
    append_both(&inc, &full, "`\n", 0);
    append_both(&inc, &full, "code", 0);
    append_both(&inc, &full, "\n", 0);
    finish_both(&inc, &full);
    free_app(&inc);
    free_app(&full);
}

int main(void) {
    case_text();
    case_long_line();
    case_reason_then_text();
    case_interleaved();
    case_empty_then_text();
    case_fence_growth();

    printf(fails ? "FAILED: %d layout stream check(s)\n"
                 : "LAYOUT STREAM CHECKS OK\n",
           fails);
    return fails != 0;
}
