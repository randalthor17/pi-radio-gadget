#include <pthread.h>
#include <unistd.h>
#include "check.h"
#include "pktq.h"

static void *producer(void *arg)
{
    struct pktq *q = arg;
    for (int i = 0; i < 1000; i++) {
        uint8_t b = (uint8_t)i;
        while (!pktq_put(q, &b, 1))
            usleep(100);               /* the consumer will make room */
    }
    return NULL;
}

int main(void)
{
    struct pktq q;
    CHECK(pktq_init(&q, 4) == 0, "init");

    /* order preserved, contents copied */
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t d[2] = { i, (uint8_t)(i + 10) };
        CHECK(pktq_put(&q, d, 2), "put %u", i);
    }
    for (uint8_t i = 0; i < 3; i++) {
        struct pkt *p = pktq_get(&q, 0);
        CHECK(p && p->len == 2 && p->data[0] == i && p->data[1] == i + 10, "get %u in order", i);
        free(p);
    }
    CHECK(pktq_get(&q, 10) == NULL, "empty queue times out");

    /* full queue refuses and counts */
    for (int i = 0; i < 4; i++)
        CHECK(pktq_put(&q, "x", 1), "fill %d", i);
    CHECK(!pktq_put(&q, "x", 1), "5th put must fail");
    CHECK(q.dropped == 1, "drop counted (%lu)", q.dropped);

    /* flush empties it */
    pktq_flush(&q);
    CHECK(pktq_get(&q, 0) == NULL, "flush empties");
    CHECK(pktq_put(&q, "y", 1), "usable after flush");
    pktq_flush(&q);

    /* producer/consumer across threads keeps order */
    pthread_t t;
    pthread_create(&t, NULL, producer, &q);
    for (int i = 0; i < 1000; i++) {
        struct pkt *p = pktq_get(&q, 5000);
        CHECK(p && p->data[0] == (uint8_t)i, "threaded order at %d", i);
        free(p);
    }
    pthread_join(t, NULL);

    /* close wakes a blocked reader */
    pktq_close(&q);
    CHECK(pktq_get(&q, -1) == NULL, "closed + empty returns NULL");
    CHECK(!pktq_put(&q, "z", 1), "closed queue refuses puts");

    pktq_destroy(&q);
    TEST_DONE("pktq");
}
