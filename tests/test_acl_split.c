/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "acl_split.h"
#include "check.h"

struct sink { int n; size_t len[8]; uint8_t first[8]; };

static void emit(const uint8_t *p, size_t len, void *ctx)
{
    struct sink *s = ctx;
    if (s->n < 8) { s->len[s->n] = len; s->first[s->n] = p[4]; }
    s->n++;
}

/* ACL packet: handle 0x0001, payload of `plen` bytes all equal to `fill`. */
static size_t mk(uint8_t *out, uint16_t plen, uint8_t fill)
{
    out[0] = 1; out[1] = 0; out[2] = plen & 0xff; out[3] = plen >> 8;
    for (unsigned i = 0; i < plen; i++) out[4 + i] = fill;
    return 4u + plen;
}

int main(void)
{
    static struct acl_split s;
    struct sink k;
    uint8_t a[64], b[64], both[128];

    /* one packet, one feed */
    memset(&k, 0, sizeof k); acl_split_reset(&s);
    size_t na = mk(a, 10, 0xA1);
    CHECK(acl_split_feed(&s, a, na, emit, &k) == 1 && k.n == 1 && k.len[0] == 14, "single packet");

    /* split across two feeds */
    memset(&k, 0, sizeof k);
    CHECK(acl_split_feed(&s, a, 7, emit, &k) == 0 && k.n == 0, "partial: nothing yet");
    CHECK(acl_split_feed(&s, a + 7, na - 7, emit, &k) == 1 && k.len[0] == 14, "completes on second feed");

    /* two packets in one feed */
    memset(&k, 0, sizeof k);
    size_t nb = mk(b, 3, 0xB2);
    memcpy(both, a, na); memcpy(both + na, b, nb);
    CHECK(acl_split_feed(&s, both, na + nb, emit, &k) == 2, "two packets");
    CHECK(k.len[0] == na && k.first[0] == 0xA1 && k.len[1] == nb && k.first[1] == 0xB2, "both intact, in order");

    /* header split in the middle of the length field */
    memset(&k, 0, sizeof k);
    CHECK(acl_split_feed(&s, b, 3, emit, &k) == 0, "3 bytes: inside the length field");
    CHECK(acl_split_feed(&s, b + 3, nb - 3, emit, &k) == 1 && k.len[0] == nb, "header split mid-field");

    /* one byte at a time */
    memset(&k, 0, sizeof k);
    for (size_t i = 0; i < na; i++)
        acl_split_feed(&s, a + i, 1, emit, &k);
    CHECK(k.n == 1 && k.len[0] == na, "byte-wise feed");

    /* a length above the cap is corruption: dropped, counted, and we recover afterwards */
    memset(&k, 0, sizeof k);
    uint8_t bad[4] = { 1, 0, 0xff, 0xff };
    CHECK(acl_split_feed(&s, bad, 4, emit, &k) == -1, "bogus length reported");
    CHECK(s.resyncs == 1 && s.len == 0, "buffer dropped");
    CHECK(acl_split_feed(&s, a, na, emit, &k) == 1, "recovers with the next valid packet");

    /* a maximum-size packet still works */
    static uint8_t big[ACL_HDR_LEN + ACL_MAX_PAYLOAD];
    memset(&k, 0, sizeof k);
    mk(big, ACL_MAX_PAYLOAD, 0x55);
    CHECK(acl_split_feed(&s, big, sizeof big, emit, &k) == 1 && k.len[0] == sizeof big, "max size packet");

    /* zero-length payload */
    memset(&k, 0, sizeof k);
    uint8_t z[4] = { 1, 0, 0, 0 };
    CHECK(acl_split_feed(&s, z, 4, emit, &k) == 1 && k.len[0] == 4, "empty ACL payload");

    TEST_DONE("acl_split");
}
