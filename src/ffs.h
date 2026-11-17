/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FunctionFS plumbing shared by every USB function this gadget exposes.
 *
 * FunctionFS wants two blobs written to ep0, in this order:
 *   1. descriptors: the interface and endpoints of the function
 *   2. strings:     human-readable names the descriptors refer to by index
 * Only after both are accepted do the ep1..epN files appear (numbered in
 * *descriptor order*, not by address).
 */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/usb/functionfs.h>

#include "pktq.h"

#define FFS_MAX_EPS 4
#define FFS_MAX_ALTS 3

struct ffs_ep_spec {
    uint8_t  addr;                 /* bit 7 set = IN (device -> host); number is a placeholder */
    uint8_t  attrs;                /* USB_ENDPOINT_XFER_* */
    uint16_t maxp_fs, maxp_hs;     /* wMaxPacketSize at full / high speed */
    uint8_t  interval_fs, interval_hs;
};

struct ffs_fn_spec {
    uint8_t cls, sub, proto;       /* interface class triple */
    uint8_t n_eps;
    struct ffs_ep_spec eps[FFS_MAX_EPS];
    /* FUNCTIONFS_ALL_CTRL_RECIP: also receive control requests addressed to the
     * *device* (not just this interface). Exactly one function may claim them. */
    /* A second interface (number 1) with alternate settings, as the Bluetooth USB transport has for SCO audio:
     * extra[i] describes alternate setting extra[i].alt. FunctionFS does not allow one endpoint in two alternate
     * settings, so every alternate setting brings its own endpoints (distinct numbers). */
    uint8_t n_extra;
    struct { uint8_t alt, n_eps; struct ffs_ep_spec eps[FFS_MAX_EPS]; } extra[FFS_MAX_ALTS];
    bool    all_ctrl_recip;
    /* The function has no control interface of its own: its only interface (number 0) is the one in extra[]. Used for the
     * audio interface, which lives in a function of its own so that an alternate-setting change (FunctionFS re-enables every
     * endpoint of the function on one) leaves the Bluetooth control/ACL endpoints alone. */
    bool    no_primary;
    const char *name;              /* string index 1 (iInterface) */
};

/* Both return bytes written, or 0 if cap is too small. */
size_t ffs_build_descriptors(const struct ffs_fn_spec *s, uint8_t *out, size_t cap);
size_t ffs_build_strings(const char *name, uint8_t *out, size_t cap);

/* Opens <dir>/ep0 and writes the descriptor and strings blobs. Returns ep0 fd or -errno. */
int ffs_start(const char *dir, const struct ffs_fn_spec *spec);
/* Waits for <dir>/ep<idx> to appear and opens it. Returns fd or -errno. */
int ffs_open_ep(const char *dir, int idx, int oflag, int timeout_ms);
/* Reads one ep0 event; 0 on success, -errno otherwise (EINTR is retried). */
int ffs_read_event(int ep0, struct usb_functionfs_event *ev);
/* Stalls the control request currently pending, by doing I/O in the wrong direction. */
void ffs_stall(int ep0, bool request_is_in);
const char *ffs_event_name(unsigned type);

#include <stdatomic.h>

/* ---- IN endpoint writer thread -------------------------------------------------
 * Pops packets from q and writes each to fd. Handles the cases that matter:
 *   EINTR            retried
 *   ESHUTDOWN/ENODEV/ECONNRESET/...  host went away: drop the packet and carry on
 *   short write      reported (FunctionFS writes are all-or-nothing, so it is a bug)
 * With zlp set, a packet that is an exact multiple of maxp is followed by a
 * zero-length write, otherwise the host cannot tell the transfer has ended. */
struct ep_writer {
    int          fd;
    struct pktq *q;
    unsigned     maxp;               /* packet size for the ZLP rule; 0 = bulk endpoint, ask ffs_bulk_maxp() */
    bool         zlp;
    const char  *name;
    pthread_t    th;
    unsigned long wrote, dropped, kicks, flushes;
    /* Written by the writer thread, read by the watchdog: start time (ms) of the write in
     * progress, 0 when idle. */
    /* Optional keep-alive: when the pipe has been idle this long, send keepalive[] (a frame the host ignores).
     * Why: on the Pi's dwc2 controller the chance that an IN write hangs grows with the time the endpoint sat idle
     * before it (measured: ~1/10 after >0.5 s, ~1/1000 after 25 ms, ~1/13000 after 3 ms, none in 90000 writes at
     * 0.5 ms). Keeping the pipe from ever idling removes the problem. 0 = off. */
    bool         drop_on_kick;       /* a write that has to be re-queued is dropped instead (isochronous audio: late is lost) */
    unsigned     keepalive_ms;
    const uint8_t *keepalive;
    size_t       keepalive_len;
    atomic_bool *keepalive_active;   /* only send while this is true (host enabled) */
    _Atomic uint64_t busy_since;
    /* Start (ms) of the packet currently being written, across re-queues; 0 when idle. */
    _Atomic uint64_t pkt_start;
    /* Writes completed since the host last (re)enabled the device. Until it is non-zero the host driver may still be
     * probing and not reading yet, so the watchdog is much more patient. */
    atomic_ulong ok_since_enable;
};
int ep_writer_start(struct ep_writer *w);
/* The host (re)enabled the device: it has not proven it reads this endpoint yet. */
void ep_writer_host_enabled(struct ep_writer *w);

/* The Pi's dwc2 controller can wedge a bulk IN transfer that is started while a control transfer is
 * in flight (the IN write then never completes). One process-wide lock serializes "handle one control
 * request" against "one bulk/interrupt IN write attempt" across every function. */
/* Tells the watchdog how to reconnect the gadget (UDC file + UDC name). */
void ffs_set_udc(const char *udc_file, const char *udc_name);
/* Bulk packet size of the negotiated link: 512 at high speed, 64 at full speed. A bulk transfer that is an exact
 * multiple of it needs a zero-length packet to end it. Call ffs_refresh_speed() when the host enables the device. */
void     ffs_refresh_speed(void);
unsigned ffs_bulk_maxp(void);
void ffs_io_lock(void);
void ffs_io_unlock(void);

/* Classification used by the writer and exposed for tests. */
bool ffs_errno_is_host_gone(int err);
