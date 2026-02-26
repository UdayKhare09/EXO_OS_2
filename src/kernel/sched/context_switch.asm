; context_switch.asm — x86_64 context switch and task trampoline
bits 64
section .text

; void task_switch_asm(uint64_t *old_rsp_ptr, uint64_t new_rsp, uint64_t new_cr3,
;                      uint8_t  *old_fpu,     uint8_t  *new_fpu)
;   RDI = &old->rsp         (store current RSP here)
;   RSI = new->rsp
;   RDX = new->cr3
;   RCX = old->fpu_state    (64-byte-aligned 4 KiB XSAVE buffer, or NULL)
;   R8  = new->fpu_state    (64-byte-aligned 4 KiB XSAVE buffer, or NULL)
global task_switch_asm
task_switch_asm:
    ; ── Save callee-saved GP registers onto old task's stack ─────────────
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; ── XSAVE64: save old task's FPU / SSE / AVX state ───────────────────
    ; XSAVE64 uses EAX:EDX as component mask and clobbers them.
    ; Stash the pointer args we still need into callee-saved regs
    ; (already stack-saved above, so using them here is safe).
    mov  r13, rsi              ; new_rsp  (RSI preserved separately)
    mov  r14, rdx              ; new_cr3  (RDX will be zeroed by xor below)
    mov  r15, r8               ; new_fpu_state

    test rcx, rcx              ; old_fpu NULL? (not allocated yet = skip)
    jz   .skip_xsave
    mov  eax, 7                ; component mask: x87(1) + SSE(2) + AVX(4)
    xor  edx, edx
    xsave64 [rcx]              ; save old task FP/SIMD to old->fpu_state
.skip_xsave:

    ; Save old RSP (RDI still valid; xsave64 does not modify GP registers)
    mov  [rdi], rsp

    ; Switch to new task's stack
    mov  rsp, r13

    ; Switch page tables if needed (new_cr3 in r14; RDX was clobbered above)
    mov  rax, cr3
    cmp  rax, r14
    je   .no_cr3
    mov  cr3, r14
.no_cr3:

    ; ── XRSTOR64: restore new task's FPU / SSE / AVX state ───────────────
    test r15, r15              ; new_fpu NULL?
    jz   .skip_xrstor
    mov  eax, 7
    xor  edx, edx
    xrstor64 [r15]             ; restore new task FP/SIMD from new->fpu_state
.skip_xrstor:

    ; ── Restore callee-saved GP registers of new task ─────────────────────
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    ret                        ; jumps to new task's saved RIP

; ── Task trampoline ──────────────────────────────────────────────────────────
; Called as RIP for a freshly created task.
; On entry, the initial frame has:
;   rbx = task_entry_t  (entry function)
;   r12 = void *arg     (argument)
extern sched_task_exit
global task_trampoline
task_trampoline:
    mov  rdi, r12    ; arg passed as first argument
    call rbx         ; call entry(arg)

    ; If entry returns, mark the task dead and yield forever
    call sched_task_exit
.halt:
    cli
    hlt
    jmp  .halt
