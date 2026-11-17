#include "bdc.h"

#include <string.h>

#define BDC_VERSION 2
#define EVENT_MSG_LEN 48

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static size_t put_bdc_hdr(uint8_t *out, uint8_t ifidx)
{
    out[0] = BDC_VERSION << 4;
    out[1] = 0;                       /* priority */
    out[2] = ifidx & 0x0f;
    out[3] = 0;                       /* no firmware signals */
    return BDC_HDR_LEN;
}

size_t bdc_wrap_data(uint8_t *out, size_t cap, uint8_t ifidx, const uint8_t *eth, size_t eth_len)
{
    if (cap < BDC_HDR_LEN + eth_len)
        return 0;
    put_bdc_hdr(out, ifidx);
    memcpy(out + BDC_HDR_LEN, eth, eth_len);
    return BDC_HDR_LEN + eth_len;
}

size_t bdc_build_event(uint8_t *out, size_t cap, const uint8_t mac[6], uint32_t code,
                       uint32_t status, uint32_t reason, uint16_t flags,
                       const uint8_t addr[6], uint8_t ifidx,
                       const void *data, size_t data_len)
{
    size_t total = BDC_HDR_LEN + ETH_HLEN_ + 10 + EVENT_MSG_LEN + data_len;
    if (cap < total)
        return 0;

    uint8_t *p = out;
    p += put_bdc_hdr(p, ifidx);

    /* Ethernet header: to/from the dongle itself, ethertype 0x886c */
    memcpy(p, mac, 6);  memcpy(p + 6, mac, 6);
    put_be16(p + 12, ETH_P_BRCM);
    p += ETH_HLEN_;

    /* brcm_ethhdr: subtype 0x8001 (vendor long), length, version, OUI 00:10:18, usr_subtype 1 (event) */
    put_be16(p + 0, 0x8001);
    put_be16(p + 2, (uint16_t)(EVENT_MSG_LEN + 6 + data_len));
    p[4] = 0;
    p[5] = 0x00; p[6] = 0x10; p[7] = 0x18;
    put_be16(p + 8, 1);
    p += 10;

    /* brcmf_event_msg_be */
    memset(p, 0, EVENT_MSG_LEN);
    put_be16(p + 0, 2);               /* version */
    put_be16(p + 2, flags);
    put_be32(p + 4, code);
    put_be32(p + 8, status);
    put_be32(p + 12, reason);
    put_be32(p + 16, 0);              /* auth_type */
    put_be32(p + 20, (uint32_t)data_len);
    if (addr)
        memcpy(p + 24, addr, 6);
    memcpy(p + 30, "wlan0", 6);       /* ifname[16] */
    p[46] = ifidx;
    p[47] = 0;                        /* bsscfgidx */
    p += EVENT_MSG_LEN;

    if (data_len)
        memcpy(p, data, data_len);
    return total;
}

int bdc_unwrap(const uint8_t *in, size_t len, uint8_t *ifidx, const uint8_t **eth, size_t *eth_len)
{
    if (len <= BDC_HDR_LEN)
        return -1;
    if ((in[0] >> 4) != BDC_VERSION)
        return -1;
    size_t off = BDC_HDR_LEN + ((size_t)in[3] << 2);
    if (off + ETH_HLEN_ > len)
        return -1;
    if (ifidx)
        *ifidx = in[2] & 0x0f;
    *eth = in + off;
    *eth_len = len - off;
    return 0;
}
