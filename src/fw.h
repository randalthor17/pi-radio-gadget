/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Emulated Broadcom FullMAC firmware, as seen by the PC's brcmfmac driver.
 *
 * It answers the driver's ioctl/iovar dialogue ("dcmds"), turns the driver's scan and join
 * requests into backend operations, and turns backend outcomes into firmware events.
 * Pure logic: no USB, no sockets. Tests drive it directly.
 */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend.h"

/* Called with a complete BDC frame (events and data) destined for the host's bulk IN. */
typedef void (*fw_to_host_fn)(const uint8_t *bdc, size_t len, void *ctx);

struct fw_kv {
    char    name[40];
    uint8_t val[256];
    size_t  len;
};

#define FW_MAX_KV 160

struct fw {
    pthread_mutex_t mu;
    uint8_t  mac[6];
    struct wl_backend *be;
    fw_to_host_fn to_host;
    void    *to_host_ctx;

    /* tunables */
    bool     advertise_sae;

    /* scan */
    bool     scan_active;
    uint16_t scan_sync_id;
    unsigned scan_results;

    /* association */
    enum { FW_IDLE, FW_JOINING, FW_UP, FW_AP } state;     /* FW_AP: running a hotspot */
    struct wl_bss cur_bss;            /* the BSS we joined, for GET_BSS_INFO / assoc IEs */
    bool     have_bss;
    bool     host_enabled;            /* USB host configured us: events are only delivered then */

    /* hotspot */
    bool     ap_mode;                 /* the driver set AP mode (WLC_SET_AP) */
    struct wl_ap_req ap;              /* what is running */
    struct { uint8_t mac[6]; uint64_t since_ms; } sta[16];   /* connected stations */
    int      n_sta;

    /* join inputs the driver sets before the join iovar/ioctl */
    uint32_t wsec, wpa_auth, auth, mfp, sup_wpa;
    bool     have_pmk, have_pass, have_sae;
    uint8_t  pmk[32];
    char     pass[65];
    char     sae_pw[129];

    uint8_t  event_mask[32];
    uint32_t ioctl_int[512];          /* last value written by "set" ioctls, returned by "get" */
    bool     ioctl_set[512];
    struct fw_kv kv[FW_MAX_KV];
    int      n_kv;

    unsigned long tx_frames, rx_frames, tx_dropped, rx_dropped;
};

void fw_init(struct fw *f, const uint8_t mac[6], struct wl_backend *be,
             fw_to_host_fn to_host, void *ctx);
void fw_destroy(struct fw *f);

/* Host -> dongle control message (BCDC dcmd). Writes a complete response (header + data) into
 * resp and returns its length, or 0 if the request was malformed. */
size_t fw_dcmd(struct fw *f, const uint8_t *req, size_t req_len, uint8_t *resp, size_t resp_cap);

/* A (re)probe by the host driver starts from a clean firmware state. */
void fw_reset(struct fw *f);

/* Host -> dongle data (BDC frame from the bulk OUT endpoint). */
void fw_host_data(struct fw *f, const uint8_t *bdc, size_t len);

/* The USB host configured / deconfigured the device. */
void fw_host_enable(struct fw *f, bool enabled);

/* ---- callbacks used by backends (any thread) */
void fw_scan_result(struct fw *f, const struct wl_bss *bss);
void fw_scan_done(struct fw *f, bool aborted);
void fw_join_done(struct fw *f, enum wl_join_result r, const struct wl_bss *bss);
void fw_link_lost(struct fw *f, uint16_t reason);
void fw_rx_frame(struct fw *f, const uint8_t *eth, size_t len);   /* AP -> PC */
/* Hotspot: a station connected or left (called by the backend after ap_start). */
void fw_ap_station(struct fw *f, const uint8_t mac[6], bool connected);
/* The hotspot could not be brought up, or died: the firmware goes back to idle. */
void fw_ap_lost(struct fw *f);
/* One line: state, hotspot stations, frame counters (and the backend's). Returns the length. */
size_t fw_stats_line(struct fw *f, char *buf, size_t size);

/* Exposed for tests. */
size_t fw_build_bss_info(uint8_t *out, size_t cap, const struct wl_bss *b);
size_t fw_trim_ies(const uint8_t *ie, size_t len, size_t max);
