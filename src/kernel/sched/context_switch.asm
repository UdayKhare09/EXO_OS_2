; context_switch.asm — x86_64 context switch, task trampoline, user-mode entry
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

; ── Task trampoline (kernel tasks) ───────────────────────────────────────────
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

; ── User-mode trampoline ─────────────────────────────────────────────────────
; First-time entry into user-space via iretq.
; On entry (from init_frame_t popped by task_switch_asm):
;   rbx = user entry point (RIP for ring 3)
;   r12 = user stack top  (RSP for ring 3)
;
; We construct an iretq frame:  SS, RSP, RFLAGS, CS, RIP
; GDT_USER_DATA | 3 = 0x1B,  GDT_USER_CODE | 3 = 0x23
global user_mode_trampoline
user_mode_trampoline:
    ; Clear all GP registers to avoid leaking kernel data to user-space
    xor  rax, rax
    xor  rcx, rcx
    xor  rdx, rdx
    xor  rsi, rsi
    xor  rdi, rdi
    xor  rbp, rbp
    xor  r8,  r8
    xor  r9,  r9
    xor  r10, r10
    xor  r11, r11
    xor  r13, r13
    xor  r14, r14
    xor  r15, r15

    ; Build iretq frame on current (kernel) stack
    push qword 0x1B           ; SS  = GDT_USER_DATA | RPL=3
    push r12                   ; RSP = user stack
    push qword 0x202          ; RFLAGS = IF set (interrupts enabled)
    push qword 0x23           ; CS  = GDT_USER_CODE | RPL=3
    push rbx                   ; RIP = user entry point

    ; Clear the last two regs we used
    xor  rbx, rbx
    xor  r12, r12

    ; Swap GS: move kernel GS base into MSR_KERN_GS_BASE so the next
    ; swapgs (on SYSCALL/interrupt entry) restores it correctly.
    swapgs

    iretq

; ── Child return trampoline for fork()/clone() ─────────────────────────────
; Resumes a child task from a saved cpu_regs_t frame so the child observes
; the exact post-syscall CPU state (with RAX=0), matching Linux fork semantics.
; On entry:
;   r12 = pointer to cpu_regs_t frame on this task's kernel stack
global user_fork_return_trampoline
user_fork_return_trampoline:
    mov  rsp, r12

    ; Restore GP registers from cpu_regs_t order
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

    ; Skip vec + err, then iretq to saved user RIP/CS/RFLAGS/RSP/SS
    add  rsp, 16
    test qword [rsp + 8], 3
    jz   .fork_ret_kern
    swapgs
.fork_ret_kern:
    iretq

; ── SYSCALL entry point ──────────────────────────────────────────────────────
; Called via the SYSCALL instruction from user-space.
; On entry:  RCX = user RIP, R11 = user RFLAGS
;            RAX = syscall number, args in RDI RSI RDX R10 R8 R9
; The SYSCALL instruction does NOT switch RSP — we must do that manually.
; SWAPGS switches GS from user (0) to kernel (per-CPU info).
extern syscall_dispatch_fast
global syscall_entry
syscall_entry:
    swapgs                             ; GS now points to per-CPU cpu_info_t

    ; Save user RSP and load kernel RSP from TSS RSP0
    ; Per-CPU TSS RSP0 is at gdt_tables[cpu].tss... but we stored
    ; the kernel stack top at cpu_info->kernel_stack_top (offset 16).
    mov  [gs:32], rsp                  ; save user RSP in cpu_info scratch area
    mov  rsp, [gs:16]                  ; load kernel stack top

    ; Build a cpu_regs_t-compatible frame on the kernel stack:
    ;   iretq order: SS, RSP, RFLAGS, CS, RIP
    push qword 0x1B                    ; SS  (user data)
    push qword [gs:32]                 ; RSP (user, saved above)
    push r11                           ; RFLAGS (saved in R11 by SYSCALL)
    push qword 0x23                    ; CS  (user code)
    push rcx                           ; RIP (saved in RCX by SYSCALL)

    ; Error code + vector (for cpu_regs_t compat)
    push qword 0                       ; error code (none)
    push qword 0x80                    ; vector = 0x80 (syscall)

    ; Push GP registers (matches isr_dispatch order)
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

    ; Protect user FP/SIMD state across syscall C code. The kernel is built
    ; with SSE/AVX enabled, so without this user XMM/YMM registers can be
    ; clobbered by the syscall path.
    mov  rcx, [gs:24]                  ; cpu_info->isr_xsave_buf
    test rcx, rcx
    jz   .syscall_call_dispatch
    mov  eax, 7                        ; x87(1) | SSE(2) | AVX(4)
    xor  edx, edx
    xsave64 [rcx]

    ; Call C dispatcher:  void syscall_dispatch_fast(cpu_regs_t *regs)
.syscall_call_dispatch:
    mov  rdi, rsp
    call syscall_dispatch_fast

    ; Restore user FP/SIMD state saved above.
    mov  rcx, [gs:24]
    test rcx, rcx
    jz   .syscall_restore_gp
    mov  eax, 7
    xor  edx, edx
    xrstor64 [rcx]

    ; Restore GP registers
.syscall_restore_gp:
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
    pop  rax     ; return value is in RAX (set by dispatcher)

    ; Skip vec + err
    add  rsp, 16

    ; ── Return to user via iretq (more robust than sysret) ───────────────
    ; Stack now has: [RIP] [CS] [RFLAGS] [RSP] [SS] — standard iretq frame.
    ; swapgs if returning to ring 3 (check saved CS RPL).
    test qword [rsp + 8], 3        ; check saved CS RPL
    jz   .syscall_ret_kern
    swapgs
.syscall_ret_kern:
    iretq
