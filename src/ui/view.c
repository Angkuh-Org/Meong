#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

enum {
    RL_HEAD_E,
    RL_TEXT,
    RL_CODE,
    RL_FENCE,
    RL_REASON,
    RL_REASON_HEAD,
    RL_NOTE,
    RL_GAP,
};


struct RLine {
    int type;
    int msg;
    int off;
    int len;
    unsigned char cont;
    unsigned char cursor;
    unsigned char head;
};

static void line_push(App *a, int type, int msg, int off, int len, int cont) {
    if (a->nlines == a->linecap) {
        a->linecap = a->linecap ? a->linecap * 2 : 128;
        a->lines = xrealloc(a->lines, (size_t)a->linecap * sizeof *a->lines);
    }
    RLine *l = &a->lines[a->nlines++];
    l->type = type;
    l->msg = msg;
    l->off = off;
    l->len = len;
    l->cont = (unsigned char)(cont != 0);
    l->cursor = 0;
    l->head = 0;
}

static int line_is_fence(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    return i + 3 <= len && memcmp(s + i, "```", 3) == 0;
}

static void wrap_para(App *a, int mi, const char *s, size_t len, size_t base,
                      int type, int width, int cont_start) {
    if (width < 8) {
        width = 8;
    }
    if (len == 0) {
        line_push(a, type, mi, (int)base, 0, cont_start);
        return;
    }
    size_t pos = 0;
    int cont = cont_start;
    while (pos < len) {
        size_t start = pos;
        size_t i = pos;
        int col = 0;
        size_t last_space = (size_t)-1;
        while (i < len) {
            unsigned char ch = (unsigned char)s[i];
            if (ch >= 32 && ch < 127) {
                int room = width - col;
                size_t j = i;
                while (j < len && room > 0) {
                    unsigned char b = (unsigned char)s[j];
                    if (b < 32 || b >= 127) {
                        break;
                    }
                    if (b == ' ' && col + (int)(j - i) + 1 < width) {
                        last_space = j;
                    }
                    j++;
                    room--;
                }
                col += (int)(j - i);
                i = j;
                if (i < len && room == 0) {
                    break;
                }
                continue;
            }
            wchar_t wc;
            int bytes = utf8_next(s + i, len - i, &wc);
            if (bytes <= 0) {
                break;
            }
            if (wc == '\r') {
                i += (size_t)bytes;
                continue;
            }
            int cw;
            if (wc == '\t') {
                cw = 4 - (col % 4);
            } else {
                cw = wcwidth(wc);
                if (cw < 0) {
                    cw = 1;
                }
            }
            if (cw > 0 && col + cw > width) {
                break;
            }
            if (wc == ' ' && col + cw < width) {
                last_space = i;
            }
            col += cw;
            i += (size_t)bytes;
        }
        size_t end = i;
        size_t next = i;
        if (i < len) {
            if (last_space != (size_t)-1 && last_space > start) {
                end = last_space;
                next = last_space + 1;
            }
        }
        if (end <= start) {
            wchar_t wc;
            int bytes = utf8_next(s + i, len - i, &wc);
            next = i + (bytes > 0 ? (size_t)bytes : 1);
            if (next > len) {
                next = len;
            }
            end = next;
        }
        while (next < len && s[next] == ' ') {
            next++;
        }
        line_push(a, type, mi, (int)(base + start), (int)(end - start), cont);
        cont = 1;
        pos = next;
    }
}

static void layout_message(App *a, int mi) {
    Msg *m = &a->msgs[mi];
    if (m->role == ROLE_NOTE) {
        if (m->len == 0) {
            line_push(a, RL_NOTE, mi, 0, 0, 0);
        } else {
            wrap_para(a, mi, m->text, m->len, 0, RL_NOTE, a->layout_width, 0);
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
        return;
    }
    if (m->role == ROLE_ERROR) {
        line_push(a, RL_HEAD_E, mi, 0, 0, 0);
        if (m->len == 0) {
            line_push(a, RL_TEXT, mi, 0, 0, 0);
        } else {
            wrap_para(a, mi, m->text, m->len, 0, RL_TEXT, a->layout_width, 0);
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
    } else {
        int first = -1;
        if (m->role == ROLE_ASSISTANT && m->rlen > 0) {
            line_push(a, RL_REASON_HEAD, mi, 0, 0, 0);
            first = a->nlines - 1;
            wrap_para(a, mi, m->reason, m->rlen, 0, RL_REASON,
                      a->layout_width, 0);
            if (m->len > 0) {
                line_push(a, RL_GAP, mi, 0, 0, 0);
            }
        }
        int text_start = a->nlines;
        if (m->len > 0) {
            int in_code = 0;
            size_t pos = 0;
            while (pos < m->len) {
                size_t nl = pos;
                while (nl < m->len && m->text[nl] != '\n') {
                    nl++;
                }
                size_t llen = nl - pos;
                if (line_is_fence(m->text + pos, llen)) {
                    line_push(a, RL_FENCE, mi, (int)pos, (int)llen, 0);
                    in_code = !in_code;
                } else if (llen == 0) {
                    line_push(a, in_code ? RL_CODE : RL_TEXT, mi, (int)pos, 0,
                              0);
                } else {
                    wrap_para(a, mi, m->text + pos, llen, pos,
                              in_code ? RL_CODE : RL_TEXT, a->layout_width, 0);
                }
                if (nl == m->len) {
                    break;
                }
                pos = nl + 1;
            }
        } else {
            line_push(a, RL_TEXT, mi, 0, 0, 0);
        }
        if (first < 0) {
            first = text_start;
        }
        if (first >= 0 && first < a->nlines) {
            a->lines[first].head = 1;
        }
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 && m->len > 0 &&
            text_start < a->nlines) {
            a->lines[text_start].head = 1;
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
    }
    if (m->streaming) {
        for (int i = a->nlines - 1; i >= 0; i--) {
            if (a->lines[i].msg != mi) {
                break;
            }
            if (a->lines[i].type != RL_GAP) {
                a->lines[i].cursor = 1;
                break;
            }
        }
    }
}

/*
 * TODO(perf): streaming only grows the last open line, but this still
 * rewraps the whole current message on every redraw (a full 200 KB wrap is
 * 1.27 ms).  Cache a per-message layout cursor and rebuild just the tail
 * once a message outgrows a few hundred KB.
 */
static void layout_build(App *a) {
    int keep = a->layout_from;
    if (keep < 0) {
        keep = 0;
    }
    if (keep > a->nmsg) {
        keep = a->nmsg;
    }
    while (a->nlines > 0 && a->lines[a->nlines - 1].msg >= keep) {
        a->nlines--;
    }
    for (int i = keep; i < a->nmsg; i++) {
        layout_message(a, i);
    }
    a->layout_from = a->nmsg;
    a->layout_dirty = 0;
    a->last_layout = now_sec();
}

int draw_slice(int y, int x, const char *s, int off, int len, int maxx,
                      int pair, int attr, int *endx) {
    int cx = x;
    int col = 0;
    int i = off;
    int end = off + len;
    wattrset(stdscr, (attr_t)pair | (attr_t)attr);
    while (i < end && cx <= maxx) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 32 && ch < 127) {
            int j = i;
            while (j < end && (unsigned char)s[j] >= 32 &&
                   (unsigned char)s[j] < 127) {
                j++;
            }
            int run = j - i;
            if (cx + run - 1 > maxx) {
                run = maxx - cx + 1;
            }
            if (run > 0) {
                mvaddnstr(y, cx, s + i, run);
                cx += run;
                col += run;
                i += run;
            }
            continue;
        }
        wchar_t wc;
        int n = utf8_next(s + i, (size_t)(end - i), &wc);
        if (n <= 0) {
            break;
        }
        if (wc == '\r') {
            i += n;
            continue;
        }
        if (wc == '\t') {
            int adv = 4 - (col % 4);
            while (adv-- > 0 && cx <= maxx) {
                mvaddch(y, cx, ' ');
                cx++;
                col++;
            }
            i += n;
            continue;
        }
        if (wc < 32) {
            i += n;
            continue;
        }
        int w = wcwidth(wc);
        if (w < 0) {
            w = 1;
        }
        if (w > 0 && cx + w - 1 > maxx) {
            break;
        }
        if (i + n <= end) {
            char tmp[8];
            memcpy(tmp, s + i, (size_t)n);
            tmp[n] = '\0';
            mvaddstr(y, cx, tmp);
        }
        cx += w;
        col += w;
        i += n;
    }
    if (endx) {
        *endx = cx;
    }
    return cx;
}

void draw_chat(App *a, int rows, int cols) {
    int y0 = 1;
    int h = chat_height(a, rows);
    int text_width = cols - 6;
    if (text_width < 10) {
        text_width = 10;
    }
    int need_layout = a->layout_dirty || a->layout_width != text_width;
    if (need_layout) {
        if (a->streaming && now_sec() - a->last_layout < 0.09) {
            need_layout = 0;
        }
    }
    if (need_layout) {
        if (a->layout_width != text_width) {
            a->layout_width = text_width;
            a->layout_from = 0;
        }
        layout_build(a);
    }
    if (a->follow) {
        a->top = a->nlines > h ? a->nlines - h : 0;
    } else if (a->top > a->nlines - h) {
        a->top = a->nlines > h ? a->nlines - h : 0;
    }
    if (a->top < 0) {
        a->top = 0;
    }
    if (a->nmsg == 0 && !a->streaming) {
        draw_splash(a, y0, h, cols);
        return;
    }
    int y = y0;
    if (a->follow && a->nlines < h) {
        y = y0 + (h - a->nlines);
    }
    int maxx = cols - 3;
    for (int i = 0; i < h; i++) {
        int li = a->top + i;
        if (li < 0 || li >= a->nlines) {
            break;
        }
        RLine *l = &a->lines[li];
        int x = 4;
        int endx = x;
        if (l->type == RL_GAP) {
            Msg *gm = &a->msgs[l->msg];
            int last_gap =
                li + 1 >= a->nlines || a->lines[li + 1].msg != l->msg;
            if (last_gap && gm->role == ROLE_ASSISTANT &&
                (gm->stopped || gm->has_stats)) {
                char st[96];
                char stat[64];
                stat[0] = '\0';
                if (gm->has_stats) {
                    char el[32];
                    fmt_elapsed(gm->elapsed, el, sizeof el);
                    if (gm->ctok > 0) {
                        snprintf(stat, sizeof stat, "%ld tok · %s",
                                 gm->ctok, el);
                    } else {
                        snprintf(stat, sizeof stat, "%s", el);
                    }
                }
                if (gm->stopped && stat[0]) {
                    snprintf(st, sizeof st, "stopped · %s", stat);
                } else if (gm->stopped) {
                    snprintf(st, sizeof st, "stopped");
                } else {
                    snprintf(st, sizeof st, "%s", stat);
                }
                int sl = (int)strlen(st);
                if (st[0] && sl < cols - 4) {
                    wattrset(stdscr, pair_attr(a, P_DIM));
                    mvaddstr(y, cols - 3 - sl, st);
                }
            }
            y++;
            continue;
        }
        if (l->type == RL_NOTE) {
            wattrset(stdscr, pair_attr(a, P_NOTE));
            if (!l->cont) {
                mvaddstr(y, 2, sym_dot(a));
            }
            draw_slice(y, x, a->msgs[l->msg].text, l->off, l->len, maxx,
                       a->color ? pair_attr(a, P_NOTE) : 0, 0, &endx);
            y++;
            continue;
        }
        Msg *m = &a->msgs[l->msg];
        int gut_pair = m->role == ROLE_USER
                           ? P_USER
                           : (m->role == ROLE_ERROR ? P_ERROR : P_AI);
        if (l->head) {
            const char *s;
            int hp = m->role == ROLE_USER ? P_USER : P_AI;
            if (l->type == RL_REASON_HEAD) {
                s = sym_think(a);
            } else {
                s = m->role == ROLE_USER ? sym_user(a) : sym_ai(a);
            }
            wattrset(stdscr, pair_attr(a, hp) | A_BOLD);
            mvaddstr(y, 2, s);
        }
        if (l->type == RL_HEAD_E) {
            wattrset(stdscr, pair_attr(a, P_ERROR) | A_BOLD);
            mvaddstr(y, 2, sym_err(a));
            mvaddstr(y, 4, "error");
            y++;
            continue;
        }
        const char *gut = sym_gutter(a);
        const char *text = m->text;
        int off = l->off;
        int len = l->len;
        int text_pair = m->role == ROLE_USER ? P_USER : P_BASE;
        int text_attr = 0;
        if (l->type == RL_REASON_HEAD) {
            wattrset(stdscr, pair_attr(a, P_DIM));
            mvaddstr(y, 4, "thinking");
            y++;
            continue;
        }
        if (l->type == RL_REASON) {
            gut = sym_reason_gutter(a);
            text = m->reason;
            text_pair = P_DIM;
        } else if (l->type == RL_CODE) {
            text_pair = P_CODE;
        } else if (l->type == RL_FENCE) {
            text_pair = P_FENCE;
        }
        if (l->type == RL_TEXT && m->role == ROLE_ERROR) {
            gut_pair = P_ERROR;
        }
        int show_gutter =
            !l->head && !(m->role == ROLE_ASSISTANT && l->type != RL_REASON);
        if (show_gutter) {
            wattrset(stdscr, pair_attr(a, gut_pair) |
                                  (m->role == ROLE_ERROR ? 0 : A_DIM));
            mvaddstr(y, 2, gut);
        }
        draw_slice(y, x, text, off, len, maxx,
                   a->color ? pair_attr(a, text_pair) : 0, text_attr, &endx);
        if (l->cursor && m->streaming && a->follow) {
            int blink = ((long long)(now_sec() * 2)) & 1;
            if (blink && endx <= maxx - 1) {
                wattrset(stdscr, pair_attr(a, P_ACCENT) | A_BOLD);
                mvaddstr(y, endx, sym_cursor(a));
            }
        }
        y++;
    }
    draw_scrollbar(a, y0, h, rows, cols);
}

