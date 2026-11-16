#ifndef MEONG_API_H
#define MEONG_API_H

#include <stddef.h>

#include "app/app.h"

int extract_api_error(const char *json, size_t len, char *out, size_t cap);
/*
 * cached_tokens is a -1 sentinel when the gateway did not report
 * usage.prompt_tokens_details.cached_tokens; 0 is a real "nothing cached".
 * Token outs are optional and fill whenever usage parses, even if the text
 * walk fails.
 */
int extract_chat_text(const char *json, size_t len, Buf *out,
                      long *prompt_tokens, long *completion_tokens,
                      long *cached_tokens);
void debug_dump_body(const Buf *b);
void format_http_error(int status, const Buf *body, char *out, size_t cap);
int drive_stream(Stream *s, double total_timeout, size_t *progress);

void models_free(char **ids, int n);
int models_fetch_live(const Endpoint *ep, char ***out, ModelCaps **caps_out,
                      int *out_n, char *err, size_t errlen);
int models_load_cache(App *a);
void models_clear(App *a);
void models_replace(App *a, char **ids, ModelCaps *caps, int n);
const ModelCaps *model_caps_for(const App *a, const char *id);
int caps_reported(const ModelCaps *c);
const char *choose_default_model(App *a);

#endif
