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

    /* Real /retry truncates and calls msg_add before the next layout. */
    App r;
    memset(&r, 0, sizeof r);
    r.layout_width = 40;
    msg_add(&r, ROLE_USER, "first question");
    msg_add(&r, ROLE_ASSISTANT,
            "first answer that wraps over a few lines on a narrow terminal "
            "because it is deliberately long");
    msg_add(&r, ROLE_USER, "second question");
    msg_add(&r, ROLE_ASSISTANT, "second answer");
    layout_build(&r);
    free(r.msgs[3].text);
    free(r.msgs[3].reason);
    r.nmsg = 3;
    int ridx = msg_add(&r, ROLE_ASSISTANT, "");
    r.layout_from = ridx > 0 ? ridx - 1 : 0;
    r.layout_dirty = 1;
    layout_build(&r);
    for (int i = 0; i < r.nlines; i++) {
        check(r.lines[i].msg < r.nmsg, "retry row points past nmsg");
        Msg *m = &r.msgs[r.lines[i].msg];
        size_t n = m->text ? strlen(m->text) : 0;
        check(r.lines[i].off >= 0, "retry row offset is negative");
        check((size_t)(r.lines[i].off + r.lines[i].len) <= n,
              "retry row points into freed text");
    }
    check(r.layout_from == r.nmsg, "retry layout covers the new message");
    free(r.lines);
    for (int i = 0; i < r.nmsg; i++) {
        free(r.msgs[i].text);
        free(r.msgs[i].reason);
    }
    free(r.msgs);

    /* Normal send_text: user msg_add, assistant msg_add, layout_from=user. */
    App s;
    memset(&s, 0, sizeof s);
    s.layout_width = 40;
    int uidx = msg_add(&s, ROLE_USER, "go");
    int aidx = msg_add(&s, ROLE_ASSISTANT, "");
    s.layout_from = aidx > 0 ? aidx - 1 : 0;
    s.layout_dirty = 1;
    layout_build(&s);
    check(s.lines[0].msg == uidx, "send_text user row missing");
    int seen_user = 0;
    for (int i = 0; i < s.nlines; i++) {
        check(s.lines[i].msg < s.nmsg, "send_text row points past nmsg");
        Msg *m = &s.msgs[s.lines[i].msg];
        size_t n = m->text ? strlen(m->text) : 0;
        check(s.lines[i].off >= 0, "send_text row offset is negative");
        check((size_t)(s.lines[i].off + s.lines[i].len) <= n,
              "send_text row points past message text");
        if (s.lines[i].msg == uidx) {
            seen_user = 1;
        }
    }
    check(seen_user, "send_text user message owns a row");
    free(s.lines);
    for (int i = 0; i < s.nmsg; i++) {
        free(s.msgs[i].text);
        free(s.msgs[i].reason);
    }
    free(s.msgs);

    printf(fails ? "FAILED: %d layout check(s)\n" : "LAYOUT CHECKS OK\n",
           fails);
    return fails != 0;
}
