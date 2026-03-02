/* syscall/proc_syscalls.c — Process and memory management syscalls
 *
 * Implements fork, execve, wait4, mmap, munmap, mprotect, clone, futex.
 */
#include "syscall.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/waitq.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/pagecache.h"
#include "mm/kmalloc.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "fs/elf.h"
#include "ipc/signal.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "arch/x86_64/cpu.h"

#include <stdint.h>
#include <stddef.h>

/* ── Helper: add VMA to a task's sorted list ─────────────────────────────── */
void vma_insert(task_t *t, vma_t *v) {
    vma_t **pp = &t->vma_list;
    while (*pp && (*pp)->start < v->start)
        pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}

/* ── Helper: clone FD table (for fork) ────────────────────────────────────── */
static void fd_table_clone(task_t *child, task_t *parent) {
    for (int i = 0; i < TASK_FD_TABLE_SIZE; i++) {
        file_t *f = parent->fd_table[i];
        if (f) {
            file_get(f);           /* bump refcount */
            child->fd_table[i] = f;
        }
        child->fd_flags[i] = parent->fd_flags[i];
    }
    /* Copy cwd */
    strncpy(child->cwd, parent->cwd, TASK_CWD_MAX - 1);
    child->cwd[TASK_CWD_MAX - 1] = '\0';
}

/* ── Helper: clone VMA list (for fork) ────────────────────────────────────── */
static void vma_list_clone(task_t *child, task_t *parent) {
    vma_t *src = parent->vma_list;
    vma_t **dst = &child->vma_list;
    while (src) {
        vma_t *v = kmalloc(sizeof(vma_t));
        if (!v) break;
        *v = *src;
        if (v->file)
            vfs_vnode_get(v->file);
        v->next = NULL;
        *dst = v;
        dst = &v->next;
        src = src->next;
    }
}

static vma_t *vma_find_for_addr(task_t *t, uint64_t addr) {
    if (!t) return NULL;
    vma_t *v = t->vma_list;
    while (v) {
        if (addr >= v->start && addr < v->end)
            return v;
        v = v->next;
    }
    return NULL;
}

static int vma_overlaps_range(task_t *t, uint64_t start, uint64_t end) {
    if (!t || start >= end) return 0;
    for (vma_t *v = t->vma_list; v; v = v->next) {
        if (v->end <= start || v->start >= end)
            continue;
        return 1;
    }
    return 0;
}

static inline uint32_t vma_perm_from_prot(int prot) {
    uint32_t perms = 0;
    if (prot & PROT_READ)  perms |= VMA_READ;
    if (prot & PROT_WRITE) perms |= VMA_WRITE;
    if (prot & PROT_EXEC)  perms |= VMA_EXEC;
    return perms;
}

static void vma_set_perm_flags(vma_t *v, uint32_t perms) {
    if (!v) return;
    v->flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
    v->flags |= perms;
}

static void vma_update_file_window(vma_t *v,
                                   uint64_t orig_start,
                                   uint64_t orig_file_offset,
                                   uint64_t orig_file_size) {
    if (!v) return;
    if (!v->file) {
        v->file_offset = 0;
        v->file_size = 0;
        return;
    }

    uint64_t seg_off = v->start - orig_start;
    uint64_t seg_len = v->end - v->start;
    v->file_offset = orig_file_offset + seg_off;

    if (seg_off >= orig_file_size) {
        v->file_size = 0;
        return;
    }

    uint64_t remain = orig_file_size - seg_off;
    v->file_size = (remain < seg_len) ? remain : seg_len;
}

static int vma_apply_mprotect(task_t *t, uint64_t start, uint64_t end, uint32_t perms) {
    if (!t || start >= end) return -EINVAL;

    vma_t **pp = &t->vma_list;
    while (*pp) {
        vma_t *v = *pp;
        if (v->end <= start) {
            pp = &v->next;
            continue;
        }
        if (v->start >= end)
            break;

        uint64_t orig_start = v->start;
        uint64_t orig_end = v->end;
        uint64_t orig_file_offset = v->file_offset;
        uint64_t orig_file_size = v->file_size;
        uint32_t orig_flags = v->flags;
        vma_t *orig_next = v->next;

        uint64_t prot_start = (orig_start > start) ? orig_start : start;
        uint64_t prot_end = (orig_end < end) ? orig_end : end;

        bool need_left = orig_start < prot_start;
        bool need_right = prot_end < orig_end;

        vma_t *left = NULL;
        vma_t *right = NULL;

        if (need_left) {
            left = kmalloc(sizeof(vma_t));
            if (!left) return -ENOMEM;
            *left = *v;
            left->start = orig_start;
            left->end = prot_start;
            left->flags = orig_flags;
            left->next = NULL;
            if (left->file) vfs_vnode_get(left->file);
            vma_update_file_window(left, orig_start, orig_file_offset, orig_file_size);
        }

        if (need_right) {
            right = kmalloc(sizeof(vma_t));
            if (!right) {
                if (left) {
                    if (left->file) vfs_vnode_put(left->file);
                    kfree(left);
                }
                return -ENOMEM;
            }
            *right = *v;
            right->start = prot_end;
            right->end = orig_end;
            right->flags = orig_flags;
            right->next = orig_next;
            if (right->file) vfs_vnode_get(right->file);
            vma_update_file_window(right, orig_start, orig_file_offset, orig_file_size);
        }

        v->start = prot_start;
        v->end = prot_end;
        v->next = need_right ? right : orig_next;
        vma_set_perm_flags(v, perms);
        vma_update_file_window(v, orig_start, orig_file_offset, orig_file_size);

        if (need_left) {
            left->next = v;
            *pp = left;
        } else {
            *pp = v;
        }

        pp = need_right ? &right->next : &v->next;
    }

    return 0;
}

static void release_shared_vma_mappings(task_t *t, uintptr_t cr3) {
    if (!t || !cr3) return;
    for (vma_t *v = t->vma_list; v; v = v->next) {
        if (!(v->flags & VMA_FILE) || !(v->flags & VMA_SHARED) || !v->file)
            continue;

        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *pte = vmm_get_pte(cr3, a);
            if (!pte || !(*pte & VMM_PRESENT))
                continue;

            uint64_t within = a - v->start;
            if (within >= v->file_size)
                continue;

            pagecache_release(v->file, v->file_offset + within);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_fork — Create a child process via COW clone                          *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_fork(cpu_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent) return -ESRCH;

    /* Clone address space with COW */
    uintptr_t child_pml4 = vmm_clone_address_space(parent->cr3);
    if (!child_pml4) return -ENOMEM;

    /* Create the child task */
    task_t *child = task_create_user(parent->name, child_pml4,
                                      regs->rip, regs->rsp,
                                      parent->cpu_id);
    if (!child) {
        vmm_destroy_address_space(child_pml4);
        return -ENOMEM;
    }

    /* Set up parent-child relationship */
    child->ppid    = parent->pid;
    child->pgid    = parent->pgid;
    child->sid     = parent->sid;
    cred_copy(&child->cred, &parent->cred);
    child->umask   = parent->umask;
    /* fork() inherits exe_path: child /proc/<pid>/exe points at same binary
     * (Linux: mm->exe_file is shared via get_file() across fork). */
    memcpy(child->exe_path, parent->exe_path, TASK_CWD_MAX);
    child->parent  = parent;

    /* Add to parent's children list */
    child->child_next = parent->children;
    parent->children  = child;

    /* Clone VMAs and fd table */
    vma_list_clone(child, parent);
    fd_table_clone(child, parent);

    /* Copy memory management state */
    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;
    child->fs_base     = parent->fs_base;
    child->sig_mask    = parent->sig_mask;

    /* Rebuild child kernel stack so first schedule resumes from a copied
     * syscall frame (exact parent state, except child sees fork return 0). */
    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    uintptr_t child_kstack_top = vmm_phys_to_virt(child->stack_phys) + TASK_STACK_SIZE;
    child_kstack_top -= sizeof(cpu_regs_t);
    cpu_regs_t *child_regs = (cpu_regs_t *)child_kstack_top;
    *child_regs = *regs;
    child_regs->rax = 0;

    child_kstack_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)child_kstack_top;
    memset(frame, 0, sizeof(*frame));
    extern void user_fork_return_trampoline(void);
    frame->rip = (uint64_t)user_fork_return_trampoline;
    frame->r12 = (uint64_t)child_regs;

    child->rsp = child_kstack_top;

    /* Enqueue child for scheduling */
    sched_add_task(child, child->cpu_id);

    KLOG_INFO("fork: parent=%u child=%u\n", parent->pid, child->pid);
    return (int64_t)child->pid;  /* parent gets child PID */
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_execve — Replace process image with an ELF binary                    *
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Auxiliary vector entry types (subset needed by mlibc) */
#define AT_NULL          0
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_BASE          7
#define AT_ENTRY         9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_HWCAP        16   /* CPU feature flags (CPUID EAX=1 EDX)          */
#define AT_CLKTCK       17   /* frequency of times(2) clock; glibc expects 100 */
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_HWCAP2       26   /* extended CPU feature flags (CPUID EAX=7 ECX)  */
#define AT_EXECFN       31

/* Read CPU feature bits for the auxiliary vector. */
static void exec_cpuid_features(uint32_t *hwcap_out, uint32_t *hwcap2_out) {
    uint32_t eax, ebx, ecx, edx;
    /* HWCAP: CPUID leaf 1, EDX */
    eax = 1; ecx = 0;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(eax), "c"(ecx));
    *hwcap_out = edx;
    /* HWCAP2: CPUID leaf 7, sub-leaf 0, ECX (extended features) */
    eax = 7; ecx = 0;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(eax), "c"(ecx));
    *hwcap2_out = ecx;
}

static inline uint64_t exec_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Minimal user-memory copy helpers for execve argument marshaling. */
static int exec_copy_user_bytes(uintptr_t cr3, const void *user_src, void *dst, size_t len) {
    if (!user_src || !dst) return -EFAULT;

    uintptr_t up = (uintptr_t)user_src;
    uint8_t *out = (uint8_t *)dst;
    for (size_t i = 0; i < len; i++) {
        uint64_t *pte = vmm_get_pte(cr3, up + i);
        if (!pte || !(*pte & VMM_PRESENT))
            return -EFAULT;
        uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
        uintptr_t off = (up + i) & (PAGE_SIZE - 1);
        out[i] = *(uint8_t *)(vmm_phys_to_virt(phys) + off);
    }
    return 0;
}

static int exec_copy_user_cstr(uintptr_t cr3, const char *user_s,
                               char *dst, size_t dst_sz, size_t *out_len) {
    if (!user_s || !dst || dst_sz == 0) return -EFAULT;

    for (size_t i = 0; i < dst_sz; i++) {
        char c = 0;
        int rc = exec_copy_user_bytes(cr3, user_s + i, &c, 1);
        if (rc < 0) return rc;
        dst[i] = c;
        if (c == '\0') {
            if (out_len) *out_len = i + 1;
            return 0;
        }
    }
    return -ENAMETOOLONG;
}

/* Copy a user-space string array (argv or envp) into a flat kernel buffer.
 * Returns count of strings, or negative errno.
 * strs_out[] is filled with pointers INTO buf_out. */
static int copy_strings(uintptr_t cr3, char *const user_arr[], char *buf, size_t buf_sz,
                        const char **strs_out, int max_strs) {
    if (!user_arr) return 0;
    int count = 0;
    size_t off = 0;
    for (int i = 0; i < max_strs; i++) {
        const char *s = NULL;
        if (exec_copy_user_bytes(cr3, &user_arr[i], &s, sizeof(s)) < 0)
            return -EFAULT;
        if (!s) break;
        size_t len = 0;
        if (off >= buf_sz) return -E2BIG;
        int rc = exec_copy_user_cstr(cr3, s, buf + off, buf_sz - off, &len);
        if (rc < 0) return (rc == -ENAMETOOLONG) ? -E2BIG : rc;
        strs_out[count] = buf + off;
        off += len;
        count++;
    }
    return count;
}

int64_t sys_execve(const char *path, char *const argv[], char *const envp[]) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;
    uint32_t exec_mode = 0;
    uint32_t exec_uid = 0;
    uint32_t exec_gid = 0;
    int exec_secure = 0;

    /* Copy argv/envp into kernel buffers (max 256 strings, 64 KiB total) */
    #define EXEC_MAX_STRS  256
    #define EXEC_BUF_SIZE  (64 * 1024)
    char *argv_buf = kmalloc(EXEC_BUF_SIZE);
    char *envp_buf = kmalloc(EXEC_BUF_SIZE);
    const char *argv_ptrs[EXEC_MAX_STRS];
    const char *envp_ptrs[EXEC_MAX_STRS];
    if (!argv_buf || !envp_buf) {
        kfree(argv_buf); kfree(envp_buf);
        return -ENOMEM;
    }

    int argc = copy_strings(cur->cr3, argv, argv_buf, EXEC_BUF_SIZE, argv_ptrs, EXEC_MAX_STRS);
    int envc = copy_strings(cur->cr3, envp, envp_buf, EXEC_BUF_SIZE, envp_ptrs, EXEC_MAX_STRS);
    if (argc < 0) { kfree(argv_buf); kfree(envp_buf); return argc; }
    if (envc < 0) { kfree(argv_buf); kfree(envp_buf); return envc; }

    char kpath[VFS_MOUNT_PATH_MAX];
    char exec_path[VFS_MOUNT_PATH_MAX];
    int prc = exec_copy_user_cstr(cur->cr3, path, kpath, sizeof(kpath), NULL);
    if (prc < 0) {
        kfree(argv_buf); kfree(envp_buf);
        return (prc == -ENAMETOOLONG) ? -ENAMETOOLONG : -EFAULT;
    }

    if (kpath[0] == '/') {
        prc = path_normalize(kpath, exec_path);
    } else {
        prc = path_join(cur->cwd, kpath, exec_path);
    }
    if (prc < 0) {
        kfree(argv_buf); kfree(envp_buf);
        return prc;
    }

    /* Open the ELF file */
    int vfs_err = 0;
    vnode_t *vn = vfs_lookup(exec_path, true, &vfs_err);
    if (!vn) { kfree(argv_buf); kfree(envp_buf); return vfs_err ? vfs_err : -ENOENT; }
    exec_mode = vn->mode;
    exec_uid = vn->uid;
    exec_gid = vn->gid;

    /* Read ELF into a temporary kernel buffer */
    uint64_t size = vn->size;
    if (size == 0 || size > (64ULL * 1024 * 1024)) {
        kfree(argv_buf); kfree(envp_buf);
        return -ENOEXEC;
    }

    void *buf = kmalloc(size);
    if (!buf) { kfree(argv_buf); kfree(envp_buf); return -ENOMEM; }

    ssize_t rd = vn->ops->read(vn, buf, size, 0);
    vfs_vnode_put(vn);
    if (rd < 0 || (uint64_t)rd != size) {
        kfree(buf); kfree(argv_buf); kfree(envp_buf);
        return -EIO;
    }

    /* Create a FRESH address space for the new image.
     * This ensures no stale mappings from the old process image survive. */
    uintptr_t old_cr3 = cur->cr3;
    uint8_t old_owns_mm = cur->owns_address_space;
    uintptr_t new_cr3 = vmm_create_address_space();
    if (!new_cr3) {
        kfree(buf); kfree(argv_buf); kfree(envp_buf);
        return -ENOMEM;
    }

    /* Parse ELF and load segments into the new address space */
    elf_info_t info;
    int r = elf_load(buf, size, new_cr3, 0, &info);
    kfree(buf);
    if (r < 0) {
        vmm_destroy_address_space(new_cr3);
        kfree(argv_buf); kfree(envp_buf);
        return r;
    }

    uint64_t user_entry = info.entry;
    uint64_t at_base = 0;
    uint64_t interp_top = USER_MMAP_BASE;

    if (info.has_interp) {
        int ivfs_err = 0;
        vnode_t *ivn = vfs_lookup(info.interp_path, true, &ivfs_err);
        if (!ivn) {
            vmm_destroy_address_space(new_cr3);
            kfree(argv_buf); kfree(envp_buf);
            return ivfs_err ? ivfs_err : -ENOENT;
        }

        uint64_t isize = ivn->size;
        if (isize == 0 || isize > (64ULL * 1024 * 1024)) {
            vfs_vnode_put(ivn);
            vmm_destroy_address_space(new_cr3);
            kfree(argv_buf); kfree(envp_buf);
            return -ENOEXEC;
        }

        void *ibuf = kmalloc(isize);
        if (!ibuf) {
            vfs_vnode_put(ivn);
            vmm_destroy_address_space(new_cr3);
            kfree(argv_buf); kfree(envp_buf);
            return -ENOMEM;
        }

        ssize_t ird = ivn->ops->read(ivn, ibuf, isize, 0);
        vfs_vnode_put(ivn);
        if (ird < 0 || (uint64_t)ird != isize) {
            kfree(ibuf);
            vmm_destroy_address_space(new_cr3);
            kfree(argv_buf); kfree(envp_buf);
            return -EIO;
        }

        elf_info_t iinfo;
        uint64_t interp_bias = USER_MMAP_BASE;
        int ir = elf_load(ibuf, isize, new_cr3, interp_bias, &iinfo);
        kfree(ibuf);
        if (ir < 0) {
            vmm_destroy_address_space(new_cr3);
            kfree(argv_buf); kfree(envp_buf);
            return ir;
        }

        user_entry = iinfo.entry;
        at_base = iinfo.load_base;
        if (iinfo.brk_start > interp_top)
            interp_top = iinfo.brk_start;
    }

    /* Switch the task to the new address space and activate it in hardware.
     * Do this before destroying the old one so we always run with a valid CR3. */
    cur->cr3 = new_cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    /* Now safe to tear down the old address space (we are no longer using it) */
    if (old_owns_mm && old_cr3 && old_cr3 != vmm_get_kernel_pml4()) {
        release_shared_vma_mappings(cur, old_cr3);
        vmm_destroy_address_space(old_cr3);
    }
    cur->owns_address_space = 1;

    /* Reset mmap bump pointer */
    cur->mmap_next = USER_MMAP_BASE;
    if (interp_top > cur->mmap_next)
        cur->mmap_next = (interp_top + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    /* Close FD_CLOEXEC fds */
    fd_close_cloexec(cur);

    /* Auto-setup stdio if fd 0/1/2 are not open (e.g. first exec from kernel) */
    if (!fd_get(cur, 0) || !fd_get(cur, 1) || !fd_get(cur, 2)) {
        int vfs_err2 = 0;
        vnode_t *tty = vfs_lookup("/dev/tty", true, &vfs_err2);
        if (tty) {
            fd_setup_stdio(cur, tty);
            vfs_vnode_put(tty);
        }
    }

    /* Reset signal handlers to SIG_DFL */
    if (cur->sig_handlers) {
        for (int i = 0; i < NSIGS; i++)
            cur->sig_handlers[i] = SIG_DFL;
    }

    /* Update brk to end of loaded segments */
    cur->brk_base    = info.brk_start;
    cur->brk_current = info.brk_start;

    uint32_t old_euid = cur->cred.euid;

    /* Apply setuid/setgid credential transitions after successful image load.
     * no_new_privs suppresses privilege gain through exec. */
    if (!cur->no_new_privs) {
        if (exec_mode & VFS_S_ISUID) {
            cur->cred.euid = exec_uid;
            cur->cred.suid = exec_uid;
            cur->cred.fsuid = exec_uid;
            exec_secure = 1;
        }
        if (exec_mode & VFS_S_ISGID) {
            cur->cred.egid = exec_gid;
            cur->cred.sgid = exec_gid;
            cur->cred.fsgid = exec_gid;
            exec_secure = 1;
        }

        if ((exec_mode & VFS_S_ISUID) && exec_uid == 0 && !(cur->cred.securebits & SECBIT_NOROOT)) {
            cur->cred.cap_permitted = (cur->cred.cap_inheritable | cur->cred.cap_bounding) & cur->cred.cap_bounding;
            cur->cred.cap_effective = cur->cred.cap_permitted;
        }

        if (!(cur->cred.securebits & SECBIT_NOROOT)) {
            if (old_euid == 0 && cur->cred.euid != 0) {
                cur->cred.cap_effective = 0;
            } else if (old_euid != 0 && cur->cred.euid == 0) {
                uint64_t full = (CAP_LAST_CAP >= 63) ? ~0ULL : ((1ULL << (CAP_LAST_CAP + 1)) - 1ULL);
                cur->cred.cap_permitted = full & cur->cred.cap_bounding;
                cur->cred.cap_effective = cur->cred.cap_permitted;
            }
        }
    }

    /* Clear old VMAs and rebuild from ELF info */
    vma_t *v = cur->vma_list;
    while (v) {
        vma_t *n = v->next;
        if (v->file) vfs_vnode_put(v->file);
        kfree(v);
        v = n;
    }
    cur->vma_list = NULL;

    /* Set up user stack.
     * Pre-map 256 KiB at the top so env/argv/auxv writes succeed immediately.
     * The VMA covers the full 8 MiB; the demand-pager handles the rest. */
    uintptr_t user_stack_top = USER_STACK_TOP;
#define EXEC_STACK_PREMAP_PAGES 64u    /* 256 KiB pre-mapped at top */
#define EXEC_STACK_VMA_PAGES    2048u  /* 8 MiB VMA (matches Linux default) */
    for (uintptr_t i = 0; i < EXEC_STACK_PREMAP_PAGES; i++) {
        uintptr_t pg = pmm_alloc_pages(1);
        if (pg) {
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(cur->cr3,
                            user_stack_top - (EXEC_STACK_PREMAP_PAGES - i) * PAGE_SIZE,
                            pg, VMM_USER_RW);
        }
    }

    /* Add stack VMA covering the full 8 MiB so stack growth is demand-paged */
    vma_t *stack_vma = kmalloc(sizeof(vma_t));
    if (stack_vma) {
        stack_vma->start = user_stack_top - EXEC_STACK_VMA_PAGES * PAGE_SIZE;
        stack_vma->end   = user_stack_top;
        stack_vma->flags = VMA_READ | VMA_WRITE | VMA_USER | VMA_STACK;
        stack_vma->file = NULL;
        stack_vma->file_offset = 0;
        stack_vma->file_size = 0;
        stack_vma->mmap_flags = 0;
        stack_vma->next  = NULL;
        vma_insert(cur, stack_vma);
    }

    /* ── Build the Linux user-stack layout ────────────────────────────────
     * High address:
     *   string data (argv[0] chars, argv[1] chars, ..., envp[0] chars, ...)
     *   padding to 16 bytes
     *   auxv[] (pairs of uint64_t: type, value, terminated by AT_NULL,0)
     *   NULL   (envp terminator)
     *   envp[envc-1]  ...  envp[0]  (pointers into string data)
     *   NULL   (argv terminator)
     *   argv[argc-1]  ...  argv[0]  (pointers into string data)
     *   argc   (uint64_t)
     * Low address  ← RSP points here
     */
    uintptr_t sp = user_stack_top;

    /* Read CPU feature flags once for the auxv */
    uint32_t hwcap = 0, hwcap2 = 0;
    exec_cpuid_features(&hwcap, &hwcap2);
    uintptr_t argv_user[EXEC_MAX_STRS];
    uintptr_t envp_user[EXEC_MAX_STRS];
    uintptr_t execfn_user = 0;
    uintptr_t random_user = 0;

    {
        size_t len = strlen(exec_path) + 1;
        sp -= len;
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            memcpy((void *)(vmm_phys_to_virt(phys) + off), exec_path, len);
            execfn_user = sp;
        }
    }

    {
        sp -= 16;
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            uint8_t *dst = (uint8_t *)(vmm_phys_to_virt(phys) + off);
            uint64_t t0 = exec_rdtsc();
            uint64_t t1 = exec_rdtsc() ^ (uintptr_t)cur ^ (uintptr_t)sp;
            memcpy(dst, &t0, 8);
            memcpy(dst + 8, &t1, 8);
            random_user = sp;
        }
    }

    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp_ptrs[i]) + 1;
        sp -= len;
        /* Write to user stack via HHDM (we have the page mapped) */
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            memcpy((void *)(vmm_phys_to_virt(phys) + off), envp_ptrs[i], len);
        }
        envp_user[i] = sp;
    }

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv_ptrs[i]) + 1;
        sp -= len;
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            memcpy((void *)(vmm_phys_to_virt(phys) + off), argv_ptrs[i], len);
        }
        argv_user[i] = sp;
    }

    /* Save NUL-separated environ copy in task for /proc/<pid>/environ */
    {
        size_t etotal = 0;
        for (int i = 0; i < envc; i++) etotal += strlen(envp_ptrs[i]) + 1;
        if (cur->env_block) { kfree(cur->env_block); cur->env_block = NULL; cur->env_block_size = 0; }
        if (etotal > 0) {
            char *eb = kmalloc(etotal);
            if (eb) {
                size_t pos = 0;
                for (int i = 0; i < envc; i++) {
                    size_t l = strlen(envp_ptrs[i]) + 1;
                    memcpy(eb + pos, envp_ptrs[i], l);
                    pos += l;
                }
                cur->env_block = eb;
                cur->env_block_size = (uint32_t)etotal;
            }
        }
    }

    kfree(argv_buf);
    kfree(envp_buf);

    /* 2) Align stack pre-push so final user %rsp alignment is stable.
     * We push an odd/even number of 8-byte words depending on argc/envc;
     * compensate up-front so final %rsp is always 16-byte aligned. */
    {
        const uint64_t auxv_words = 34; /* 17 (type,value) pairs including AT_NULL */
        uint64_t total_push_words = auxv_words + (uint64_t)envc + (uint64_t)argc + 3;
        sp &= ~0xFULL;
        if (total_push_words & 1ULL)
            sp -= 8;
    }

    /* Helper: push a uint64_t to the user stack */
    #define PUSH_U64(val) do {                                              \
        sp -= 8;                                                            \
        uint64_t *_pte = vmm_get_pte(cur->cr3, sp);                        \
        if (_pte) {                                                         \
            uintptr_t _ph = (*_pte) & VMM_PTE_ADDR_MASK;                   \
            uintptr_t _of = sp & (PAGE_SIZE - 1);                          \
            *(uint64_t *)(vmm_phys_to_virt(_ph) + _of) = (uint64_t)(val);  \
        }                                                                   \
    } while(0)

    /* 3) Auxiliary vector */
    PUSH_U64(0);                     /* AT_NULL value */
    PUSH_U64(AT_NULL);               /* AT_NULL type  */
    PUSH_U64(exec_secure);           /* AT_SECURE     */
    PUSH_U64(AT_SECURE);
    PUSH_U64(cur->cred.egid);        /* AT_EGID       */
    PUSH_U64(AT_EGID);
    PUSH_U64(cur->cred.euid);        /* AT_EUID       */
    PUSH_U64(AT_EUID);
    PUSH_U64(cur->cred.gid);         /* AT_GID        */
    PUSH_U64(AT_GID);
    PUSH_U64(cur->cred.uid);         /* AT_UID        */
    PUSH_U64(AT_UID);
    PUSH_U64(execfn_user);           /* AT_EXECFN     */
    PUSH_U64(AT_EXECFN);
    PUSH_U64(random_user);           /* AT_RANDOM     */
    PUSH_U64(AT_RANDOM);
    PUSH_U64(at_base);               /* AT_BASE       */
    PUSH_U64(AT_BASE);
    PUSH_U64(info.entry);            /* AT_ENTRY      */
    PUSH_U64(AT_ENTRY);
    PUSH_U64(PAGE_SIZE);             /* AT_PAGESZ     */
    PUSH_U64(AT_PAGESZ);
    PUSH_U64(info.phdr_size);        /* AT_PHENT      */
    PUSH_U64(AT_PHENT);
    PUSH_U64(info.phdr_count);       /* AT_PHNUM      */
    PUSH_U64(AT_PHNUM);
    PUSH_U64(info.phdr_vaddr);       /* AT_PHDR       */
    PUSH_U64(AT_PHDR);
    PUSH_U64(hwcap2);                /* AT_HWCAP2     */
    PUSH_U64(AT_HWCAP2);
    PUSH_U64(100ULL);                /* AT_CLKTCK = 100 Hz (Linux default) */
    PUSH_U64(AT_CLKTCK);
    PUSH_U64(hwcap);                 /* AT_HWCAP      */
    PUSH_U64(AT_HWCAP);

    /* 4) envp array (NULL-terminated) */
    PUSH_U64(0);  /* NULL terminator */
    for (int i = envc - 1; i >= 0; i--)
        PUSH_U64(envp_user[i]);

    /* 5) argv array (NULL-terminated) */
    PUSH_U64(0);  /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--)
        PUSH_U64(argv_user[i]);

    /* 6) argc */
    PUSH_U64(argc);

    #undef PUSH_U64

    /* Do not realign after pushing argc; [RSP] must point at argc. */

    KLOG_INFO("execve: '%s' entry=%p at_entry=%p at_base=%p brk=%p argc=%d envc=%d sp=%p\n",
              exec_path, (void *)user_entry, (void *)info.entry, (void *)at_base,
              (void *)info.brk_start, argc, envc, (void*)sp);

    strncpy(cur->exe_path, exec_path, TASK_CWD_MAX - 1);
    cur->exe_path[TASK_CWD_MAX - 1] = '\0';

    const char *base = exec_path;
    for (const char *p = exec_path; *p; ++p)
        if (*p == '/') base = p + 1;
    if (!*base) base = exec_path;
    strncpy(cur->name, base, TASK_NAME_MAX - 1);
    cur->name[TASK_NAME_MAX - 1] = '\0';

    /* Jump to user-mode at the new entry point via user_mode_trampoline */
    extern void user_mode_trampoline(void);

    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    uintptr_t kstack_top = vmm_phys_to_virt(cur->stack_phys) + TASK_STACK_SIZE;
    kstack_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)kstack_top;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)user_mode_trampoline;
    frame->rbx = user_entry;         /* new user RIP */
    frame->r12 = sp;                 /* new user RSP (with argc/argv/auxv) */

    cur->rsp = kstack_top;

    /* Jump there NOW */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%rbx\n\t"
        "pop %%rbp\n\t"
        "ret\n\t"
        : : "r"(kstack_top) : "memory"
    );

    __builtin_unreachable();
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_wait4 — Wait for child process state change                          *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)rusage;
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* WNOHANG = 1 */
    int wnohang = options & 1;

retry:
    /* Search children */
    for (task_t *child = cur->children; child; child = child->child_next) {
        /* If pid > 0, wait for specific child */
        if (pid > 0 && child->pid != (uint32_t)pid)
            continue;
        /* pid == -1: wait for any child; pid == 0: same pgid; pid < -1: pgid==-pid */
        if (pid == 0 && child->pgid != cur->pgid)
            continue;
        if (pid < -1 && child->pgid != (uint32_t)(-pid))
            continue;

        if (child->state == TASK_ZOMBIE) {
            int status = child->exit_status;
            if (wstatus) *wstatus = status;

            int child_pid = (int)child->pid;

            /* Remove from children list */
            task_t **pp = &cur->children;
            while (*pp && *pp != child) pp = &(*pp)->child_next;
            if (*pp) *pp = child->child_next;

            /* Fully reclaim */
            task_destroy(child);
            return child_pid;
        }
    }

    /* No zombie children found */
    if (!cur->children)
        return -ECHILD;

    if (wnohang)
        return 0;

    /* Block and retry when woken by child exit */
    sched_block();
    goto retry;
}

/* Forward declaration used by MAP_FIXED replacement path in sys_mmap(). */
int64_t sys_munmap(uint64_t addr, uint64_t len);

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_mmap — Map memory into the process address space                     *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                 int fd, int64_t offset) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (len == 0) return -EINVAL;

    flags &= ~MAP_DENYWRITE; /* Linux-compat ignored flag */
    if ((flags & MAP_FIXED_NOREPLACE) && !(flags & MAP_FIXED))
        return -EINVAL;

    bool is_anon = (flags & MAP_ANONYMOUS) != 0;
    file_t *map_file = NULL;
    vnode_t *map_vnode = NULL;

    if (!is_anon) {
        if (!((flags & MAP_PRIVATE) || (flags & MAP_SHARED)))
            return -EINVAL;
        if ((flags & MAP_PRIVATE) && (flags & MAP_SHARED))
            return -EINVAL;
        if (offset < 0 || (offset & (PAGE_SIZE - 1)))
            return -EINVAL;

        map_file = fd_get(cur, fd);
        if (!map_file) return -EBADF;
        if ((map_file->flags & O_ACCMODE) == O_WRONLY) return -EACCES;

        map_vnode = map_file->vnode;

        /* ── Device/driver mmap (e.g. /dev/fb0) ─────────────────────────────
         * If the opened file has its own mmap handler, call it directly
         * instead of going through the regular file-backed page fault path.  */
        if (map_file->f_ops && map_file->f_ops->mmap) {
            uint64_t dlen  = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t daddr = addr ? (addr & ~(PAGE_SIZE - 1)) : cur->mmap_next;
            if (!addr) cur->mmap_next += dlen;
            int64_t r = map_file->f_ops->mmap(map_file, daddr, dlen,
                                               prot, flags, offset);
            if (r < 0) return r;
            /* Track VMA so that munmap can later release the mapping */
            vma_t *vd = kmalloc(sizeof(vma_t));
            if (vd) {
                vd->start = daddr; vd->end = daddr + dlen;
                vd->flags = VMA_USER | VMA_FILE | VMA_READ | VMA_SHARED;
                if (prot & PROT_WRITE) vd->flags |= VMA_WRITE;
                vd->file = map_vnode; vd->file_offset = (uint64_t)offset;
                vd->file_size = dlen; vd->mmap_flags = (uint32_t)flags;
                if (vd->file) vfs_vnode_get(vd->file);
                vma_insert(cur, vd);
            }
            return r;
        }

        if (!map_vnode || !map_vnode->ops || !map_vnode->ops->read)
            return -EINVAL;
        if (!VFS_S_ISREG(map_vnode->mode))
            return -EINVAL;
    }

    /* Page-align length */
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if ((flags & MAP_FIXED) && addr == 0)
        return -EINVAL;

    /* Choose address */
    uint64_t map_addr;
    uint64_t old_mmap_next = cur->mmap_next;
    if ((flags & MAP_FIXED) && addr) {
        map_addr = addr & ~(PAGE_SIZE - 1);
    } else {
        map_addr = cur->mmap_next;
        cur->mmap_next += len;
    }

    if ((flags & MAP_FIXED_NOREPLACE) &&
        vma_overlaps_range(cur, map_addr, map_addr + len)) {
        return -EEXIST;
    }

    /* MAP_FIXED replaces any previous mappings in the target range.
     * Without this, stale PTEs/VMAs can survive and cause protection faults. */
    if (flags & MAP_FIXED) {
        int64_t ur = sys_munmap(map_addr, len);
        if (ur < 0)
            return ur;
    }

    /* Convert prot to page flags */
    uint64_t pflags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) pflags |= VMM_WRITE;
    if (!(prot & PROT_EXEC)) pflags |= VMM_NX;

    uint64_t mapped_len = 0;

    if (is_anon) {
        /* Map anonymous pages eagerly */
        for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
            uintptr_t pg = pmm_alloc_pages(1);
            if (!pg) {
                if (!(flags & MAP_FIXED)) cur->mmap_next = old_mmap_next;
                for (uint64_t u = 0; u < mapped_len; u += PAGE_SIZE) {
                    uintptr_t phys = vmm_unmap_page_in(cur->cr3, map_addr + u);
                    if (phys) pmm_page_unref(phys);
                }
                return -ENOMEM;
            }
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(cur->cr3, map_addr + off, pg, pflags);
            mapped_len += PAGE_SIZE;
        }
    }

    /* Track VMA */
    vma_t *v = kmalloc(sizeof(vma_t));
    if (v) {
        v->start = map_addr;
        v->end   = map_addr + len;
        v->flags = VMA_USER;
        if (is_anon) v->flags |= VMA_ANON;
        else v->flags |= VMA_FILE;
        if (prot & PROT_READ)  v->flags |= VMA_READ;
        if (prot & PROT_WRITE) v->flags |= VMA_WRITE;
        if (prot & PROT_EXEC)  v->flags |= VMA_EXEC;
        if (flags & MAP_SHARED) v->flags |= VMA_SHARED;
        v->file = is_anon ? NULL : map_vnode;
        v->file_offset = (uint64_t)offset;
        v->file_size = (v->file && v->file->size > (uint64_t)offset)
                     ? (v->file->size - (uint64_t)offset)
                     : 0;
        if (v->file_size > len) v->file_size = len;
        v->mmap_flags = (uint32_t)flags;
        if (v->file) vfs_vnode_get(v->file);
        vma_insert(cur, v);
    }

    return (int64_t)map_addr;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_munmap — Unmap memory                                                *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_munmap(uint64_t addr, uint64_t len) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Unmap pages and free physical memory */
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t page_addr = addr + off;
        vma_t *mv = vma_find_for_addr(cur, page_addr);

        uintptr_t phys = vmm_unmap_page_in(cur->cr3, page_addr);
        if (phys) pmm_page_unref(phys);

        if (mv && (mv->flags & VMA_FILE) && (mv->flags & VMA_SHARED) && mv->file) {
            uint64_t within = page_addr - mv->start;
            if (within < mv->file_size)
                pagecache_release(mv->file, mv->file_offset + within);
        }
    }

    /* Remove/split VMAs that overlap [addr, addr+len) */
    vma_t **pp = &cur->vma_list;
    while (*pp) {
        vma_t *v = *pp;
        uint64_t unmap_start = addr;
        uint64_t unmap_end   = addr + len;

        /* No overlap → skip */
        if (v->end <= unmap_start || v->start >= unmap_end) {
            pp = &v->next;
            continue;
        }

        /* Fully contained → remove */
        if (v->start >= unmap_start && v->end <= unmap_end) {
            *pp = v->next;
            if (v->file) vfs_vnode_put(v->file);
            kfree(v);
            continue;
        }

        /* Partial overlap: split needed */
        if (v->start < unmap_start && v->end > unmap_end) {
            /* VMA spans both sides: split into two.
             * [v->start, unmap_start) and [unmap_end, v->end) */
            vma_t *right = kmalloc(sizeof(vma_t));
            if (right) {
                right->start = unmap_end;
                right->end   = v->end;
                right->flags = v->flags;
                right->file  = v->file;
                right->file_offset = v->file_offset + (unmap_end - v->start);
                right->file_size   = (v->end > unmap_end) ? (v->end - unmap_end) : 0;
                right->mmap_flags  = v->mmap_flags;
                if (right->file) vfs_vnode_get(right->file);
                right->next  = v->next;
                v->end  = unmap_start;
                if (v->file_size > (v->end - v->start))
                    v->file_size = v->end - v->start;
                v->next = right;
            }
            pp = &v->next;
            if (right) pp = &right->next;
            continue;
        }

        /* Overlap on left side: trim end */
        if (v->start < unmap_start) {
            v->end = unmap_start;
            if (v->file_size > (v->end - v->start))
                v->file_size = v->end - v->start;
            pp = &v->next;
            continue;
        }

        /* Overlap on right side: trim start */
        if (v->end > unmap_end) {
            if (v->file)
                v->file_offset += (unmap_end - v->start);
            v->start = unmap_end;
            if (v->file_size > (v->end - v->start))
                v->file_size = v->end - v->start;
            pp = &v->next;
            continue;
        }

        pp = &v->next;
    }

    /* Flush TLB */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_mprotect — Change page protections                                   *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if ((addr & (PAGE_SIZE - 1)) || len == 0)
        return -EINVAL;

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t end = addr + len;
    if (end < addr)
        return -EINVAL;

    bool have_vma_overlap = false;
    for (vma_t *v = cur->vma_list; v; v = v->next) {
        if (v->end <= addr || v->start >= end)
            continue;
        have_vma_overlap = true;
        break;
    }

    for (uint64_t a = addr; a < end; a += PAGE_SIZE) {
        if (vma_find_for_addr(cur, a))
            continue;
        uint64_t *pte = vmm_get_pte(cur->cr3, a);
        if (!pte || !(*pte & VMM_PRESENT))
            return -ENOMEM;
    }

    if (have_vma_overlap) {
        int vrc = vma_apply_mprotect(cur, addr, end, vma_perm_from_prot(prot));
        if (vrc < 0)
            return vrc;
    }

    uint64_t pflags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) pflags |= VMM_WRITE;
    if (!(prot & PROT_EXEC)) pflags |= VMM_NX;

    /* Update page table entries */
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t *pte = vmm_get_pte(cur->cr3, addr + off);
        if (pte && (*pte & VMM_PRESENT)) {
            uintptr_t phys = *pte & VMM_PTE_ADDR_MASK;
            *pte = phys | pflags;
        }
    }

    /* Flush TLB */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_clone — Create a new thread or process (Linux clone())               *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid_ptr,
                  uint64_t child_tid_ptr, uint64_t tls, cpu_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent || !regs) return -ESRCH;

    bool share_vm     = (flags & CLONE_VM) != 0;
    bool share_files  = (flags & CLONE_FILES) != 0;
    bool is_thread    = (flags & CLONE_THREAD) != 0;

    /* Thread groups must share an address space. Keep this strict check,
     * but tolerate runtimes that omit CLONE_SIGHAND while still expecting
     * Linux-like thread creation semantics. */
    if ((flags & CLONE_SIGHAND) && !share_vm) return -EINVAL;
    if (is_thread && !share_vm) return -EINVAL;

    uintptr_t child_pml4;
    task_t *child;

    if (share_vm) {
        /* Thread: share the parent's address space */
        child_pml4 = parent->cr3;
        child = task_create_user(parent->name, child_pml4,
                                 regs->rip, stack ? stack : regs->rsp,
                                 parent->cpu_id);
        if (!child) return -ENOMEM;
        child->owns_address_space = 0;
    } else {
        /* New process: COW clone */
        child_pml4 = vmm_clone_address_space(parent->cr3);
        if (!child_pml4) return -ENOMEM;
        child = task_create_user(parent->name, child_pml4,
                                 regs->rip, stack ? stack : regs->rsp,
                                 parent->cpu_id);
        if (!child) {
            vmm_destroy_address_space(child_pml4);
            return -ENOMEM;
        }
        child->owns_address_space = 1;
    }

    child->ppid   = parent->pid;
    child->pgid   = parent->pgid;
    child->sid    = parent->sid;
    cred_copy(&child->cred, &parent->cred);
    child->umask  = parent->umask;
    child->parent = parent;

    if (is_thread) {
        child->pid = parent->pid;
    }

    child->child_next = parent->children;
    parent->children  = child;

    if (share_files) {
        for (int i = 0; i < TASK_FD_TABLE_SIZE; i++) {
            file_t *f = parent->fd_table[i];
            if (f) { file_get(f); child->fd_table[i] = f; }
            child->fd_flags[i] = parent->fd_flags[i];
        }
        strncpy(child->cwd, parent->cwd, TASK_CWD_MAX - 1);
    } else {
        fd_table_clone(child, parent);
    }

    vma_list_clone(child, parent);

    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;

    if (tls) {
        child->fs_base = tls;
    } else {
        child->fs_base = parent->fs_base;
    }

    if (flags & 0x00200000) /* CLONE_CHILD_CLEARTID */
        child->clear_child_tid = (uint32_t *)child_tid_ptr;

    if ((flags & 0x00100000) && parent_tid_ptr) /* CLONE_PARENT_SETTID */
        *(uint32_t *)parent_tid_ptr = child->tid;
    if ((flags & 0x01000000) && child_tid_ptr)  /* CLONE_CHILD_SETTID */
        *(uint32_t *)child_tid_ptr = child->tid;

    /* Copy parent's register frame for child */
    uintptr_t kstack_child = vmm_phys_to_virt(child->stack_phys) + TASK_STACK_SIZE;
    kstack_child -= sizeof(cpu_regs_t);
    cpu_regs_t *child_regs = (cpu_regs_t *)kstack_child;
    *child_regs = *regs;
    child_regs->rax = 0;

    if (stack)
        child_regs->rsp = stack;

    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    extern void user_fork_return_trampoline(void);
    kstack_child -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)kstack_child;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)user_fork_return_trampoline;
    frame->r12 = (uint64_t)child_regs;

    child->rsp = kstack_child;

    int target_cpu = sched_pick_cpu();
    child->cpu_id = target_cpu;
    sched_add_task(child, target_cpu);

    KLOG_DEBUG("clone: child tid=%u flags=%llx\n", child->tid, flags);
    return (int64_t)child->tid;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_futex — Fast user-space mutex support                                *
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FUTEX_HASH_SIZE 64
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu

typedef struct futex_entry {
    uintptr_t  key;
    waitq_t    wq;
    struct futex_entry *next;
} futex_entry_t;

static futex_entry_t *futex_hash[FUTEX_HASH_SIZE];
static volatile int futex_lock_var = 0;

static void futex_lock_acquire(void) { while (__atomic_test_and_set(&futex_lock_var, __ATOMIC_ACQUIRE)) __asm__("pause"); }
static void futex_lock_release(void) { __atomic_clear(&futex_lock_var, __ATOMIC_RELEASE); }

static futex_entry_t *futex_find_or_create(uintptr_t cr3, uintptr_t uaddr) {
    uintptr_t bucket = (cr3 ^ uaddr) % FUTEX_HASH_SIZE;
    uintptr_t key = cr3 ^ uaddr;

    futex_entry_t *e = futex_hash[bucket];
    while (e) {
        if (e->key == key) return e;
        e = e->next;
    }

    e = kmalloc(sizeof(futex_entry_t));
    if (!e) return NULL;
    e->key = key;
    waitq_init(&e->wq);
    e->next = futex_hash[bucket];
    futex_hash[bucket] = e;
    return e;
}

static int copy_user_bytes(uintptr_t cr3, const void *user_src, void *dst, size_t len) {
    if (!user_src || !dst) return -EFAULT;

    uintptr_t up = (uintptr_t)user_src;
    uint8_t *out = (uint8_t *)dst;
    for (size_t i = 0; i < len; i++) {
        uint64_t *pte = vmm_get_pte(cr3, up + i);
        if (!pte || !(*pte & VMM_PRESENT)) return -EFAULT;
        uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
        uintptr_t off = (up + i) & (PAGE_SIZE - 1);
        out[i] = *(uint8_t *)(vmm_phys_to_virt(phys) + off);
    }
    return 0;
}

static int futex_parse_timeout_deadline(uintptr_t cr3,
                                        const kernel_timespec_t *timeout,
                                        uint64_t *deadline_out) {
    if (!timeout) {
        *deadline_out = 0;
        return 0;
    }

    kernel_timespec_t ts;
    int cr = copy_user_bytes(cr3, timeout, &ts, sizeof(ts));
    if (cr < 0) return cr;

    int64_t sec = ts.tv_sec;
    int64_t nsec = ts.tv_nsec;

    if (sec == 0 && nsec == 0) return -ETIMEDOUT;

    int valid_native = (sec >= 0 && nsec >= 0 && nsec < 1000000000LL);
    if (!valid_native) {
        int64_t sec32 = (int32_t)(sec & 0xFFFFFFFF);
        int64_t nsec32 = (int32_t)(nsec & 0xFFFFFFFF);
        if (sec32 == 0 && nsec32 == 0) return -ETIMEDOUT;
        if (!(sec32 >= 0 && nsec32 >= 0 && nsec32 < 1000000000LL))
            return -EINVAL;
        sec = sec32;
        nsec = nsec32;
    }

    uint64_t timeout_ms = (uint64_t)sec * 1000ULL + (uint64_t)(nsec / 1000000LL);
    if (timeout_ms == 0) return -ETIMEDOUT;
    *deadline_out = sched_get_ticks() + timeout_ms;
    return 0;
}

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
                  const kernel_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)uaddr2;
    if (!uaddr) return -EFAULT;

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    int futex_op = op & 0x7F;

    uint64_t timeout_deadline = 0;

    switch (futex_op) {
        case 0: { /* FUTEX_WAIT */
            int tr = futex_parse_timeout_deadline(cur->cr3, timeout, &timeout_deadline);
            if (tr < 0) return tr;
            futex_lock_acquire();
            if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
                futex_lock_release();
                return -EAGAIN;
            }
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return -ENOMEM;

            if (timeout) {
                while (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) == val) {
                    uint64_t now = sched_get_ticks();
                    if (now >= timeout_deadline) return -ETIMEDOUT;
                    sched_sleep(1);
                }
                return 0;
            }

            waitq_wait(&e->wq);
            return 0;
        }

        case 1: { /* FUTEX_WAKE */
            futex_lock_acquire();
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return 0;
            int woken = 0;
            for (uint32_t i = 0; i < val; i++) {
                waitq_wake_one(&e->wq);
                woken++;
            }
            return woken;
        }

        case FUTEX_WAIT_BITSET: {
            int tr = futex_parse_timeout_deadline(cur->cr3, timeout, &timeout_deadline);
            if (tr < 0) return tr;
            if (val3 == 0) return -EINVAL;
            futex_lock_acquire();
            if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
                futex_lock_release();
                return -EAGAIN;
            }
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return -ENOMEM;

            if (timeout) {
                while (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) == val) {
                    uint64_t now = sched_get_ticks();
                    if (now >= timeout_deadline) return -ETIMEDOUT;
                    sched_sleep(1);
                }
                return 0;
            }

            waitq_wait(&e->wq);
            return 0;
        }

        case FUTEX_WAKE_BITSET: {
            if (val3 == 0) return -EINVAL;
            futex_lock_acquire();
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return 0;
            if ((val3 & FUTEX_BITSET_MATCH_ANY) == 0) return 0;
            int woken = 0;
            for (uint32_t i = 0; i < val; i++) {
                waitq_wake_one(&e->wq);
                woken++;
            }
            return woken;
        }

        default:
            return -ENOSYS;
    }
}

int64_t sys_gettid(void) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;
    return (int64_t)cur->tid;
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    if (tgid <= 0 || tid <= 0) return -EINVAL;
    if (sig < 0 || sig >= NSIGS) return -EINVAL;

    task_t *target = task_lookup((uint32_t)tid);
    if (!target || target->state == TASK_DEAD) return -ESRCH;
    if ((int)target->pid != tgid) return -ESRCH;

    if (sig == 0) return 0; /* existence check */
    signal_send(target, sig);
    return 0;
}

/* tkill(tid, sig) — like tgkill but no tgid check */
int64_t sys_tkill(int tid, int sig) {
    if (tid <= 0) return -EINVAL;
    if (sig < 0 || sig >= NSIGS) return -EINVAL;

    task_t *target = task_lookup((uint32_t)tid);
    if (!target || target->state == TASK_DEAD) return -ESRCH;

    if (sig == 0) return 0; /* existence check */
    signal_send(target, sig);
    return 0;
}

int64_t sys_set_robust_list(uint64_t head, uint64_t len) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* Linux x86_64 robust_list_head is 24 bytes. */
    if (len != 24) return -EINVAL;

    cur->robust_list_head = head;
    cur->robust_list_len = len;
    return 0;
}

int64_t sys_get_robust_list(int pid, uint64_t *head_ptr, uint64_t *len_ptr) {
    if (!head_ptr || !len_ptr) return -EFAULT;

    task_t *target;
    if (pid == 0) {
        target = sched_current();
    } else {
        if (pid < 0) return -EINVAL;
        target = task_lookup((uint32_t)pid);
    }

    if (!target || target->state == TASK_DEAD) return -ESRCH;

    *head_ptr = target->robust_list_head;
    *len_ptr = target->robust_list_len;
    return 0;
}

/* ── sysinfo(2) ─────────────────────────────────────────────────────────────
 * Fills struct sysinfo with basic system statistics.                        */
typedef struct {
    int64_t  uptime;      /* seconds since boot        */
    uint64_t loads[3];    /* 1/5/15 min load avg       */
    uint64_t totalram;    /* total usable RAM (bytes)  */
    uint64_t freeram;     /* free RAM                  */
    uint64_t sharedram;   /* shared RAM                */
    uint64_t bufferram;   /* buffer RAM                */
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;       /* process count             */
    uint16_t pad[3];
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;    /* page size                 */
    char     _f[20];      /* pad to 64 bytes           */
} kernel_sysinfo_t;

int64_t sys_sysinfo(kernel_sysinfo_t *info) {
    if (!info) return -EINVAL;
    uint64_t now_ms = sched_get_ticks();
    info->uptime    = (int64_t)(now_ms / 1000ULL);
    info->loads[0]  = info->loads[1] = info->loads[2] = 0;
    info->totalram  = pmm_get_total_pages() * PAGE_SIZE;
    info->freeram   = pmm_get_free_pages()  * PAGE_SIZE;
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap  = 0;
    info->procs     = 0;        /* TODO: count active tasks */
    info->totalhigh = 0;
    info->freehigh  = 0;
    info->mem_unit  = (uint32_t)PAGE_SIZE;
    return 0;
}

/* ── sigaltstack(2) ─────────────────────────────────────────────────────────
 * Get/set the alternate signal stack for the current thread.               */
#define SS_ONSTACK  1
#define SS_DISABLE  4

typedef struct {
    void    *ss_sp;
    int      ss_flags;
    uint64_t ss_size;
} kernel_stack_t;

int64_t sys_sigaltstack(const kernel_stack_t *ss, kernel_stack_t *old_ss) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (old_ss) {
        old_ss->ss_sp    = (void *)(uintptr_t)cur->altstack_sp;
        old_ss->ss_flags = (int)cur->altstack_flags;
        old_ss->ss_size  = cur->altstack_size;
    }
    if (ss) {
        if (ss->ss_flags & ~(SS_ONSTACK | SS_DISABLE)) return -EINVAL;
        if (ss->ss_flags & SS_DISABLE) {
            cur->altstack_sp    = 0;
            cur->altstack_size  = 0;
            cur->altstack_flags = SS_DISABLE;
        } else {
            if (!ss->ss_sp || ss->ss_size < 2048) return -ENOMEM;
            cur->altstack_sp    = (uint64_t)(uintptr_t)ss->ss_sp;
            cur->altstack_size  = ss->ss_size;
            cur->altstack_flags = 0;
        }
    }
    return 0;
}

/* ── rt_sigsuspend(2) ───────────────────────────────────────────────────────
 * Temporarily replace the signal mask and suspend until a signal arrives.  */
int64_t sys_rt_sigsuspend(const uint64_t *mask, uint64_t sigsetsize) {
    (void)sigsetsize;
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    uint32_t old_mask = cur->sig_mask;
    if (mask) cur->sig_mask = (uint32_t)(*mask & 0xFFFFFFFFULL);

    /* Wait until a signal is pending that is not blocked by the new mask */
    while (1) {
        uint32_t pending = cur->sig_pending & ~cur->sig_mask;
        if (pending) break;
        sched_sleep(1);
    }

    cur->sig_mask = old_mask;
    return -EINTR; /* POSIX: sigsuspend always returns -EINTR */
}

/* ── clock_getres(2) ────────────────────────────────────────────────────────
 * Return the resolution of the given clock (we report 1 ms = 1,000,000 ns) */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#endif

int64_t sys_clock_getres(int clock_id, kernel_timespec_t *res) {
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
        return -EINVAL;
    if (res) {
        res->tv_sec  = 0;
        res->tv_nsec = 1000000; /* 1 ms resolution */
    }
    return 0;
}
