#ifndef MEONG_CLI_H
#define MEONG_CLI_H

#include <stdio.h>

typedef struct {
    const char *url;
    const char *model;
    const char *system;
    const char *oneshot;
    int stream;
    int list_models;
    int ascii;
    int no_nerd;
    int no_mouse;
    int no_color;
    int help;
    int version;
} Options;

int parse_args(int argc, char **argv, Options *o);
void usage(FILE *out);
void version(void);
char *resolve_url(const char *cli_url);
int cli_oneshot(const Options *o);
int cli_list_models(const Options *o);

#endif
