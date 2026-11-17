#include "check.h"
#include "sco.h"

struct cap { uint8_t d[16][32]; size_t n[16]; int k; };
static void on_frame(const uint8_t *f, size_t n, void *c) { struct cap *p = c; if (p->k < 16) { memcpy(p->d[p->k], f, n); p->n[p->k] = n; } p->k++; }

int main(void)
{
    CHECK(sco_alt_for_voice(0x0060) == 2, "16-bit voice (Linux default) -> alternate setting 2");
    CHECK(sco_alt_for_voice(0x0040) == 1, "8-bit voice -> alternate setting 1");

    /* the exact command the PC sent in the capture: Setup Synchronous Connection, transparent voice (mSBC) */
    static const uint8_t pkt0[] = { 0x03, 0x0c, 0x00 };
    static const uint8_t setup_msbc[] = { 0x28,0x04,0x11, 0x0c,0x00, 0x40,0x1f,0x00,0x00, 0x40,0x1f,0x00,0x00, 0x0d,0x00, 0x03,0x00, 0x02, 0x80,0x03 };
    int alt = 0;
    CHECK(hci_cmd_sync_conn_alt(setup_msbc, sizeof setup_msbc, 2, &alt) && alt == 1, "mSBC setup -> alternate setting 1");
    uint8_t cvsd[sizeof setup_msbc];
    memcpy(cvsd, setup_msbc, sizeof cvsd);
    cvsd[15] = 0x60; cvsd[16] = 0x00;
    CHECK(hci_cmd_sync_conn_alt(cvsd, sizeof cvsd, 1, &alt) && alt == 2, "CVSD 16-bit setup -> alternate setting 2");
    CHECK(!hci_cmd_sync_conn_alt(pkt0, 3, 1, &alt), "other commands ignored");

    /* chunking: 3-byte header + 48 bytes of audio into 17-byte frames */
    uint8_t pkt[51];
    for (int i = 0; i < 51; i++) pkt[i] = (uint8_t)i;
    pkt[2] = 48;
    struct cap c = { 0 };
    CHECK(sco_chunk(pkt, 51, 17, on_frame, &c) == 3 && c.n[0] == 17 && c.n[1] == 17 && c.n[2] == 17, "3 frames of 17");
    memset(&c, 0, sizeof c);
    CHECK(sco_chunk(pkt, 51, 9, on_frame, &c) == 6 && c.n[5] == 6, "6 frames of 9, last short");

    /* re-assembly from arbitrary frame sizes */
    struct sco_reasm r;
    struct cap out = { 0 };
    sco_reasm_reset(&r);
    int got = 0;
    for (size_t o = 0; o < 51; o += 17)
        got += sco_reasm_feed(&r, pkt + o, 17, on_frame, &out);
    CHECK(got == 1 && out.n[0] == 51 && !memcmp(out.d[0], pkt, 32), "17-byte frames re-assemble");

    memset(&out, 0, sizeof out); sco_reasm_reset(&r); got = 0;
    for (size_t o = 0; o < 51; o += 9)
        got += sco_reasm_feed(&r, pkt + o, 51 - o < 9 ? 51 - o : 9, on_frame, &out);
    CHECK(got == 1 && out.n[0] == 51, "9-byte frames re-assemble");

    /* two packets back to back in odd chunks */
    uint8_t two[102];
    memcpy(two, pkt, 51); memcpy(two + 51, pkt, 51);
    memset(&out, 0, sizeof out); sco_reasm_reset(&r); got = 0;
    for (size_t o = 0; o < 102; o += 13)
        got += sco_reasm_feed(&r, two + o, 102 - o < 13 ? 102 - o : 13, on_frame, &out);
    CHECK(got == 2 && out.n[0] == 51 && out.n[1] == 51, "two packets, 13-byte frames");

    /* zero-length audio payload */
    uint8_t empty[3] = { 1, 0, 0 };
    memset(&out, 0, sizeof out); sco_reasm_reset(&r);
    CHECK(sco_reasm_feed(&r, empty, 3, on_frame, &out) == 1 && out.n[0] == 3, "empty SCO packet");

    /* HCI helpers */
    uint8_t vs_cmd[5] = { 0x26, 0x0c, 2, 0x60, 0x00 };
    uint16_t v = 0;
    CHECK(hci_cmd_voice_setting(vs_cmd, 5, &v) && v == 0x0060, "Write_Voice_Setting parsed");
    uint8_t other[4] = { 0x03, 0x0c, 1, 0 };
    CHECK(!hci_cmd_voice_setting(other, 4, &v), "other commands ignored");
    uint8_t cc[6] = { 0x0e, 4, 1, 0x03, 0x0c, 0x00 };
    uint8_t st = 9;
    CHECK(hci_evt_is_cc(cc, 6, HCI_OP_RESET, &st) && st == 0, "reset command-complete recognised");
    CHECK(!hci_evt_is_cc(cc, 6, HCI_OP_WRITE_VOICE, &st), "different opcode not matched");

    uint8_t vc[16];
    size_t n = bcm_sco_over_hci_cmd(vc, sizeof vc);
    CHECK(n == 8 && vc[0] == 0x1c && vc[1] == 0xfc && vc[2] == 5 && vc[3] == 1, "vendor SCO-over-HCI command");
    CHECK(bcm_sco_over_hci_cmd(vc, 4) == 0, "too small buffer");

    TEST_DONE("sco");
}
