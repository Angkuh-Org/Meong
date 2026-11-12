#include "chat/chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        a->dirty = 1;
        picker_open(a);
    } else {
        Msg *m = &a->msgs[idx];
        m->role = ROLE_ERROR;
        msg_set(m, err);
        a->layout_dirty = 1;
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

