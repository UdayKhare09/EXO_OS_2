#pragma once
/* syscall/syscall.h — INT 0x80 syscall interface (Linux x86-64 ABI) */
#include "arch/x86_64/idt.h"
#include <stdint.h>

/* Linux x86-64 syscall numbers (subset implemented) */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_STAT        4
#define SYS_FSTAT       5
#define SYS_LSTAT       6
#define SYS_LSEEK       8
#define SYS_BRK         12
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_EXIT        60
#define SYS_GETCWD      79
#define SYS_CHDIR       80
#define SYS_RENAME      82
#define SYS_MKDIR       83
#define SYS_RMDIR       84
#define SYS_UNLINK      87
#define SYS_GETDENTS64  217

/* Linux open flags */
#define SYS_O_RDONLY    0
#define SYS_O_WRONLY    1
#define SYS_O_RDWR      2
#define SYS_O_CREAT     0100      /* octal 0100 = 0x40 */
#define SYS_O_TRUNC     01000     /* octal 01000 = 0x200 */
#define SYS_O_APPEND    02000     /* octal 02000 = 0x400 */
#define SYS_O_DIRECTORY 0200000

/* Linux stat structure (x86-64 ABI) */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_ns;
    uint64_t st_mtime;
    uint64_t st_mtime_ns;
    uint64_t st_ctime;
    uint64_t st_ctime_ns;
    int64_t  __unused[3];
} linux_stat_t;

/* Linux getdents64 entry */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];  /* null-terminated */
} __attribute__((packed)) linux_dirent64_t;

void syscall_init(void);
