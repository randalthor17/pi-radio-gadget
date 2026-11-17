#include "sco.h"

#include <string.h>

int sco_alt_for_voice(uint16_t vs)
{
    return (vs & 0x0020) ? 2 : 1;      /* bit 5: 16-bit input samples */
}

int sco_chunk(const uint8_t *pkt, size_t len, size_t maxp, sco_frame_fn emit, void *ctx)
{
    int n = 0;
    if (!maxp)
        return 0;
    for (size_t o = 0; o < len; o += maxp, n++)
        emit(pkt + o, len - o < maxp ? len - o : maxp, ctx);
    return n;
}

void sco_reasm_reset(struct sco_reasm *r) { r->len = 0; }

int sco_reasm_feed(struct sco_reasm *r, const uint8_t *f, size_t n, sco_pkt_fn emit, void *ctx)
{
    int emitted = 0;
    while (n) {
        size_t want = r->len < SCO_HDR_LEN ? SCO_HDR_LEN : SCO_HDR_LEN + r->buf[2];
        size_t take = want - r->len;
        if (take > n)
            take = n;
        memcpy(r->buf + r->len, f, take);
        r->len += take; f += take; n -= take;
        if (r->len >= SCO_HDR_LEN && r->len == SCO_HDR_LEN + (size_t)r->buf[2]) {
            emit(r->buf, r->len, ctx);
            emitted++;
            r->len = 0;
        }
    }
    return emitted;
}

bool hci_cmd_is(const uint8_t *c, size_t len, uint16_t op)
{
    return len >= 3 && (uint16_t)(c[0] | c[1] << 8) == op && c[2] == len - 3;
}

bool hci_cmd_voice_setting(const uint8_t *c, size_t len, uint16_t *voice)
{
    if (!hci_cmd_is(c, len, HCI_OP_WRITE_VOICE) || len != 5)
        return false;
    *voice = (uint16_t)(c[3] | c[4] << 8);
    return true;
}

bool hci_evt_is_cc(const uint8_t *e, size_t len, uint16_t op, uint8_t *status)
{
    /* event code 0x0e, length, num_cmds, opcode(2), return params (status first) */
    if (len < 6 || e[0] != 0x0e || (uint16_t)(e[3] | e[4] << 8) != op)
        return false;
    if (status)
        *status = e[5];
    return true;
}

size_t bcm_sco_over_hci_cmd(uint8_t *cmd, size_t cap)
{
    /* routing=1 (HCI), PCM rate=2, frame type=0, sync mode=1, clock mode=1 (the documented Raspberry Pi values) */
    static const uint8_t c[] = { BCM_OP_SCO_PCM_PARAM & 0xff, BCM_OP_SCO_PCM_PARAM >> 8, 5, 1, 2, 0, 1, 1 };
    if (cap < sizeof c)
        return 0;
    memcpy(cmd, c, sizeof c);
    return sizeof c;
}

bool hci_cmd_sync_conn_alt(const uint8_t *c, size_t len, int cur_alt, int *alt)
{
    if (len < 3 || c[2] != len - 3)
        return false;
    uint16_t op = (uint16_t)(c[0] | c[1] << 8);
    size_t voice_off;                                   /* offset of the 16-bit voice setting in the command */
    switch (op) {
    case 0x0428: voice_off = 15; break;                 /* handle2 tx4 rx4 latency2 voice2 ... */
    case 0x0429: voice_off = 19; break;                 /* bdaddr6 tx4 rx4 latency2 voice2 ... */
    case 0x043D:                                        /* handle2 tx4 rx4 tx_coding5 ...: first byte is the codec id */
    case 0x043E:                                        /* bdaddr6 tx4 rx4 tx_coding5 ... */
    {
        size_t o = op == 0x043D ? 13 : 17;
        if (len <= o)
            return false;
        *alt = c[o] == 0x03 ? 1 : cur_alt;               /* 0x03 transparent (mSBC); CVSD keeps the voice-setting choice */
        return true;
    }
    default: return false;
    }
    if (len < voice_off + 2)
        return false;
    uint16_t vs = (uint16_t)(c[voice_off] | c[voice_off + 1] << 8);
    *alt = (vs & 0x0003) == 0x0003 ? 1 : sco_alt_for_voice(vs);   /* air coding transparent: alternate setting 1 */
    return true;
}
