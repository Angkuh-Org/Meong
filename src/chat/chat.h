#ifndef MEONG_CHAT_H
#define MEONG_CHAT_H

#include "app/app.h"

#define SLASH_MAX 16

typedef struct {
    const char *name;
    const char *arg;
    const char *desc;
    const char *label;
    unsigned char label_len;
} SlashCmd;

const SlashCmd *slash_cmds(int *n);
int slash_matches(const App *a, int *idx, int cap);
int slash_open_list(const App *a, int *idx, int cap);
int slash_open(const App *a);
void slash_move(App *a, int dir);
void slash_complete(App *a);
void slash_accept(App *a);
void slash_close(App *a);

void set_model(App *a, const char *id);
void refresh_models(App *a);
int retry_last(App *a);
void run_command(App *a, const char *cmdline);

int start_generation(App *a);
int finish_generation(App *a, int ok);
void cancel_generation(App *a);
void drain_queue(App *a);
void submit_input(App *a);

#endif
