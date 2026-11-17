/* The real radio: the Pi's own WiFi chip, driven through a private wpa_supplicant, with an
 * AF_PACKET tap on the interface as the data path.
 *
 *   scan / join / leave   -> wpa_supplicant control socket (SCAN, SELECT_NETWORK, ...)
 *   PC frames  -> Ethernet frames sent out of the interface  (the Pi's chip does 802.11)
 *   radio frames -> Ethernet frames captured on the interface and handed to the PC
 *
 * The PC does all the real WiFi policy (which network, which key). Because the Pi's chip is a
 * FullMAC part, it handles the 4-way handshake itself: the PC's PMK (or SAE password) is passed
 * straight to wpa_supplicant as network settings.
 *
 * The Pi's WiFi interface is taken away from NetworkManager while this runs and handed back on
 * exit (and by pi-radiogadget-radio.sh if the daemon dies without cleaning up).
 *
 * Threading: one supervisor thread owns the control sockets and runs the queued jobs (so the
 * firmware-facing operations return immediately, as backend.h requires); one thread reads the
 * interface for received frames.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "backend.h"
#include "fw.h"
#include "log.h"
#include "tap.h"
#include "wpa_parse.h"

#define TAG "radio"

#define JOIN_TIMEOUT_MS   25000
#define SCAN_TIMEOUT_MS    9000
#define LOSS_GRACE_MS      3000      /* a disconnect followed by a reconnect inside this is a roam */
#define LINK_POLL_MS       2000
#define MAX_BSS            64
#define MAX_AGE_S          30      /* cfg80211 on the PC expires BSS entries after about this long */

enum { J_SCAN = 1, J_JOIN, J_LEAVE, J_MAC, J_COUNTRY, J_AP_START, J_AP_STOP, J_AP_KICK };

struct job {
    int type;
    struct wl_scan_req scan;
    struct wl_join_req join;
    uint8_t mac[6];
    char cc[3];
    struct wl_ap_req ap;
    uint16_t reason;
};

#define JOBQ 16

struct sup {
    struct wl_backend be;
    char ifname[IFNAMSIZ];
    char dir[64];                       /* wpa_supplicant control directory */
    char sock_path[108];
    char conf_path[128];
    char country[3];                    /* regulatory domain chosen by the PC ("" = none yet) */
    pid_t wpa;
    int cmd_fd, ev_fd, ifindex;
    struct tap tap;
    int wake_r, wake_w;
    bool nm_unmanaged;
    bool ap_active;                     /* hostapd (not wpa_supplicant) owns the radio; supervisor thread only */

    pthread_mutex_t mu;
    struct job q[JOBQ];
    int q_head, q_n;
    atomic_bool abort_scan, cancel_join;

    /* owned by the supervisor thread */
    uint64_t loss_deadline, next_poll;
    int loss_reason;

    /* shared */
    atomic_bool up;
    struct wl_link link;                /* guarded by mu */
    uint8_t mac[6], orig_mac[6];
};

static struct sup *S(struct wl_backend *be) { return be->priv; }

/* ---------------------------------------------------------------- small utilities */

static int run_cmd(const char *const argv[])
{
    pid_t p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* out = <dir>/<rest> (compiled as a function of its own: gcc wrongly reports overlap when both are members of struct sup). */
__attribute__((noinline)) static void join_path(char *out, size_t cap, const char *dir, const char *rest)
{
    snprintf(out, cap, "%s/%s", dir, rest);
}

static bool have_cmd(const char *name)
{
    char path[160];
    static const char *const dirs[] = { "/usr/bin", "/bin", "/usr/sbin", "/sbin", "/usr/local/bin" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        if (access(path, X_OK) == 0)
            return true;
    }
    return false;
}

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t n = write(fd, text, strlen(text));
    int e = n < 0 ? -errno : 0;
    close(fd);
    return e;
}

static int read_mac_file(const char *path, uint8_t mac[6])
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    char buf[64];
    int ok = fgets(buf, sizeof buf, fp) != NULL;
    fclose(fp);
    unsigned m[6];
    if (!ok || sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)m[i];
    return 0;
}

static int if_set_flags(const char *ifname, short set, short clear)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    int rc = ioctl(fd, SIOCGIFFLAGS, &ifr);
    if (!rc) {
        ifr.ifr_flags = (short)((ifr.ifr_flags | set) & ~clear);
        rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    int e = rc ? -errno : 0;
    close(fd);
    return e;
}

static int if_set_mac(const char *ifname, const uint8_t mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);
    int rc = ioctl(fd, SIOCSIFHWADDR, &ifr);
    int e = rc ? -errno : 0;
    close(fd);
    return e;
}

static void wake(struct sup *s)
{
    char c = 1;
    ssize_t r = write(s->wake_w, &c, 1);
    (void)r;
}

/* ---------------------------------------------------------------- radio ownership */

static void sysctl_ipv6(struct sup *s, int disable)
{
    char path[128];
    snprintf(path, sizeof path, "/proc/sys/net/ipv6/conf/%s/disable_ipv6", s->ifname);
    write_file(path, disable ? "1" : "0");
}

static void radio_take(struct sup *s)
{
    /* The PC's WiFi stack is the only IP stack that should use this radio: the Pi must not
     * answer ARP, send router solicitations or otherwise talk on it. */
    if (have_cmd("nmcli")) {
        const char *const a[] = { "nmcli", "device", "set", s->ifname, "managed", "no", NULL };
        if (run_cmd(a) == 0)
            s->nm_unmanaged = true;
        sleep_ms(1000);                 /* let NetworkManager release the interface and its supplicant */
    }
    const char *const flush[] = { "ip", "addr", "flush", "dev", s->ifname, NULL };
    run_cmd(flush);
    sysctl_ipv6(s, 1);
    if_set_flags(s->ifname, IFF_UP, 0);
    if_set_flags(s->ifname, IFF_ALLMULTI, 0);       /* the PC's multicast needs (mDNS, IPv6 ND) are unknown */
}

static void radio_give_back(struct sup *s)
{
    if_set_flags(s->ifname, 0, IFF_ALLMULTI);
    if (memcmp(s->mac, s->orig_mac, 6) != 0) {
        if_set_flags(s->ifname, 0, IFF_UP);
        if (if_set_mac(s->ifname, s->orig_mac) == 0)
            memcpy(s->mac, s->orig_mac, 6);
        if_set_flags(s->ifname, IFF_UP, 0);
    }
    sysctl_ipv6(s, 0);
    if (s->nm_unmanaged) {
        const char *const m[] = { "nmcli", "device", "set", s->ifname, "managed", "yes", NULL };
        run_cmd(m);
        const char *const c[] = { "nmcli", "--wait", "0", "device", "connect", s->ifname, NULL };
        run_cmd(c);
        s->nm_unmanaged = false;
    }
}

/* ---------------------------------------------------------------- wpa_supplicant process + sockets */

static void wpa_stop(struct sup *s)
{
    if (s->cmd_fd >= 0) { close(s->cmd_fd); s->cmd_fd = -1; }
    if (s->ev_fd >= 0)  { close(s->ev_fd);  s->ev_fd = -1; }
    if (s->wpa > 0) {
        kill(s->wpa, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (waitpid(s->wpa, NULL, WNOHANG) == s->wpa) { s->wpa = 0; break; }
            sleep_ms(100);
        }
        if (s->wpa > 0) {
            kill(s->wpa, SIGKILL);
            waitpid(s->wpa, NULL, 0);
            s->wpa = 0;
        }
    }
    unlink(s->sock_path);
}

static int ctl_open(struct sup *s, int idx)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un la = { .sun_family = AF_UNIX };
    snprintf(la.sun_path, sizeof la.sun_path, "%s/cli-%d-%d", s->dir, getpid(), idx);
    unlink(la.sun_path);
    struct sockaddr_un ra = { .sun_family = AF_UNIX };
    snprintf(ra.sun_path, sizeof ra.sun_path, "%s", s->sock_path);
    if (bind(fd, (struct sockaddr *)&la, sizeof la) < 0 || connect(fd, (struct sockaddr *)&ra, sizeof ra) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

/* One request, one reply. Returns reply length, or -1. */
static int ctl_cmd(struct sup *s, const char *cmd, char *rep, size_t cap, int ms)
{
    if (s->cmd_fd < 0 || send(s->cmd_fd, cmd, strlen(cmd), 0) < 0)
        return -1;
    struct pollfd p = { .fd = s->cmd_fd, .events = POLLIN };
    for (;;) {
        if (poll(&p, 1, ms) <= 0)
            return -1;
        ssize_t n = recv(s->cmd_fd, rep, cap - 1, 0);
        if (n < 0)
            return -1;
        rep[n] = '\0';
        if (n && rep[0] == '<')
            continue;                                   /* an unsolicited message: not our reply */
        while (n > 0 && (rep[n - 1] == '\n' || rep[n - 1] == ' '))
            rep[--n] = '\0';
        return (int)n;
    }
}

static bool ctl_ok(struct sup *s, const char *cmd)
{
    char rep[128];
    int n = ctl_cmd(s, cmd, rep, sizeof rep, 3000);
    if (n < 0 || strncmp(rep, "OK", 2) != 0) {
        LOGW(TAG, "wpa_supplicant refused \"%.60s\": %s", cmd, n < 0 ? "no reply" : rep);
        return false;
    }
    return true;
}

static void apply_country(struct sup *s)
{
    char cmd[32], rep[32];
    snprintf(cmd, sizeof cmd, "SET country %s", s->country);
    if (ctl_cmd(s, cmd, rep, sizeof rep, 2000) > 0 && !strncmp(rep, "OK", 2))
        LOGI(TAG, "regulatory domain %s applied to the radio", s->country);
    else
        LOGW(TAG, "radio refused regulatory domain %s", s->country);
}

static int wpa_start(struct sup *s)
{
    mkdir("/run/pi-radiogadget", 0755);
    mkdir(s->dir, 0755);
    join_path(s->conf_path, sizeof s->conf_path, s->dir, "../wpa.conf");
    FILE *fp = fopen(s->conf_path, "w");
    if (!fp)
        return -errno;
    fprintf(fp, "ctrl_interface=%s\nupdate_config=0\nap_scan=1\np2p_disabled=1\n", s->dir);
    fclose(fp);

    join_path(s->sock_path, sizeof s->sock_path, s->dir, s->ifname);
    unlink(s->sock_path);

    pid_t p = fork();
    if (p < 0)
        return -errno;
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int nul = open("/dev/null", O_RDWR);
        char logpath[160];
        join_path(logpath, sizeof logpath, s->dir, "../wpa.log");
        if (log_level() >= LOG_DBG) {
            int lf = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        } else if (nul >= 0) {
            dup2(nul, 1); dup2(nul, 2);
        }
        execl("/usr/sbin/wpa_supplicant", "wpa_supplicant", "-i", s->ifname, "-D", "nl80211",
              "-C", s->dir, "-c", s->conf_path, log_level() >= LOG_DBG ? "-dd" : "-q", (char *)NULL);
        _exit(127);
    }
    s->wpa = p;

    for (int i = 0; i < 100; i++) {                     /* up to 5 s for the control socket */
        if (waitpid(p, NULL, WNOHANG) == p) {
            s->wpa = 0;
            LOGE(TAG, "wpa_supplicant exited immediately (is another one still holding %s?)", s->ifname);
            return -EBUSY;
        }
        if (access(s->sock_path, F_OK) == 0)
            break;
        sleep_ms(50);
    }
    s->cmd_fd = ctl_open(s, 0);
    s->ev_fd = ctl_open(s, 1);
    if (s->cmd_fd < 0 || s->ev_fd < 0) {
        LOGE(TAG, "cannot open the wpa_supplicant control socket %s", s->sock_path);
        wpa_stop(s);
        return -ECONNREFUSED;
    }
    char rep[64];
    if (ctl_cmd(s, "PING", rep, sizeof rep, 2000) < 0 || strcmp(rep, "PONG") != 0) {
        LOGE(TAG, "wpa_supplicant does not answer PING");
        wpa_stop(s);
        return -EIO;
    }
    if (send(s->ev_fd, "ATTACH", 6, 0) < 0 || ctl_cmd(s, "STATUS", rep, sizeof rep, 2000) < 0) {
        wpa_stop(s);
        return -EIO;
    }
    char ack[32];
    struct pollfd pf = { .fd = s->ev_fd, .events = POLLIN };
    if (poll(&pf, 1, 2000) > 0)
        recv(s->ev_fd, ack, sizeof ack, 0);              /* "OK" for ATTACH */
    /* Power saving makes a radio that mostly sits idle answer late (tens to hundreds of ms): a dongle should not. */
    {
        const char *const ps[] = { "iw", "dev", s->ifname, "set", "power_save", "off", NULL };
        LOGI(TAG, "radio power save %s", run_cmd(ps) == 0 ? "off" : "could not be turned off");
    }
    if (s->country[0])
        apply_country(s);
    LOGI(TAG, "wpa_supplicant ready on %s", s->ifname);
    return 0;
}

/* ---------------------------------------------------------------- events */

static const char *ev_text(const char *msg)
{
    if (msg[0] == '<') {
        const char *e = strchr(msg, '>');
        if (e)
            return e + 1;
    }
    return msg;
}

/* Events that matter when no job is waiting for them. */
static void handle_idle_event(struct sup *s, const char *msg)
{
    const char *t = ev_text(msg);
    if (s->ap_active) {
        unsigned m[6];
        bool join = !strncmp(t, "AP-STA-CONNECTED ", 17), leave = !strncmp(t, "AP-STA-DISCONNECTED ", 20);
        if ((join || leave) && sscanf(t + (join ? 17 : 20), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            uint8_t mac[6];
            for (int i = 0; i < 6; i++)
                mac[i] = (uint8_t)m[i];
            fw_ap_station(s->be.fw, mac, join);
        }
        return;
    }
    if (!atomic_load(&s->up))
        return;
    if (!strncmp(t, "CTRL-EVENT-DISCONNECTED", 23)) {
        if (!s->loss_deadline) {
            s->loss_deadline = now_ms() + LOSS_GRACE_MS;
            int r = wpa_event_int(t, "reason");
            s->loss_reason = r > 0 ? r : 1;
            LOGI(TAG, "disconnected (reason %d); waiting %d ms for a reconnect", s->loss_reason, LOSS_GRACE_MS);
        }
    } else if (!strncmp(t, "CTRL-EVENT-CONNECTED", 20)) {
        if (s->loss_deadline) {
            LOGI(TAG, "reconnected within the grace period (roam)");
            s->loss_deadline = 0;
        }
    }
}

/* Waits for an event starting with one of pats[]. -1 timeout, -2 cancelled. Other events are
 * handled normally. */
static int wait_event(struct sup *s, const char *const *pats, int n, int ms, atomic_bool *cancel, char *out, size_t cap)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    for (;;) {
        if (cancel && atomic_load(cancel))
            return -2;
        uint64_t now = now_ms();
        if (now >= end)
            return -1;
        struct pollfd p = { .fd = s->ev_fd, .events = POLLIN };
        int slice = (int)(end - now < 100 ? end - now : 100);
        if (poll(&p, 1, slice) <= 0)
            continue;
        char msg[1024];
        ssize_t r = recv(s->ev_fd, msg, sizeof msg - 1, 0);
        if (r <= 0)
            continue;
        msg[r] = '\0';
        const char *t = ev_text(msg);
        for (int i = 0; i < n; i++) {
            if (!strncmp(t, pats[i], strlen(pats[i]))) {
                if (out)
                    snprintf(out, cap, "%.*s", (int)cap - 1, t);
                return i;
            }
        }
        handle_idle_event(s, msg);
    }
}

/* ---------------------------------------------------------------- scan */

static bool fetch_bss(struct sup *s, const char *which, struct wl_bss *b, int *age)
{
    char cmd[48];
    static char rep[4096];
    snprintf(cmd, sizeof cmd, "BSS %s", which);
    int n = ctl_cmd(s, cmd, rep, sizeof rep, 3000);
    return n > 8 && wpa_parse_bss(rep, b, age);
}

static bool chan_wanted(const struct wl_scan_req *req, uint8_t ch)
{
    if (!req->n_channels)
        return true;
    for (int i = 0; i < req->n_channels; i++)
        if (req->channels[i] == ch)
            return true;
    return false;
}

static int scan_once(struct sup *s, const char *cmd)
{
    char rep[128];
    for (int tries = 0; tries < 8; tries++) {
        int n = ctl_cmd(s, cmd, rep, sizeof rep, 3000);
        if (n >= 0 && !strncmp(rep, "OK", 2))
            break;
        if (atomic_load(&s->abort_scan))
            return -2;
        if (n < 0 || tries == 7)
            return -1;
        sleep_ms(400);                                  /* FAIL-BUSY: a scan is already running */
    }
    static const char *const pats[] = { "CTRL-EVENT-SCAN-RESULTS", "CTRL-EVENT-SCAN-FAILED" };
    int r = wait_event(s, pats, 2, SCAN_TIMEOUT_MS, &s->abort_scan, NULL, 0);
    return r == 0 ? 0 : r == -2 ? -2 : -1;
}

static void do_scan(struct sup *s, const struct wl_scan_req *req)
{
    if (s->ap_active) {                                 /* no supplicant while hostapd runs the radio */
        fw_scan_done(s->be.fw, false);
        return;
    }
    atomic_store(&s->abort_scan, false);
    char cmd[200];

    wpa_build_scan(req, false, 0, cmd, sizeof cmd);
    int rc = scan_once(s, cmd);
    if (rc == -2) {
        fw_scan_done(s->be.fw, true);
        return;
    }
    if (rc < 0)
        LOGW(TAG, "scan did not complete cleanly");

    /* Collect the cache: only fresh entries on the requested channels. */
    static struct wl_bss list[MAX_BSS];
    int n = 0;
    for (int i = 0; i < 128 && n < MAX_BSS; i++) {
        char which[16];
        snprintf(which, sizeof which, "%d", i);
        struct wl_bss b;
        int age = 0;
        if (!fetch_bss(s, which, &b, &age))
            break;
        if (age <= MAX_AGE_S && chan_wanted(req, b.channel))
            list[n++] = b;
    }

    /* Hidden networks the host asked for by name only answer a directed probe. */
    for (int k = 0; k < req->n_ssids && !atomic_load(&s->abort_scan); k++) {
        if (!req->ssids[k].len)
            continue;
        bool seen = false;
        for (int i = 0; i < n; i++)
            if (list[i].ssid_len == req->ssids[k].len && !memcmp(list[i].ssid, req->ssids[k].ssid, list[i].ssid_len))
                seen = true;
        if (seen)
            continue;
        wpa_build_scan(req, true, k, cmd, sizeof cmd);
        if (scan_once(s, cmd) == 0) {
            for (int i = 0; i < 128 && n < MAX_BSS; i++) {
                char which[16];
                snprintf(which, sizeof which, "%d", i);
                struct wl_bss b;
                int age = 0;
                if (!fetch_bss(s, which, &b, &age))
                    break;
                bool dup = false;
                for (int j = 0; j < n; j++)
                    if (!memcmp(list[j].bssid, b.bssid, 6))
                        dup = true;
                if (!dup && age <= MAX_AGE_S && chan_wanted(req, b.channel))
                    list[n++] = b;
            }
        }
    }

    for (int i = 0; i < n && !atomic_load(&s->abort_scan); i++)
        fw_scan_result(s->be.fw, &list[i]);
    fw_scan_done(s->be.fw, atomic_load(&s->abort_scan));
}

/* ---------------------------------------------------------------- join / leave */

static void do_leave(struct sup *s)
{
    char rep[64];
    ctl_cmd(s, "DISCONNECT", rep, sizeof rep, 2000);
    ctl_cmd(s, "REMOVE_NETWORK all", rep, sizeof rep, 2000);
    atomic_store(&s->up, false);
    atomic_store(&s->tap.enabled, false);
    s->loss_deadline = 0;
    pthread_mutex_lock(&s->mu);
    s->link.up = false;
    pthread_mutex_unlock(&s->mu);
}

static void update_link(struct sup *s)
{
    char rep[512];
    struct wl_link l;
    memset(&l, 0, sizeof l);
    if (ctl_cmd(s, "SIGNAL_POLL", rep, sizeof rep, 1500) <= 0)
        return;
    const char *v;
    l.up = atomic_load(&s->up);
    if ((v = wpa_kv(rep, "RSSI", NULL)))
        l.rssi = atoi(v);
    if ((v = wpa_kv(rep, "LINKSPEED", NULL)))
        l.rate_kbps = (uint32_t)atoi(v) * 1000u;
    pthread_mutex_lock(&s->mu);
    l.channel = s->link.channel;
    l.ssid_len = s->link.ssid_len;
    memcpy(l.ssid, s->link.ssid, sizeof l.ssid);
    memcpy(l.bssid, s->link.bssid, 6);
    s->link = l;
    pthread_mutex_unlock(&s->mu);
}

/* Current association -> wl_bss (the host reads assoc IEs and BSS info from it). */
static bool snapshot_bss(struct sup *s, struct wl_bss *b)
{
    char rep[1024];
    if (ctl_cmd(s, "STATUS", rep, sizeof rep, 2000) <= 0)
        return false;
    size_t l;
    const char *bssid = wpa_kv(rep, "bssid", &l);
    if (!bssid)
        return false;
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%.*s", (int)(l < 17 ? l : 17), bssid);
    if (fetch_bss(s, tmp, b, NULL))
        return true;
    /* Not in the BSS cache (aged out): build what we can. */
    memset(b, 0, sizeof *b);
    unsigned m[6];
    if (sscanf(tmp, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        b->bssid[i] = (uint8_t)m[i];
    const char *f = wpa_kv(rep, "freq", NULL);
    b->channel = (uint8_t)(f ? wpa_freq_to_chan(atoi(f)) : 1);
    b->beacon_int = 100;
    b->capability = 0x0411;
    return true;
}

static void publish_link(struct sup *s, const struct wl_bss *b)
{
    pthread_mutex_lock(&s->mu);
    s->link.up = true;
    s->link.channel = b->channel;
    s->link.ssid_len = b->ssid_len;
    memcpy(s->link.ssid, b->ssid, sizeof s->link.ssid);
    memcpy(s->link.bssid, b->bssid, 6);
    pthread_mutex_unlock(&s->mu);
}

static void do_join(struct sup *s, const struct wl_join_req *j)
{
    if (s->ap_active) {
        LOGW(TAG, "join refused: the radio is running a hotspot");
        fw_join_done(s->be.fw, WL_JOIN_NO_NETWORK, NULL);
        return;
    }
    atomic_store(&s->cancel_join, false);
    struct wpa_net net;
    wpa_net_from_join(j, &net);
    if (net.unsupported) {
        LOGW(TAG, "refusing join: %s", net.why);
        fw_join_done(s->be.fw, WL_JOIN_AUTH_FAILED, NULL);
        return;
    }

    char rep[256], cmd[320];
    ctl_cmd(s, "REMOVE_NETWORK all", rep, sizeof rep, 2000);
    if (ctl_cmd(s, "ADD_NETWORK", rep, sizeof rep, 2000) <= 0 || !isdigit((unsigned char)rep[0])) {
        LOGW(TAG, "ADD_NETWORK failed");
        fw_join_done(s->be.fw, WL_JOIN_AUTH_FAILED, NULL);
        return;
    }
    int id = atoi(rep);

    bool ok = true;
    for (int i = 0; i < net.n && ok; i++) {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d %s %s", id, net.opt[i].key, net.opt[i].value);
        ok = ctl_ok(s, cmd);
    }
    /* Honour the BSS the host picked, if our own scan has seen it. */
    if (ok && !j->bssid_any) {
        char b[24], q[48];
        snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", j->bssid[0], j->bssid[1], j->bssid[2],
                 j->bssid[3], j->bssid[4], j->bssid[5]);
        snprintf(q, sizeof q, "BSS %s", b);
        if (ctl_cmd(s, q, rep, sizeof rep, 2000) > 8) {
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d bssid %s", id, b);
            ctl_ok(s, cmd);
        }
    }
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
    if (!ok || !ctl_ok(s, cmd)) {
        fw_join_done(s->be.fw, WL_JOIN_AUTH_FAILED, NULL);
        return;
    }

    static const char *const pats[] = {
        "CTRL-EVENT-CONNECTED", "CTRL-EVENT-SSID-TEMP-DISABLED", "CTRL-EVENT-NETWORK-NOT-FOUND",
        "CTRL-EVENT-AUTH-REJECT", "CTRL-EVENT-ASSOC-REJECT",
    };
    int rejects = 0;
    enum wl_join_result result = WL_JOIN_AUTH_FAILED;
    bool connected = false;
    uint64_t end = now_ms() + JOIN_TIMEOUT_MS;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) {
            LOGW(TAG, "join timed out");
            break;
        }
        char msg[512];
        int r = wait_event(s, pats, 5, left, &s->cancel_join, msg, sizeof msg);
        if (r == -2) {
            LOGI(TAG, "join cancelled");
            do_leave(s);
            return;                                     /* the firmware already moved on */
        }
        if (r < 0)
            continue;
        if (r == 0) { connected = true; break; }
        if (r == 2) { result = WL_JOIN_NO_NETWORK; LOGW(TAG, "network not found"); break; }
        if (r == 1) {
            LOGW(TAG, "join failed: %s", msg);
            break;                                      /* WRONG_KEY, CONN_FAILED, ... */
        }
        if (++rejects >= 3) {
            LOGW(TAG, "join rejected repeatedly: %s", msg);
            break;
        }
    }

    if (!connected) {
        ctl_cmd(s, "REMOVE_NETWORK all", rep, sizeof rep, 2000);
        fw_join_done(s->be.fw, result, NULL);
        return;
    }
    struct wl_bss bss;
    if (!snapshot_bss(s, &bss)) {
        LOGW(TAG, "connected but cannot read the association");
        do_leave(s);
        fw_join_done(s->be.fw, WL_JOIN_AUTH_FAILED, NULL);
        return;
    }
    if (!bss.ssid_len) {                                /* keep the name the host asked for */
        bss.ssid_len = j->ssid_len;
        memcpy(bss.ssid, j->ssid, j->ssid_len);
    }
    atomic_store(&s->up, true);
    atomic_store(&s->tap.enabled, true);
    s->loss_deadline = 0;
    publish_link(s, &bss);
    update_link(s);
    s->next_poll = now_ms() + LINK_POLL_MS;
    fw_join_done(s->be.fw, WL_JOIN_OK, &bss);
}

static void do_mac(struct sup *s, const uint8_t mac[6])
{
    if (!memcmp(mac, s->mac, 6))
        return;
    if (s->ap_active) {
        LOGW(TAG, "MAC change requested while a hotspot runs: ignored");
        return;
    }
    if (atomic_load(&s->up)) {
        LOGW(TAG, "MAC change requested while associated: ignored");
        return;
    }
    LOGI(TAG, "changing %s MAC to %02x:%02x:%02x:%02x:%02x:%02x", s->ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    /* The supplicant keeps interface state: restart it around the change. */
    wpa_stop(s);
    if_set_flags(s->ifname, 0, IFF_UP);
    int e = if_set_mac(s->ifname, mac);
    if (e) {
        LOGW(TAG, "cannot set the MAC address: %s", strerror(-e));
    } else {
        memcpy(s->mac, mac, 6);
        memcpy(s->tap.mac, mac, 6);                     /* frames from the PC must carry the new address */
    }
    if_set_flags(s->ifname, IFF_UP | IFF_ALLMULTI, 0);
    if (wpa_start(s) < 0)
        LOGE(TAG, "could not restart wpa_supplicant after the MAC change");
}

/* ---------------------------------------------------------------- supervisor */

static bool pop_job(struct sup *s, struct job *out)
{
    bool got = false;
    pthread_mutex_lock(&s->mu);
    if (s->q_n) {
        *out = s->q[s->q_head];
        s->q_head = (s->q_head + 1) % JOBQ;
        s->q_n--;
        got = true;
    }
    pthread_mutex_unlock(&s->mu);
    return got;
}

static void push_job(struct sup *s, const struct job *j)
{
    pthread_mutex_lock(&s->mu);
    if (s->q_n < JOBQ) {
        s->q[(s->q_head + s->q_n) % JOBQ] = *j;
        s->q_n++;
    } else {
        LOGW(TAG, "job queue full: dropping a request");
    }
    pthread_mutex_unlock(&s->mu);
    wake(s);
}

/* ---------------------------------------------------------------- hotspot (hostapd) */

static void hex_bytes(char *out, const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", in[i]);
}

static int hostapd_write_conf(struct sup *s, const struct wl_ap_req *r)
{
    FILE *fp = fopen(s->conf_path, "w");
    if (!fp)
        return -errno;
    char ssid[65];
    hex_bytes(ssid, r->ssid, r->ssid_len);
    fprintf(fp, "interface=%s\ndriver=nl80211\nctrl_interface=%s\nssid2=%s\n", s->ifname, s->dir, ssid);
    if (s->country[0])
        fprintf(fp, "country_code=%s\n", s->country);
    fprintf(fp, "hw_mode=g\nchannel=%u\nwmm_enabled=1\nieee80211n=1\n", r->channel);
    if (r->beacon_interval)
        fprintf(fp, "beacon_int=%u\n", r->beacon_interval);
    if (r->dtim_period)
        fprintf(fp, "dtim_period=%u\n", r->dtim_period);
    if (r->hidden)
        fprintf(fp, "ignore_broadcast_ssid=1\n");
    if (r->wpa2_psk) {
        fprintf(fp, "wpa=2\nwpa_key_mgmt=WPA-PSK%s\nrsn_pairwise=CCMP\n", r->psk_sha256 ? " WPA-PSK-SHA256" : "");
        if (r->pmf)
            fprintf(fp, "ieee80211w=%u\n", r->pmf);
        if (r->have_pmk) {
            char psk[65];
            hex_bytes(psk, r->pmk, 32);
            fprintf(fp, "wpa_psk=%s\n", psk);
        } else {
            fprintf(fp, "wpa_passphrase=%s\n", r->passphrase);
        }
    }
    fclose(fp);
    chmod(s->conf_path, 0600);                          /* it holds the key */
    return 0;
}

static void do_ap_stop(struct sup *s, bool restart_supplicant)
{
    if (!s->ap_active && !restart_supplicant)
        return;
    bool was = s->ap_active;
    s->ap_active = false;
    atomic_store(&s->tap.enabled, false);
    wpa_stop(s);                                        /* kills hostapd: the child pid is kept in the same field */
    if (was)
        LOGI(TAG, "hotspot stopped");
    if_set_flags(s->ifname, 0, IFF_UP);
    unlink(s->conf_path);
    if (restart_supplicant) {
        if_set_flags(s->ifname, IFF_UP | IFF_ALLMULTI, 0);
        if (wpa_start(s) < 0)
            LOGE(TAG, "could not restart wpa_supplicant after the hotspot");
    }
}

static void do_ap_start(struct sup *s, const struct wl_ap_req *r)
{
    if (!have_cmd("hostapd")) {
        LOGE(TAG, "hotspot needs hostapd on the Pi (sudo apt install hostapd); not started");
        fw_ap_lost(s->be.fw);
        return;
    }
    if (s->ap_active)
        do_ap_stop(s, false);
    do_leave(s);
    wpa_stop(s);                                        /* the supplicant must let go of the interface */
    if_set_flags(s->ifname, 0, IFF_UP);
    join_path(s->conf_path, sizeof s->conf_path, s->dir, "../hostapd.conf");
    if (hostapd_write_conf(s, r) < 0) {
        LOGE(TAG, "cannot write the hostapd configuration");
        do_ap_stop(s, true);
        fw_ap_lost(s->be.fw);
        return;
    }
    join_path(s->sock_path, sizeof s->sock_path, s->dir, s->ifname);
    unlink(s->sock_path);

    pid_t p = fork();
    if (p < 0) {
        do_ap_stop(s, true);
        fw_ap_lost(s->be.fw);
        return;
    }
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int nul = open("/dev/null", O_RDWR);
        /* hostapd's own output is what explains a hotspot that does not come up: keep it next to the daemon's log on the boot
         * partition (readable from the PC), or in /run when there is none. */
        const char *logpath = access("/boot/firmware", W_OK) == 0 ? "/boot/firmware/pi-radiogadget-hostapd.log" : "/run/pi-radiogadget/hostapd.log";
        int lf = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); } else if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("hostapd", "hostapd", "-d", s->conf_path, (char *)NULL);
        _exit(127);
    }
    s->wpa = p;

    for (int i = 0; i < 100 && access(s->sock_path, F_OK) != 0; i++) {
        if (waitpid(p, NULL, WNOHANG) == p) {
            s->wpa = 0;
            break;
        }
        sleep_ms(50);
    }
    s->cmd_fd = s->wpa ? ctl_open(s, 0) : -1;
    s->ev_fd = s->wpa ? ctl_open(s, 1) : -1;
    char rep[64];
    bool ok = s->cmd_fd >= 0 && s->ev_fd >= 0 && ctl_cmd(s, "PING", rep, sizeof rep, 2000) > 0 && !strcmp(rep, "PONG") &&
              send(s->ev_fd, "ATTACH", 6, 0) > 0;
    if (ok) {
        /* AP-ENABLED may have been sent before we attached (hostapd is quick), so ask for the state instead. */
        uint64_t end = now_ms() + 8000;
        ok = false;
        while (now_ms() < end) {
            if (waitpid(p, NULL, WNOHANG) == p) {          /* hostapd exited: it gave up */
                s->wpa = 0;
                break;
            }
            char st[512];
            if (ctl_cmd(s, "STATUS", st, sizeof st, 1000) > 0 && strstr(st, "state=ENABLED")) {
                ok = true;
                break;
            }
            sleep_ms(100);
        }
    }
    if (!ok) {
        LOGE(TAG, "hostapd did not bring the hotspot up");
        do_ap_stop(s, true);
        fw_ap_lost(s->be.fw);
        return;
    }
    if_set_flags(s->ifname, IFF_ALLMULTI, 0);
    s->ap_active = true;
    atomic_store(&s->tap.enabled, true);
    LOGI(TAG, "hotspot \"%.*s\" is up on channel %u (%s)", r->ssid_len, r->ssid, r->channel, r->wpa2_psk ? "WPA2-PSK" : "open");
}

static void do_ap_kick(struct sup *s, const uint8_t mac[6], uint16_t reason)
{
    if (!s->ap_active)
        return;
    char cmd[96], rep[64];
    snprintf(cmd, sizeof cmd, "DEAUTHENTICATE %02x:%02x:%02x:%02x:%02x:%02x reason=%u", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             reason ? reason : 3);
    ctl_cmd(s, cmd, rep, sizeof rep, 2000);
}

static void *supervisor(void *arg) __attribute__((noreturn));
static void *supervisor(void *arg)
{
    struct sup *s = arg;
    for (;;) {
        struct pollfd p[2] = { { .fd = s->ev_fd, .events = POLLIN }, { .fd = s->wake_r, .events = POLLIN } };
        if (s->ev_fd < 0)
            p[0].fd = -1;
        poll(p, 2, 500);
        if (p[0].revents & POLLIN) {
            char msg[1024];
            ssize_t r = recv(s->ev_fd, msg, sizeof msg - 1, MSG_DONTWAIT);
            if (r > 0) {
                msg[r] = '\0';
                handle_idle_event(s, msg);
            }
        }
        if (p[1].revents & POLLIN) {
            char junk[32];
            ssize_t r = read(s->wake_r, junk, sizeof junk);
            (void)r;
        }

        struct job j;
        while (pop_job(s, &j)) {
            switch (j.type) {
            case J_SCAN:  do_scan(s, &j.scan); break;
            case J_JOIN:  do_join(s, &j.join); break;
            case J_LEAVE: do_leave(s); break;
            case J_MAC:   do_mac(s, j.mac); break;
            case J_AP_START: do_ap_start(s, &j.ap); break;
            case J_AP_STOP:  do_ap_stop(s, true); break;
            case J_AP_KICK:  do_ap_kick(s, j.mac, j.reason); break;
            case J_COUNTRY:
                memcpy(s->country, j.cc, 3);
                apply_country(s);
                break;
            }
        }

        if (s->ap_active && s->wpa > 0 && waitpid(s->wpa, NULL, WNOHANG) == s->wpa) {
            s->wpa = 0;
            LOGE(TAG, "hostapd died: the hotspot is gone");
            do_ap_stop(s, true);
            fw_ap_lost(s->be.fw);
        }

        uint64_t now = now_ms();
        if (s->loss_deadline && now >= s->loss_deadline) {
            s->loss_deadline = 0;
            LOGW(TAG, "link lost (reason %d)", s->loss_reason);
            do_leave(s);
            fw_link_lost(s->be.fw, (uint16_t)s->loss_reason);
        }
        if (atomic_load(&s->up) && now >= s->next_poll) {
            update_link(s);
            s->next_poll = now + LINK_POLL_MS;
        }
    }
}

/* ---------------------------------------------------------------- data path */

static void tap_rx(const uint8_t *eth, size_t len, void *ctx)
{
    struct sup *s = ctx;
    fw_rx_frame(s->be.fw, eth, len);
}

static void sup_tx(struct wl_backend *be, const uint8_t *eth, size_t len)
{
    struct sup *s = S(be);
    tap_send(&s->tap, eth, len);
}

/* ---------------------------------------------------------------- backend ops */

static void sup_scan(struct wl_backend *be, const struct wl_scan_req *r)
{
    struct job j = { .type = J_SCAN, .scan = *r };
    push_job(S(be), &j);
}

static void sup_scan_abort(struct wl_backend *be)
{
    atomic_store(&S(be)->abort_scan, true);
}

static void sup_join(struct wl_backend *be, const struct wl_join_req *r)
{
    struct job j = { .type = J_JOIN, .join = *r };
    push_job(S(be), &j);
}

static void sup_leave(struct wl_backend *be)
{
    struct sup *s = S(be);
    atomic_store(&s->cancel_join, true);                /* abort a join in progress */
    struct job j = { .type = J_LEAVE };
    push_job(s, &j);
}

static void sup_set_country(struct wl_backend *be, const char cc[3])
{
    struct job j = { .type = J_COUNTRY };
    memcpy(j.cc, cc, 3);
    push_job(S(be), &j);
}

static void sup_ap_start(struct wl_backend *be, const struct wl_ap_req *r)
{
    struct job j = { .type = J_AP_START, .ap = *r };
    push_job(S(be), &j);
}

static void sup_ap_stop(struct wl_backend *be)
{
    struct job j = { .type = J_AP_STOP };
    push_job(S(be), &j);
}

static void sup_ap_deauth(struct wl_backend *be, const uint8_t mac[6], uint16_t reason)
{
    struct job j = { .type = J_AP_KICK, .reason = reason };
    memcpy(j.mac, mac, 6);
    push_job(S(be), &j);
}

static size_t sup_status(struct wl_backend *be, char *buf, size_t size)
{
    struct sup *s = S(be);
    int n = snprintf(buf, size, "tap[rx=%lu tx=%lu tx_drop=%lu eapol_drop=%lu sa_rewrites=%lu enabled=%d] radio=%s",
                     (unsigned long)atomic_load(&s->tap.rx_frames), (unsigned long)atomic_load(&s->tap.tx_frames),
                     (unsigned long)atomic_load(&s->tap.tx_dropped), (unsigned long)atomic_load(&s->tap.eapol_dropped),
                     (unsigned long)atomic_load(&s->tap.sa_rewrites), atomic_load(&s->tap.enabled) ? 1 : 0,
                     s->ap_active ? "hotspot" : atomic_load(&s->up) ? "station" : "idle");
    return n < 0 ? 0 : ((size_t)n >= size ? size - 1 : (size_t)n);
}

static void sup_set_mac(struct wl_backend *be, const uint8_t mac[6])
{
    struct job j = { .type = J_MAC };
    memcpy(j.mac, mac, 6);
    push_job(S(be), &j);
}

static bool sup_link(struct wl_backend *be, struct wl_link *out)
{
    struct sup *s = S(be);
    pthread_mutex_lock(&s->mu);
    *out = s->link;
    pthread_mutex_unlock(&s->mu);
    return out->up;
}

static int sup_start(struct wl_backend *be)
{
    struct sup *s = S(be);
    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/phy80211/macaddress", s->ifname);
    if (read_mac_file(path, s->orig_mac) < 0) {
        snprintf(path, sizeof path, "/sys/class/net/%s/address", s->ifname);
        if (read_mac_file(path, s->orig_mac) < 0)
            return -ENODEV;
    }
    snprintf(path, sizeof path, "/sys/class/net/%s/address", s->ifname);
    if (read_mac_file(path, s->mac) < 0)
        memcpy(s->mac, s->orig_mac, 6);
    s->ifindex = (int)if_nametoindex(s->ifname);
    if (!s->ifindex)
        return -ENODEV;

    radio_take(s);
    int e = wpa_start(s);
    if (e) {
        radio_give_back(s);
        return e;
    }

    if ((e = tap_open(&s->tap, s->ifname))) {
        LOGE(TAG, "packet socket on %s: %s", s->ifname, strerror(-e));
        wpa_stop(s);
        radio_give_back(s);
        return e;
    }
    memcpy(s->tap.mac, s->mac, 6);

    pthread_t t;
    if ((e = -pthread_create(&t, NULL, supervisor, s)) || (e = tap_start(&s->tap, tap_rx, s))) {
        wpa_stop(s);
        radio_give_back(s);
        return e;
    }
    LOGI(TAG, "radio %s ready (MAC %02x:%02x:%02x:%02x:%02x:%02x)", s->ifname, s->mac[0], s->mac[1], s->mac[2],
         s->mac[3], s->mac[4], s->mac[5]);
    return 0;
}

static void sup_stop(struct wl_backend *be)
{
    struct sup *s = S(be);
    atomic_store(&s->up, false);
    tap_close(&s->tap);
    wpa_stop(s);
    radio_give_back(s);
    LOGI(TAG, "radio %s returned to the system", s->ifname);
}

const struct wl_backend_ops wl_backend_sup_ops = {
    .name = "sup", .start = sup_start, .stop = sup_stop, .scan = sup_scan, .scan_abort = sup_scan_abort,
    .join = sup_join, .leave = sup_leave, .tx = sup_tx, .link = sup_link, .set_mac = sup_set_mac, .set_country = sup_set_country,
    .ap_start = sup_ap_start, .ap_stop = sup_ap_stop, .ap_deauth = sup_ap_deauth, .status = sup_status,
};

struct wl_backend *wl_backend_sup_new(const char *ifname, const char *ctrl_dir)
{
    struct sup *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    snprintf(s->ifname, sizeof s->ifname, "%s", ifname);
    snprintf(s->dir, sizeof s->dir, "%s", ctrl_dir);
    s->cmd_fd = s->ev_fd = -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
        free(s);
        return NULL;
    }
    s->wake_r = p[0];
    s->wake_w = p[1];
    pthread_mutex_init(&s->mu, NULL);
    atomic_init(&s->up, false);
    atomic_init(&s->abort_scan, false);
    atomic_init(&s->cancel_join, false);
    s->be.ops = &wl_backend_sup_ops;
    s->be.priv = s;
    return &s->be;
}
