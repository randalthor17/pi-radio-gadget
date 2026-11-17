#include "hci.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <linux/aio_abi.h>
#include <unistd.h>
#include <linux/usb/ch9.h>

#include "log.h"
#include "update.h"

#define TAG "hci"

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI       1
#define HCI_CHANNEL_USER  1
#define HCIDEVDOWN        _IOW('H', 202, int)

struct sockaddr_hci_ {                /* defined here so libbluetooth-dev is not needed */
    sa_family_t    hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

#define EVT_MAX 258                   /* 2 + 255, with slack */
#define ACL_MAX_ON_WIRE (ACL_HDR_LEN + 65535)

void hci_fill_spec(struct ffs_fn_spec *s, unsigned int_maxp)
{
    memset(s, 0, sizeof *s);
    s->cls = 0xE0; s->sub = 0x01; s->proto = 0x01;           /* Bluetooth, USB transport */
    s->n_eps = 3;
    s->name = "Pi BT HCI";
    s->all_ctrl_recip = true;         /* HCI commands are class requests aimed at the *device* */
    /* FunctionFS numbers endpoint files ep1..epN in descriptor order. Full speed: bulk max 64;
     * interrupt bInterval is in frames (1 ms). High speed: bulk 512; bInterval 4 = 2^3 uframes = 1 ms. */
    s->eps[0] = (struct ffs_ep_spec){ HCI_EP_EVT_IN,  USB_ENDPOINT_XFER_INT,  int_maxp, int_maxp, 1, 4 };
    s->eps[1] = (struct ffs_ep_spec){ HCI_EP_ACL_IN,  USB_ENDPOINT_XFER_BULK, 64, 512, 0, 0 };
    s->eps[2] = (struct ffs_ep_spec){ HCI_EP_ACL_OUT, USB_ENDPOINT_XFER_BULK, 64, 512, 0, 0 };
}

/* The audio interface, in a function of its own (ffs.sco, linked after ffs.hci so the PC sees it as the interface right after
 * the Bluetooth one, as btusb expects). Alternate setting 0 has no endpoints (no audio); 1 and 2 carry isochronous audio,
 * 1 ms per frame. FunctionFS re-enables all endpoints of a function whenever the host selects an alternate setting, so keeping
 * this apart protects the Bluetooth control/ACL endpoints from every audio start and stop. */
void hci_fill_sco_spec(struct ffs_fn_spec *s)
{
    memset(s, 0, sizeof *s);
    s->cls = 0xE0; s->sub = 0x01; s->proto = 0x01;
    s->name = "Pi BT audio";
    s->no_primary = true;
    s->n_extra = 3;
    s->extra[0].alt = 0;
    s->extra[1].alt = 1;
    s->extra[1].n_eps = 2;
    s->extra[1].eps[0] = (struct ffs_ep_spec){ HCI_EP_SCO1_OUT, USB_ENDPOINT_XFER_ISOC, SCO_ALT1_MAXP, SCO_ALT1_MAXP, 1, 4 };
    s->extra[1].eps[1] = (struct ffs_ep_spec){ HCI_EP_SCO1_IN,  USB_ENDPOINT_XFER_ISOC, SCO_ALT1_MAXP, SCO_ALT1_MAXP, 1, 4 };
    s->extra[2].alt = 2;
    s->extra[2].n_eps = 2;
    s->extra[2].eps[0] = (struct ffs_ep_spec){ HCI_EP_SCO2_OUT, USB_ENDPOINT_XFER_ISOC, SCO_ALT2_MAXP, SCO_ALT2_MAXP, 1, 4 };
    s->extra[2].eps[1] = (struct ffs_ep_spec){ HCI_EP_SCO2_IN,  USB_ENDPOINT_XFER_ISOC, SCO_ALT2_MAXP, SCO_ALT2_MAXP, 1, 4 };
}

/* ------------------------------------------------------------------ chip side */

static void rfkill_unblock_bluetooth(void)
{
    struct { uint32_t idx; uint8_t type, op, soft, hard; } __attribute__((packed)) ev = {
        .idx = 0, .type = 2 /* RFKILL_TYPE_BLUETOOTH */, .op = 3 /* CHANGE_ALL */, .soft = 0, .hard = 0,
    };
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (write(fd, &ev, sizeof ev) < 0)
        LOGD(TAG, "rfkill unblock: %s", strerror(errno));
    close(fd);
}

int hci_open_user_channel(int dev)
{
    rfkill_unblock_bluetooth();

    int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (s < 0)
        return -errno;

    /* User channel needs the controller DOWN. Harmless if it already is. */
    if (ioctl(s, HCIDEVDOWN, dev) < 0 && errno != EALREADY && errno != ENODEV)
        LOGD(TAG, "HCIDEVDOWN hci%d: %s", dev, strerror(errno));

    struct sockaddr_hci_ a = { AF_BLUETOOTH, (unsigned short)dev, HCI_CHANNEL_USER };
    for (int attempt = 0;; attempt++) {
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0)
            return s;
        int e = errno;
        /* EBUSY: still going down / being released by bluetoothd. Give it a moment. */
        if (e == EBUSY && attempt < 20) {
            sleep_ms(250);
            continue;
        }
        LOGE(TAG, "bind(hci%d, USER) failed: %s (hci%d must be down, bluetoothd stopped, "
             "bluetooth rfkill unblocked, and we need CAP_NET_ADMIN)", dev, strerror(e), dev);
        close(s);
        return -e;
    }
}

/* Every send() is exactly one whole packet, so two threads cannot interleave inside one.
 * The mutex keeps that guarantee explicit (and keeps the debug log in order). */
static void hci_send(struct hci_fn *h, uint8_t type, const uint8_t *pkt, size_t len)
{
    uint8_t buf[ACL_MAX_ON_WIRE + 1];
    if (len > sizeof buf - 1) {
        LOGW(TAG, "refusing to send %zu-byte packet", len);
        return;
    }
    buf[0] = type;
    memcpy(buf + 1, pkt, len);
    if (log_level() >= LOG_TRACE) {
        char hx[64];
        hex_prefix(pkt, len, 24, hx, sizeof hx);
        LOGT(TAG, "PC->chip type=%u len=%zu %s", type, len, hx);
    }
    pthread_mutex_lock(&h->send_mu);
    /* Never block while the global I/O lock may be held: a chip that stops taking packets must not freeze the
     * USB control path (and with it the WiFi function). Wait briefly for room, then drop. */
    ssize_t n = send(h->sock, buf, len + 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    for (int tries = 0; n < 0 && (errno == EAGAIN || errno == ENOBUFS) && tries < 20; tries++) {
        sleep_ms(5);
        n = send(h->sock, buf, len + 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    pthread_mutex_unlock(&h->send_mu);
    if (n < 0) {
        if (errno == ENETDOWN || errno == ENODEV || errno == EBADFD)
            log_fatal(TAG, "controller went away (%s)", strerror(errno));
        LOGW(TAG, "send type %u failed: %s", type, strerror(errno));
    } else if ((size_t)n != len + 1) {
        LOGW(TAG, "short send: %zd of %zu", n, len + 1);
    } else {
        h->to_chip++;
    }
}

size_t hci_stats_line(void *arg, char *buf, size_t size)
{
    struct hci_fn *h = arg;
    int n = snprintf(buf, size, "sco[alt=%d chip_pkts=%lu to_chip=%lu a1(q_drop=%lu data=%lu lost=%lu idle=%lu err=%lu/%d) a2(q_drop=%lu data=%lu lost=%lu idle=%lu err=%lu/%d)] rd[a1=%lu/%lu/%d a2=%lu/%lu/%d] acl[to_chip=%lu from_chip=%lu]",
                     atomic_load(&h->sco_alt), h->sco_to_pc, h->sco_to_chip, h->q_sco[0].dropped, h->w_sco[0].wrote, h->w_sco[0].dropped, h->w_sco[0].kicks, h->sco_err[0], h->sco_err_last[0],
                     h->q_sco[1].dropped, h->w_sco[1].wrote, h->w_sco[1].dropped, h->w_sco[1].kicks, h->sco_err[1], h->sco_err_last[1],
                     h->sco_rd[0], h->sco_rd_err[0], h->sco_rd_errno[0], h->sco_rd[1], h->sco_rd_err[1], h->sco_rd_errno[1], h->to_chip, h->from_chip);
    return n < 0 ? 0 : ((size_t)n >= size ? size - 1 : (size_t)n);
}

static void sco_to_queue(const uint8_t *frame, size_t len, void *ctx)
{
    pktq_put(ctx, frame, len);
}

static void *chip_reader(void *arg) __attribute__((noreturn));
static void *chip_reader(void *arg)
{
    struct hci_fn *h = arg;
    uint8_t *buf = malloc(ACL_MAX_ON_WIRE + 8);
    if (!buf)
        log_fatal(TAG, "out of memory");

    for (;;) {
        ssize_t n = recv(h->sock, buf, ACL_MAX_ON_WIRE + 8, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            log_fatal(TAG, "HCI socket closed (%s)", n < 0 ? strerror(errno) : "EOF");

        uint8_t type = buf[0];
        const uint8_t *body = buf + 1;
        size_t len = (size_t)n - 1;
        h->from_chip++;

        if (type == H4_EVT) {
            uint8_t st;
            if (atomic_load(&h->swallow_vendor) && hci_evt_is_cc(body, len, BCM_OP_SCO_PCM_PARAM, &st)) {
                atomic_store(&h->swallow_vendor, false);   /* the answer to our own command: not for the host */
                LOGD(TAG, "SCO routed over HCI (status %u)", st);
                continue;
            }
            if (h->sco_over_hci && hci_evt_is_cc(body, len, HCI_OP_RESET, &st) && st == 0) {
                /* The chip forgets its SCO routing on every reset and defaults to its PCM pins. */
                uint8_t vc[8];
                size_t vn = bcm_sco_over_hci_cmd(vc, sizeof vc);
                atomic_store(&h->swallow_vendor, true);
                if (atomic_load(&h->enabled))
                    pktq_put(&h->q_evt, body, len);        /* the reset answer first */
                hci_send(h, H4_CMD, vc, vn);
                continue;
            }
        }

        bool en = atomic_load(&h->enabled);
        if (log_level() >= LOG_TRACE) {
            char hx[64];
            hex_prefix(body, len, 24, hx, sizeof hx);
            LOGT(TAG, "chip->PC type=%u len=%zu %s%s", type, len, hx, en ? "" : "  [dropped: host not enabled]");
        }
        if (!en)
            continue;                  /* stale events from before a reconnect confuse the host stack */

        switch (type) {
        case H4_EVT: pktq_put(&h->q_evt, body, len); break;
        case H4_ACL: pktq_put(&h->q_acl, body, len); break;
        case H4_SCO: {
            int a = atomic_load(&h->sco_alt) - 1;
            struct sco_ctx { struct pktq *q; } c = { &h->q_sco[a] };
            sco_chunk(body, len, a ? SCO_ALT2_MAXP : SCO_ALT1_MAXP, sco_to_queue, c.q);
            h->sco_to_pc++;
            break;
        }
        default:     h->sco_dropped++; break;
        }
    }
}

/* ------------------------------------------------------------------ USB side */

static void acl_to_chip(const uint8_t *pkt, size_t len, void *ctx)
{
    hci_send(ctx, H4_ACL, pkt, len);
}

static void *acl_out_reader(void *arg) __attribute__((noreturn));
static void *acl_out_reader(void *arg)
{
    struct hci_fn *h = arg;
    uint8_t buf[16384];
    unsigned seen_epoch = atomic_load(&h->epoch);

    for (;;) {
        ssize_t n = read(h->ep_acl_out, buf, sizeof buf);
        unsigned e = atomic_load(&h->epoch);
        if (e != seen_epoch) {                    /* reconnect: partial data belongs to the old session */
            acl_split_reset(&h->split);
            seen_epoch = e;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!ffs_errno_is_host_gone(errno))
                LOGW(TAG, "ACL OUT read failed: %s", strerror(errno));
            sleep_ms(50);
            continue;
        }
        if (n == 0)
            continue;                              /* zero-length packet */
        if (!atomic_load(&h->enabled))
            continue;
        if (acl_split_feed(&h->split, buf, (size_t)n, acl_to_chip, h) < 0)
            LOGW(TAG, "bogus ACL length from host; dropped buffered data (resync #%lu)", h->split.resyncs);
    }
}

struct sco_out { struct hci_fn *h; int idx; };

static void sco_pkt_to_chip(const uint8_t *pkt, size_t len, void *ctx)
{
    struct hci_fn *h = ctx;
    hci_send(h, H4_SCO, pkt, len);
    h->sco_to_chip++;
}

/* Audio from the PC (isochronous OUT): frames of at most one endpoint packet; re-assemble whole SCO packets. */
static void *sco_out_reader(void *arg)
{
    struct sco_out *so = arg;
    struct hci_fn *h = so->h;
    uint8_t buf[64];
    unsigned seen = atomic_load(&h->epoch);
    for (;;) {
        /* dwc2 rejects (EINVAL) an isochronous OUT request longer than wMaxPacketSize */
        ssize_t n = read(h->ep_sco_out[so->idx], buf, SCO_ALT1_MAXP);
        unsigned e = atomic_load(&h->epoch);
        if (e != seen) {
            sco_reasm_reset(&h->reasm[so->idx]);
            seen = e;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            h->sco_rd_err[so->idx]++;
            if (errno != h->sco_rd_errno[so->idx])
                LOGW(TAG, "audio OUT endpoint %d read failed: %s", so->idx + 1, strerror(errno));
            h->sco_rd_errno[so->idx] = errno;
            /* ENODATA = an isochronous frame was missed; retry at once or we lose the following frames too */
            if (errno != ENODATA)
                sleep_ms(50);
            continue;
        }
        h->sco_rd[so->idx]++;
        if (n == 0 || !atomic_load(&h->enabled))
            continue;
        sco_reasm_feed(&h->reasm[so->idx], buf, (size_t)n, sco_pkt_to_chip, h);
    }
    return NULL;
}

/* Vendor requests to the device that update the daemon itself (see update.h). */
static void upd_ep0(struct hci_fn *h, const struct usb_ctrlrequest *r)
{
    bool in = r->bRequestType & USB_DIR_IN;
    uint16_t len = le16toh(r->wLength);
    uint8_t buf[UPD_CHUNK];

    if (in && r->bRequest == UPD_REQ_STATUS && len) {
        char st[1024];
        size_t n = upd_status(h->upd, st, sizeof st);
        if (write(h->ep0, st, n < len ? n : len) < 0)
            LOGW(TAG, "update status reply failed: %s", strerror(errno));
        return;
    }
    if (!in && len && len <= sizeof buf && r->bRequest >= UPD_REQ_BEGIN && r->bRequest <= UPD_REQ_STOP) {
        ssize_t n = read(h->ep0, buf, len);          /* the data stage; reading it also completes the request */
        if (n <= 0) {
            LOGW(TAG, "update data stage failed: %s", n < 0 ? strerror(errno) : "empty");
            return;
        }
        if (r->bRequest == UPD_REQ_BEGIN)
            upd_begin(h->upd, buf, (size_t)n);
        else if (r->bRequest == UPD_REQ_DATA)
            upd_data(h->upd, (uint32_t)le16toh(r->wValue) | (uint32_t)le16toh(r->wIndex) << 16, buf, (size_t)n);
        else if (n == 4 && !memcmp(buf, "COMT", 4) && r->bRequest == UPD_REQ_COMMIT)
            upd_commit(h->upd);
        else if (n == 4 && !memcmp(buf, "STOP", 4) && r->bRequest == UPD_REQ_STOP)
            upd_stop(h->upd);
        return;
    }
    ffs_stall(h->ep0, in);
}

static void ep0_setup(struct hci_fn *h, const struct usb_ctrlrequest *r)
{
    if (h->upd && (r->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR && (r->bRequestType & 0x1f) == USB_RECIP_DEVICE &&
        r->bRequest >= UPD_REQ_BEGIN && r->bRequest <= UPD_REQ_STOP) {
        upd_ep0(h, r);
        return;
    }

    bool in = r->bRequestType & USB_DIR_IN;
    uint16_t len = le16toh(r->wLength);

    if ((r->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS && !in && r->bRequest == 0 && len) {
        uint8_t cmd[512];
        if (len > 3 + 255) {                       /* bigger than any HCI command: refuse */
            LOGW(TAG, "oversized HCI command request (%u bytes)", len);
            ffs_stall(h->ep0, false);
            return;
        }
        ssize_t n = read(h->ep0, cmd, len);         /* the data stage of this OUT request */
        if (n > 0) {
            uint16_t vs;
            if (hci_cmd_voice_setting(cmd, (size_t)n, &vs)) {
                atomic_store(&h->sco_alt, sco_alt_for_voice(vs));
                LOGI(TAG, "voice setting 0x%04x: SCO audio on alternate setting %d", vs, sco_alt_for_voice(vs));
            }
            int salt;
            if (hci_cmd_sync_conn_alt(cmd, (size_t)n, atomic_load(&h->sco_alt), &salt)) {
                atomic_store(&h->sco_alt, salt);
                LOGI(TAG, "synchronous connection: SCO audio on alternate setting %d", salt);
            }
            hci_send(h, H4_CMD, cmd, (size_t)n);
        }
        else
            LOGW(TAG, "HCI command data stage failed: %s", n < 0 ? strerror(errno) : "empty");
        return;
    }
    LOGW(TAG, "unhandled setup type=%02x req=%02x val=%04x idx=%04x len=%u",
         r->bRequestType, r->bRequest, le16toh(r->wValue), le16toh(r->wIndex), len);
    ffs_stall(h->ep0, in);
}

static void *ep0_loop(void *arg) __attribute__((noreturn));
static void *ep0_loop(void *arg)
{
    struct hci_fn *h = arg;
    struct usb_functionfs_event ev;

    for (;;) {
        int err = ffs_read_event(h->ep0, &ev);
        if (err) {
            LOGW(TAG, "ep0 read: %s", strerror(-err));
            sleep_ms(100);
            continue;
        }
        if (ev.type == FUNCTIONFS_SETUP) {
            ffs_io_lock();
            ep0_setup(h, &ev.u.setup);
            ffs_io_unlock();
            continue;
        }
        LOGI(TAG, "usb event %s", ffs_event_name(ev.type));
        switch (ev.type) {
        case FUNCTIONFS_ENABLE:
            ffs_refresh_speed();
            ep_writer_host_enabled(&h->w_evt);
            ep_writer_host_enabled(&h->w_acl);
            pktq_flush(&h->q_evt);
            pktq_flush(&h->q_acl);
            pktq_flush(&h->q_sco[0]);
            pktq_flush(&h->q_sco[1]);
            atomic_fetch_add(&h->epoch, 1);
            atomic_store(&h->enabled, true);
            break;
        case FUNCTIONFS_DISABLE:
        case FUNCTIONFS_UNBIND:
            atomic_store(&h->enabled, false);
            pktq_flush(&h->q_evt);
            pktq_flush(&h->q_acl);
            pktq_flush(&h->q_sco[0]);
            pktq_flush(&h->q_sco[1]);
            atomic_fetch_add(&h->epoch, 1);
            break;
        default:
            break;
        }
    }
}

/* ep0 of the audio function: it has no control requests of its own; the events tell when the host switches the audio
 * interface (any alternate setting), at which point stale audio is dropped. */
static void *sco_ep0_loop(void *arg) __attribute__((noreturn));
static void *sco_ep0_loop(void *arg)
{
    struct hci_fn *h = arg;
    struct usb_functionfs_event ev;
    for (;;) {
        int err = ffs_read_event(h->ep0_sco, &ev);
        if (err) {
            LOGW(TAG, "audio ep0 read: %s", strerror(-err));
            sleep_ms(100);
            continue;
        }
        if (ev.type == FUNCTIONFS_SETUP) {
            ffs_stall(h->ep0_sco, ev.u.setup.bRequestType & USB_DIR_IN);
            continue;
        }
        LOGI(TAG, "audio usb event %s", ffs_event_name(ev.type));
        if (ev.type == FUNCTIONFS_ENABLE || ev.type == FUNCTIONFS_DISABLE || ev.type == FUNCTIONFS_UNBIND) {
            pktq_flush(&h->q_sco[0]);
            pktq_flush(&h->q_sco[1]);
            atomic_fetch_add(&h->epoch, 1);
        }
    }
}

/* ------------------------------------------------------------------ lifecycle */

int hci_fn_prepare(struct hci_fn *h)
{
    pthread_mutex_init(&h->send_mu, NULL);
    atomic_init(&h->enabled, false);
    atomic_init(&h->epoch, 0);
    atomic_init(&h->sco_alt, 2);                    /* the Linux default voice setting is 16-bit: alternate setting 2 */
    atomic_init(&h->swallow_vendor, false);

    h->sock = hci_open_user_channel(h->hci_dev);
    if (h->sock < 0)
        return h->sock;
    LOGI(TAG, "bound hci%d user channel", h->hci_dev);

    struct ffs_fn_spec spec;
    hci_fill_spec(&spec, h->int_maxp);
    h->ep0 = ffs_start(h->ffs_dir, &spec);
    if (h->ep0 < 0)
        return h->ep0;
    LOGI(TAG, "descriptors written to %s", h->ffs_dir);
    struct ffs_fn_spec sspec;
    hci_fill_sco_spec(&sspec);
    h->ep0_sco = ffs_start(h->ffs_sco_dir, &sspec);
    if (h->ep0_sco < 0)
        return h->ep0_sco;
    LOGI(TAG, "descriptors written to %s", h->ffs_sco_dir);
    return 0;
}

/*
 * Isochronous audio towards the host, paced.
 *
 * The Pi's USB controller (dwc2) gives every queued isochronous IN request its own USB frame, one after the other, and drops
 * a request with ENODATA when it arrives after its frame has gone. So requests must go in at the frame rate (1 per ms)
 * whether or not there is audio: when the queue is empty an empty (zero-length) request keeps the rhythm. A single blocking
 * write per audio packet, as the bulk writers use, falls out of step and loses about half the packets. Requests are kept
 * SCO_LEAD frames ahead, through AIO so several can be outstanding.
 */
#define SCO_LEAD   3
#define SCO_SLOTS  8
#define SCO_FRAME_MAX 64

struct sco_pace { struct hci_fn *h; int i; };

static void *sco_pace_writer(void *arg) __attribute__((noreturn));
static void *sco_pace_writer(void *arg)
{
    struct sco_pace *sp = arg;
    struct hci_fn *h = sp->h;
    int i = sp->i;
    aio_context_t ctx = 0;
    if (syscall(SYS_io_setup, SCO_SLOTS, &ctx) < 0)
        log_fatal(TAG, "io_setup for audio: %s", strerror(errno));
    struct iocb cbs[SCO_SLOTS];
    uint8_t bufs[SCO_SLOTS][SCO_FRAME_MAX];
    bool busy[SCO_SLOTS] = { false }, has_data[SCO_SLOTS] = { false };
    int inflight = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 1000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        struct io_event ev[SCO_SLOTS];
        struct timespec zero = { 0, 0 };
        long n = syscall(SYS_io_getevents, ctx, 0, SCO_SLOTS, ev, &zero);
        for (long k = 0; k < n; k++) {
            int s = (int)ev[k].data;
            busy[s] = false;
            inflight--;
            if (ev[k].res < 0) {
                h->sco_err[i]++;
                h->sco_err_last[i] = (int)-ev[k].res;
                if (has_data[s])
                    h->w_sco[i].dropped++;
            } else if (has_data[s])
                h->w_sco[i].wrote++;
        }
        if (!atomic_load(&h->enabled))
            continue;
        while (inflight < SCO_LEAD) {
            int s = 0;
            while (s < SCO_SLOTS && busy[s])
                s++;
            if (s == SCO_SLOTS)
                break;
            struct pkt *p = pktq_get(&h->q_sco[i], 0);
            size_t len = 0;
            if (p) {
                len = p->len <= SCO_FRAME_MAX ? p->len : SCO_FRAME_MAX;
                memcpy(bufs[s], p->data, len);
                free(p);
            }
            has_data[s] = len > 0;
            if (!len)
                h->w_sco[i].kicks++;                /* counted as "idle" in the status line */
            cbs[s] = (struct iocb){ .aio_lio_opcode = IOCB_CMD_PWRITE, .aio_fildes = (uint32_t)h->ep_sco_in[i],
                                    .aio_buf = (uint64_t)(uintptr_t)bufs[s], .aio_nbytes = len, .aio_data = (uint64_t)s };
            struct iocb *pc = &cbs[s];
            if (syscall(SYS_io_submit, ctx, 1, &pc) != 1) {
                h->sco_err[i]++;
                h->sco_err_last[i] = errno;
                if (len)
                    h->w_sco[i].dropped++;
                break;
            }
            busy[s] = true;
            inflight++;
        }
    }
}

int hci_fn_run(struct hci_fn *h)
{
    int e;
    if ((e = pktq_init(&h->q_evt, 256)) || (e = pktq_init(&h->q_acl, 512)))
        return e;

    /* Endpoint files appear in descriptor order; the three of ours are ep1..ep3. */
    if ((h->ep_evt     = ffs_open_ep(h->ffs_dir, 1, O_WRONLY, 10000)) < 0 ||
        (h->ep_acl_in  = ffs_open_ep(h->ffs_dir, 2, O_WRONLY, 10000)) < 0 ||
        (h->ep_acl_out = ffs_open_ep(h->ffs_dir, 3, O_RDONLY, 10000)) < 0) {
        LOGE(TAG, "endpoint files did not appear in %s", h->ffs_dir);
        return -ENOENT;
    }

    static struct sco_out so[2];
    for (int i = 0; i < 2; i++) {
        h->ep_sco_out[i] = ffs_open_ep(h->ffs_sco_dir, 1 + 2 * i, O_RDONLY, 10000);
        h->ep_sco_in[i]  = ffs_open_ep(h->ffs_sco_dir, 2 + 2 * i, O_WRONLY, 10000);
        if (h->ep_sco_out[i] < 0 || h->ep_sco_in[i] < 0) {
            LOGE(TAG, "SCO endpoint files did not appear in %s", h->ffs_sco_dir);
            return -ENOENT;
        }
        if ((e = pktq_init(&h->q_sco[i], 256)))
            return e;
        h->w_sco[i] = (struct ep_writer){ .fd = h->ep_sco_in[i], .q = &h->q_sco[i], .maxp = 1, .zlp = false,
                                          .name = i ? "hci-sco2" : "hci-sco1", .drop_on_kick = true };
        so[i] = (struct sco_out){ h, i };
    }

    h->w_evt = (struct ep_writer){ .fd = h->ep_evt, .q = &h->q_evt, .maxp = h->int_maxp, .zlp = h->zlp, .name = "hci-evt" };
    h->w_acl = (struct ep_writer){ .fd = h->ep_acl_in, .q = &h->q_acl, .maxp = 0, .zlp = h->zlp, .name = "hci-acl" };

    pthread_t t;
    pthread_t ts;
    static struct sco_pace sp[2];
    sp[0] = (struct sco_pace){ h, 0 };
    sp[1] = (struct sco_pace){ h, 1 };
    pthread_t tp;
    if ((e = -pthread_create(&tp, NULL, sco_pace_writer, &sp[0])) || (e = -pthread_create(&tp, NULL, sco_pace_writer, &sp[1])) ||
        (e = -pthread_create(&ts, NULL, sco_out_reader, &so[0])) || (e = -pthread_create(&ts, NULL, sco_out_reader, &so[1])))
        return e;
    if ((e = ep_writer_start(&h->w_evt)) || (e = ep_writer_start(&h->w_acl)) ||
        (e = -pthread_create(&t, NULL, chip_reader, h)) ||
        (e = -pthread_create(&t, NULL, acl_out_reader, h)) ||
        (e = -pthread_create(&t, NULL, ep0_loop, h)) ||
        (e = -pthread_create(&t, NULL, sco_ep0_loop, h)))
        return e;
    return 0;
}
