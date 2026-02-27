; isr.asm — 256 ISR stubs for x86_64
; Each stub pushes {vec, err} then calls isr_dispatch (C function via isr.c)

bits 64
section .text

extern isr_dispatch

; Common handler: full GP save + per-CPU XSAVE64 FPU protection
common_isr_stub_handler:
    ; ── swapgs if coming from ring 3 ─────────────────────────────────────
    ; After stub push: stack has [vec, err, RIP, CS, RFLAGS, RSP, SS]
    ; CS is at [rsp + 24] (vec=0, err=8, RIP=16, CS=24)
    test qword [rsp + 24], 3      ; check RPL bits of saved CS
    jz   .no_swapgs_entry
    swapgs                         ; user→kernel GS
.no_swapgs_entry:

    ; Push all GP registers (15 × 8 = 120 bytes).
    ; Stack alignment proof: CPU pushes 40 bytes (SS/RSP/RFLAGS/CS/RIP) +
    ; stub pushes 16 bytes (err+vec) = 56 bytes; 56 mod 16 = 8 (misaligned).
    ; After 15 GP pushes (120 bytes): 56+120=176, 176 mod 16 = 0. RSP is
    ; 16-byte aligned at this point — safe for XSAVE64 (requires 64-byte
    ; alignment of the *buffer*, not of RSP itself).
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ── Debug: trace CPU exceptions (vec < 32) via COM1 ──────────────────
    ; vec is at [rsp + 120] (15 GP regs × 8 = 120 bytes above current RSP)
    mov  rcx, [rsp + 120]      ; rcx = vector number
    cmp  rcx, 32
    jae  .no_fault_trace
    ; Emit 'F' then the vector as 2-digit hex on COM1
    push rax
    push rdx
    mov  dx, 0x3F8
    mov  al, 'F'
    out  dx, al
    ; high nibble
    mov  rax, rcx
    shr  al, 4
    add  al, '0'
    cmp  al, '9'
    jbe  .fhi_ok
    add  al, 7
.fhi_ok:
    out  dx, al
    ; low nibble
    mov  rax, rcx
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'
    jbe  .flo_ok
    add  al, 7
.flo_ok:
    out  dx, al
    mov  al, 10
    out  dx, al
    pop  rdx
    pop  rax
.no_fault_trace:

    ; ── Protect FPU / SSE / AVX across C ISR code ────────────────────────
    ; Per-CPU ISR XSAVE buffer is at offset 24 of cpu_info_t (GS base).
    ; Using XSAVE64 (component mask 0x7 = x87+SSE+AVX) so full SIMD state is
    ; preserved even if isr_dispatch C code emits XMM/YMM instructions.
    ; Skip gracefully if GS is not yet configured (interrupts fire before
    ; smp_init sets the GS base, e.g. from very early exceptions).
    mov  rcx, [gs:24]          ; rcx = cpu_info->isr_xsave_buf (page-aligned)
    test rcx, rcx
    jz   .call_dispatch
    mov  eax, 7                ; component mask: x87(1) | SSE(2) | AVX(4)
    xor  edx, edx
    xsave64 [rcx]

.call_dispatch:
    mov  rdi, rsp              ; arg0 = &cpu_regs_t (top of GP frame)
    call isr_dispatch

    ; ── Restore FPU / SSE / AVX state ────────────────────────────────────
    mov  rcx, [gs:24]
    test rcx, rcx
    jz   .restore_gp
    mov  eax, 7
    xor  edx, edx
    xrstor64 [rcx]

.restore_gp:
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16               ; discard vec + err code

    ; ── swapgs if returning to ring 3 ────────────────────────────────────
    ; CS is now at [rsp + 8] (RIP=0, CS=8 relative to current RSP)
    test qword [rsp + 8], 3    ; check RPL bits of saved CS
    jz   .no_swapgs_exit
    swapgs                     ; kernel→user GS
.no_swapgs_exit:
    iretq

; Macro: exceptions WITH error code already pushed by CPU (vecs 8,10-14,17,21,29,30)
%macro isr_err 1
isr_stub_%1:
    push qword %1
    jmp  common_isr_stub_handler
%endmacro

; Macro: no error code — push dummy 0
%macro isr_noerr 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp  common_isr_stub_handler
%endmacro

; Exceptions 0–31
isr_noerr  0   ; #DE Divide Error
isr_noerr  1   ; #DB Debug
isr_noerr  2   ; NMI
isr_noerr  3   ; #BP Breakpoint
isr_noerr  4   ; #OF Overflow
isr_noerr  5   ; #BR BOUND Range
isr_noerr  6   ; #UD Invalid Opcode
isr_noerr  7   ; #NM Device Not Available
isr_err    8   ; #DF Double Fault (error code = 0)
isr_noerr  9   ; Coprocessor Segment Overrun (legacy)
isr_err   10   ; #TS Invalid TSS
isr_err   11   ; #NP Segment Not Present
isr_err   12   ; #SS Stack-Segment Fault
isr_err   13   ; #GP General Protection
isr_err   14   ; #PF Page Fault
isr_noerr 15   ; Reserved
isr_noerr 16   ; #MF x87 FP Exception
isr_err   17   ; #AC Alignment Check
isr_noerr 18   ; #MC Machine Check
isr_noerr 19   ; #XF SIMD FP Exception
isr_noerr 20   ; #VE Virtualization Exception
isr_err   21   ; #CP Control Protection
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_err   29   ; #HV Hypervisor Injection
isr_err   30   ; #VC VMM Communication
isr_noerr 31   ; #SX Security Exception

; IRQs / APIC vectors 32–255
%assign i 32
%rep 224
    isr_noerr i
%assign i i+1
%endrep

; ── stub pointer table ────────────────────────────────────────────────────────
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
