/*
 * Locks the incremental layout contract: dropping messages (as /retry does)
 * must drop their rows, and the next append must lay out only the new one.
 */
#include "app/app.h"
#include "ui/ui.h"
#include "util/util.h"

#include <stdio.h>
#include <string.h>

#include "view.c"

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

int main(void) {
    App a;
    memset(&a, 0, sizeof a);
    a.layout_width = 40;
    msg_add(&a, ROLE_USER, "first question");
    msg_add(&a, ROLE_ASSISTANT,
            "first answer that wraps over a few lines on a narrow terminal "
            "because it is deliberately long");
    msg_add(&a, ROLE_USER, "second question");
    msg_add(&a, ROLE_ASSISTANT, "second answer");
    layout_build(&a);
    check(a.nlines > 0, "initial layout produced rows");
    check(a.layout_from == a.nmsg, "layout_from covers every message");

    /* /retry truncates to the last user message (index 2) and re-lays out. */
    a.nmsg = 3;
    a.layout_dirty = 1;
    layout_build(&a);
    check(a.layout_from == 3, "truncation moved layout_from to nmsg");
    for (int i = 0; i < a.nlines; i++) {
        check(a.lines[i].msg < 3,
              "truncation left a row for a dropped message");
    }

    /* The regenerated assistant message must appear on the next pass. */
    msg_add(&a, ROLE_ASSISTANT, "regenerated answer");
    a.layout_dirty = 1;
    layout_build(&a);
    int seen = 0;
    for (int i = 0; i < a.nlines; i++) {
        check(a.lines[i].msg < a.nmsg, "row points past nmsg");
        if (a.lines[i].msg == 3) {
            seen++;
            check(a.lines[i].off >= 0, "row offset is negative");
            check(a.lines[i].off + a.lines[i].len <=
                      (int)strlen(a.msgs[3].text),
                  "row extends past the message text");
        }
    }
    check(seen > 0, "regenerated message has rows");
    check(a.layout_from == a.nmsg, "layout_from covers the append");

    printf(fails ? "FAILED: %d layout check(s)\n" : "LAYOUT CHECKS OK\n",
           fails);
    return fails != 0;
}
