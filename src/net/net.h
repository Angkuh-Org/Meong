#ifndef MEONG_NET_H
#define MEONG_NET_H

#include <stddef.h>

#include "util/util.h"

typedef struct {
    char host[256];
    int port;
    char base[256];
    char host_header[320];
} Endpoint;

typedef struct Stream Stream;
typedef int (*sse_cb)(void *ud, const char *data, size_t len);

int endpoint_parse(const char *url, Endpoint *ep, char *err, size_t errlen);

int stream_start(Stream **out, const Endpoint *ep, const char *method,
                 const char *path, const char *body, size_t body_len,
                 int expect_sse, sse_cb cb, void *ud, int idle_timeout_ms,
                 char *err, size_t errlen);
int stream_tick(Stream *s, size_t *progress);
void stream_free(Stream *s);

int stream_status(const Stream *s);
int stream_was_sse(const Stream *s);
const Buf *stream_body(const Stream *s);
const char *stream_error(const Stream *s);
const char *stream_content_type(const Stream *s);

#endif
