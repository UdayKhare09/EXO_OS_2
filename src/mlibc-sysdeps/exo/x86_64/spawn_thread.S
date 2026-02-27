.section .text

.global __exo_spawn_thread
.type __exo_spawn_thread, @function
__exo_spawn_thread:
	/* SysV args:
	 *   rdi = flags
	 *   rsi = child_stack
	 *   rdx = parent_tid_ptr
	 *   rcx = tls
	 * Linux clone syscall args:
	 *   rdi = flags, rsi = child_stack, rdx = parent_tid_ptr,
	 *   r10 = child_tid_ptr (unused), r8 = tls
	 */
	mov %rcx, %r8
	xor %r10d, %r10d
	mov $56, %rax
	syscall
	test %rax, %rax
	jnz 1f
	/* Child thread: stack contains [entry, user_arg]. */
	xor %ebp, %ebp
	pop %rdi
	pop %rsi
	call __mlibc_enter_thread
	hlt
1:
	ret

.size __exo_spawn_thread, .-__exo_spawn_thread
.section .note.GNU-stack,"",%progbits
