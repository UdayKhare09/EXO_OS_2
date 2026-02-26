/* net/skbuff.c — Network packet buffer implementation */
#include "skbuff.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include <stddef.h>

/* ── Allocator ───────────────────────────────────────────────────────────── */
skbuff_t *skb_alloc(size_t size) {
    skbuff_t *skb = kzalloc(sizeof(skbuff_t));
    if (!skb) return NULL;

    size_t total = SKB_HEADROOM + size;
    uint8_t *buf = kmalloc(total);
    if (!buf) { kfree(skb); return NULL; }

    skb->head = buf;
    skb->data = buf + SKB_HEADROOM;
    skb->tail = skb->data;
    skb->end  = buf + total;
    skb->len  = 0;
    skb->next = NULL;
    return skb;
}

void skb_free(skbuff_t *skb) {
    if (!skb) return;
    if (skb->head) kfree(skb->head);
    kfree(skb);
}

/* ── Data pointer manipulation ───────────────────────────────────────────── */
void *skb_push(skbuff_t *skb, size_t len) {
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}

void *skb_pull(skbuff_t *skb, size_t len) {
    if (len > skb->len) len = skb->len;
    skb->data += len;
    skb->len  -= len;
    return skb->data;
}

void *skb_put(skbuff_t *skb, size_t len) {
    uint8_t *old_tail = skb->tail;
    skb->tail += len;
    skb->len  += len;
    return old_tail;
}

void skb_reserve(skbuff_t *skb, size_t len) {
    skb->data += len;
    skb->tail += len;
}

/* ── Queue operations ────────────────────────────────────────────────────── */
static inline void sq_lock(skb_queue_t *q) {
    while (__atomic_test_and_set(&q->lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}
static inline void sq_unlock(skb_queue_t *q) {
    __atomic_clear(&q->lock, __ATOMIC_RELEASE);
}

void skb_queue_init(skb_queue_t *q) {
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
    q->lock  = 0;
}

void skb_queue_push(skb_queue_t *q, skbuff_t *skb) {
    skb->next = NULL;
    sq_lock(q);
    if (q->tail) {
        q->tail->next = skb;
        q->tail = skb;
    } else {
        q->head = q->tail = skb;
    }
    q->count++;
    sq_unlock(q);
}

skbuff_t *skb_queue_pop(skb_queue_t *q) {
    sq_lock(q);
    skbuff_t *skb = q->head;
    if (skb) {
        q->head = skb->next;
        if (!q->head) q->tail = NULL;
        skb->next = NULL;
        q->count--;
    }
    sq_unlock(q);
    return skb;
}

int skb_queue_empty(skb_queue_t *q) {
    return q->head == NULL;
}

void skb_queue_purge(skb_queue_t *q) {
    skbuff_t *skb;
    while ((skb = skb_queue_pop(q)) != NULL)
        skb_free(skb);
}
