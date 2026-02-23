; trampoline.asm — 16-bit real-mode AP startup code
; Copied to physical 0x8000 by BSP before sending SIPIs.
;
; Mailbox layout at physical 0x8F00 (within same 4 KiB page):
;   +0x00 : uint64_t  cr3_val
;   +0x08 : uint64_t  ap_stack_top     (virtual)
;   +0x10 : uint64_t  ap_entry_virt    (virtual address of smp_ap_entry)
;   +0x18 : uint64_t  cpu_info_virt    (virtual address of cpu_info_t)
;   +0x20 : uint32_t  ap_ready_flag
;
; Note: mailbox is read in 32-bit PM mode (before paging) for cr3,
;       and again in 64-bit mode for virtual pointers.

bits 16
ORG 0x8000
section .trampoline
global trampoline_start
trampoline_start:
    cli
    cld
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax

    ; Load temporary GDT (includes both 32-bit and 64-bit code segments)
    lgdt [tmp_gdtr]

    ; Enter 32-bit protected mode
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:.pm32                ; selector 0x08 = 32-bit code segment

bits 32
.pm32:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  fs, ax
    mov  gs, ax

    ; Read CR3 from mailbox (physical 0x8F00) BEFORE enabling paging
    mov  eax, dword [0x8F00]    ; cr3 low 32 bits (kernel PML4 will be < 4 GB)

    ; Enable PAE
    mov  edx, cr4
    or   edx, (1 << 5)
    mov  cr4, edx

    ; Load CR3
    mov  cr3, eax

    ; Set LME + NXE in EFER (NXE needed because Limine's page tables use NX)
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, (1 << 8) | (1 << 11)   ; LME + NXE
    wrmsr

    ; Enable paging + PE
    mov  eax, cr0
    or   eax, (1 << 31) | 1
    mov  cr0, eax

    ; Far-jump to 64-bit code segment (selector 0x18)
    jmp  0x18:.lm64

bits 64
.lm64:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  ax, ax
    mov  fs, ax
    mov  gs, ax

    ; Now in 64-bit mode — read virtual pointers from mailbox
    ; The mailbox physical address 0x8F00 is identity-mapped
    mov  rsi, 0x8F00
    mov  rsp, qword [rsi + 0x08]   ; ap_stack_top
    mov  rdi, qword [rsi + 0x18]   ; cpu_info_virt  (arg 1)
    mov  rax, qword [rsi + 0x10]   ; ap_entry_virt

    ; Align stack to 16 bytes (ABI requirement)
    and  rsp, ~0xF

    call rax

.halt:
    cli
    hlt
    jmp  .halt

; Temporary GDT — 4 entries:
;   0x00 : null
;   0x08 : 32-bit code (D=1, L=0, G=1) — used for real→PM transition
;   0x10 : data         (D/B=1, G=1)
;   0x18 : 64-bit code  (D=0, L=1, G=1) — used for PM→LM transition
align 8
tmp_gdt:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00CF9A000000FFFF          ; 0x08 32-bit code (D=1, L=0)
    dq 0x00CF92000000FFFF          ; 0x10 data
    dq 0x00AF9A000000FFFF          ; 0x18 64-bit code (D=0, L=1)

tmp_gdtr:
    dw  (tmp_gdtr - tmp_gdt - 1)
    dd  tmp_gdt

global trampoline_end
trampoline_end:
