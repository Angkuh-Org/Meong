#include "chat/chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "api/api.h"
#include "ui/ui.h"

static void note(App *a, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    msg_add(a, ROLE_NOTE, buf);
}

void set_model(App *a, const char *id) {
    char *trimmed = xstrdup(id);
    char *v = trim_ws(trimmed);
    if (!*v) {
        free(trimmed);
        return;
    }
    free(a->model);
    a->model = xstrdup(v);
    state_save_model(a->model);
    note(a, "model %s", a->model);
    free(trimmed);
    a->dirty = 1;
}

static void refresh_models(App *a) {
    int idx = msg_add(a, ROLE_NOTE, "refreshing models");
    ui_draw(a);
    char err[512];
    char **ids = NULL;
    int n = 0;
    if (models_fetch_live(&a->ep, &ids, &n, err, sizeof err) == 0) {
        models_replace(a, ids, n);
        char buf[96];
        snprintf(buf, sizeof buf, "refreshed %d models", n);
        msg_set(&a->msgs[idx], buf);
        a->layout_dirty = 1;
        a->layout_from = 0;
        a->dirty = 1;
        picker_open(a);
    } else {
        Msg *m = &a->msgs[idx];
        m->role = ROLE_ERROR;
        msg_set(m, err);
        a->layout_dirty = 1;
        a->layout_from = 0;
        a->dirty = 1;
    }
}

void run_command(App *a, const char *cmdline) {
    char *copy = xstrdup(cmdline);
    char *cmd = copy;
    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = '\0';
        arg = trim_ws(arg);
    }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 ||
        strcmp(cmd, "exit") == 0) {
        a->quit = 1;
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        a->help_open = 1;
        a->picker_open = 0;
        a->pet_open = 0;
        a->dirty = 1;
    } else if (strcmp(cmd, "model") == 0) {
        if (arg && *arg) {
            set_model(a, arg);
        } else {
            picker_open(a);
        }
    } else if (strcmp(cmd, "models") == 0) {
        refresh_models(a);
    } else if (strcmp(cmd, "system") == 0) {
        if (!arg || !*arg) {
            if (a->system_prompt && *a->system_prompt) {
                char buf[512];
                snprintf(buf, sizeof buf, "system: %.400s",
                         a->system_prompt);
                note(a, "%s", buf);
            } else {
                note(a, "no system prompt");
            }
        } else if (str_ieq(arg, "off")) {
            free(a->system_prompt);
            a->system_prompt = NULL;
            note(a, "system prompt cleared");
        } else {
            free(a->system_prompt);
            a->system_prompt = xstrdup(arg);
            note(a, "system prompt set (%d bytes)", (int)strlen(arg));
        }
    } else if (strcmp(cmd, "clear") == 0) {
        msgs_clear(a);
    } else if (strcmp(cmd, "retry") == 0) {
        if (a->streaming) {
            note(a, "wait for the current reply, or ctrl-c");
        } else {
            int i = a->nmsg - 1;
            while (i >= 0 && a->msgs[i].role != ROLE_USER) {
                i--;
            }
            if (i < 0) {
                note(a, "nothing to retry");
            } else {
                for (int j = i + 1; j < a->nmsg; j++) {
                    free(a->msgs[j].text);
                    free(a->msgs[j].reason);
                }
                a->nmsg = i + 1;
                a->follow = 1;
                a->new_below = 0;
                start_generation(a);
            }
        }
    } else {
        note(a, "unknown command: %s", cmd);
    }
    free(copy);
}

/* label_len = sizeof n: the leading "/" adds one byte, sizeof counts the NUL. */
#define SLASH_CMD(n, a, d) {n, a, d, "/" n, (unsigned char)sizeof(n)}

static const SlashCmd CMDS[] = {
    SLASH_CMD("model", "[id]", "pick a model, or switch directly"),
    SLASH_CMD("models", "", "refresh the gateway model list"),
    SLASH_CMD("system", "[p]", "show, set, or clear the system prompt"),
    SLASH_CMD("clear", "", "forget the conversation"),
    SLASH_CMD("retry", "", "regenerate the last answer"),
    SLASH_CMD("help", "", "this panel"),
    SLASH_CMD("quit", "", "leave Meong"),
};
enum { NCMDS = (int)(sizeof CMDS / sizeof CMDS[0]) };
_Static_assert(NCMDS <= SLASH_MAX, "slash table exceeds SLASH_MAX");

const SlashCmd *slash_cmds(int *n) {
    if (n) {
        *n = NCMDS;
    }
    return CMDS;
}

static int prefix_ci(const wchar_t *q, size_t qlen, const char *name) {
    for (size_t i = 0; i < qlen; i++) {
        wint_t wc = (wint_t)q[i];
        if (wc > 127) {
            return 0;
        }
        unsigned char c = (unsigned char)wc;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + ('a' - 'A'));
        }
        if (!name[i] || c != (unsigned char)name[i]) {
            return 0;
        }
    }
    return 1;
}

int slash_matches(const App *a, int *idx, int cap) {
    if (a->ilen == 0 || a->input[0] != L'/') {
        return 0;
    }
    for (size_t i = 1; i < a->ilen; i++) {
        if (a->input[i] == L' ' || a->input[i] == L'\t') {
            return 0;
        }
    }
    int n = 0;
    for (int i = 0; i < NCMDS; i++) {
        if (!prefix_ci(a->input + 1, a->ilen - 1, CMDS[i].name)) {
            continue;
        }
        if (idx) {
            if (n >= cap) {
                break;
            }
            idx[n] = i;
        }
        n++;
    }
    return n;
}

/* TODO(perf): slash_open("/m") is ~132 ns/op, dominated by getmaxy/getmaxx
 * and chat_height. ui_draw already has rows/cols; passing them in skips both
 * calls. Caching open state across events is the riskier option. */
int slash_open_list(const App *a, int *idx, int cap) {
    if (a->ilen == 0 || a->input[0] != L'/') {
        return 0;
    }
    if (a->cmd_hide || a->picker_open || a->help_open || a->pet_open) {
        return 0;
    }
    int n = slash_matches(a, idx, cap);
    if (n == 0) {
        return 0;
    }
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < UI_MIN_ROWS || cols < UI_MIN_COLS ||
        chat_height(a, rows) < n + 2) {
        return 0;
    }
    return n;
}

int slash_open(const App *a) {
    if (a->ilen == 0 || a->input[0] != L'/') {
        return 0;
    }
    return slash_open_list(a, NULL, 0) != 0;
}

void slash_close(App *a) {
    a->cmd_hide = 1;
    a->cmd_sel = 0;
    a->dirty = 1;
}

void slash_move(App *a, int dir) {
    int n = slash_matches(a, NULL, 0);
    if (n == 0) {
        return;
    }
    if (a->cmd_sel >= n) {
        a->cmd_sel = n - 1;
    }
    a->cmd_sel += dir;
    if (a->cmd_sel < 0) {
        a->cmd_sel = 0;
    }
    if (a->cmd_sel >= n) {
        a->cmd_sel = n - 1;
    }
    a->dirty = 1;
}

static void complete_at(App *a, const SlashCmd *c) {
    wchar_t buf[16];
    size_t n = strlen(c->name);
    int space = c->arg[0] != '\0';
    if (n + 3 > sizeof buf / sizeof buf[0]) {
        return;
    }
    buf[0] = L'/';
    for (size_t i = 0; i < n; i++) {
        buf[i + 1] = (wchar_t)(unsigned char)c->name[i];
    }
    if (space) {
        buf[n + 1] = L' ';
    }
    buf[n + 1 + (size_t)space] = L'\0';
    input_set_text(a, buf, n + 1 + (size_t)space);
    a->cmd_hide = 1;
    a->cmd_sel = 0;
}

void slash_complete(App *a) {
    int idx[SLASH_MAX];
    int n = slash_matches(a, idx, SLASH_MAX);
    if (n == 0) {
        return;
    }
    if (a->cmd_sel >= n) {
        a->cmd_sel = n - 1;
    }
    complete_at(a, &CMDS[idx[a->cmd_sel]]);
}

static int token_is(const App *a, const char *name) {
    size_t n = strlen(name);
    if (a->ilen != n + 1) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (a->input[i + 1] != (wchar_t)(unsigned char)name[i]) {
            return 0;
        }
    }
    return 1;
}

void slash_accept(App *a) {
    int idx[SLASH_MAX];
    int n = slash_matches(a, idx, SLASH_MAX);
    if (n == 0) {
        submit_input(a);
        return;
    }
    if (a->cmd_sel >= n) {
        a->cmd_sel = n - 1;
    }
    const SlashCmd *c = &CMDS[idx[a->cmd_sel]];
    if (token_is(a, c->name)) {
        submit_input(a);
        return;
    }
    complete_at(a, c);
}

