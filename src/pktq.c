/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pktq.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int pktq_init(struct pktq *q, size_t cap)
{
    memset(q, 0, sizeof *q);
    q->ring = calloc(cap, sizeof *q->ring);
    if (!q->ring)
        return -ENOMEM;
    q->cap = cap;
    pthread_mutex_init(&q->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);     /* timeouts must not follow wall-clock jumps */
    pthread_cond_init(&q->cv, &ca);
    pthread_condattr_destroy(&ca);
    return 0;
}

static void flush_locked(struct pktq *q)
{
    while (q->count) {
        free(q->ring[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
}

void pktq_destroy(struct pktq *q)
{
    pthread_mutex_lock(&q->mu);
    flush_locked(q);
    pthread_mutex_unlock(&q->mu);
    free(q->ring);
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    memset(q, 0, sizeof *q);
}

bool pktq_put(struct pktq *q, const void *data, size_t len)
{
    struct pkt *p = malloc(sizeof *p + len);
    if (!p)
        return false;
    p->len = len;
    memcpy(p->data, data, len);

    pthread_mutex_lock(&q->mu);
    if (q->closed || q->count == q->cap) {
        q->dropped++;
        pthread_mutex_unlock(&q->mu);
        free(p);
        return false;
    }
    q->ring[(q->head + q->count) % q->cap] = p;
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return true;
}

struct pkt *pktq_get(struct pktq *q, int timeout_ms)
{
    struct timespec dl;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += timeout_ms / 1000;
        dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->cv, &q->mu);
        } else if (pthread_cond_timedwait(&q->cv, &q->mu, &dl) == ETIMEDOUT) {
            break;
        }
    }
    struct pkt *p = NULL;
    if (q->count) {
        p = q->ring[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return p;
}

void pktq_flush(struct pktq *q)
{
    pthread_mutex_lock(&q->mu);
    flush_locked(q);
    pthread_mutex_unlock(&q->mu);
}

void pktq_close(struct pktq *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

bool pktq_is_closed(struct pktq *q)
{
    pthread_mutex_lock(&q->mu);
    bool c = q->closed;
    pthread_mutex_unlock(&q->mu);
    return c;
}
