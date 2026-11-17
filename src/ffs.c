#include "ffs.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/usb/ch9.h>

#include "log.h"

/* Tiny append-only writer. Once it overflows it stays failed; callers check once at the end. */
struct wbuf {
    uint8_t *p;
    size_t   cap, len;
    int      overflow;
};

static void put(struct wbuf *b, const void *src, size_t n)
{
    if (b->overflow || b->len + n > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_le32(struct wbuf *b, uint32_t v)
{
    __le32 le = htole32(v);
    put(b, &le, sizeof le);
}

static void put_interface(struct wbuf *b, const struct ffs_fn_spec *s, uint8_t num, uint8_t alt, uint8_t n_eps)
{
    struct usb_interface_descriptor d = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = num,          /* function-relative; the gadget core renumbers */
        .bAlternateSetting  = alt,
        .bNumEndpoints      = n_eps,
        .bInterfaceClass    = s->cls,
        .bInterfaceSubClass = s->sub,
        .bInterfaceProtocol = s->proto,
        .iInterface         = 1,            /* index into the strings blob */
    };
    put(b, &d, sizeof d);
}

/* The _no_audio variant is the real 7-byte descriptor; plain usb_endpoint_descriptor
 * has two extra audio-only bytes and would be 9. */
static void put_endpoint(struct wbuf *b, uint8_t addr, uint8_t attrs, uint16_t maxp, uint8_t interval)
{
    struct usb_endpoint_descriptor_no_audio d = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = addr,
        .bmAttributes     = attrs,
        .wMaxPacketSize   = htole16(maxp),
        .bInterval        = interval,
    };
    put(b, &d, sizeof d);
}

size_t ffs_build_descriptors(const struct ffs_fn_spec *s, uint8_t *out, size_t cap)
{
    struct wbuf b = { .p = out, .cap = cap };
    if (s->n_eps > FFS_MAX_EPS)
        return 0;

    struct usb_functionfs_descs_head_v2 head = {
        .magic  = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = 0,                         /* patched below once the size is known */
        .flags  = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC |
                          (s->all_ctrl_recip ? FUNCTIONFS_ALL_CTRL_RECIP : 0)),
    };
    put(&b, &head, sizeof head);

    /* With HAS_FS_DESC and HAS_HS_DESC set, a descriptor *count* precedes each list. */
    if (s->n_extra > FFS_MAX_ALTS)
        return 0;
    unsigned total = s->no_primary ? 0u : 1u + s->n_eps;
    for (int a = 0; a < s->n_extra; a++)
        total += 1u + s->extra[a].n_eps;
    put_le32(&b, total);
    put_le32(&b, total);

    for (int speed = 0; speed < 2; speed++) {            /* full speed list, then high speed list */
        if (!s->no_primary) {
            put_interface(&b, s, 0, 0, s->n_eps);
            for (int i = 0; i < s->n_eps; i++)
                put_endpoint(&b, s->eps[i].addr, s->eps[i].attrs, speed ? s->eps[i].maxp_hs : s->eps[i].maxp_fs,
                             speed ? s->eps[i].interval_hs : s->eps[i].interval_fs);
        }
        for (int a = 0; a < s->n_extra; a++) {
            put_interface(&b, s, s->no_primary ? 0 : 1, s->extra[a].alt, s->extra[a].n_eps);
            for (int i = 0; i < s->extra[a].n_eps; i++) {
                const struct ffs_ep_spec *e = &s->extra[a].eps[i];
                put_endpoint(&b, e->addr, e->attrs, speed ? e->maxp_hs : e->maxp_fs, speed ? e->interval_hs : e->interval_fs);
            }
        }
    }

    if (b.overflow)
        return 0;
    __le32 len = htole32((uint32_t)b.len);
    memcpy(out + offsetof(struct usb_functionfs_descs_head_v2, length), &len, sizeof len);
    return b.len;
}

size_t ffs_build_strings(const char *name, uint8_t *out, size_t cap)
{
    struct wbuf b = { .p = out, .cap = cap };
    struct usb_functionfs_strings_head head = {
        .magic      = htole32(FUNCTIONFS_STRINGS_MAGIC),
        .length     = 0,
        .str_count  = htole32(1),
        .lang_count = htole32(1),
    };
    put(&b, &head, sizeof head);
    __le16 lang = htole16(0x0409);           /* en-US */
    put(&b, &lang, sizeof lang);
    put(&b, name, strlen(name) + 1);         /* includes the NUL */
    if (b.overflow)
        return 0;
    __le32 len = htole32((uint32_t)b.len);
    memcpy(out + offsetof(struct usb_functionfs_strings_head, length), &len, sizeof len);
    return b.len;
}

/* ------------------------------------------------------------------ device side */

static int write_all(int fd, const void *buf, size_t n)
{
    ssize_t w = write(fd, buf, n);
    if (w < 0)
        return -errno;
    return (size_t)w == n ? 0 : -EIO;
}

int ffs_start(const char *dir, const struct ffs_fn_spec *spec)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/ep0", dir);
    int ep0 = open(path, O_RDWR | O_CLOEXEC);
    if (ep0 < 0)
        return -errno;

    uint8_t buf[512];
    size_t n = ffs_build_descriptors(spec, buf, sizeof buf);
    int err = n ? write_all(ep0, buf, n) : -ENOSPC;
    if (!err) {
        n = ffs_build_strings(spec->name, buf, sizeof buf);
        err = n ? write_all(ep0, buf, n) : -ENOSPC;
    }
    if (err) {
        LOGE("ffs", "%s: writing descriptors failed: %s", dir, strerror(-err));
        close(ep0);
        return err;
    }
    return ep0;
}

int ffs_open_ep(const char *dir, int idx, int oflag, int timeout_ms)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/ep%d", dir, idx);
    for (int waited = 0;; waited += 50) {
        int fd = open(path, oflag | O_CLOEXEC);
        if (fd >= 0)
            return fd;
        if (errno != ENOENT || waited >= timeout_ms)
            return -errno;
        usleep(50 * 1000);
    }
}

int ffs_read_event(int ep0, struct usb_functionfs_event *ev)
{
    for (;;) {
        ssize_t n = read(ep0, ev, sizeof *ev);
        if (n == (ssize_t)sizeof *ev)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 ? -errno : -EIO;
    }
}

void ffs_stall(int ep0, bool request_is_in)
{
    char dummy = 0;
    ssize_t r;
    if (request_is_in)
        r = read(ep0, &dummy, 0);
    else
        r = write(ep0, &dummy, 0);
    (void)r;                                 /* the kernel answers with EL2HLT; that *is* the stall */
}

const char *ffs_event_name(unsigned type)
{
    static const char *const names[] = { "BIND", "UNBIND", "ENABLE", "DISABLE", "SETUP", "SUSPEND", "RESUME" };
    return type < sizeof names / sizeof *names ? names[type] : "?";
}

/* ------------------------------------------------------------------ IN writer */

bool ffs_errno_is_host_gone(int err)
{
    switch (err) {
    case ESHUTDOWN: case ENODEV: case ECONNRESET: case ECONNABORTED:
    case EPIPE: case ENOTCONN: case EL2HLT: case EBUSY: case EAGAIN:
        return true;
    default:
        return false;
    }
}

/* ---- stuck-write watchdog -------------------------------------------------------
 * On the Pi's dwc2 controller an IN request can occasionally sit queued without ever being
 * started: the host would take the data, but the hardware never gets armed, so write() blocks
 * forever and everything queued behind it stalls (seen as lost WiFi events and scan timeouts).
 * FunctionFS cancels a blocked write when the thread receives a signal (write returns EINTR,
 * the request is dequeued), and issuing the write again re-queues it from scratch. The
 * watchdog sends that signal to any writer whose current write has been pending for too long. */
#define WD_MAX_WRITERS 8
#define WD_KICK_MS     100
/* A write that survives this long (many re-queues and FIFO flushes) is wedged in the controller: reconnect the
 * gadget. A normal stall clears within a few hundred ms. */
#define WD_RECONNECT_MS 2000
/* Before the host has read anything from the endpoint (its driver may take seconds to probe): */
#define WD_RECONNECT_FIRST_MS 20000
#define WD_RECONNECT_COOLDOWN_MS 30000

static struct ep_writer *g_wd_writers[WD_MAX_WRITERS];
static int g_wd_n;
static pthread_mutex_t g_wd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_wd_once = PTHREAD_ONCE_INIT;

static void wd_noop(int sig) { (void)sig; }

static char g_udc_file[160], g_udc_name[64];

void ffs_set_udc(const char *udc_file, const char *udc_name)
{
    snprintf(g_udc_file, sizeof g_udc_file, "%s", udc_file ? udc_file : "");
    snprintf(g_udc_name, sizeof g_udc_name, "%s", udc_name ? udc_name : "");
}

static _Atomic unsigned g_bulk_maxp = 512;

unsigned ffs_bulk_maxp(void) { return atomic_load(&g_bulk_maxp); }

void ffs_refresh_speed(void)
{
    char path[160], buf[32] = "";
    if (!g_udc_name[0])
        return;
    snprintf(path, sizeof path, "/sys/class/udc/%s/current_speed", g_udc_name);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    if (!fgets(buf, sizeof buf, fp))
        buf[0] = '\0';
    fclose(fp);
    unsigned mp = strstr(buf, "full") || strstr(buf, "low") ? 64 : 512;
    atomic_store(&g_bulk_maxp, mp);
    LOGI("ffs", "USB link speed: %.*s (bulk packets of %u bytes)", (int)strcspn(buf, "\n"), buf, mp);
}

static void udc_write(const char *text)
{
    int fd = open(g_udc_file, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, text, strlen(text)) < 0)
        LOGW("ffs", "writing the UDC file failed: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
}

/* Soft-disconnect and reconnect: the PC sees the device go away and come back, re-probes it, and the
 * controller's endpoints start from a clean state. Last resort for a wedged IN endpoint. */
/* Records the controller's own view of its endpoints (dwc2 debugfs) so a wedge can be diagnosed afterwards. */
static void dump_controller_state(void)
{
    static const char *const files[] = { "state", "ep1in", "ep2in", "ep3in", "ep4in" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char path[160], buf[900];
        snprintf(path, sizeof path, "/sys/kernel/debug/usb/%s/%s", g_udc_name, files[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        for (char *p = buf; *p; p++)
            if (*p == '\n' || *p == '\t')
                *p = ' ';
        LOGW("ffs", "%s: %s", files[i], buf);
    }
}

static void usb_reconnect(void)
{
    LOGW("ffs", "an IN transfer has been stuck for over %d ms: reconnecting the USB gadget", WD_RECONNECT_MS);
    dump_controller_state();
    udc_write("\n");
    sleep_ms(1500);
    udc_write(g_udc_name);
}

static void *wd_main(void *arg)
{
    (void)arg;
    uint64_t last_reconnect = 0;
    for (;;) {
        sleep_ms(25);
        uint64_t now = now_ms();
        bool reconnect = false;
        pthread_mutex_lock(&g_wd_mu);
        for (int i = 0; i < g_wd_n; i++) {
            struct ep_writer *w = g_wd_writers[i];
            uint64_t since = atomic_load(&w->busy_since);
            if (since && now - since >= WD_KICK_MS) {
                w->kicks++;
                /* Re-arm the deadline so we kick again only after another interval. */
                atomic_store(&w->busy_since, now);
                pthread_kill(w->th, SIGUSR2);
            }
            if (w->drop_on_kick)
                continue;                 /* isochronous audio may sit unpolled for as long as the host likes: never a USB fault */
            uint64_t start = atomic_load(&w->pkt_start);
            unsigned long limit = atomic_load(&w->ok_since_enable) ? WD_RECONNECT_MS : WD_RECONNECT_FIRST_MS;
            if (start && now - start >= limit && g_udc_file[0] &&
                (!last_reconnect || now - last_reconnect >= WD_RECONNECT_COOLDOWN_MS))
                reconnect = true;
        }
        pthread_mutex_unlock(&g_wd_mu);
        if (reconnect) {
            last_reconnect = now_ms();
            usb_reconnect();
        }
    }
    return NULL;
}

static void wd_init(void)
{
    /* No SA_RESTART: the whole point is that write() returns EINTR. */
    struct sigaction sa = { .sa_handler = wd_noop };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, wd_main, NULL);
    pthread_detach(t);
}

static pthread_mutex_t g_io_mu = PTHREAD_MUTEX_INITIALIZER;
void ffs_io_lock(void)   { pthread_mutex_lock(&g_io_mu); }
void ffs_io_unlock(void) { pthread_mutex_unlock(&g_io_mu); }

static void *writer_main(void *arg)
{
    struct ep_writer *w = arg;
    uint64_t last_warn = 0;

    for (;;) {
        struct pkt *p = pktq_get(w->q, w->keepalive_ms ? (int)w->keepalive_ms : -1);
        if (!p) {
            if (pktq_is_closed(w->q))
                return NULL;                  /* queue closed */
            if (!w->keepalive_ms || !w->keepalive || (w->keepalive_active && !atomic_load(w->keepalive_active)))
                continue;
            p = malloc(sizeof *p + w->keepalive_len);
            if (!p)
                continue;
            p->len = w->keepalive_len;
            memcpy(p->data, w->keepalive, w->keepalive_len);
        }

        unsigned retries = 0;
        atomic_store(&w->pkt_start, now_ms());
        for (;;) {
            ffs_io_lock();
            atomic_store(&w->busy_since, now_ms());
            ssize_t n = write(w->fd, p->data, p->len);
            int err = n < 0 ? errno : EIO;
            atomic_store(&w->busy_since, 0);
            ffs_io_unlock();

            if (n == (ssize_t)p->len) {
                if (retries)
                    LOGI(w->name, "write of %zu bytes went through after %u re-queue(s) (%lu flushes so far)", p->len, retries, w->flushes);
                unsigned mp = w->maxp ? w->maxp : ffs_bulk_maxp();
                if (w->zlp && p->len && p->len % mp == 0) {
                    ffs_io_lock();
                    while (write(w->fd, p->data, 0) < 0 && errno == EINTR)
                        ;
                    ffs_io_unlock();
                }
                w->wrote++;
                atomic_fetch_add(&w->ok_since_enable, 1);
                break;
            }
            if (err == EINTR && w->drop_on_kick) {
                w->dropped++;                 /* isochronous: a late frame is worthless */
                break;
            }
            if (err == EINTR) {               /* watchdog kick (or a stray signal): queue it again */
                /* The request was dequeued. Before queueing it again, flush the endpoint's TX FIFO: a stuck
                 * start-after-idle in the controller is often cleared by this. */
                if (retries == 0 || retries % 10 == 9) {
                    if (ioctl(w->fd, FUNCTIONFS_FIFO_FLUSH, 0) == 0)
                        w->flushes++;
                }
                sleep_ms(2);                      /* let the control thread take the I/O lock */
                if (++retries == 1 || retries % 20 == 0)
                    LOGD(w->name, "write of %zu bytes stuck, re-queueing (attempt %u)", p->len, retries + 1);
                continue;
            }
            w->dropped++;
            /* A host that is gone is normal (unplug, reset, reboot): say it once in a while, quietly. */
            uint64_t now = now_ms();
            if (!ffs_errno_is_host_gone(err) || now - last_warn > 5000) {
                logmsg(ffs_errno_is_host_gone(err) ? LOG_DBG : LOG_WARN, w->name,
                       "write of %zu bytes failed: %s (packet dropped)", p->len,
                       n < 0 ? strerror(err) : "short write");
                last_warn = now;
            }
            if (!ffs_errno_is_host_gone(err))
                sleep_ms(20);
            break;
        }
        atomic_store(&w->pkt_start, 0);
        free(p);
    }
}

int ep_writer_start(struct ep_writer *w)
{
    atomic_init(&w->busy_since, 0);
    atomic_init(&w->pkt_start, 0);
    atomic_init(&w->ok_since_enable, 0);
    int e = pthread_create(&w->th, NULL, writer_main, w);
    if (e)
        return -e;
    pthread_once(&g_wd_once, wd_init);
    pthread_mutex_lock(&g_wd_mu);
    if (g_wd_n < WD_MAX_WRITERS)
        g_wd_writers[g_wd_n++] = w;
    pthread_mutex_unlock(&g_wd_mu);
    return 0;
}

void ep_writer_host_enabled(struct ep_writer *w)
{
    atomic_store(&w->ok_since_enable, 0);
}
