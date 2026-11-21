#ifndef MEONG_TOOLS_WRITE_H
#define MEONG_TOOLS_WRITE_H

#include "json/json.h"
#include "tools/tools.h"

int tool_prep_write(const char *cwd, const char *resolved, const JDoc *d,
                    int root, ToolPrep *out, char *err, size_t err_cap);

int tool_run_write(const char *cwd, const JDoc *d, int args, void *ud,
                   ToolAuthorizeFn authorize, ToolAsk *ask,
                   const ToolPrep *prep, Buf *out, char *err, size_t err_cap);

#endif
