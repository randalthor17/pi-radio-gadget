/* Synthetic radio: a couple of made-up networks, joins that succeed or fail on demand.
 * No hardware is touched. Used to bring up the USB/driver side, and by the tests. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "fw.h"
#include "log.h"

#define TAG "fake"

struct fake {
    struct wl_backend be;
    pthread_t th;
    struct wl_scan_req scan;
    struct wl_join_req join;
    int job;                          /* 1 scan, 2 join */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct wl_link link;
    volatile bool abort_scan;
    pthread_t flood_th;
    bool flood_started;
};

static uint8_t *ie_put(uint8_t *p, uint8_t id, const void *d, uint8_t n)
{
    *p++ = id; *p++ = n;
    memcpy(p, d, n);
    return p + n;
}

static void make_bss(struct wl_bss *b, const char *ssid, uint8_t mac_last, uint8_t ch, int rssi, int kind)
{
    memset(b, 0, sizeof *b);
    const uint8_t mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0x00, mac_last };
    memcpy(b->bssid, mac, 6);
    b->ssid_len = (uint8_t)strlen(ssid);
    memcpy(b->ssid, ssid, b->ssid_len);
    b->beacon_int = 100;
    b->capability = kind ? 0x0431 : 0x0421;          /* ESS (+privacy) + short preamble/slot */
    b->channel = ch;
    b->rssi = (int8_t)rssi;

    uint8_t *p = b->ie;
    p = ie_put(p, 0, ssid, b->ssid_len);
    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    p = ie_put(p, 1, rates, sizeof rates);
    p = ie_put(p, 3, &ch, 1);
    if (kind == 1) {                                   /* RSN: CCMP group+pairwise, PSK */
        static const uint8_t rsn[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                       1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
        p = ie_put(p, 48, rsn, sizeof rsn);
    } else if (kind == 2) {                            /* RSN: CCMP, AKM 8 (SAE), PMF required */
        static const uint8_t rsn[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                       1, 0, 0x00, 0x0f, 0xac, 8, 0xc0, 0 };
        p = ie_put(p, 48, rsn, sizeof rsn);
    }
    b->ie_len = (uint16_t)(p - b->ie);
}

#define FAKE_EXTRA 12
static const struct { const char *ssid; uint8_t last, ch; int rssi; int kind; } nets[] = {
    { "PiGadget-Open",  1, 1, -45, 0 },
    { "PiGadget-WPA2",  2, 6, -52, 1 },
    { "PiGadget-Far",   3, 11, -81, 1 },
    { "PiGadget-WPA3",  4, 9, -55, 2 },
};

static void *worker(void *arg) __attribute__((noreturn));
static void *worker(void *arg)
{
    struct fake *f = arg;
    for (;;) {
        pthread_mutex_lock(&f->mu);
        while (!f->job)
            pthread_cond_wait(&f->cv, &f->mu);
        int job = f->job;
        f->job = 0;
        struct wl_join_req j = f->join;
        pthread_mutex_unlock(&f->mu);

        if (job == 1) {
            f->abort_scan = false;
            const char *burst_env = getenv("PI_RADIO_GADGET_FAKE_BURST");
            const bool burst = burst_env && *burst_env;
            for (size_t i = 0; i < sizeof nets / sizeof *nets + (burst ? FAKE_EXTRA : 0) && !f->abort_scan; i++) {
                if (!burst)
                    sleep_ms(150);
                struct wl_bss b;
                if (i < sizeof nets / sizeof *nets) {
                    make_bss(&b, nets[i].ssid, nets[i].last, nets[i].ch, nets[i].rssi, nets[i].kind);
                } else {
                    char name[24];
                    snprintf(name, sizeof name, "Burst-%02zu", i);
                    make_bss(&b, name, (uint8_t)(0x10 + i), (uint8_t)(1 + i % 13), -60 - (int)i, (int)(i % 2));
                }
                fw_scan_result(f->be.fw, &b);
            }
            fw_scan_done(f->be.fw, f->abort_scan);
        } else if (job == 2) {
            sleep_ms(300);
            size_t i;
            for (i = 0; i < sizeof nets / sizeof *nets; i++)
                if (strlen(nets[i].ssid) == j.ssid_len && !memcmp(nets[i].ssid, j.ssid, j.ssid_len))
                    break;
            if (i == sizeof nets / sizeof *nets) {
                fw_join_done(f->be.fw, WL_JOIN_NO_NETWORK, NULL);
            } else if (nets[i].kind == 1 && !j.have_pmk) {
                LOGW(TAG, "WPA2 network but the host provided no PMK");
                fw_join_done(f->be.fw, WL_JOIN_AUTH_FAILED, NULL);
            } else if (nets[i].kind == 2 && !j.have_sae) {
                LOGW(TAG, "WPA3 network but the host provided no SAE password");
                fw_join_done(f->be.fw, WL_JOIN_AUTH_FAILED, NULL);
            } else {
                struct wl_bss b;
                make_bss(&b, nets[i].ssid, nets[i].last, nets[i].ch, nets[i].rssi, nets[i].kind);
                pthread_mutex_lock(&f->mu);
                f->link = (struct wl_link){ .up = true, .channel = b.channel, .rssi = b.rssi,
                                            .rate_kbps = 72200, .ssid_len = b.ssid_len };
                memcpy(f->link.bssid, b.bssid, 6);
                memcpy(f->link.ssid, b.ssid, b.ssid_len);
                pthread_mutex_unlock(&f->mu);
                fw_join_done(f->be.fw, WL_JOIN_OK, &b);
            }
        }
    }
}

/* PI_RADIO_GADGET_FAKE_FLOOD_PPS=N: after a join, stream N frames/s of 1000 bytes (an experimental ethertype the PC
 * ignores) towards the host. A stand-in for a download, to stress the USB receive path without a real AP. */
static void *flood_main(void *arg)
{
    struct fake *f = arg;
    const char *e = getenv("PI_RADIO_GADGET_FAKE_FLOOD_PPS");
    long pps = e ? atol(e) : 0;
    if (pps <= 0)
        return NULL;
    uint8_t frame[1000];
    memset(frame, 0xA5, sizeof frame);
    static const uint8_t dst[6] = { 0x02, 0x50, 0x49, 0x47, 0x41, 0x44 };
    memcpy(frame, dst, 6);
    memcpy(frame + 6, "\x02\xaa\xbb\xcc\x00\x01", 6);
    frame[12] = 0x88; frame[13] = 0xb5;
    unsigned long sent = 0, total = 0;
    uint64_t t0 = now_ms(), last_log = t0;
    for (;;) {
        pthread_mutex_lock(&f->mu);
        bool up = f->link.up;
        pthread_mutex_unlock(&f->mu);
        if (up) {
            fw_rx_frame(f->be.fw, frame, sizeof frame);
            sent++;
            total++;
        }
        uint64_t now = now_ms();
        if (now - last_log >= 5000) {
            LOGI(TAG, "flood: %lu frames sent in total (target %ld pps, link %s)", total, pps, up ? "up" : "down");
            last_log = now;
        }
        /* pace to the target rate */
        uint64_t due = t0 + (uint64_t)(sent * 1000UL / (unsigned long)pps);
        if (due > now_ms())
            sleep_ms((unsigned)(due - now_ms()));
        if (!up) {
            sleep_ms(200);
            t0 = now_ms(); sent = 0;
        }
    }
}


static void post(struct fake *f, int job)
{
    pthread_mutex_lock(&f->mu);
    f->job = job;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

static int fk_start(struct wl_backend *be)
{
    struct fake *f = be->priv;
    int e = -pthread_create(&f->th, NULL, worker, f);
    if (!e && getenv("PI_RADIO_GADGET_FAKE_FLOOD_PPS"))
        e = -pthread_create(&f->flood_th, NULL, flood_main, f);
    return e;
}
static void fk_stop(struct wl_backend *be) { (void)be; }
static void fk_scan(struct wl_backend *be, const struct wl_scan_req *r)
{
    struct fake *f = be->priv;
    f->scan = *r;
    post(f, 1);
}
static void fk_scan_abort(struct wl_backend *be) { ((struct fake *)be->priv)->abort_scan = true; }
static void fk_join(struct wl_backend *be, const struct wl_join_req *r)
{
    struct fake *f = be->priv;
    pthread_mutex_lock(&f->mu);
    f->join = *r;
    pthread_mutex_unlock(&f->mu);
    post(f, 2);
}
static void fk_leave(struct wl_backend *be)
{
    struct fake *f = be->priv;
    pthread_mutex_lock(&f->mu);
    f->link.up = false;
    pthread_mutex_unlock(&f->mu);
}
static void fk_tx(struct wl_backend *be, const uint8_t *eth, size_t len)
{
    (void)be;
    LOGD(TAG, "tx %zu bytes ethertype %02x%02x (dropped: no radio)", len, eth[12], eth[13]);
}
static bool fk_link(struct wl_backend *be, struct wl_link *out)
{
    struct fake *f = be->priv;
    pthread_mutex_lock(&f->mu);
    *out = f->link;
    pthread_mutex_unlock(&f->mu);
    return out->up;
}

const struct wl_backend_ops wl_backend_fake_ops = {
    .name = "fake", .start = fk_start, .stop = fk_stop, .scan = fk_scan, .scan_abort = fk_scan_abort,
    .join = fk_join, .leave = fk_leave, .tx = fk_tx, .link = fk_link,
};

struct wl_backend *wl_backend_fake_new(void)
{
    struct fake *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->be.ops = &wl_backend_fake_ops;
    f->be.priv = f;
    return &f->be;
}
