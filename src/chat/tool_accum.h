#ifndef MEONG_TOOL_ACCUM_H
#define MEONG_TOOL_ACCUM_H

#include <stddef.h>

#include "tools/tools.h"

typedef struct {
    ToolCall calls[TOOL_MAX_CALLS];
    unsigned char used[TOOL_MAX_CALLS];
    int ncalls;
    int whole;
} ToolAccum;

/* whole=1 replaces a slot (cc); whole=0 appends argument fragments. */
void tool_accum_reset(ToolAccum *ta, int whole);
void tool_accum_clear(ToolAccum *ta);

/*
 * Add one streamed tool_calls[] entry. has_index=0 means the provider omitted
 * the index. A named call without index/id starts a new slot; an argument-only
 * fragment with no index/id continues the last used slot. Two named id-less
 * calls never merge. Returns 0 on success, -1 on overflow.
 */
int tool_accum_add(ToolAccum *ta, int has_index, int index,
                   const char *id, size_t id_len,
                   const char *name, size_t name_len,
                   const char *args, size_t args_len);

#endif
