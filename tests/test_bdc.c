/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bdc.h"
#include "check.h"

int main(void)
{
    uint8_t eth[60], out[256];
    memset(eth, 0xab, sizeof eth);
    eth[12] = 0x08; eth[13] = 0x00;

    /* data wrap/unwrap round trip */
    size_t n = bdc_wrap_data(out, sizeof out, 0, eth, sizeof eth);
    CHECK(n == 64 && out[0] == 0x20 && out[2] == 0 && out[3] == 0, "BDC header: version 2, ifidx 0, no signals");
    uint8_t ifidx = 9;
    const uint8_t *e; size_t el;
    CHECK(bdc_unwrap(out, n, &ifidx, &e, &el) == 0 && ifidx == 0 && el == 60 && !memcmp(e, eth, 60), "round trip");
    CHECK(bdc_wrap_data(out, 63, 0, eth, sizeof eth) == 0, "too small buffer refused");

    /* firmware signals (data_offset words) are skipped on the way in */
    uint8_t in[128];
    in[0] = 0x20; in[1] = 0; in[2] = 0; in[3] = 2;              /* 2 words = 8 bytes of signals */
    memset(in + 4, 0xee, 8);
    memcpy(in + 12, eth, 60);
    CHECK(bdc_unwrap(in, 72, NULL, &e, &el) == 0 && el == 60 && !memcmp(e, eth, 60), "signals skipped");
    CHECK(bdc_unwrap(in, 4, NULL, &e, &el) < 0, "header only is malformed");
    in[0] = 0x10;
    CHECK(bdc_unwrap(in, 72, NULL, &e, &el) < 0, "wrong protocol version rejected");

    /* event layout, field by field (big endian), as brcmf_fweh_process_event() reads it */
    const uint8_t mac[6] = { 2, 1, 2, 3, 4, 5 }, ap[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    const uint8_t payload[3] = { 7, 8, 9 };
    n = bdc_build_event(out, sizeof out, mac, 69, 8, 2, 1, ap, 0, payload, 3);
    CHECK(n == 4 + 14 + 10 + 48 + 3, "event length %zu", n);
    const uint8_t *p = out + 4;
    CHECK(p[12] == 0x88 && p[13] == 0x6c, "ethertype 0x886c");
    p += 14;
    CHECK(p[0] == 0x80 && p[1] == 0x01, "subtype 0x8001");
    CHECK(p[5] == 0x00 && p[6] == 0x10 && p[7] == 0x18, "Broadcom OUI");
    CHECK(p[8] == 0 && p[9] == 1, "usr_subtype 1 = event");
    p += 10;
    CHECK(p[2] == 0 && p[3] == 1, "flags (be16) = 1");
    CHECK(p[4] == 0 && p[7] == 69, "event type (be32) = 69");
    CHECK(p[11] == 8, "status = 8");
    CHECK(p[15] == 2, "reason = 2");
    CHECK(p[23] == 3, "datalen = 3");
    CHECK(!memcmp(p + 24, ap, 6), "addr");
    CHECK(!memcmp(out + n - 3, payload, 3), "payload at the end");
    CHECK(bdc_build_event(out, 50, mac, 0, 0, 0, 0, ap, 0, NULL, 0) == 0, "too small buffer refused");

    TEST_DONE("bdc");
}
