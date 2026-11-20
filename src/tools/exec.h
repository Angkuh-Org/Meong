#ifndef MEONG_TOOLS_EXEC_H
#define MEONG_TOOLS_EXEC_H

#include <stddef.h>

#include "json/json.h"
#include "tools/tools.h"

int tool_run_read_file(const JDoc *d, int args, void *ud,
                       ToolAuthorizeFn authorize, ToolAsk *ask, Buf *out,
                       char *err, size_t err_cap);

int tool_run_search(const JDoc *d, int args, void *ud,
                    ToolAuthorizeFn authorize, ToolAsk *ask, Buf *out,
                    char *err, size_t err_cap);

int tool_run_list_dir(const JDoc *d, int args, void *ud,
                      ToolAuthorizeFn authorize, ToolAsk *ask, Buf *out,
                      char *err, size_t err_cap);

#endif
