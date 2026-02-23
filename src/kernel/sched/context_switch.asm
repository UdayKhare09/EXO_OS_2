; context_switch.asm — x86_64 context switch and task trampoline
bits 64
section .text

; void task_switch(task_t **old_rsp_ptr, uint64_t new_rsp, uint64_t new_cr3)
;   RDI = &old->rsp   (store current RSP here)
;   RSI = new_task->rsp
;   RDX = new_task->cr3
global task_switch_asm
task_switch_asm:
    ; ── Save callee-saved registers of current task ─────────────────────
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP into old->rsp
    mov  [rdi], rsp

    ; Switch page tables if needed
    mov  rax, cr3
    cmp  rax, rdx
    je   .no_cr3
    mov  cr3, rdx
.no_cr3:
    ; Switch to new stack
    mov  rsp, rsi

    ; ── Restore callee-saved registers of new task ───────────────────────
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    ret          ; jumps to new task's saved RIP

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
