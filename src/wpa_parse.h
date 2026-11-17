/* Pure helpers for talking to wpa_supplicant's control interface: text parsing and the mapping
 * from the firmware-style join parameters to supplicant network settings. No I/O here, so it is
 * all unit tested. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend.h"

/* ---- encoding */
size_t wpa_hex_encode(const void *in, size_t n, char *out, size_t cap);          /* returns chars written */
int    wpa_hex_decode(const char *hex, uint8_t *out, size_t cap);                /* bytes, or -1 */
int    wpa_freq_to_chan(int mhz);                                                /* 2.4 GHz only, 0 = unknown */
int    wpa_chan_to_freq(int chan);

/* ---- replies */
/* Value of "key=" at the start of a line, or NULL. The result points into text and is NOT
 * NUL-terminated at the line end; len gets the line length. */
const char *wpa_kv(const char *text, const char *key, size_t *len);
/* Parses one `BSS <n>` reply. Returns false if it is not a usable 2.4 GHz BSS. */
bool wpa_parse_bss(const char *text, struct wl_bss *out, int *age_s);
/* Extracts "reason=N" / "status_code=N" style integers from an event line, or -1. */
int  wpa_event_int(const char *msg, const char *key);

/* ---- requests */
/* "SCAN freq=2412,2437 ssid 686f6d65" (no directed SSIDs when none is requested). */
void wpa_build_scan(const struct wl_scan_req *req, bool with_directed, int directed_idx, char *out, size_t cap);

#define WPA_MAX_NET_OPTS 16
struct wpa_net_opt { const char *key; char value[160]; };
struct wpa_net {
    struct wpa_net_opt opt[WPA_MAX_NET_OPTS];
    int  n;
    bool unsupported;                 /* an auth mode we cannot honour (e.g. 802.1X, WEP) */
    const char *why;
};
/* Translates a firmware join request into SET_NETWORK key/value pairs. */
void wpa_net_from_join(const struct wl_join_req *j, struct wpa_net *net);
