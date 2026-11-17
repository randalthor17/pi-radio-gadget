/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "acl_split.h"

#include <string.h>

void acl_split_reset(struct acl_split *s)
{
    s->len = 0;
}

static size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

int acl_split_feed(struct acl_split *s, const uint8_t *data, size_t n, acl_emit_fn emit, void *ctx)
{
    int emitted = 0;

    while (n > 0) {
        if (s->len < ACL_HDR_LEN) {                       /* still collecting the header */
            size_t take = min_sz(ACL_HDR_LEN - s->len, n);
            memcpy(s->buf + s->len, data, take);
            s->len += take; data += take; n -= take;
            if (s->len < ACL_HDR_LEN)
                break;
        }
        size_t payload = (size_t)s->buf[2] | ((size_t)s->buf[3] << 8);
        if (payload > ACL_MAX_PAYLOAD) {
            s->len = 0;
            s->resyncs++;
            return -1;
        }
        size_t total = ACL_HDR_LEN + payload;
        size_t take = min_sz(total - s->len, n);
        memcpy(s->buf + s->len, data, take);
        s->len += take; data += take; n -= take;
        if (s->len == total) {
            emit(s->buf, total, ctx);
            emitted++;
            s->len = 0;
        }
    }
    return emitted;
}
