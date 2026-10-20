/*
HOST_ENTRY.S

The two ways into guest code (host.h).

host_enter_guest calls the guest function at window + function with the
window's base in x28, as the guest's code expects (tools/macos_asm_rebase.py).
The guest's code never writes x28 and uses x27 as scratch; both are the
host's callee-saved registers, so they are saved here and restored after.
The guest's own calls into the host keep them, as any callee does.

host_call_on_stack runs a host function on another stack: the main thread
runs the game on a stack in guest memory (host_run_guest_main), since the
guest's code keeps stack addresses in 32-bit registers.
*/

	.text

/* uint32_t host_enter_guest(uint8_t *window, uint32_t function,
	uint32_t a, uint32_t b, uint32_t c, uint32_t d) */
	.globl _host_enter_guest
	.p2align 2
_host_enter_guest:
	stp x29, x30, [sp, #-32]!
	mov x29, sp
	stp x27, x28, [sp, #16]
	mov x28, x0
	add x27, x28, w1, uxtw
	mov w0, w2
	mov w1, w3
	mov w2, w4
	mov w3, w5
	blr x27
	ldp x27, x28, [sp, #16]
	ldp x29, x30, [sp], #32
	ret

/* void *host_call_on_stack(void *(*function)(void *), void *argument,
	void *stack_top) */
	.globl _host_call_on_stack
	.p2align 2
_host_call_on_stack:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov x9, sp
	and x2, x2, #~15
	mov sp, x2
	/* the old stack pointer, on the new stack */
	stp x9, xzr, [sp, #-16]!
	mov x9, x0
	mov x0, x1
	blr x9
	ldr x9, [sp]
	mov sp, x9
	ldp x29, x30, [sp], #16
	ret
