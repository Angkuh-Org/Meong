#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api/api.h"
#include "app/app.h"
#include "chat/chat.h"
#include "cli/cli.h"
#include "ui/ui.h"

static int terminal_small(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    return rows < 10 || cols < 36;
}

static void picker_key(App *a, wint_t wch, int special) {
    if (special) {
        if (wch == KEY_UP) {
            if (a->picker_sel > 0) {
                a->picker_sel--;
            }
        } else if (wch == KEY_DOWN) {
            if (a->picker_sel + 1 < a->pick_n) {
                a->picker_sel++;
            }
        } else if (wch == KEY_PPAGE) {
            a->picker_sel -= 10;
            if (a->picker_sel < 0) {
                a->picker_sel = 0;
            }
        } else if (wch == KEY_NPAGE) {
            a->picker_sel += 10;
            if (a->picker_sel >= a->pick_n) {
                a->picker_sel = a->pick_n > 0 ? a->pick_n - 1 : 0;
            }
        } else if (wch == KEY_ENTER) {
            if (a->pick_n > 0) {
                int mi = a->pick_matches[a->picker_sel];
                char *id = xstrdup(a->models[mi]);
                picker_close(a);
                set_model(a, id);
                free(id);
            }
        } else if (wch == KEY_BACKSPACE) {
            picker_filter_backspace(a);
        } else if (wch == KEY_RESIZE) {
            a->dirty = 1;
        }
        a->dirty = 1;
        return;
    }
    if (wch == 27 || wch == 3 || wch == 9) {
        picker_close(a);
    } else if (wch == '\n' || wch == '\r') {
        if (a->pick_n > 0) {
            int mi = a->pick_matches[a->picker_sel];
            char *id = xstrdup(a->models[mi]);
            picker_close(a);
            set_model(a, id);
            free(id);
        }
    } else if (wch == 127 || wch == 8) {
        picker_filter_backspace(a);
    } else if (wch == 21) {
        picker_filter_clear(a);
    } else if (wch >= 32 && wch < 127) {
        picker_filter_append(a, (char)wch);
    }
    a->dirty = 1;
}

static void pet_meong(App *a) {
    a->pet_count++;
    a->pet_open = 1;
    a->help_open = 0;
    a->picker_open = 0;
    a->model_open = 0;
    a->dirty = 1;
}

static void handle_mouse(App *a) {
    MEVENT ev;
    if (getmouse(&ev) != OK) {
        return;
    }
    int up = 0;
    int down = 0;
#ifdef BUTTON4_PRESSED
    if (ev.bstate & BUTTON4_PRESSED) {
        up = 1;
    }
#endif
#ifdef BUTTON5_PRESSED
    if (ev.bstate & BUTTON5_PRESSED) {
        down = 1;
    }
#endif
    if (a->pet_open) {
        if (ev.bstate & BUTTON1_PRESSED) {
            if (a->pet_y0 >= 0 && ev.y >= a->pet_y0 &&
                ev.y <= a->pet_y1 && ev.x >= a->pet_x0 &&
                ev.x <= a->pet_x1) {
                pet_meong(a);
            } else {
                a->pet_open = 0;
                a->dirty = 1;
            }
        }
        return;
    }
    if (a->help_open) {
        if (ev.bstate & BUTTON1_PRESSED) {
            a->help_open = 0;
            a->dirty = 1;
        }
        return;
    }
    if (a->picker_open) {
        if (up && a->picker_sel > 0) {
            a->picker_sel--;
        }
        if (down && a->picker_sel + 1 < a->pick_n) {
            a->picker_sel++;
        }
        a->dirty = 1;
        return;
    }
    if (a->model_open) {
        if (ev.bstate & BUTTON1_PRESSED) {
            int inside = a->model_py0 >= 0 && ev.y >= a->model_py0 &&
                         ev.y <= a->model_py1 && ev.x >= a->model_px0 &&
                         ev.x <= a->model_px1;
            if (!inside) {
                a->model_open = 0;
            }
            a->dirty = 1;
        }
        return;
    }
    if (slash_open(a)) {
        if (up) {
            slash_move(a, -1);
            return;
        }
        if (down) {
            slash_move(a, 1);
            return;
        }
        if (a->cmd_y0 >= 0 && ev.y >= a->cmd_y0 &&
            ev.y < a->cmd_y0 + a->cmd_h && ev.x >= a->cmd_x0 &&
            ev.x < a->cmd_x0 + a->cmd_w) {
            if (ev.bstate & BUTTON1_PRESSED) {
                int sel = ev.y - a->cmd_y0 - 1;
                if (sel >= 0 && sel < a->cmd_h - 2) {
                    a->cmd_sel = sel;
                    a->dirty = 1;
                }
            }
            return;
        }
    }
    if (up) {
        chat_scroll(a, 3);
    } else if (down) {
        chat_scroll(a, -3);
    } else if (ev.bstate & BUTTON1_PRESSED) {
        if (a->brand_y >= 0 && ev.y == a->brand_y &&
            ev.x >= a->brand_x0 && ev.x < a->brand_x1) {
            pet_meong(a);
            return;
        }
        if (a->model_y >= 0 && ev.y == a->model_y &&
            ev.x >= a->model_x0 && ev.x < a->model_x1) {
            model_info_open(a);
            return;
        }
        chat_click(a, ev.y);
    }
}

static void handle_key(App *a, wint_t wch, int special) {
    if (special) {
        if (wch == KEY_RESIZE) {
            a->layout_dirty = 1;
            a->dirty = 1;
            return;
        }
        if (wch == KEY_MOUSE) {
            handle_mouse(a);
            return;
        }
        if (wch == KEY_F(1)) {
            pet_meong(a);
            return;
        }
        if (wch == KEY_F(2)) {
            model_info_open(a);
            return;
        }
        if (a->model_open) {
            a->model_open = 0;
            a->dirty = 1;
            return;
        }
        if (a->pet_open) {
            a->pet_open = 0;
            a->dirty = 1;
            return;
        }
        if (a->help_open) {
            a->help_open = 0;
            a->dirty = 1;
            return;
        }
        if (a->picker_open) {
            picker_key(a, wch, 1);
            return;
        }
        if (slash_open(a)) {
            if (wch == KEY_UP) {
                slash_move(a, -1);
                return;
            }
            if (wch == KEY_DOWN) {
                slash_move(a, 1);
                return;
            }
            if (wch == KEY_ENTER) {
                slash_accept(a);
                return;
            }
        }
        if (wch == KEY_PPAGE) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            (void)cols;
            int page = chat_height(a, rows) - 2;
            chat_scroll(a, page > 0 ? page : 1);
            return;
        }
        if (wch == KEY_NPAGE) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            (void)cols;
            int page = chat_height(a, rows) - 2;
            chat_scroll(a, page > 0 ? -(page) : -1);
            return;
        }
        if (wch == KEY_UP) {
            history_move(a, -1);
            return;
        }
        if (wch == KEY_DOWN) {
            history_move(a, 1);
            return;
        }
        if (wch == KEY_ENTER) {
            submit_input(a);
            return;
        }
        if (wch == KEY_BACKSPACE) {
            input_backspace(a);
            return;
        }
        if (wch == KEY_DC) {
            input_del(a);
            return;
        }
        if (wch == KEY_LEFT) {
            if (a->icur > 0) {
                a->icur--;
                a->dirty = 1;
            }
            return;
        }
        if (wch == KEY_RIGHT) {
            if (a->icur < a->ilen) {
                a->icur++;
                a->dirty = 1;
            }
            return;
        }
        if (wch == KEY_HOME) {
            a->icur = 0;
            a->dirty = 1;
            return;
        }
        if (wch == KEY_END) {
            a->icur = a->ilen;
            a->dirty = 1;
            return;
        }
        return;
    }
    if (wch == 3) {
        if (a->model_open) {
            a->model_open = 0;
            a->dirty = 1;
        } else if (a->pet_open) {
            a->pet_open = 0;
            a->dirty = 1;
        } else if (a->help_open) {
            a->help_open = 0;
            a->dirty = 1;
        } else if (a->picker_open) {
            picker_close(a);
        } else if (a->streaming) {
            cancel_generation(a);
        } else {
            a->quit = 1;
        }
        return;
    }
    if (a->model_open) {
        a->model_open = 0;
        a->dirty = 1;
    }
    if (a->pet_open) {
        a->pet_open = 0;
        a->dirty = 1;
    }
    if (a->help_open) {
        a->help_open = 0;
        a->dirty = 1;
        return;
    }
    if (a->picker_open) {
        picker_key(a, wch, 0);
        return;
    }
    if (terminal_small()) {
        if (wch == 'q' || wch == 'Q') {
            a->quit = 1;
        }
        return;
    }
    if (wch == 12) {
        clearok(stdscr, TRUE);
        a->dirty = 1;
        return;
    }
    if (wch == 15) {
        chat_toggle_reason(a);
        return;
    }
    if (wch == 7) {
        scroll_bottom(a);
        return;
    }
    if (wch == 20) {
        a->follow = 0;
        a->top = 0;
        a->dirty = 1;
        return;
    }
    if (wch == '\n' || wch == '\r') {
        if (slash_open(a)) {
            slash_accept(a);
        } else {
            submit_input(a);
        }
    } else if (wch == 9) {
        if (slash_open(a)) {
            slash_complete(a);
        } else if (!a->streaming) {
            picker_open(a);
        }
    } else if (wch == 127 || wch == 8) {
        input_backspace(a);
    } else if (wch == 4) {
        if (a->ilen == 0) {
            if (!a->streaming) {
                a->quit = 1;
            }
        } else {
            input_del(a);
        }
    } else if (wch == 1) {
        a->icur = 0;
        a->dirty = 1;
    } else if (wch == 5) {
        a->icur = a->ilen;
        a->dirty = 1;
    } else if (wch == 21) {
        input_reset(a);
    } else if (wch == 11) {
        a->ilen = a->icur;
        a->dirty = 1;
    } else if (wch == 23) {
        input_kill_word(a);
    } else if (wch == 16) {
        history_move(a, -1);
    } else if (wch == 14) {
        history_move(a, 1);
    } else if (wch == 27) {
        if (slash_open(a)) {
            slash_close(a);
        }
    } else if (wch == '?' && a->ilen == 0) {
        a->help_open = 1;
        a->dirty = 1;
    } else if (wch >= 32) {
        input_insert(a, (wchar_t)wch);
    }
}

static int tui_run(App *a) {
    if (ui_init(a) != 0) {
        return 1;
    }
    if (a->initial_picker && a->nmodels > 0) {
        picker_open(a);
    }
    a->dirty = 1;
    double last_draw = 0;
    while (!a->quit) {
        wtimeout(stdscr, a->streaming ? 50 : 150);
        wint_t wch = 0;
        int rc = wget_wch(stdscr, &wch);
        int had_key = 0;
        if (rc == OK || rc == KEY_CODE_YES) {
            had_key = 1;
            handle_key(a, wch, rc == KEY_CODE_YES);
        }
        if (a->streaming && a->stream) {
            int r = stream_tick(a->stream, NULL);
            if (r == 1) {
                if (finish_generation(a, 1)) {
                    drain_queue(a);
                }
            } else if (r < 0) {
                finish_generation(a, 0);
            }
            if (a->streaming && now_sec() - a->last_anim >= 0.1) {
                a->last_anim = now_sec();
                a->dirty = 1;
            }
        }
        double now = now_sec();
        if (a->dirty && (had_key || now - last_draw >= 0.03)) {
            ui_draw(a);
            a->dirty = 0;
            last_draw = now;
        }
    }
    endwin();
    return 0;
}

static int tui_main(const Options *o) {
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "Meong: this is a full-screen TUI; use --oneshot when piped\n");
        return 1;
    }
    App a;
    memset(&a, 0, sizeof a);
    a.color = 1;
    a.mouse = 1;
    a.chat_y0 = -1;
    a.cmd_y0 = -1;
    a.unicode = 1;
    a.hist_pos = -1;
    a.gen_idx = -1;
    a.follow = 1;

    char *url = resolve_url(o->url);
    char err[512];
    if (endpoint_parse(url, &a.ep, err, sizeof err) != 0) {
        fprintf(stderr, "Meong: %s\n", err);
        free(url);
        return 1;
    }
    a.url = url;
    if (o->ascii || (getenv("MEONG_ASCII") && *getenv("MEONG_ASCII"))) {
        a.ascii = 1;
        a.unicode = 0;
    }
    a.nerd = !a.ascii;
    if (o->no_nerd || getenv("MEONG_NO_NERD")) {
        a.nerd = 0;
    }
    if (o->no_mouse || getenv("MEONG_NO_MOUSE")) {
        a.mouse = 0;
    }
    if (o->no_color || getenv("NO_COLOR")) {
        a.color = 0;
    }

    char *saved_model = state_load_model();
    int legacy_cache = models_load_cache(&a);
    if (a.nmodels == 0 || legacy_cache) {
        if (a.nmodels == 0) {
            fprintf(stderr, "Meong: loading models from %s\n", a.url);
        }
        char **ids = NULL;
        ModelCaps *caps = NULL;
        int n = 0;
        if (models_fetch_live(&a.ep, &ids, &caps, &n, err, sizeof err) == 0) {
            models_replace(&a, ids, caps, n);
        } else if (a.nmodels == 0) {
            snprintf(a.models_warn, sizeof a.models_warn,
                     "gateway unreachable at %.480s (%.500s)", a.url, err);
        }
    }

    const char *env_model = getenv("MEONG_MODEL");
    int explicit_model = (o->model && *o->model) ||
                         (env_model && *env_model) || saved_model != NULL;
    if (o->model && *o->model) {
        a.model = xstrdup(o->model);
    } else if (env_model && *env_model) {
        a.model = xstrdup(env_model);
    } else if (saved_model) {
        a.model = xstrdup(saved_model);
    } else {
        a.model = xstrdup(choose_default_model(&a));
    }
    free(saved_model);
    a.initial_picker = !explicit_model && a.nmodels > 0;
    if (o->system && *o->system) {
        a.system_prompt = xstrdup(o->system);
    }

    int rc = tui_run(&a);
    jdoc_free(&a.stream_doc);
    buf_free(&a.stream_buf);
    msgs_clear(&a);
    models_clear(&a);
    free(a.model);
    free(a.system_prompt);
    free(a.url);
    free(a.pick_matches);
    free(a.lines);
    free(a.msgs);
    queue_clear(&a);
    history_free(&a);
    return rc;
}

int main(int argc, char **argv) {
    Options o;
    if (parse_args(argc, argv, &o) != 0) {
        usage(stderr);
        return 2;
    }
    if (o.help) {
        usage(stdout);
        return 0;
    }
    if (o.version) {
        version();
        return 0;
    }
    if (o.list_models) {
        return cli_list_models(&o);
    }
    if (o.oneshot) {
        return cli_oneshot(&o);
    }
    return tui_main(&o);
}
