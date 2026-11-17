/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Re-frames the byte stream arriving on the bulk OUT endpoint into whole HCI ACL packets.
 *
 * read() on the endpoint returns whatever the USB layer delivered: possibly part of a
 * packet, possibly several back to back. An ACL packet is a 4-byte header
 * (handle+flags u16, data length u16, little endian) followed by the data.
 *
 * A bogus length must not make us wait forever or grow without bound, so payloads
 * above ACL_MAX_PAYLOAD are treated as corruption: everything buffered is dropped and
 * the rest of that feed is ignored (the next USB transfer starts a fresh packet).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ACL_HDR_LEN     4
#define ACL_MAX_PAYLOAD 8192

struct acl_split {
    uint8_t       buf[ACL_HDR_LEN + ACL_MAX_PAYLOAD];
    size_t        len;
    unsigned long resyncs;           /* how often a bogus header made us drop data */
};

typedef void (*acl_emit_fn)(const uint8_t *pkt, size_t len, void *ctx);

void acl_split_reset(struct acl_split *s);
/* Feeds n bytes; emit() is called once per complete packet. Returns the number of
 * packets emitted, or -1 if corruption was detected (packets emitted before it still count
 * as delivered). */
int  acl_split_feed(struct acl_split *s, const uint8_t *data, size_t n, acl_emit_fn emit, void *ctx);
