; isr.asm — 256 ISR stubs for x86_64
; Each stub pushes {vec, err} then calls isr_dispatch (C function via isr.c)

bits 64
section .text

extern isr_dispatch

; Common handler: all registers already saved by the stub macro
common_isr_stub_handler:
    ; Push all GP registers
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

    mov  rdi, rsp          ; first arg = pointer to cpu_regs_t
    call isr_dispatch

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

    add  rsp, 16           ; pop vec + err
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
