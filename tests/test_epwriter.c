/* The IN-endpoint writer, exercised against a pipe: ordering, keep-alive, gating, shutdown. */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "check.h"
#include "ffs.h"
#include "log.h"

static size_t read_for(int fd, uint8_t *buf, size_t want, int ms)
{
    size_t got = 0;
    uint64_t end = now_ms() + (uint64_t)ms;
    while (got < want && now_ms() < end) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 10) <= 0)
            continue;
        ssize_t n = read(fd, buf + got, want - got);
        if (n > 0)
            got += (size_t)n;
    }
    return got;
}

int main(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    struct pktq q;
    CHECK(pktq_init(&q, 64) == 0, "queue");
    static const uint8_t ka[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
    atomic_bool active;
    atomic_init(&active, false);
    struct ep_writer w = { .fd = fds[1], .q = &q, .maxp = 64, .zlp = true, .name = "test",
                           .keepalive_ms = 5, .keepalive = ka, .keepalive_len = sizeof ka, .keepalive_active = &active };
    CHECK(ep_writer_start(&w) == 0, "writer start");

    /* inactive: no keep-alives */
    uint8_t buf[256];
    CHECK(read_for(fds[0], buf, 1, 150) == 0, "no keep-alive while the host is not enabled");

    /* data packets pass through in order, exactly as queued */
    pktq_put(&q, "\x01\x02\x03", 3);
    pktq_put(&q, "\x04\x05", 2);
    CHECK(read_for(fds[0], buf, 5, 500) == 5 && !memcmp(buf, "\x01\x02\x03\x04\x05", 5), "queued packets delivered in order");

    /* active: keep-alives flow, and real data still gets through in between */
    atomic_store(&active, true);
    size_t n = read_for(fds[0], buf, sizeof ka * 3, 500);
    CHECK(n == sizeof ka * 3, "keep-alives arrive when idle (%zu bytes)", n);
    for (size_t i = 0; i < n; i++)
        CHECK(buf[i] == 0xEE, "keep-alive content");
    pktq_put(&q, "\x07\x08", 2);
    size_t got = 0; bool found = false;
    for (int tries = 0; tries < 100 && !found; tries++) {
        got = read_for(fds[0], buf, 6, 50);
        for (size_t i = 0; i + 1 < got; i++)
            if (buf[i] == 7 && buf[i + 1] == 8)
                found = true;
    }
    CHECK(found, "real data is delivered between keep-alives");

    /* a packet that is a multiple of the packet size is followed by a zero-length write: on a pipe that is
     * invisible, but the data itself must be intact */
    uint8_t blk[64];
    memset(blk, 0x5a, sizeof blk);
    atomic_store(&active, false);
    usleep(30 * 1000);
    while (read_for(fds[0], buf, 1, 20) == 1)       /* drain leftover keep-alives */
        ;
    pktq_put(&q, blk, sizeof blk);
    CHECK(read_for(fds[0], buf, sizeof blk, 500) == sizeof blk && !memcmp(buf, blk, sizeof blk), "64-byte packet intact");

    /* gating works again, and closing the queue ends the thread */
    CHECK(read_for(fds[0], buf, 1, 100) == 0, "keep-alives stop when the host is disabled");
    pktq_close(&q);
    pthread_join(w.th, NULL);
    CHECK(w.wrote >= 5, "write counter (%lu)", w.wrote);

    TEST_DONE("epwriter");
}
