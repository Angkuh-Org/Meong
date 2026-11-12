#ifndef MEONG_CHAT_H
#define MEONG_CHAT_H

#include "app/app.h"

void set_model(App *a, const char *id);
void run_command(App *a, const char *cmdline);

int start_generation(App *a);
int finish_generation(App *a, int ok);
void cancel_generation(App *a);
void drain_queue(App *a);
void submit_input(App *a);

#endif
