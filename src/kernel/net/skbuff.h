/* net/skbuff.h — Network packet buffer (similar to Linux sk_buff)
 *
 * A skbuff_t wraps a contiguous buffer with head/data/tail/end pointers
 * to allow efficient header push (prepend) and pull (strip) operations
 * as a packet moves through the protocol stack layers.
 *
 *  ┌─── head ───┬─── data ───┬─── tail ───┬─── end ───┐
 *  │  headroom  │   payload  │  tailroom   │           │
 *  └────────────┴────────────┴─────────────┴───────────┘
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Forward-declare netdev */
struct netdev;

typedef struct skbuff {
    uint8_t        *head;       /* start of allocated buffer                 */
    uint8_t        *data;       /* start of packet data                      */
    uint8_t        *tail;       /* end of packet data                        */
    uint8_t        *end;        /* end of allocated buffer                   */
    size_t          len;        /* tail - data (current payload length)      */
    uint16_t        protocol;   /* EtherType / next-layer protocol           */
    struct netdev  *dev;        /* network device this packet came from/to   */
    struct skbuff  *next;       /* for linked-list queues                    */

    /* Transport-layer header pointer (set during demux) */
    uint8_t        *transport_hdr;
    /* Network-layer header pointer */
    uint8_t        *network_hdr;
    /* Link-layer header pointer */
    uint8_t        *mac_hdr;

    /* Source/dest addresses filled by lower layers for upper-layer use */
    uint32_t        src_ip;     /* network byte order */
    uint32_t        dst_ip;     /* network byte order */
    uint16_t        src_port;   /* network byte order */
    uint16_t        dst_port;   /* network byte order */
} skbuff_t;

/* Default headroom to reserve for protocol headers (eth + ip + tcp max) */
#define SKB_HEADROOM  (14 + 60 + 60)

/* ── Allocator ───────────────────────────────────────────────────────────── */

/* Allocate a skbuff with room for `size` bytes of payload + headroom.
 * data and tail both start at head + headroom. */
skbuff_t *skb_alloc(size_t size);

/* Free a skbuff and its buffer */
void skb_free(skbuff_t *skb);

/* ── Data pointer manipulation ───────────────────────────────────────────── */

/* Prepend `len` bytes of headroom into the data area (for adding headers).
 * Returns pointer to the new data start.  Caller writes the header there. */
void *skb_push(skbuff_t *skb, size_t len);

/* Remove `len` bytes from the front of data (strip a header).
 * Returns pointer to the new data start. */
void *skb_pull(skbuff_t *skb, size_t len);

/* Extend the tail by `len` bytes (add payload).
 * Returns pointer to the old tail (where new data should be written). */
void *skb_put(skbuff_t *skb, size_t len);

/* Reserve `len` bytes of headroom (move data and tail forward).
 * Must be called before any skb_put. */
void skb_reserve(skbuff_t *skb, size_t len);

/* ── Queue operations ────────────────────────────────────────────────────── */

typedef struct {
    skbuff_t *head;
    skbuff_t *tail;
    int       count;
    volatile int lock;
} skb_queue_t;

void      skb_queue_init(skb_queue_t *q);
void      skb_queue_push(skb_queue_t *q, skbuff_t *skb);
skbuff_t *skb_queue_pop(skb_queue_t *q);
int       skb_queue_empty(skb_queue_t *q);
void      skb_queue_purge(skb_queue_t *q);
