/* BDC framing: how Ethernet frames and firmware events travel over the bulk endpoints.
 *
 *   [flags prio flags2 data_offset] [data_offset*4 bytes of firmware signals] [Ethernet frame]
 *
 * flags = protocol version (2) << 4; flags2 low nibble = interface index.
 * Events are Ethernet frames of type 0x886c carrying a Broadcom event header.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BDC_HDR_LEN   4
#define ETH_HLEN_     14
#define ETH_P_BRCM    0x886c

/* Wrap an Ethernet frame for the host. Returns bytes written or 0 if cap is too small. */
size_t bdc_wrap_data(uint8_t *out, size_t cap, uint8_t ifidx, const uint8_t *eth, size_t eth_len);

/* Build a complete firmware event (BDC header included). `data` is the event payload.
 * Returns bytes written or 0 if cap is too small. */
size_t bdc_build_event(uint8_t *out, size_t cap, const uint8_t mac[6], uint32_t code,
                       uint32_t status, uint32_t reason, uint16_t flags,
                       const uint8_t addr[6], uint8_t ifidx,
                       const void *data, size_t data_len);

/* Parse a BDC frame from the host (bulk OUT). Returns 0 and the Ethernet frame location,
 * or -1 if the frame is malformed. */
int bdc_unwrap(const uint8_t *in, size_t len, uint8_t *ifidx, const uint8_t **eth, size_t *eth_len);
