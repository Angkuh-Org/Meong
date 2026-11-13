/*
 * Measures the streaming layout path.  Built against the baseline tree it
 * re-wraps the whole message per append; built against the cursor tree it
 * re-wraps only the tail row.  The harness only uses the public App/Msg APIs
 * plus layout_build(), so the same source compiles on both trees.
 */
#include "app/app.h"
#include "ui/ui.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "view.c"

static void fill(char *buf, int n, int newlines, size_t *off) {
    static const char pat[] =
        "lorem ipsum dolor sit amet consectetur adipiscing elit sed do\n"
        "eiusmod tempor incididunt ut labore et dolore magna aliqua\n";
    size_t plen = sizeof pat - 1;
    for (int i = 0; i < n; i++) {
        if (newlines) {
            buf[i] = pat[*off];
            *off = (*off + 1) % plen;
        } else {
            buf[i] = 'x';
        }
    }
}

static void bench_stream(int bytes, int chunk, int newlines) {
    App a;
    memset(&a, 0, sizeof a);
    a.layout_width = 100;
    a.gen_idx = msg_add(&a, ROLE_ASSISTANT, "");
    a.msgs[a.gen_idx].streaming = 1;
    char buf[512];
    size_t off = 0;
    int done = 0;
    int steps = 0;
    double t0 = now_sec();
    while (done < bytes) {
        int n = bytes - done < chunk ? bytes - done : chunk;
        fill(buf, n, newlines, &off);
        if (msg_append(&a.msgs[a.gen_idx], buf, (size_t)n) != 0) {
            fprintf(stderr, "append failed at %d\n", done);
            exit(1);
        }
        a.layout_from = steps == 0 ? 0 : a.gen_idx;
        a.layout_dirty = 1;
        layout_build(&a);
        done += n;
        steps++;
    }
    double t1 = now_sec();
    printf("%s bytes=%d chunk=%d steps=%d rows=%d ms=%.3f us_per_step=%.2f\n",
           newlines ? "mixed" : "single", bytes, chunk, steps, a.nlines,
           (t1 - t0) * 1000.0, (t1 - t0) * 1e6 / steps);
    free(a.lines);
    free(a.msgs[0].text);
    free(a.msgs);
}

static void bench_full(int bytes, int newlines) {
    char *text = xmalloc((size_t)bytes + 1);
    size_t off = 0;
    fill(text, bytes, newlines, &off);
    text[bytes] = '\0';
    App a;
    memset(&a, 0, sizeof a);
    a.layout_width = 100;
    msg_add(&a, ROLE_ASSISTANT, text);
    double t0 = now_sec();
    layout_build(&a);
    double t1 = now_sec();
    printf("%s-full bytes=%d rows=%d ms=%.3f\n",
           newlines ? "mixed" : "single", bytes, a.nlines,
           (t1 - t0) * 1000.0);
    free(text);
    free(a.lines);
    free(a.msgs[0].text);
    free(a.msgs);
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "stream") == 0) {
        bench_stream(atoi(argv[2]), atoi(argv[3]), 0);
    } else if (argc == 4 && strcmp(argv[1], "mixed") == 0) {
        bench_stream(atoi(argv[2]), atoi(argv[3]), 1);
    } else if (argc == 3 && strcmp(argv[1], "full") == 0) {
        bench_full(atoi(argv[2]), 0);
    } else if (argc == 3 && strcmp(argv[1], "full-mixed") == 0) {
        bench_full(atoi(argv[2]), 1);
    } else {
        fprintf(stderr,
                "usage: %s stream|mixed <bytes> <chunk>\n"
                "       %s full|full-mixed <bytes>\n",
                argv[0], argv[0]);
        return 2;
    }
    return 0;
}
