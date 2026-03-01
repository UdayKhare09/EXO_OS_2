#pragma once

#include <stdint.h>
#include <string.h>

#define TASK_MAX_GROUPS 32

/* Linux capability numbers (subset used by EXO_OS) */
#define CAP_CHOWN              0
#define CAP_DAC_OVERRIDE       1
#define CAP_DAC_READ_SEARCH    2
#define CAP_FOWNER             3
#define CAP_FSETID             4
#define CAP_KILL               5
#define CAP_SETGID             6
#define CAP_SETUID             7
#define CAP_SETPCAP            8
#define CAP_NET_BIND_SERVICE  10
#define CAP_NET_RAW           13
#define CAP_SYS_CHROOT        18
#define CAP_SYS_NICE          23
#define CAP_SYS_RESOURCE      24
#define CAP_SYS_ADMIN         21

#define CAP_LAST_CAP          40

#define SECBIT_NOROOT         0x00000001u

typedef struct cred {
    uint32_t uid;
    uint32_t euid;
    uint32_t suid;
    uint32_t fsuid;

    uint32_t gid;
    uint32_t egid;
    uint32_t sgid;
    uint32_t fsgid;

    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t cap_bounding;

    uint32_t securebits;

    uint32_t groups[TASK_MAX_GROUPS];
    uint32_t group_count;
} cred_t;

static inline void cred_init(cred_t *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->group_count = 1;
    c->groups[0] = 0;
    uint64_t full = (CAP_LAST_CAP >= 63) ? ~0ULL : ((1ULL << (CAP_LAST_CAP + 1)) - 1ULL);
    c->cap_effective = full;
    c->cap_permitted = full;
    c->cap_bounding = full;
}

static inline void cred_copy(cred_t *dst, const cred_t *src) {
    if (!dst || !src) return;
    *dst = *src;
}

static inline int cred_uid_eq(const cred_t *c, uint32_t uid) {
    return c && c->uid == uid;
}

static inline int cred_gid_eq(const cred_t *c, uint32_t gid) {
    return c && c->gid == gid;
}

static inline int capable(const cred_t *c, uint32_t cap) {
    if (!c || cap >= 64) return 0;
    return (c->cap_effective & (1ULL << cap)) != 0;
}

static inline int cap_permitted(const cred_t *c, uint32_t cap) {
    if (!c || cap >= 64) return 0;
    return (c->cap_permitted & (1ULL << cap)) != 0;
}