/* SPDX-License-Identifier: GPL-2.0-or-later */
/* WiFi function: the PC's stock Linux `brcmfmac` driver sees a Broadcom FullMAC USB WiFi
 * dongle that is already running its firmware, and talks to an emulation of that firmware
 * (fw.c) backed by the Pi's own radio.
 *
 *   ep0 control  vendor IN  DL_GETVER etc. (the bootloader dialogue; answered "firmware already
 *                           running", so the driver skips the firmware download)
 *                class OUT 0  SEND_ENCAPSULATED_COMMAND   the host's ioctl/iovar request (BCDC)
 *                class IN  1  GET_ENCAPSULATED_RESPONSE   our answer
 *   bulk OUT     BDC data frames from the host (Ethernet frames to transmit)
 *   bulk IN      BDC data frames to the host (received frames) and firmware events
 */
#pragma once
#include <stdatomic.h>
#include "fw.h"
#include "ffs.h"
#include "pktq.h"

#define WLAN_USB_VID 0x0a5c
#define WLAN_USB_PID 0x0bdc      /* "Broadcom BCM firmware-loaded dongle": driver skips the download */

struct wlan_fn {
    char     ffs_dir[128];
    uint8_t  mac[6];
    struct wl_backend *be;
    int      ep0, ep_in, ep_out;
    struct fw fw;
    struct pktq q_in;
    struct ep_writer w_in;
    atomic_bool enabled;             /* the USB host has the device configured */
    unsigned keepalive_ms;           /* 0 = off */
    uint8_t  keepalive_frame[96];
    uint8_t  resp[DCMD_MAXLEN + 64];
    size_t   resp_len;
};

void wlan_fill_spec(struct ffs_fn_spec *spec);
int  wlan_fn_prepare(struct wlan_fn *w);      /* before the UDC is attached */
int  wlan_fn_run(struct wlan_fn *w);          /* after */
int  wlan_fn_start_backend(struct wlan_fn *w);   /* last: slow */
