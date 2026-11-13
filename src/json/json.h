#ifndef MEONG_JSON_H
#define MEONG_JSON_H

#include "util/util.h"

enum {
    /* jsmn token types are bit flags; json.c asserts they still match. */
    J_UNDEFINED = 0,
    J_OBJECT = 1,
    J_ARRAY = 2,
    J_STRING = 4,
    J_PRIMITIVE = 8,
};

/*
 * Zero-initialize a JDoc before its first parse.  jdoc_parse() keeps the
 * token and skip storage across calls, so a streaming loop reuses one JDoc
 * instead of allocating per event.  jdoc_free() releases it once.
 */
typedef struct {
    void *tok;
    int *skip;
    int ntok;
    int cap;
    const char *text;
} JDoc;

int jdoc_parse(JDoc *d, const char *text, size_t len);
void jdoc_free(JDoc *d);

int jdoc_root(const JDoc *d);
int jget(const JDoc *d, int obj, const char *key);
int jlen(const JDoc *d, int arr);
int jgeti(const JDoc *d, int arr, int index);
int jtype(const JDoc *d, int tok);
int jstring(const JDoc *d, int tok, Buf *out);
char *jstring_alloc(const JDoc *d, int tok);
long long jint(const JDoc *d, int tok, long long def);
void json_unescape(const char *s, int n, Buf *out);

/*
 * Fast path for OpenAI-compatible SSE chunks.  Returns 0 only when the frame
 * is a plain object that the common delta/usage shape fully explains; -1 means
 * the caller must fall back to jdoc_parse() (errors, odd shapes, malformed
 * input).  Spans point into the caller's frame and still need json_unescape().
 */
typedef struct {
    const char *content;
    const char *reason;
    int content_len;
    int reason_len;
    long long prompt_tokens;
    long long completion_tokens;
    unsigned char has_content;
    unsigned char has_reason;
    unsigned char has_ptok;
    unsigned char has_ctok;
} JDelta;

int jdelta_scan(const char *text, size_t len, JDelta *out);

#endif
