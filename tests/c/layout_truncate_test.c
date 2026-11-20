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

static void app_free(App *a) {
    for (int i = 0; i < a->nmsg; i++) {
        msg_release(&a->msgs[i]);
    }
    free(a->msgs);
    free(a->lines);
    memset(a, 0, sizeof *a);
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
    msg_release(&a.msgs[3]);
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
    app_free(&a);

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
    msg_release(&r.msgs[3]);
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
    app_free(&r);

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
    app_free(&s);

    /* Tool rows: the rail state comes from (parent, pos), not row order. */
    App t;
    memset(&t, 0, sizeof t);
    t.layout_width = 40;
    msg_add(&t, ROLE_USER, "tools");
    int ta = msg_add(&t, ROLE_ASSISTANT, "");
    t.msgs[ta].ncalls = 2;
    t.msgs[ta].calls = xcalloc(2, sizeof *t.msgs[ta].calls);
    int t1 = msg_add(&t, ROLE_TOOL, "one\ntwo\nthree");
    t.msgs[t1].tool_parent = ta;
    t.msgs[t1].tool_pos = 0;
    t.msgs[t1].tool_state = TOOL_VIEW_OK;
    int t2 = msg_add(&t, ROLE_TOOL, "four");
    t.msgs[t2].tool_parent = ta;
    t.msgs[t2].tool_pos = 1;
    t.msgs[t2].tool_state = TOOL_VIEW_OK;
    layout_build(&t);
    check(!tool_is_last(&t, &t.msgs[t1]), "first call is marked last");
    check(tool_is_last(&t, &t.msgs[t2]), "second call is not marked last");
    int heads = 0;
    int boxes = 0;
    for (int i = 0; i < t.nlines; i++) {
        if (t.lines[i].type == RL_TOOL_HEAD) {
            heads++;
        }
        if (t.lines[i].type >= RL_TOOL_TOP &&
            t.lines[i].type <= RL_TOOL_BOT) {
            boxes++;
        }
    }
    check(heads == 2, "collapsed tool calls do not have two head rows");
    check(boxes == 0, "collapsed tool calls leaked box rows");

    t.msgs[t1].tool_open = 1;
    t.layout_from = t1;
    layout_build(&t);
    heads = 0;
    boxes = 0;
    for (int i = 0; i < t.nlines; i++) {
        RLine *l = &t.lines[i];
        if (l->type == RL_TOOL_HEAD) {
            heads++;
        }
        if (l->type >= RL_TOOL_TOP && l->type <= RL_TOOL_BOT) {
            boxes++;
            check(l->msg == t1, "box row points at the wrong tool call");
        }
        if (l->type == RL_TOOL_BODY) {
            Msg *m = &t.msgs[l->msg];
            size_t n = m->text ? strlen(m->text) : 0;
            check(l->off >= 0, "tool body offset is negative");
            check((size_t)(l->off + l->len) <= n,
                  "tool body points past the result text");
            check(text_cells(m->text + l->off, l->len) <= t.layout_width - 5,
                  "tool body row exceeds the box width");
        }
    }
    check(heads == 2, "expanding one call lost the other head row");
    check(boxes >= 3, "expanded tool call has no box rows");
    app_free(&t);

    /* Folded thinking with no answer text attaches tools to the head row. */
    App h;
    memset(&h, 0, sizeof h);
    h.layout_width = 40;
    msg_add(&h, ROLE_USER, "q");
    int ha = msg_add(&h, ROLE_ASSISTANT, "");
    h.msgs[ha].reason = xstrdup("why");
    h.msgs[ha].rlen = 3;
    h.msgs[ha].reason_collapsed = 1;
    h.msgs[ha].has_stats = 1;
    h.msgs[ha].ctok = 97;
    h.msgs[ha].elapsed = 3.0;
    h.msgs[ha].ncalls = 1;
    h.msgs[ha].calls = xcalloc(1, sizeof *h.msgs[ha].calls);
    int h1 = msg_add(&h, ROLE_TOOL, "out");
    h.msgs[h1].tool_parent = ha;
    h.msgs[h1].tool_pos = 0;
    h.msgs[h1].tool_state = TOOL_VIEW_OK;
    layout_build(&h);
    int head_row = -1;
    int tool_row = -1;
    for (int i = 0; i < h.nlines; i++) {
        if (h.lines[i].type == RL_REASON_HEAD && head_row < 0) {
            head_row = i;
        }
        if (h.lines[i].type == RL_TOOL_HEAD && tool_row < 0) {
            tool_row = i;
        }
    }
    check(head_row >= 0, "folded thinking head row is missing");
    check(tool_row == head_row + 1,
          "folded thinking left a blank row before tools");

    h.msgs[ha].reason_collapsed = 0;
    h.layout_from = 0;
    h.layout_dirty = 1;
    layout_build(&h);
    int marker_row = -1;
    int reason_row = -1;
    tool_row = -1;
    for (int i = 0; i < h.nlines; i++) {
        if (h.lines[i].type == RL_TEXT && h.lines[i].msg == ha &&
            h.lines[i].head) {
            marker_row = i;
        }
        if (h.lines[i].type == RL_REASON && h.lines[i].msg == ha) {
            reason_row = i;
        }
        if (h.lines[i].type == RL_TOOL_HEAD && tool_row < 0) {
            tool_row = i;
        }
    }
    check(marker_row >= 0, "expanded thinking lost its tool-call marker");
    check(reason_row >= 0 && h.lines[reason_row].close,
          "last reasoning row is not closed with a corner");
    check(tool_row == marker_row + 1,
          "expanded thinking left a blank row after the tool-call marker");
    app_free(&h);

    /* A live reasoning-only stream has no empty placeholder or gap. */
    App r2;
    memset(&r2, 0, sizeof r2);
    r2.layout_width = 40;
    msg_add(&r2, ROLE_USER, "q");
    int ra = msg_add(&r2, ROLE_ASSISTANT, "");
    r2.msgs[ra].reason = xstrdup("why");
    r2.msgs[ra].rlen = 3;
    r2.streaming = 1;
    r2.gen_idx = ra;
    layout_build(&r2);
    int texts = 0;
    int gaps = 0;
    for (int i = 0; i < r2.nlines; i++) {
        if (r2.lines[i].msg != ra) {
            continue;
        }
        if (r2.lines[i].type == RL_TEXT) {
            texts++;
        }
        if (r2.lines[i].type == RL_GAP) {
            gaps++;
        }
    }
    check(texts == 0, "streaming reasoning kept an empty text row");
    check(gaps == 0, "streaming reasoning kept a blank gap row");
    r2.streaming = 0;
    r2.layout_from = 0;
    layout_build(&r2);
    gaps = 0;
    for (int i = 0; i < r2.nlines; i++) {
        if (r2.lines[i].msg == ra && r2.lines[i].type == RL_GAP) {
            gaps++;
        }
    }
    check(gaps == 1, "finished reasoning lost its stats gap");
    app_free(&r2);

    /* A tool round rebuilds from the user turn, not the last tool row. */
    App g;
    memset(&g, 0, sizeof g);
    g.layout_width = 40;
    msg_add(&g, ROLE_USER, "q");
    int ga = msg_add(&g, ROLE_ASSISTANT, "");
    g.msgs[ga].reason = xstrdup("why");
    g.msgs[ga].rlen = 3;
    g.msgs[ga].has_stats = 1;
    g.msgs[ga].ctok = 97;
    g.msgs[ga].elapsed = 3.0;
    layout_build(&g);
    g.msgs[ga].ncalls = 1;
    g.msgs[ga].calls = xcalloc(1, sizeof *g.msgs[ga].calls);
    int gt = msg_add(&g, ROLE_TOOL, "out");
    g.msgs[gt].tool_parent = ga;
    g.msgs[gt].tool_pos = 0;
    g.msgs[gt].tool_state = TOOL_VIEW_OK;
    g.layout_from = 0;
    layout_build(&g);
    marker_row = -1;
    tool_row = -1;
    for (int i = 0; i < g.nlines; i++) {
        if (g.lines[i].type == RL_TEXT && g.lines[i].msg == ga &&
            g.lines[i].head) {
            marker_row = i;
        }
        if (g.lines[i].type == RL_TOOL_HEAD && tool_row < 0) {
            tool_row = i;
        }
    }
    check(marker_row >= 0, "rebuilt tool round lost its marker");
    check(tool_row == marker_row + 1,
          "stale gap survived the tool-round rebuild");
    app_free(&g);

    /* Reasoning keeps its own line structure: paragraphs breathe, a bullet
     * hangs its continuation, and an ATX marker is not shown. */
    App rs;
    memset(&rs, 0, sizeof rs);
    rs.layout_width = 20;
    msg_add(&rs, ROLE_USER, "q");
    int rsa = msg_add(&rs, ROLE_ASSISTANT, "");
    rs.msgs[rsa].reason = xstrdup(
        "first thought\n\n\n- alpha beta gamma delta\n\n## Step 1\nbody\n\n");
    rs.msgs[rsa].rlen = strlen(rs.msgs[rsa].reason);
    layout_build(&rs);
    int rrows = 0;
    int rblank = 0;
    int rhang = 0;
    int rclose = -1;
    int rlast = -1;
    int rhead = -1;
    for (int i = 0; i < rs.nlines; i++) {
        RLine *l = &rs.lines[i];
        if (l->msg != rsa || l->type != RL_REASON) {
            continue;
        }
        int rlen = (int)rs.msgs[rsa].rlen;
        check(l->off >= 0 && l->off + l->len <= rlen,
              "reasoning row points past the reasoning text");
        rrows++;
        rlast = i;
        if (l->len == 0) {
            rblank++;
        }
        if (l->hang > 0) {
            rhang = l->hang;
        }
        if (l->close) {
            rclose = i;
        }
        if (l->len == 6 &&
            memcmp(rs.msgs[rsa].reason + l->off, "Step 1", 6) == 0) {
            rhead = i;
            check(l->style == MD_BOLD, "heading row lost its bold style");
        }
    }
    check(rrows == 7, "reasoning lost or joined its source lines");
    check(rblank == 2, "reasoning collapsed or kept every blank line");
    check(rhang == 2, "bullet continuation is not hanging indented");
    check(rhead >= 0, "ATX marker was not stripped from a reasoning row");
    check(rlast >= 0 && rclose == rlast,
          "reasoning close corner is not on the last row");
    app_free(&rs);

    /* A nested quote hangs its continuation under both markers. */
    App rq;
    memset(&rq, 0, sizeof rq);
    rq.layout_width = 16;
    msg_add(&rq, ROLE_USER, "q");
    int rqa = msg_add(&rq, ROLE_ASSISTANT, "");
    rq.msgs[rqa].reason = xstrdup("> > alpha beta gamma delta");
    rq.msgs[rqa].rlen = strlen(rq.msgs[rqa].reason);
    layout_build(&rq);
    int qrows = 0;
    int qhang = 0;
    for (int i = 0; i < rq.nlines; i++) {
        if (rq.lines[i].msg == rqa && rq.lines[i].type == RL_REASON) {
            qrows++;
            if (rq.lines[i].hang > 0) {
                qhang = rq.lines[i].hang;
            }
        }
    }
    check(qrows == 2 && qhang == 4,
          "nested quote did not hang under both markers");
    app_free(&rq);

    printf(fails ? "FAILED: %d layout check(s)\n" : "LAYOUT CHECKS OK\n",
           fails);
    return fails != 0;
}
