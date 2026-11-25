#ifndef MEONG_TOOLS_EXEC_H
#define MEONG_TOOLS_EXEC_H

#include <stddef.h>

#include "json/json.h"
#include "tools/tools.h"

int tool_run_read_file(const char *cwd, const JDoc *d, int args, void *ud,
                       ToolAuthorizeFn authorize, ToolAsk *ask,
                       const ToolPrep *prep, Buf *out, char *err,
                       size_t err_cap);

int tool_run_search(const char *cwd, const JDoc *d, int args, void *ud,
                    ToolAuthorizeFn authorize, ToolAsk *ask,
                    const ToolPrep *prep, Buf *out, char *err,
                    size_t err_cap);

int tool_run_list_dir(const char *cwd, const JDoc *d, int args, void *ud,
                      ToolAuthorizeFn authorize, ToolAsk *ask,
                      const ToolPrep *prep, Buf *out, char *err,
                      size_t err_cap);

int tool_run_glob(const char *cwd, const JDoc *d, int args, void *ud,
                  ToolAuthorizeFn authorize, ToolAsk *ask,
                  const ToolPrep *prep, Buf *out, char *err, size_t err_cap);

#endif
