/* Data-path test against a virtual Ethernet pair. Needs root (to create the veth pair);
 * exits 77 (meson: skipped) otherwise. */
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "check.h"
#include "log.h"
#include "tap.h"

#define A "pbt-t0"
#define B "pbt-t1"
#define TEST_ETYPE 0x88b5        /* IEEE 802 local experimental ethertype: nothing else sends it */

static int sh(const char *cmd) { int r = system(cmd); return r; }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int got;
static uint8_t last[2048];
static size_t last_len;

static void on_rx(const uint8_t *eth, size_t len, void *ctx)
{
    (void)ctx;
    if (len < ETH_HLEN)
        return;
    uint16_t et = (uint16_t)(eth[12] << 8 | eth[13]);
    if (et != TEST_ETYPE && et != 0x888e)
        return;                                  /* ignore IPv6 noise from the kernel */
    pthread_mutex_lock(&mu);
    got++;
    last_len = len < sizeof last ? len : sizeof last;
    memcpy(last, eth, last_len);
    pthread_mutex_unlock(&mu);
}

static int rx_count(void) { pthread_mutex_lock(&mu); int g = got; pthread_mutex_unlock(&mu); return g; }

static bool wait_count(int want)
{
    for (int i = 0; i < 100; i++) {
        if (rx_count() >= want)
            return true;
        usleep(10 * 1000);
    }
    return false;
}

static void frame(uint8_t *f, size_t n, const uint8_t *dst, const uint8_t *src, uint16_t et, uint8_t fill)
{
    memcpy(f, dst, 6); memcpy(f + 6, src, 6);
    f[12] = (uint8_t)(et >> 8); f[13] = (uint8_t)et;
    memset(f + 14, fill, n - 14);
}

/* Raw socket on the peer end, receiving only our test ethertype. */
static int peer_open(void)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = (int)if_nametoindex(B) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0)
        return -1;
    return fd;
}

/* Returns the length of the next frame of ethertype et seen within ms, or 0. */
static size_t peer_recv(int fd, uint16_t et, uint8_t *buf, size_t cap, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    while ((uint64_t)now_ms() < end) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 20) <= 0)
            continue;
        struct sockaddr_ll from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, cap, 0, (struct sockaddr *)&from, &fl);
        if (n >= ETH_HLEN && from.sll_pkttype != PACKET_OUTGOING && ((buf[12] << 8) | buf[13]) == et)
            return (size_t)n;
    }
    return 0;
}

int main(void)
{
    if (geteuid() != 0)
        return 77;
    sh("ip link del " A " 2>/dev/null");
    if (sh("ip link add " A " type veth peer name " B " 2>/dev/null") != 0)
        return 77;
    if (sh("ip link set " A " up && ip link set " B " up && sysctl -qw net.ipv6.conf." A ".disable_ipv6=1 "
               "net.ipv6.conf." B ".disable_ipv6=1 2>/dev/null") != 0) {
        sh("ip link del " A);
        return 77;
    }

    struct tap t;
    CHECK(tap_open(&t, A) == 0, "tap_open");
    const uint8_t station[6] = { 0x02, 0, 0, 0, 0, 0xaa }, other[6] = { 0x02, 0, 0, 0, 0, 0xbb },
                  foreign[6] = { 0x02, 0, 0, 0, 0, 0xcc }, bcast[6] = { 255, 255, 255, 255, 255, 255 };
    memcpy(t.mac, station, 6);
    CHECK(tap_start(&t, on_rx, NULL) == 0, "tap_start");
    int peer = peer_open();
    CHECK(peer >= 0, "peer socket");
    uint8_t f[100], rx[2048];

    /* disabled: nothing passes in either direction */
    frame(f, 100, bcast, other, TEST_ETYPE, 1);
    send(peer, f, 100, 0);
    usleep(200 * 1000);
    CHECK(rx_count() == 0, "disabled tap delivers nothing");
    frame(f, 100, bcast, station, TEST_ETYPE, 2);
    CHECK(!tap_send(&t, f, 100), "disabled tap sends nothing");

    atomic_store(&t.enabled, true);

    /* received frames reach the callback intact */
    frame(f, 100, bcast, other, TEST_ETYPE, 3);
    send(peer, f, 100, 0);
    CHECK(wait_count(1), "frame from the wire delivered");
    pthread_mutex_lock(&mu);
    CHECK(last_len == 100 && !memcmp(last, f, 100), "delivered unchanged");
    pthread_mutex_unlock(&mu);

    /* EAPOL belongs to the Pi's supplicant: never delivered */
    int before = rx_count();
    frame(f, 100, bcast, other, 0x888e, 4);
    send(peer, f, 100, 0);
    usleep(200 * 1000);
    CHECK(rx_count() == before && t.eapol_dropped >= 1, "EAPOL from the wire dropped");

    /* PC -> wire: our own source address is kept ... */
    frame(f, 100, bcast, station, TEST_ETYPE, 5);
    CHECK(tap_send(&t, f, 100), "send");
    size_t n = peer_recv(peer, TEST_ETYPE, rx, sizeof rx, 1000);
    CHECK(n == 100 && !memcmp(rx, f, 100) && t.sa_rewrites == 0, "frame sent as-is (%zu)", n);

    /* ... a foreign source address is rewritten to the station's */
    frame(f, 100, bcast, foreign, TEST_ETYPE, 6);
    CHECK(tap_send(&t, f, 100), "send foreign SA");
    n = peer_recv(peer, TEST_ETYPE, rx, sizeof rx, 1000);
    CHECK(n == 100 && !memcmp(rx + 6, station, 6) && t.sa_rewrites == 1, "SA rewritten to the station address");
    CHECK(rx[20] == 6, "payload untouched");

    /* the tap does not hear itself */
    usleep(200 * 1000);
    CHECK(rx_count() == before, "own transmissions are not looped back (got %d, want %d)", rx_count(), before);

    /* EAPOL from the PC is not put on the wire */
    frame(f, 100, bcast, station, 0x888e, 7);
    CHECK(!tap_send(&t, f, 100), "EAPOL from the PC refused");
    CHECK(peer_recv(peer, 0x888e, rx, sizeof rx, 300) == 0, "EAPOL never reached the wire");

    /* size limits */
    CHECK(!tap_send(&t, f, 10), "runt refused");
    uint8_t big[3000] = { 0 };
    CHECK(!tap_send(&t, big, sizeof big), "oversized refused");

    tap_close(&t);
    close(peer);
    sh("ip link del " A " 2>/dev/null");
    TEST_DONE("tap");
}
