/* net/arp.c — ARP implementation (RFC 826) */
#include "arp.h"
#include "ethernet.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"

/* ── ARP cache ───────────────────────────────────────────────────────────── */
static arp_entry_t g_arp_cache[ARP_CACHE_SIZE];
static volatile int g_arp_lock = 0;

static inline void arp_lock(void) {
    while (__atomic_test_and_set(&g_arp_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}
static inline void arp_unlock(void) {
    __atomic_clear(&g_arp_lock, __ATOMIC_RELEASE);
}

void arp_init(void) {
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    KLOG_INFO("arp: cache initialised (%d entries)\n", ARP_CACHE_SIZE);
}

/* ── Cache operations ────────────────────────────────────────────────────── */
static arp_entry_t *cache_lookup(uint32_t ip) {
    uint64_t now = sched_get_ticks();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            /* Check TTL */
            if (now - g_arp_cache[i].timestamp > ARP_CACHE_TTL_MS) {
                g_arp_cache[i].valid = false;
                return NULL;
            }
            return &g_arp_cache[i];
        }
    }
    return NULL;
}

static void cache_update(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    uint64_t now = sched_get_ticks();

    /* Update existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].ip == ip) {
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            g_arp_cache[i].timestamp = now;
            g_arp_cache[i].valid = true;
            return;
        }
    }

    /* Find empty or oldest entry */
    int best = 0;
    uint64_t oldest = ~0ULL;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp_cache[i].valid) { best = i; break; }
        if (g_arp_cache[i].timestamp < oldest) {
            oldest = g_arp_cache[i].timestamp;
            best = i;
        }
    }

    g_arp_cache[best].ip = ip;
    memcpy(g_arp_cache[best].mac, mac, ETH_ALEN);
    g_arp_cache[best].timestamp = now;
    g_arp_cache[best].valid = true;
}

/* ── Send ARP packet ─────────────────────────────────────────────────────── */
static int arp_send(netdev_t *dev, uint16_t opcode,
                    const uint8_t *target_mac, uint32_t target_ip) {
    skbuff_t *skb = skb_alloc(sizeof(arp_header_t));
    if (!skb) return -1;

    arp_header_t *arp = (arp_header_t *)skb_put(skb, sizeof(arp_header_t));
    arp->hw_type    = htons(0x0001);
    arp->proto_type = htons(ETH_P_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->opcode     = htons(opcode);
    memcpy(arp->sender_mac, dev->mac, ETH_ALEN);
    arp->sender_ip  = dev->ip_addr;
    memcpy(arp->target_mac, target_mac, ETH_ALEN);
    arp->target_ip  = target_ip;

    /* Determine dest MAC for ethernet frame */
    const uint8_t *dst_mac = (opcode == ARP_OP_REPLY) ? target_mac : ETH_BROADCAST;

    int ret = eth_tx(dev, skb, dst_mac, ETH_P_ARP);
    skb_free(skb);
    return ret;
}

/* ── RX handler ──────────────────────────────────────────────────────────── */
void arp_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < sizeof(arp_header_t)) {
        skb_free(skb);
        return;
    }

    arp_header_t *arp = (arp_header_t *)skb->data;

    /* Validate: Ethernet + IPv4 */
    if (ntohs(arp->hw_type) != 0x0001 || ntohs(arp->proto_type) != ETH_P_IP) {
        skb_free(skb);
        return;
    }

    /* Always update cache with sender's info (merge flag in RFC 826) */
    arp_lock();
    cache_update(arp->sender_ip, arp->sender_mac);
    arp_unlock();

    /* If it's a request for our IP, send a reply */
    uint16_t opcode = ntohs(arp->opcode);
    if (opcode == ARP_OP_REQUEST && arp->target_ip == dev->ip_addr) {
        KLOG_INFO("arp: request for our IP from %u.%u.%u.%u\n",
                  IP4_A(arp->sender_ip), IP4_B(arp->sender_ip),
                  IP4_C(arp->sender_ip), IP4_D(arp->sender_ip));
        arp_send(dev, ARP_OP_REPLY, arp->sender_mac, arp->sender_ip);
    }

    skb_free(skb);
}

/* ── Resolve IP → MAC ────────────────────────────────────────────────────── */
int arp_resolve(netdev_t *dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    /* Broadcast → use broadcast MAC */
    if (ip == IP4_BROADCAST || ip == 0) {
        memcpy(mac_out, ETH_BROADCAST, ETH_ALEN);
        return 0;
    }

    arp_lock();
    arp_entry_t *e = cache_lookup(ip);
    if (e) {
        memcpy(mac_out, e->mac, ETH_ALEN);
        arp_unlock();
        return 0;
    }
    arp_unlock();

    /* Not in cache — send ARP request */
    static const uint8_t zero_mac[ETH_ALEN] = {0};
    arp_send(dev, ARP_OP_REQUEST, zero_mac, ip);
    return -1;  /* caller should retry after short delay */
}

/* ── Gratuitous ARP ──────────────────────────────────────────────────────── */
void arp_gratuitous(netdev_t *dev) {
    if (dev->ip_addr == 0) return;
    KLOG_INFO("arp: gratuitous ARP for %u.%u.%u.%u\n",
              IP4_A(dev->ip_addr), IP4_B(dev->ip_addr),
              IP4_C(dev->ip_addr), IP4_D(dev->ip_addr));
    static const uint8_t zero_mac[ETH_ALEN] = {0};
    arp_send(dev, ARP_OP_REQUEST, zero_mac, dev->ip_addr);
}

/* ── Static ARP entry ────────────────────────────────────────────────────── */
void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    arp_lock();
    cache_update(ip, mac);
    arp_unlock();
}

/* ── Dump ARP table ──────────────────────────────────────────────────────── */
int arp_get_table(arp_entry_t *out, int max) {
    arp_lock();
    int count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE && count < max; i++) {
        if (g_arp_cache[i].valid) {
            out[count++] = g_arp_cache[i];
        }
    }
    arp_unlock();
    return count;
}
