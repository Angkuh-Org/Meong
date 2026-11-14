#include "ui/ui.h"

#include <limits.h>
#include <stdio.h>

/* The pet card stays ASCII, so byte offsets are safe for mvaddnstr. */
static int pet_wrap(int *y, int ymax, int x, int width, const char *s,
                    int draw) {
    int start = *y;
    while (*s && *y < ymax) {
        int n = 0;
        int split = -1;
        while (s[n] && n < width) {
            if (s[n] == ' ') {
                split = n;
            }
            n++;
        }
        int done = s[n] == '\0';
        if (!done && split > 0) {
            n = split;
        }
        if (draw) {
            mvaddnstr(*y, x, s, n);
        }
        if (done) {
            (*y)++;
            break;
        }
        s += n;
        while (*s == ' ') {
            s++;
        }
        (*y)++;
    }
    return *y - start;
}

static int pet_stat(App *a, int y, int ymax, int x, int width,
                    const char *label, const char *value) {
    if (y >= ymax) {
        return y;
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    mvaddstr(y, x, label);
    wattrset(stdscr, pair_attr(a, P_PANEL));
    mvaddnstr(y, x + 9, value, width - 9);
    return y + 1;
}

void pet_draw(App *a, int rows, int cols) {
    static const char *quips[] = {
        "purrr... jangan berhenti.",
        "meow. aku dengerin kok.",
        "kucing juga butuh dihargai.",
        "kalau jawabanku aneh, itu modelnya.",
        "token itu ikan buat kucing digital.",
        "elus lagi. aku belum puas.",
        "aku bukan asisten, aku kucing.",
        "jangan lupa /clear kalau bingung.",
        "ctrl-l kalau layar kacau.",
        "meong. (artinya: lanjut.)",
        "kamu ngetik, aku dengerin.",
        "purr. ini hidupku sekarang.",
    };
    static const char *busy[] = {
        "lagi mikir... jangan diganggu.",
        "ssst. kucing sedang konsentrasi.",
        "aku dengerin kok, sabar.",
    };
    const char *cat[3] = {
        " /\\_/\\ ",
        a->streaming ? "( -.- )" : "( ^.^ )",
        " > ^ < ",
    };

    char dyn[96];
    const char *reaction;
    int pets = a->pet_count > 0 ? a->pet_count : 1;
    if (a->streaming) {
        reaction = busy[(pets - 1) % (int)(sizeof busy / sizeof busy[0])];
    } else if (a->nqueued > 0) {
        snprintf(dyn, sizeof dyn, "ada %d pesan ngantre. sabar.", a->nqueued);
        reaction = dyn;
    } else if (pets % 5 == 0) {
        snprintf(dyn, sizeof dyn, "kamu udah elus aku %dx. aku catat.",
                 pets);
        reaction = dyn;
    } else {
        reaction = quips[(pets - 1) % (int)(sizeof quips / sizeof quips[0])];
    }

    int asked = 0;
    long tin = 0;
    long tout = 0;
    for (int i = 0; i < a->nmsg; i++) {
        Msg *m = &a->msgs[i];
        if (m->role == ROLE_USER) {
            asked++;
        } else if (m->role == ROLE_ASSISTANT && m->has_stats) {
            tin += m->ptok;
            tout += m->ctok;
        }
    }
    char tok[96];
    if (tin > 0 || tout > 0) {
        char in[32];
        char out[32];
        fmt_count(tin, in, sizeof in);
        fmt_count(tout, out, sizeof out);
        snprintf(tok, sizeof tok, "%s in / %s out", in, out);
    } else {
        snprintf(tok, sizeof tok, "-");
    }
    char asked_s[24];
    char pet_s[24];
    snprintf(asked_s, sizeof asked_s, "%d", asked);
    snprintf(pet_s, sizeof pet_s, "%dx", pets);

    const char *mood;
    if (a->streaming) {
        mood = "lagi mikir";
    } else if (a->nqueued > 0) {
        mood = "sibuk";
    } else if (a->last_failed) {
        mood = "ngambek";
    } else if (asked == 0) {
        mood = "nganggur";
    } else {
        mood = "santai";
    }

    int w = cols - 2;
    if (w > 56) {
        w = 56;
    }
    if (w < 30) {
        return;
    }
    int width = w - 6;
    int yy = 0;
    int h = 12 + pet_wrap(&yy, INT_MAX, 0, width, reaction, 0);
    if (h > rows - 2) {
        h = rows - 2;
    }
    if (h < 8) {
        return;
    }
    int px = (cols - w) / 2;
    int py = (rows - h) / 2;
    if (py + h - 1 >= rows - 4) {
        py = 1;
        h = rows - 2;
    }
    a->pet_y0 = py;
    a->pet_y1 = py + h - 1;
    a->pet_x0 = px;
    a->pet_x1 = px + w - 1;
    panel_frame(a, px, py, w, h, NULL, 0);

    int x = px + 3;
    int y = py + 1;
    int ymax = py + h - 1;
    wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    int catx = px + (w - 7) / 2;
    for (int i = 0; i < 3 && y < ymax; i++, y++) {
        mvaddstr(y, catx, cat[i]);
    }
    if (y < ymax) {
        y++;
    }
    if (y < ymax) {
        wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT));
        pet_wrap(&y, ymax, x, width, reaction, 1);
    }
    if (y + 1 < ymax) {
        y++;
    }
    y = pet_stat(a, y, ymax, x, width, "dielus", pet_s);
    y = pet_stat(a, y, ymax, x, width, "mood", mood);
    y = pet_stat(a, y, ymax, x, width, "model", a->model);
    y = pet_stat(a, y, ymax, x, width, "pesan", asked_s);
    y = pet_stat(a, y, ymax, x, width, "token", tok);
    a->cursor_on = 0;
}
