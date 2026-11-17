#define _GNU_SOURCE
#include "tap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "log.h"

#define TAG "tap"
#define MAX_FRAME 2048

static bool is_eapol(const uint8_t *eth) { return eth[12] == 0x88 && eth[13] == 0x8e; }

int tap_open(struct tap *t, const char *ifname)
{
    memset(t, 0, sizeof *t);
    t->fd = -1;
    t->ifindex = (int)if_nametoindex(ifname);
    if (!t->ifindex)
        return -ENODEV;
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (fd < 0)
        return -errno;
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = t->ifindex };
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    t->fd = fd;
    atomic_init(&t->enabled, false);
    return 0;
}

static void *rx_main(void *arg) __attribute__((noreturn));
static void *rx_main(void *arg)
{
    struct tap *t = arg;
    uint8_t buf[MAX_FRAME];
    for (;;) {
        struct sockaddr_ll from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(t->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBADF)
                pthread_exit(NULL);
            /* ENETDOWN: the interface is briefly down (e.g. a MAC change); the socket recovers on its own. */
            logmsg(errno == ENETDOWN ? LOG_DBG : LOG_WARN, TAG, "recv: %s", strerror(errno));
            sleep_ms(100);
            continue;
        }
        if (n < ETH_HLEN || from.sll_pkttype == PACKET_OUTGOING || !atomic_load(&t->enabled))
            continue;
        if (is_eapol(buf)) {
            atomic_fetch_add(&t->eapol_dropped, 1);
            continue;
        }
        atomic_fetch_add(&t->rx_frames, 1);
        t->rx(buf, (size_t)n, t->ctx);
    }
}

int tap_start(struct tap *t, tap_rx_fn rx, void *ctx)
{
    t->rx = rx;
    t->ctx = ctx;
    return -pthread_create(&t->th, NULL, rx_main, t);
}

void tap_close(struct tap *t)
{
    atomic_store(&t->enabled, false);
    if (t->fd >= 0) {
        int fd = t->fd;
        t->fd = -1;
        close(fd);
    }
}

bool tap_send(struct tap *t, const uint8_t *eth, size_t len)
{
    uint8_t buf[MAX_FRAME];
    if (!atomic_load(&t->enabled) || len < ETH_HLEN || len > sizeof buf)
        return false;
    if (is_eapol(eth)) {
        atomic_fetch_add(&t->eapol_dropped, 1);
        return false;
    }
    memcpy(buf, eth, len);
    if (memcmp(buf + 6, t->mac, 6) != 0) {
        memcpy(buf + 6, t->mac, 6);
        atomic_fetch_add(&t->sa_rewrites, 1);
    }
    /* Back-pressure, not loss: when the interface queue is full (the radio is the bottleneck on upload), wait for room.
     * Blocking here stalls the USB bulk-OUT reader, so the PC's driver throttles itself instead of us dropping frames
     * and forcing TCP to back off. */
    for (int tries = 0;; tries++) {
        if (send(t->fd, buf, len, MSG_DONTWAIT) >= 0)
            break;
        if ((errno == ENOBUFS || errno == EAGAIN) && tries < 200) {
            struct pollfd p = { .fd = t->fd, .events = POLLOUT };
            poll(&p, 1, 5);
            if (tries > 20)
                sleep_ms(1);
            continue;
        }
        if (errno != ENOBUFS && errno != EAGAIN)
            LOGD(TAG, "send: %s", strerror(errno));
        atomic_fetch_add(&t->tx_dropped, 1);
        return false;
    }
    atomic_fetch_add(&t->tx_frames, 1);
    return true;
}
