/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wlan.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <sys/syscall.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <linux/usb/ch9.h>

#include "bdc.h"
#include "log.h"

#define TAG "wlan"

/* Bootloader ("DL") requests, bRequest values (usb.c in brcmfmac) */
#define DL_GETSTATE 0
#define DL_GETVER   5
#define DL_RESETCFG 8
#define POSTBOOT_ID 0xA123

#define BRCM_CDC_SEND_ENCAPSULATED_COMMAND 0
#define BRCM_CDC_GET_ENCAPSULATED_RESPONSE 1

void wlan_fill_spec(struct ffs_fn_spec *s)
{
    memset(s, 0, sizeof *s);
    s->cls = 0xFF; s->sub = 0x02; s->proto = 0xFF;        /* what brcmfmac insists on */
    s->n_eps = 2;
    s->name = "Pi WLAN";
    s->all_ctrl_recip = false;                            /* interface-addressed requests only */
    s->eps[0] = (struct ffs_ep_spec){ 0x81, USB_ENDPOINT_XFER_BULK, 64, 512, 0, 0 };   /* IN  */
    s->eps[1] = (struct ffs_ep_spec){ 0x02, USB_ENDPOINT_XFER_BULK, 64, 512, 0, 0 };   /* OUT */
}

/* The fw layer hands us complete BDC frames for the host. */
static void to_host(const uint8_t *bdc, size_t len, void *ctx)
{
    struct wlan_fn *w = ctx;
    if (!pktq_put(&w->q_in, bdc, len))
        LOGD(TAG, "bulk IN queue full: dropped %zu bytes", len);
}

static void handle_dl(struct wlan_fn *w, uint8_t req, uint16_t len)
{
    uint8_t b[24] = { 0 };
    if (req == DL_GETVER) {
        /* struct bootrom_id_le: chip, chiprev, ramsize, remapbase, boardtype, boardrev */
        uint32_t id[6] = { POSTBOOT_ID, 1, 0x80000, 0, 0, 0 };
        for (int i = 0; i < 6; i++) {
            uint32_t v = htole32(id[i]);
            memcpy(b + 4 * i, &v, 4);
        }
        fw_reset(&w->fw);                 /* a probe is starting: forget any previous session */
        LOGI(TAG, "driver probing: reporting firmware already running");
    } else if (req == DL_GETSTATE || req == DL_RESETCFG) {
        LOGD(TAG, "DL request %u", req);
    } else {
        LOGW(TAG, "unexpected DL request %u", req);
    }
    size_t n = len < sizeof b ? len : sizeof b;
    if (write(w->ep0, b, n) < 0)
        LOGD(TAG, "DL reply failed: %s", strerror(errno));
}

static void ep0_setup(struct wlan_fn *w, const struct usb_ctrlrequest *r)
{
    bool in = r->bRequestType & USB_DIR_IN;
    unsigned type = r->bRequestType & USB_TYPE_MASK;
    uint16_t len = le16toh(r->wLength);
    LOGT(TAG, "setup %s type=%02x req=%02x len=%u", in ? "IN " : "OUT", r->bRequestType, r->bRequest, len);

    if (type == USB_TYPE_VENDOR && in) {
        handle_dl(w, r->bRequest, len);
        return;
    }
    if (type == USB_TYPE_CLASS && !in && r->bRequest == BRCM_CDC_SEND_ENCAPSULATED_COMMAND && len) {
        uint8_t req[DCMD_MAXLEN + 64];
        if (len > sizeof req) {
            LOGW(TAG, "oversized control request (%u bytes)", len);
            ffs_stall(w->ep0, false);
            return;
        }
        ssize_t n = read(w->ep0, req, len);               /* data stage of this OUT request */
        if (n <= 0) {
            LOGW(TAG, "control data stage failed: %s", n < 0 ? strerror(errno) : "empty");
            return;
        }
        w->resp_len = fw_dcmd(&w->fw, req, (size_t)n, w->resp, sizeof w->resp);
        if (!w->resp_len)
            LOGW(TAG, "malformed dcmd (%zd bytes)", n);
        return;
    }
    if (type == USB_TYPE_CLASS && in && r->bRequest == BRCM_CDC_GET_ENCAPSULATED_RESPONSE) {
        if (!w->resp_len) {
            LOGW(TAG, "response requested but none pending");
            ffs_stall(w->ep0, true);
            return;
        }
        size_t n = w->resp_len < len ? w->resp_len : len;
        uint64_t t0 = now_ms();
        ssize_t wr = write(w->ep0, w->resp, n);
        uint64_t dt = now_ms() - t0;
        if (wr < 0)
            LOGW(TAG, "response write failed: %s", strerror(errno));
        else if (dt > 100)
            LOGW(TAG, "response write took %llu ms", (unsigned long long)dt);
        w->resp_len = 0;
        return;
    }
    LOGW(TAG, "unhandled setup type=%02x req=%02x val=%04x idx=%04x len=%u", r->bRequestType, r->bRequest,
         le16toh(r->wValue), le16toh(r->wIndex), len);
    ffs_stall(w->ep0, in);
}

static void handle_event(struct wlan_fn *w, const struct usb_functionfs_event *ev)
{
    if (ev->type == FUNCTIONFS_SETUP) {
        ep0_setup(w, &ev->u.setup);
        return;
    }
    LOGI(TAG, "usb event %s", ffs_event_name(ev->type));
    switch (ev->type) {
    case FUNCTIONFS_ENABLE:
        ffs_refresh_speed();
        pktq_flush(&w->q_in);
        atomic_store(&w->enabled, true);
        ep_writer_host_enabled(&w->w_in);
        fw_host_enable(&w->fw, true);
        break;
    case FUNCTIONFS_DISABLE:
    case FUNCTIONFS_UNBIND:
        atomic_store(&w->enabled, false);
        fw_host_enable(&w->fw, false);
        pktq_flush(&w->q_in);
        break;
    default:
        break;
    }
}

static void *ep0_loop(void *arg) __attribute__((noreturn));
static void *ep0_loop(void *arg)
{
    struct wlan_fn *w = arg;
    struct usb_functionfs_event ev;
    for (;;) {
        int err = ffs_read_event(w->ep0, &ev);
        if (err) {
            LOGW(TAG, "ep0 read: %s", strerror(-err));
            sleep_ms(100);
            continue;
        }
        /* A dcmd is an OUT request followed by an IN request for the answer. Hold the global I/O
         * lock across both halves so no bulk-IN transfer is armed in between (see ffs_io_lock). */
        ffs_io_lock();
        handle_event(w, &ev);
        if (w->resp_len) {
            struct pollfd pfd = { .fd = w->ep0, .events = POLLIN };
            if (poll(&pfd, 1, 60) > 0 && ffs_read_event(w->ep0, &ev) == 0)
                handle_event(w, &ev);
        }
        ffs_io_unlock();
    }
}

/* Bulk OUT (PC -> Pi) with several read requests always queued (FunctionFS async I/O). With one synchronous read() at
 * a time the endpoint has no request pending between frames, the PC's transfers get NAKed and retried, and upload
 * is capped at about one frame per millisecond (~12 Mbit/s). */
#define OUT_DEPTH 8
#define OUT_BUF   2048

static long sys_io_setup(unsigned n, aio_context_t *c) { return syscall(SYS_io_setup, n, c); }
static long sys_io_submit(aio_context_t c, long n, struct iocb **p) { return syscall(SYS_io_submit, c, n, p); }
static long sys_io_getevents(aio_context_t c, long min, long n, struct io_event *e, struct timespec *t)
{
    return syscall(SYS_io_getevents, c, min, n, e, t);
}

static void *bulk_out_reader(void *arg) __attribute__((noreturn));
static void *bulk_out_reader(void *arg)
{
    struct wlan_fn *w = arg;
    static uint8_t bufs[OUT_DEPTH][OUT_BUF];
    struct iocb cbs[OUT_DEPTH];
    aio_context_t ctx = 0;
    if (sys_io_setup(OUT_DEPTH, &ctx) < 0)
        log_fatal(TAG, "io_setup: %s", strerror(errno));

    for (;;) {                                              /* (re)arm everything that is not in flight */
        int inflight = 0;
        for (int i = 0; i < OUT_DEPTH; i++) {
            memset(&cbs[i], 0, sizeof cbs[i]);
            cbs[i].aio_lio_opcode = IOCB_CMD_PREAD;
            cbs[i].aio_fildes = (uint32_t)w->ep_out;
            cbs[i].aio_buf = (uint64_t)(uintptr_t)bufs[i];
            cbs[i].aio_nbytes = OUT_BUF;
            cbs[i].aio_data = (uint64_t)i;
            struct iocb *p = &cbs[i];
            if (sys_io_submit(ctx, 1, &p) == 1)
                inflight++;
        }
        if (!inflight) {
            sleep_ms(50);
            continue;
        }
        while (inflight > 0) {
            struct io_event ev[OUT_DEPTH];
            long n = sys_io_getevents(ctx, 1, OUT_DEPTH, ev, NULL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                log_fatal(TAG, "io_getevents: %s", strerror(errno));
            }
            bool failed = false;
            for (long k = 0; k < n; k++) {
                inflight--;
                int i = (int)ev[k].data;
                long res = (long)ev[k].res;
                if (res > 0) {
                    fw_host_data(&w->fw, bufs[i], (size_t)res);
                } else if (res < 0) {
                    failed = true;
                    if (!ffs_errno_is_host_gone((int)-res))
                        LOGW(TAG, "bulk OUT read failed: %s", strerror((int)-res));
                }
                if (res >= 0 && !failed) {                  /* resubmit straight away: keep the pipe full */
                    struct iocb *p = &cbs[i];
                    if (sys_io_submit(ctx, 1, &p) == 1)
                        inflight++;
                }
            }
            if (failed)
                sleep_ms(20);                               /* host gone/reset: let the rest drain, then re-arm */
        }
    }
}

int wlan_fn_prepare(struct wlan_fn *w)
{
    int e = pktq_init(&w->q_in, 512);
    if (e)
        return e;
    fw_init(&w->fw, w->mac, w->be, to_host, w);

    struct ffs_fn_spec spec;
    wlan_fill_spec(&spec);
    w->ep0 = ffs_start(w->ffs_dir, &spec);
    if (w->ep0 < 0)
        return w->ep0;
    LOGI(TAG, "descriptors written to %s", w->ffs_dir);
    return 0;
}

int wlan_fn_run(struct wlan_fn *w)
{
    if ((w->ep_in = ffs_open_ep(w->ffs_dir, 1, O_WRONLY, 10000)) < 0 ||
        (w->ep_out = ffs_open_ep(w->ffs_dir, 2, O_RDONLY, 10000)) < 0) {
        LOGE(TAG, "endpoint files did not appear in %s", w->ffs_dir);
        return -ENOENT;
    }
    w->w_in = (struct ep_writer){ .fd = w->ep_in, .q = &w->q_in, .maxp = 0, .zlp = true, .name = "wlan-in" };
    if (w->keepalive_ms) {
        /* An event with a code the driver has no handler for (127): received and ignored. */
        static const uint8_t none[6] = { 0 };
        size_t n = bdc_build_event(w->keepalive_frame, sizeof w->keepalive_frame, w->mac, 127, 0, 0, 0, none, 0, NULL, 0);
        w->w_in.keepalive_ms = w->keepalive_ms;
        w->w_in.keepalive = w->keepalive_frame;
        w->w_in.keepalive_len = n;
        w->w_in.keepalive_active = &w->enabled;
        LOGI(TAG, "IN keep-alive every %u ms", w->keepalive_ms);
    }

    pthread_t t;
    int e;
    if ((e = ep_writer_start(&w->w_in)) ||
        (e = -pthread_create(&t, NULL, bulk_out_reader, w)) ||
        (e = -pthread_create(&t, NULL, ep0_loop, w)))
        return e;
    return 0;
}

/* Taking over the radio is slow (seconds); do it only after every USB function is answering requests, or the
 * host's drivers time out while probing (seen at boot). */
int wlan_fn_start_backend(struct wlan_fn *w)
{
    return w->be->ops->start(w->be);
}
