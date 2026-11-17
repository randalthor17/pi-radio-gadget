/* Bounded FIFO of byte packets, producer/consumer safe (mutex + condvar).
 *
 * Why it exists: a write() to a USB IN endpoint blocks until the *host* polls it.
 * One slow endpoint must never stall the thread reading the Bluetooth chip or the
 * other endpoint, so every IN endpoint gets its own queue and writer thread.
 */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pkt {
    size_t  len;
    uint8_t data[];
};

struct pktq {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    struct pkt    **ring;
    size_t          cap, head, count;
    bool            closed;
    unsigned long   dropped;      /* packets refused because the queue was full */
};

int         pktq_init(struct pktq *q, size_t cap);
void        pktq_destroy(struct pktq *q);
/* Copies data. Returns false (and counts a drop) if the queue is full or closed. */
bool        pktq_put(struct pktq *q, const void *data, size_t len);
/* Blocks up to timeout_ms (-1 = forever). NULL on timeout or when closed and empty.
 * The caller owns the result and must free() it. */
struct pkt *pktq_get(struct pktq *q, int timeout_ms);
bool        pktq_is_closed(struct pktq *q);
/* Discards everything queued. */
void        pktq_flush(struct pktq *q);
/* Wakes all waiters; pktq_get returns NULL once the queue is empty. */
void        pktq_close(struct pktq *q);
