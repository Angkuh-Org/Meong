#ifndef MEONG_TITLE_H
#define MEONG_TITLE_H

#include "app/app.h"

void title_request(App *a, const char *prompt);
void title_tick_slow(App *a);

/* Called every poll: keep the common "no title stream" path out of the slow
 * frame. */
static inline void title_tick(App *a) {
    if (a && a->title_stream) {
        title_tick_slow(a);
    }
}

void title_reset(App *a);

#endif
