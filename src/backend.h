/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What the emulated Broadcom firmware needs from "the radio". Two implementations:
 *   backend_sup.c   drives the Pi's real WiFi chip through wpa_supplicant + an AF_PACKET tap
 *   backend_fake.c  synthetic networks, no radio touched: for bring-up and tests
 *
 * Every operation is asynchronous: the backend reports outcomes through the fw_* callbacks
 * declared in fw.h, from any thread.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl.h"

struct wl_bss {
    uint8_t  bssid[6];
    uint8_t  ssid_len;
    uint8_t  ssid[32];
    uint16_t beacon_int;
    uint16_t capability;
    uint8_t  channel;
    int8_t   rssi;                    /* dBm */
    uint16_t ie_len;
    uint8_t  ie[BSS_MAX_IE];          /* the beacon/probe-response information elements */
};

#define WL_MAX_SCAN_SSIDS 4
#define WL_MAX_SCAN_CHANS 32

struct wl_scan_req {
    uint8_t  n_ssids;
    struct { uint8_t len; uint8_t ssid[32]; } ssids[WL_MAX_SCAN_SSIDS];   /* len 0 = wildcard */
    uint8_t  n_channels;              /* 0 = every channel */
    uint8_t  channels[WL_MAX_SCAN_CHANS];
    bool     passive;
};

struct wl_join_req {
    uint8_t  ssid_len;
    uint8_t  ssid[32];
    uint8_t  bssid[6];
    bool     bssid_any;
    uint8_t  channel;                 /* 0 = any */
    uint32_t wpa_auth;                /* WPA_AUTH_* bit mask */
    uint32_t wsec;                    /* cipher bits: 2 WEP? 4 TKIP, 8 AES */
    uint32_t mfp;                     /* 0 off, 1 capable, 2 required */
    bool     have_pmk;                /* 32-byte PMK derived by the PC (WPA/WPA2-PSK) */
    uint8_t  pmk[32];
    bool     have_passphrase;         /* raw passphrase (when the PC leaves derivation to us) */
    char     passphrase[65];
    bool     have_sae;
    char     sae_password[129];
};

struct wl_link {
    bool     up;
    uint8_t  bssid[6];
    uint8_t  ssid_len;
    uint8_t  ssid[32];
    uint8_t  channel;
    int      rssi;                    /* dBm */
    uint32_t rate_kbps;
};

/* A hotspot (access point) the host driver asks the radio to run. Only open and WPA2-PSK (CCMP) are supported. */
struct wl_ap_req {
    uint8_t  ssid_len;
    uint8_t  ssid[32];
    uint8_t  channel;                 /* 1..13 */
    bool     hidden;                  /* do not broadcast the SSID */
    uint16_t beacon_interval;         /* TU, 0 = default */
    uint8_t  dtim_period;             /* 0 = default */
    bool     wpa2_psk;                /* false = open */
    bool     psk_sha256;              /* also offer the PSK-SHA256 key management (needed for protected management frames) */
    uint8_t  pmf;                     /* 0 off, 1 optional, 2 required */
    bool     have_pmk;                /* the 32-byte PSK the host derived, or ... */
    uint8_t  pmk[32];
    bool     have_passphrase;         /* ... the passphrase */
    char     passphrase[65];
};

enum wl_join_result {
    WL_JOIN_OK = 0,
    WL_JOIN_NO_NETWORK,               /* nothing answered / SSID not found */
    WL_JOIN_AUTH_FAILED,              /* wrong key, 4-way handshake failed, rejected */
};

struct wl_backend;

struct wl_backend_ops {
    const char *name;
    int  (*start)(struct wl_backend *be);
    void (*stop)(struct wl_backend *be);
    void (*scan)(struct wl_backend *be, const struct wl_scan_req *req);
    void (*scan_abort)(struct wl_backend *be);
    void (*join)(struct wl_backend *be, const struct wl_join_req *req);
    void (*leave)(struct wl_backend *be);
    /* Ethernet frame from the PC towards the AP. */
    void (*tx)(struct wl_backend *be, const uint8_t *eth, size_t len);
    bool (*link)(struct wl_backend *be, struct wl_link *out);
    /* The host driver changed the dongle MAC address (NetworkManager randomizes it). Optional. */
    void (*set_mac)(struct wl_backend *be, const uint8_t mac[6]);
    /* The host driver chose a regulatory domain (ISO alpha-2, NUL-terminated). Optional. */
    void (*set_country)(struct wl_backend *be, const char cc[3]);
    /* Hotspot: start, stop, and remove one station. All optional (without them the firmware refuses AP mode).
 * Stations that (dis)connect are reported with fw_ap_station(). */
    void (*ap_start)(struct wl_backend *be, const struct wl_ap_req *req);
    void (*ap_stop)(struct wl_backend *be);
    void (*ap_deauth)(struct wl_backend *be, const uint8_t mac[6], uint16_t reason);
    /* One line of live counters for the status request. Optional. */
    size_t (*status)(struct wl_backend *be, char *buf, size_t size);
};

/* Backends must never call fw_* from inside one of the ops above (the firmware holds its lock
 * while calling them); hand the work to a thread and call back from there. link() likewise
 * must not call into the firmware. */
struct fw;
struct wl_backend {
    const struct wl_backend_ops *ops;
    struct fw *fw;                    /* set before start() */
    void *priv;
};

extern const struct wl_backend_ops wl_backend_fake_ops;
extern const struct wl_backend_ops wl_backend_sup_ops;

/* Backend-specific construction. */
struct wl_backend *wl_backend_fake_new(void);
struct wl_backend *wl_backend_sup_new(const char *ifname, const char *ctrl_dir);
