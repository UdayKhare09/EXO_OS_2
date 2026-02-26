#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── MSR helpers ──────────────────────────────────────────────────────────── */
#define MSR_APIC_BASE       0x1B
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_GS_BASE         0xC0000101
#define MSR_KERN_GS_BASE    0xC0000102

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* ── CPUID ────────────────────────────────────────────────────────────────── */
static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline bool cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx >> 9) & 1;
}

/* ── Port I/O ─────────────────────────────────────────────────────────────── */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ── Memory barriers / fences ─────────────────────────────────────────────── */
static inline void cpu_halt(void)  { __asm__ volatile("hlt"); }
static inline void cpu_pause(void) { __asm__ volatile("pause"); }
static inline void cpu_cli(void)   { __asm__ volatile("cli"); }
static inline void cpu_sti(void)   { __asm__ volatile("sti"); }
static inline void cpu_mfence(void){ __asm__ volatile("mfence" ::: "memory"); }

/* ── LAPIC ID of current CPU ──────────────────────────────────────────────── */
static inline uint8_t cpu_lapic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (uint8_t)(ebx >> 24);
}

/* ── CR3 ──────────────────────────────────────────────────────────────────── */
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* ── TLB flush ────────────────────────────────────────────────────────────── */
static inline void invlpg(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ── FPU / SSE / AVX hardware init ─────────────────────────────────────── */
/*
 * Must be called on EVERY logical CPU (BSP + each AP) before any
 * floating-point or SIMD instruction is executed on that CPU.
 *
 * Sets up:
 *   CR0  — clears EM (disable FP emulation), sets MP, clears TS
 *   CR4  — sets OSFXSR, OSXMMEXCPT, OSXSAVE
 *   XCR0 — enables x87, SSE/XMM, AVX/YMM state components
 *   x87  — FNINIT (FCW=0x037F, all exceptions masked)
 *   SSE  — LDMXCSR 0x1F80 (all exceptions masked, round-to-nearest)
 */
static inline void cpu_enable_fpu(void) {
    uint64_t cr0, cr4;
    uint32_t eax, ebx, ecx, edx;

    /* CR0: clear EM (no emulation), set MP (monitor FPU), clear TS */
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   /* clear EM  */
    cr0 |=  (1ULL << 1);   /* set   MP  */
    cr0 &= ~(1ULL << 3);   /* clear TS  */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    /* Probe CPUID.1 for feature bits */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    bool has_fxsr    = (edx >> 24) & 1;  /* bit 24: FXSR      */
    bool has_sse     = (edx >> 25) & 1;  /* bit 25: SSE       */
    bool has_xsave   = (ecx >> 26) & 1;  /* bit 26: XSAVE     */
    bool has_avx     = (ecx >> 28) & 1;  /* bit 28: AVX       */

    /* CR4: conditionally enable OSFXSR, OSXMMEXCPT, OSXSAVE */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (has_fxsr)
        cr4 |= (1ULL <<  9);   /* OSFXSR     */
    if (has_sse)
        cr4 |= (1ULL << 10);   /* OSXMMEXCPT */
    if (has_xsave)
        cr4 |= (1ULL << 18);   /* OSXSAVE    */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    /* XCR0: enable state components (requires OSXSAVE set first) */
    if (has_xsave) {
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        xcr0_lo |= 0x3u;       /* x87 (bit0) + SSE/XMM (bit1) */
        if (has_avx)
            xcr0_lo |= 0x4u;   /* AVX/YMM (bit2) */
        __asm__ volatile("xsetbv" :: "c"(0), "a"(xcr0_lo), "d"(xcr0_hi));
    }

    /* Reset x87 FPU to initial state (FCW=0x037F) */
    __asm__ volatile("fninit");

    /* Set MXCSR: all SSE exceptions masked, round-to-nearest */
    if (has_sse) {
        static const uint32_t mxcsr_dflt = 0x1F80;
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr_dflt));
    }
}
