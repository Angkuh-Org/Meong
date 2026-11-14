#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

#include "api/api.h"

/* Everything here stays ASCII, like the pet card. */

static void fmt_tokens(long long value, char *out, size_t cap) {
    if (value < 0) {
        snprintf(out, cap, "-");
        return;
    }
    char raw[32];
    int n = snprintf(raw, sizeof raw, "%lld", value);
    if (n <= 0) {
        snprintf(out, cap, "-");
        return;
    }
    char tmp[48];
    int t = 0;
    int digits = 0;
    for (int i = n - 1; i >= 0 && t + 2 < (int)sizeof tmp; i--) {
        tmp[t++] = raw[i];
        digits++;
        if (digits % 3 == 0 && i > 0) {
            tmp[t++] = ',';
        }
    }
    int o = 0;
    while (t > 0 && o + 1 < (int)cap) {
        out[o++] = tmp[--t];
    }
    out[o] = '\0';
}

static const char *yes_no(int flag) {
    if (flag > 0) {
        return "yes";
    }
    if (flag == 0) {
        return "no";
    }
    return "-";
}

static void fmt_cost(const ModelCaps *c, char *out, size_t cap) {
    if (c->in_cost < 0 && c->out_cost < 0) {
        snprintf(out, cap, "-");
        return;
    }
    if (c->in_cost == 0 && c->out_cost == 0) {
        snprintf(out, cap, "free");
        return;
    }
    char in[24];
    char cout[24];
    if (c->in_cost < 0) {
        snprintf(in, sizeof in, "?");
    } else {
        snprintf(in, sizeof in, "$%.2f", c->in_cost * 1e6);
    }
    if (c->out_cost < 0) {
        snprintf(cout, sizeof cout, "?");
    } else {
        snprintf(cout, sizeof cout, "$%.2f", c->out_cost * 1e6);
    }
    snprintf(out, cap, "%s in / %s out per 1M tok", in, cout);
}

static int info_row(App *a, int y, int ymax, int x, const char *label,
                    const char *value, int width) {
    if (y >= ymax) {
        return y;
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    mvaddstr(y, x, label);
    wattrset(stdscr, pair_attr(a, P_PANEL));
    int avail = width - 13;
    if (avail > 0) {
        mvaddnstr(y, x + 13, value, avail);
    }
    return y + 1;
}

void model_info_open(App *a) {
    a->model_open = 1;
    a->pet_open = 0;
    a->help_open = 0;
    a->picker_open = 0;
    a->dirty = 1;
}

static void append_name(char *buf, size_t cap, const char *name) {
    size_t n = strlen(buf);
    if (n + 2 >= cap) {
        return;
    }
    snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", name);
}

void model_info_draw(App *a, int rows, int cols) {
    const ModelCaps *caps = model_caps_for(a, a->model);
    int w = cols - 2;
    if (w > 58) {
        w = 58;
    }
    if (w < 30) {
        return;
    }
    int width = w - 6;
    int reported = caps && caps_reported(caps);
    int nrows = 0;
    int flags_missing = 0;
    char missing[64] = "";
    if (reported) {
        nrows++;
        if (caps->context_window >= 0) {
            nrows++;
        }
        if (caps->max_output_tokens >= 0) {
            nrows++;
        }
        if (caps->vision >= 0) {
            nrows++;
        } else {
            flags_missing++;
        }
        if (caps->function_calling >= 0) {
            nrows++;
        } else {
            flags_missing++;
        }
        if (caps->reasoning >= 0) {
            nrows++;
        } else {
            flags_missing++;
        }
        if (caps->prompt_caching >= 0) {
            nrows++;
        } else {
            flags_missing++;
        }
        if (caps->in_cost >= 0 || caps->out_cost >= 0) {
            nrows++;
        }
        if (caps->vision < 0) {
            append_name(missing, sizeof missing, "vision");
        }
        if (caps->function_calling < 0) {
            append_name(missing, sizeof missing, "tools");
        }
        if (caps->reasoning < 0) {
            append_name(missing, sizeof missing, "reasoning");
        }
        if (caps->prompt_caching < 0) {
            append_name(missing, sizeof missing, "cache");
        }
    }
    int h = reported
                ? nrows + 6 + (flags_missing ? 1 : 0) + (caps->estimated ? 1 : 0)
                : 10;
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
    a->model_py0 = py;
    a->model_py1 = py + h - 1;
    a->model_px0 = px;
    a->model_px1 = px + w - 1;
    /* TODO(ui): the card lost its title and shadow together. The title
     * matches the "card names itself" rule; no test pins the shadow. */
    panel_frame(a, px, py, w, h, NULL, 0);

    int x = px + 3;
    int y = py + 1;
    int ymax = py + h - 1;
    wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    mvaddnstr(y, x, a->model, width);
    y++;
    if (y < ymax) {
        y++;
    }

    if (!reported) {
        wattrset(stdscr, pair_attr(a, P_PANEL));
        if (y < ymax) {
            mvaddnstr(y, x, "the gateway reported no capability fields", width);
            y++;
        }
        wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
        if (y < ymax) {
            mvaddnstr(y, x, "run /models to refresh the catalogue", width);
        }
        a->cursor_on = 0;
        return;
    }

    char ctx[32];
    char out[32];
    char cost[96];
    char provider[32];
    backend_label(a->model, provider, sizeof provider);
    fmt_tokens(caps->context_window, ctx, sizeof ctx);
    fmt_tokens(caps->max_output_tokens, out, sizeof out);
    fmt_cost(caps, cost, sizeof cost);
    y = info_row(a, y, ymax, x, "provider", provider, width);
    if (caps->context_window >= 0) {
        y = info_row(a, y, ymax, x, "context", ctx, width);
    }
    if (caps->max_output_tokens >= 0) {
        y = info_row(a, y, ymax, x, "max output", out, width);
    }
    if (caps->vision >= 0) {
        y = info_row(a, y, ymax, x, "vision", yes_no(caps->vision), width);
    }
    if (caps->function_calling >= 0) {
        y = info_row(a, y, ymax, x, "tools",
                     yes_no(caps->function_calling), width);
    }
    if (caps->reasoning >= 0) {
        y = info_row(a, y, ymax, x, "reasoning", yes_no(caps->reasoning),
                     width);
    }
    if (caps->prompt_caching >= 0) {
        y = info_row(a, y, ymax, x, "prompt cache",
                     yes_no(caps->prompt_caching), width);
    }
    if (caps->in_cost >= 0 || caps->out_cost >= 0) {
        y = info_row(a, y, ymax, x, "cost", cost, width);
    }
    if (flags_missing && y < ymax) {
        char line[96];
        snprintf(line, sizeof line, "not reported: %s", missing);
        wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
        mvaddnstr(y, x, line, width);
        y++;
    }
    if (caps->estimated && y < ymax) {
        wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
        mvaddnstr(y, x, "~ models.dev estimate for missing fields", width);
    }
    a->cursor_on = 0;
}
