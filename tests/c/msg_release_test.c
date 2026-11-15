/*
 * msg_release() owns both heap strings on a Msg and must zero the size
 * bookkeeping, so the previously latent failure branches can call it twice
 * (or on an already-empty Msg) without a double free or stale lengths.
 */
#include "app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

int main(void) {
    Msg m;
    memset(&m, 0, sizeof m);
    m.role = ROLE_ASSISTANT;
    m.text = malloc(16);
    m.reason = malloc(8);
    check(m.text != NULL && m.reason != NULL, "test allocation");
    if (!m.text || !m.reason) {
        return 1;
    }
    memcpy(m.text, "hello", 6);
    memcpy(m.reason, "why", 4);
    m.len = 5;
    m.cap = 16;
    m.chars = 5;
    m.rlen = 3;
    m.rcap = 8;
    m.lay_valid = 1;
    m.lay_off = 4;
    m.lay_row = 2;

    msg_release(&m);
    check(m.text == NULL, "text was not released");
    check(m.reason == NULL, "reason was not released");
    check(m.len == 0 && m.cap == 0 && m.chars == 0,
          "text bookkeeping survived");
    check(m.rlen == 0 && m.rcap == 0, "reason bookkeeping survived");
    check(m.lay_valid == 0, "layout cache flag survived");

    msg_release(&m);
    check(m.text == NULL && m.reason == NULL, "second release resurrected");
    check(m.len == 0 && m.cap == 0 && m.chars == 0 && m.rlen == 0 &&
              m.rcap == 0 && m.lay_valid == 0,
          "second release dirtied the counters");

    printf(fails ? "FAILED: %d msg_release check(s)\n" : "MSG_RELEASE OK\n",
           fails);
    return fails != 0;
}
