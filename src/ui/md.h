#ifndef MEONG_MD_H
#define MEONG_MD_H

#include <stddef.h>

#include "app/app.h"

/*
 * Display buffer for assistant markdown.  The wire keeps Msg.text verbatim;
 * md_sync() appends a rendering to Msg.disp with zero-width control bytes:
 *
 *   01 <mask>          inline style (mask below)
 *   01 <0x80|kind> <hang>   display-line metadata, stripped by the layout
 *
 * The buffer is append-only: an ambiguous tail waits in MdState until the
 * next chunk decides it, so layout offsets never go stale.
 */
enum {
    MD_BOLD = 1,
    MD_ITALIC = 2,
    MD_CODE = 4,
    MD_STRIKE = 8,
    MD_LINK = 16,
    MD_DIM = 32,
    MD_ACCENT = 64,
};

enum {
    MDL_TEXT = 0,
    MDL_CODE,
    MDL_FENCE_OPEN,
    MDL_FENCE_CLOSE,
    MDL_RULE,
    MDL_HEAD,
    MDL_LIST,
    MDL_QUOTE,
    MDL_TABLE,
};

#define MD_CMD 0x01
#define MD_META 0x80

size_t md_sync(Msg *m, int unicode);
void md_reset(Msg *m);
void md_release(Msg *m);

/* Returns the line kind and hanging indent, or -1 for a plain text line. */
int md_line_meta(const char *s, size_t len, int *hang, size_t *skip);

#endif
