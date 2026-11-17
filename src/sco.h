/* Bluetooth SCO (call audio) over the USB transport, as pure logic.
 *
 * On USB a SCO packet (3-byte header: handle+flags u16, data length u8, then the audio) travels on isochronous
 * endpoints cut into frames of at most the endpoint's packet size, one frame per millisecond. Which pair of
 * endpoints is live depends on the alternate setting the host chose; FunctionFS never reports that, so it is
 * derived from the voice setting the host programmed (the Linux driver picks alt 2 for 16-bit voice, alt 1 for
 * 8-bit). Only one SCO link at a time is supported.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCO_HDR_LEN     3
#define SCO_MAX_PAYLOAD 255
#define SCO_ALT1_MAXP   63
#define SCO_ALT2_MAXP   63

/* Voice setting (HCI Write_Voice_Setting) -> alternate setting used by the host for one link: 1 or 2. */
int sco_alt_for_voice(uint16_t voice_setting);

/* Splits one SCO packet (header + data) into frames of at most maxp bytes. Returns the number of frames. */
typedef void (*sco_frame_fn)(const uint8_t *frame, size_t len, void *ctx);
int sco_chunk(const uint8_t *pkt, size_t len, size_t maxp, sco_frame_fn emit, void *ctx);

/* Re-assembles SCO packets from a stream of frames. */
struct sco_reasm {
    uint8_t       buf[SCO_HDR_LEN + SCO_MAX_PAYLOAD];
    size_t        len;
    unsigned long dropped;          /* partial packets discarded because a frame was lost or malformed */
};
typedef void (*sco_pkt_fn)(const uint8_t *pkt, size_t len, void *ctx);
void sco_reasm_reset(struct sco_reasm *r);
int  sco_reasm_feed(struct sco_reasm *r, const uint8_t *frame, size_t n, sco_pkt_fn emit, void *ctx);

/* HCI helpers. cmd = HCI command packet without the H4 byte, evt = HCI event packet without it. */
bool hci_cmd_is(const uint8_t *cmd, size_t len, uint16_t opcode);
/* A synchronous-connection command (Setup 0x0428, Accept 0x0429, Enhanced Setup 0x043D, Enhanced Accept 0x043E): sets *alt to the
 * alternate setting the Linux driver will use for that link (transparent/mSBC: 1; CVSD: by 16-bit bit of the voice setting, else keep). */
bool hci_cmd_sync_conn_alt(const uint8_t *cmd, size_t len, int cur_alt, int *alt);
bool hci_cmd_voice_setting(const uint8_t *cmd, size_t len, uint16_t *voice);          /* Write_Voice_Setting */
bool hci_evt_is_cc(const uint8_t *evt, size_t len, uint16_t opcode, uint8_t *status);  /* Command_Complete */

/* Broadcom vendor command (0xFC1C Write_SCO_PCM_Int_Param) that routes SCO audio over HCI instead of the chip's PCM pins.
 * The chip forgets it on every HCI reset. Fills cmd (without the H4 byte); returns its length. */
size_t bcm_sco_over_hci_cmd(uint8_t *cmd, size_t cap);
#define BCM_OP_SCO_PCM_PARAM 0xFC1C
#define HCI_OP_RESET         0x0C03
#define HCI_OP_WRITE_VOICE   0x0C26
