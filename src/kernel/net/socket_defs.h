/* net/socket_defs.h — POSIX-compatible socket type and struct definitions
 *
 * These match the Linux x86-64 ABI so mlibc sysdep headers can mirror them
 * directly.  All constants and struct layouts are intentionally identical to
 * the Linux kernel UAPI headers.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── Address families ────────────────────────────────────────────────────── */
#define AF_UNSPEC   0
#define AF_LOCAL    1
#define AF_UNIX     AF_LOCAL
#define AF_INET     2
#define AF_INET6    10

#define PF_UNSPEC   AF_UNSPEC
#define PF_INET     AF_INET
#define PF_INET6    AF_INET6

/* ── Socket types ────────────────────────────────────────────────────────── */
#define SOCK_STREAM    1    /* TCP */
#define SOCK_DGRAM     2    /* UDP */
#define SOCK_RAW       3    /* raw IP */
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

/* ── Protocol numbers ────────────────────────────────────────────────────── */
/* Also defined in netutil.h — keep in sync */
#ifndef IPPROTO_TCP
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#endif

/* ── Socket level for setsockopt/getsockopt ──────────────────────────────── */
#define SOL_SOCKET     1
#define SOL_IP         0
#define SOL_TCP        6
#define SOL_UDP        17

/* ── Socket options (SOL_SOCKET level) ───────────────────────────────────── */
#define SO_DEBUG       1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_LINGER      13
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30
#define SO_BINDTODEVICE 25

/* ── TCP options ─────────────────────────────────────────────────────────── */
#define TCP_NODELAY    1
#define TCP_MAXSEG     2
#define TCP_KEEPIDLE   4
#define TCP_KEEPINTVL  5
#define TCP_KEEPCNT    6

/* ── Shutdown how ────────────────────────────────────────────────────────── */
#define SHUT_RD    0
#define SHUT_WR    1
#define SHUT_RDWR  2

/* ── poll(2) event flags ─────────────────────────────────────────────────── */
#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM POLLOUT

/* ── ioctl network commands ──────────────────────────────────────────────── */
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFHWADDR  0x8927
#define SIOCSIFHWADDR  0x8924
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914

/* ── Generic socket address (Linux ABI) ──────────────────────────────────── */
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* ── IPv4 address ────────────────────────────────────────────────────────── */
struct in_addr {
    uint32_t s_addr;   /* network byte order */
};

/* ── IPv4 socket address ─────────────────────────────────────────────────── */
struct sockaddr_in {
    sa_family_t    sin_family;   /* AF_INET          */
    uint16_t       sin_port;     /* port, net order   */
    struct in_addr sin_addr;     /* IPv4 addr         */
    uint8_t        sin_zero[8];  /* padding to 16 B   */
};

/* ── socklen_t ───────────────────────────────────────────────────────────── */
typedef uint32_t socklen_t;

/* ── struct pollfd (POSIX) ───────────────────────────────────────────────── */
struct pollfd {
    int     fd;
    int16_t events;
    int16_t revents;
};

/* ── MSG flags for sendto / recvfrom ─────────────────────────────────────── */
#define MSG_DONTWAIT   0x40
#define MSG_NOSIGNAL   0x4000
#define MSG_PEEK       0x02
#define MSG_WAITALL    0x100

/* ── ifreq for ioctl ────────────────────────────────────────────────────── */
#define IFNAMSIZ  16

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_netmask;
        struct sockaddr ifr_hwaddr;
        int             ifr_flags;
        int             ifr_ifindex;
    };
};

/* ── Maximum backlog for listen() ────────────────────────────────────────── */
#define SOMAXCONN  128
