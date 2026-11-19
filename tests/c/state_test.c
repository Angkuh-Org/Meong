#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/app.h"

static int fails;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            fprintf(stderr, "state: %s\n", msg);            \
            fails++;                                        \
        }                                                   \
    } while (0)

int main(void) {
    char dir[] = "/tmp/meong-state-XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "state: mkdtemp failed\n");
        return 1;
    }
    setenv("MEONG_CONFIG_DIR", dir, 1);

    state_save_state("mock/effort", "high");
    char *model = state_load_model();
    char *effort = state_load_effort();
    CHECK(model && strcmp(model, "mock/effort") == 0, "model round-trip");
    CHECK(effort && strcmp(effort, "high") == 0, "effort round-trip");
    free(model);
    free(effort);

    state_save_state("mock/plain", "");
    model = state_load_model();
    effort = state_load_effort();
    CHECK(model && strcmp(model, "mock/plain") == 0,
          "model survives an effort clear");
    CHECK(effort == NULL, "cleared effort disappears");
    free(model);
    free(effort);

    char path[512];
    snprintf(path, sizeof path, "%s/state", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "legacy state write");
    if (f) {
        fputs("model=mock/legacy\n", f);
        fclose(f);
    }
    model = state_load_model();
    effort = state_load_effort();
    CHECK(model && strcmp(model, "mock/legacy") == 0, "legacy model loads");
    CHECK(effort == NULL, "legacy state has no effort");
    free(model);
    free(effort);

    CHECK(strcmp(effort_canon(" HIGH "), "high") == 0,
          "canonicalizer trims and lowercases");
    CHECK(strcmp(effort_canon("default"), "") == 0, "default clears");
    CHECK(strcmp(effort_canon("auto"), "") == 0, "auto clears");
    CHECK(effort_canon("turbo") == NULL, "unknown level rejected");

    unlink(path);
    rmdir(dir);
    return fails ? 1 : 0;
}
