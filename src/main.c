/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pi-radio-gadget: Raspberry Pi Zero 2 W as a USB Bluetooth + WiFi dongle for a PC.
 *
 * One process drives both USB functions of a composite gadget:
 *   - FunctionFS "ffs.wlan" : emulated Broadcom USB WiFi dongle backed by the Pi's radio
 *   - FunctionFS "ffs.hci"  : USB Bluetooth dongle backed by the Pi's HCI controller
 *
 * The gadget itself (configfs) is created by pi-radio-gadget-setup.sh; this daemon writes the
 * function descriptors, attaches the UDC once both functions are ready, and then relays.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "backend.h"
#include "ffs.h"
#include "hci.h"
#include "log.h"
#include "wlan.h"

#define TAG "main"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_stats;

static volatile sig_atomic_t g_restart;
static pthread_t g_main_th;
static void on_term(int s) { (void)s; g_stop = 1; }
static void on_hup(int s) { (void)s; g_restart = 1; g_stop = 1; }
/* Called from the USB thread after an update has been installed: wake the main thread and leave with a failure code, so
 * systemd (Restart=on-failure) starts the new binary. */
static void request_restart(void) { pthread_kill(g_main_th, SIGHUP); }
static void request_stop(void) { pthread_kill(g_main_th, SIGTERM); }
static void on_usr1(int s) { (void)s; g_stats = 1; }

static struct hci_fn *g_hci;
static struct wlan_fn *g_wlan;
static size_t status_line(void *ctx, char *buf, size_t size)
{
    (void)ctx;
    size_t n = g_hci ? hci_stats_line(g_hci, buf, size) : 0;
    if (g_wlan && n + 2 < size) {
        buf[n++] = ' ';
        n += fw_stats_line(&g_wlan->fw, buf + n, size - n);
    }
    return n;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --ffs-hci DIR        FunctionFS mount for Bluetooth   (default /dev/ffs-hci)\n"
        "  --ffs-sco DIR        FunctionFS mount for call audio  (default /dev/ffs-sco)\n"
        "  --ffs-wlan DIR       FunctionFS mount for WiFi        (default /dev/ffs-wlan)\n"
        "  --no-hci | --no-wlan disable one function\n"
        "  --hci-dev N          controller index                  (default 0)\n"
        "  --wlan-backend B     sup (real radio) or fake          (default sup)\n"
        "  --wlan-iface IF      the Pi's WiFi interface           (default wlan0)\n"
        "  --wlan-ctrl DIR      wpa_supplicant control directory  (default /run/pi-radio-gadget/wpa)\n"
        "  --wlan-mac MAC       override the dongle MAC (fake backend only)\n"
        "  --udc-file PATH      configfs UDC file to write once ready\n"
        "  --udc NAME           UDC to attach (default: first in /sys/class/udc)\n"
        "  --int-maxp N         Bluetooth event endpoint packet size (default 16)\n"
        "  --no-zlp             do not send zero-length packets on Bluetooth IN transfers\n"
        "  --sco-over-hci       experimental: send call audio over HCI (can hang the Pi's Bluetooth chip)\n"
        "  --log-file PATH      also write INFO+ lines to PATH (max 256 KB)\n"
        "  --keepalive-ms N     WiFi IN-pipe keep-alive period, 0 = off    (default 2)\n"
        "  -v / -q              more / less logging (also PI_RADIO_GADGET_LOG=0..4)\n",
        argv0);
}

static int first_udc(char *out, size_t cap)
{
    DIR *d = opendir("/sys/class/udc");
    if (!d)
        return -errno;
    struct dirent *e;
    int rc = -ENODEV;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(out, cap, "%.*s", (int)cap - 1, e->d_name);
        rc = 0;
        break;
    }
    closedir(d);
    return rc;
}

static int read_mac(const char *ifname, uint8_t mac[6])
{
    char path[128], buf[64];
    snprintf(path, sizeof path, "/sys/class/net/%s/address", ifname);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -errno;
    int ok = fgets(buf, sizeof buf, fp) != NULL;
    fclose(fp);
    unsigned m[6];
    if (!ok || sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return -EINVAL;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)m[i];
    return 0;
}

int main(int argc, char **argv)
{
    struct hci_fn hci = { .ffs_dir = "/dev/ffs-hci", .ffs_sco_dir = "/dev/ffs-sco", .hci_dev = 0, .int_maxp = 16, .zlp = true };
    /* keepalive_ms: see ffs.h (ep_writer). 2 ms keeps the WiFi IN pipe from ever idling. */
    struct wlan_fn wlan = { .ffs_dir = "/dev/ffs-wlan", .keepalive_ms = 2 };
    bool use_hci = true, use_wlan = true;
    const char *backend = "sup", *iface = "wlan0", *ctrl = "/run/pi-radio-gadget/wpa";
    const char *udc_file = NULL, *udc_name = NULL, *mac_opt = NULL;
    char udc_buf[64];
    int level = LOG_INFO;

    if (getenv("PI_RADIO_GADGET_LOG"))
        level = atoi(getenv("PI_RADIO_GADGET_LOG"));

    enum { O_FFS_HCI = 256, O_FFS_SCO, O_FFS_WLAN, O_NO_HCI, O_NO_WLAN, O_HCI_DEV, O_BACKEND, O_IFACE, O_CTRL,
           O_MAC, O_UDC_FILE, O_UDC, O_INT_MAXP, O_NO_ZLP, O_KEEPALIVE, O_LOGFILE, O_SCOHCI };
    static const struct option opts[] = {
        { "ffs-hci", 1, 0, O_FFS_HCI }, { "ffs-sco", 1, 0, O_FFS_SCO }, { "ffs-wlan", 1, 0, O_FFS_WLAN },
        { "no-hci", 0, 0, O_NO_HCI }, { "no-wlan", 0, 0, O_NO_WLAN },
        { "hci-dev", 1, 0, O_HCI_DEV }, { "wlan-backend", 1, 0, O_BACKEND },
        { "wlan-iface", 1, 0, O_IFACE }, { "wlan-ctrl", 1, 0, O_CTRL }, { "wlan-mac", 1, 0, O_MAC },
        { "udc-file", 1, 0, O_UDC_FILE }, { "udc", 1, 0, O_UDC },
        { "int-maxp", 1, 0, O_INT_MAXP }, { "no-zlp", 0, 0, O_NO_ZLP }, { "keepalive-ms", 1, 0, O_KEEPALIVE }, { "log-file", 1, 0, O_LOGFILE }, { "sco-over-hci", 0, 0, O_SCOHCI },
        { "help", 0, 0, 'h' }, { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "vqh", opts, NULL)) != -1) {
        switch (c) {
        case O_FFS_HCI:  snprintf(hci.ffs_dir, sizeof hci.ffs_dir, "%s", optarg); break;
        case O_FFS_SCO:  snprintf(hci.ffs_sco_dir, sizeof hci.ffs_sco_dir, "%s", optarg); break;
        case O_FFS_WLAN: snprintf(wlan.ffs_dir, sizeof wlan.ffs_dir, "%s", optarg); break;
        case O_NO_HCI:   use_hci = false; break;
        case O_NO_WLAN:  use_wlan = false; break;
        case O_HCI_DEV:  hci.hci_dev = atoi(optarg); break;
        case O_BACKEND:  backend = optarg; break;
        case O_IFACE:    iface = optarg; break;
        case O_CTRL:     ctrl = optarg; break;
        case O_MAC:      mac_opt = optarg; break;
        case O_UDC_FILE: udc_file = optarg; break;
        case O_UDC:      udc_name = optarg; break;
        case O_INT_MAXP: hci.int_maxp = (unsigned)atoi(optarg); break;
        case O_NO_ZLP:   hci.zlp = false; break;
        case O_KEEPALIVE: wlan.keepalive_ms = (unsigned)atoi(optarg); break;
        case O_SCOHCI:   hci.sco_over_hci = true; break;
        case O_LOGFILE:  log_set_file(optarg, 256 * 1024); break;
        case 'v': level++; break;
        case 'q': level--; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    log_set_level(level < 0 ? 0 : level > LOG_TRACE ? LOG_TRACE : level);
    if (hci.int_maxp < 8 || hci.int_maxp > 64) {
        fprintf(stderr, "--int-maxp must be 8..64\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    char upd_exe[256] = "";
    {
        ssize_t n = readlink("/proc/self/exe", upd_exe, sizeof upd_exe - 1);
        upd_exe[n > 0 ? n : 0] = 0;
    }
    struct upd upd;
    upd_init(&upd, upd_exe[0] ? upd_exe : "/nonexistent/pi-radio-gadget", request_restart);
    hci.upd = &upd;
    upd.stop = request_stop;
    g_hci = use_hci ? &hci : NULL;
    g_wlan = use_wlan ? &wlan : NULL;
    upd.extra = status_line;
    upd.extra_ctx = NULL;
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    struct sigaction sh = { .sa_handler = on_hup };
    sigaction(SIGHUP, &sh, NULL);
    g_main_th = pthread_self();
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    /* ---- phase 1: prepare both functions (writes descriptors; the UDC is not attached yet) */
    if (use_wlan) {
        if (!strcmp(backend, "fake"))
            wlan.be = wl_backend_fake_new();
        else if (!strcmp(backend, "sup"))
            wlan.be = wl_backend_sup_new(iface, ctrl);
        else {
            fprintf(stderr, "unknown backend '%s'\n", backend);
            return 2;
        }
        if (!wlan.be)
            log_fatal(TAG, "could not create the '%s' WiFi backend", backend);

        if (mac_opt && !strcmp(backend, "fake")) {
            unsigned m[6];
            if (sscanf(mac_opt, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
                log_fatal(TAG, "bad --wlan-mac");
            for (int i = 0; i < 6; i++)
                wlan.mac[i] = (uint8_t)m[i];
        } else if (!strcmp(backend, "fake")) {
            static const uint8_t def[6] = { 0x02, 0x50, 0x49, 0x47, 0x41, 0x44 };
            memcpy(wlan.mac, def, 6);
        } else if (read_mac(iface, wlan.mac) < 0) {
            log_fatal(TAG, "cannot read the MAC address of %s", iface);
        }
        int e = wlan_fn_prepare(&wlan);
        if (e)
            log_fatal(TAG, "wlan prepare failed: %s", strerror(-e));
    }
    if (use_hci) {
        int e = hci_fn_prepare(&hci);
        if (e)
            log_fatal(TAG, "bluetooth prepare failed: %s", strerror(-e));
    }

    /* ---- phase 2: attach to the USB device controller, now that the descriptors are in place */
    if (udc_file) {
        if (!udc_name) {
            if (first_udc(udc_buf, sizeof udc_buf) < 0)
                log_fatal(TAG, "no UDC found in /sys/class/udc (is dtoverlay=dwc2 set?)");
            udc_name = udc_buf;
        }
        int fd = open(udc_file, O_WRONLY | O_CLOEXEC);
        if (fd < 0 || write(fd, udc_name, strlen(udc_name)) < 0)
            log_fatal(TAG, "attaching UDC %s via %s failed: %s", udc_name, udc_file, strerror(errno));
        close(fd);
        ffs_set_udc(udc_file, udc_name);
        LOGI(TAG, "gadget attached to %s", udc_name);
    }

    /* ---- phase 3: endpoints exist now; start relaying */
    if (use_wlan) {
        int e = wlan_fn_run(&wlan);
        if (e)
            log_fatal(TAG, "wlan start failed: %s", strerror(-e));
    }
    if (use_hci) {
        int e = hci_fn_run(&hci);
        if (e)
            log_fatal(TAG, "bluetooth start failed: %s", strerror(-e));
    }
    if (use_wlan) {
        int e = wlan_fn_start_backend(&wlan);
        if (e)
            log_fatal(TAG, "radio backend start failed: %s", strerror(-e));
    }
    LOGI(TAG, "running (wlan=%s%s%s, bluetooth=%s)", use_wlan ? "on" : "off", use_wlan ? "/" : "",
         use_wlan ? backend : "", use_hci ? "on" : "off");

    time_t started = time(NULL);
    bool confirmed = false;
    if (use_hci && upd_exe[0] && access(upd.pending, F_OK) == 0)
        LOGI(TAG, "running a freshly installed version: it is confirmed after 60 s of uptime");
    while (!g_stop) {
        sleep(1);                                   /* signals set flags; wake up every second to look at them */
        if (!confirmed && time(NULL) - started >= 60) {
            confirmed = true;
            if (upd_exe[0] && unlink(upd.pending) == 0)
                LOGI(TAG, "new version confirmed (ran 60 s), rollback marker removed");
        }
        if (g_stats) {
            g_stats = 0;
            if (use_hci)
                LOGI(TAG, "bt: to_chip=%lu from_chip=%lu sco_to_pc=%lu sco_to_chip=%lu acl_resyncs=%lu", hci.to_chip,
                     hci.from_chip, hci.sco_to_pc, hci.sco_to_chip, hci.split.resyncs);
            if (use_wlan)
                LOGI(TAG, "wlan: tx=%lu rx=%lu tx_dropped=%lu rx_dropped=%lu", wlan.fw.tx_frames,
                     wlan.fw.rx_frames, wlan.fw.tx_dropped, wlan.fw.rx_dropped);
        }
    }
    LOGI(TAG, "terminating");
    if (use_wlan && wlan.be && wlan.be->ops->stop)
        wlan.be->ops->stop(wlan.be);
    return g_restart ? 75 : 0;
}
