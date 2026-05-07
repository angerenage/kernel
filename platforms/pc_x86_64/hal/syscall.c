#include <core/cpu.h>
#include <core/syscall.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts_private.h"

#define X86_RFLAGS_INTERRUPT_ENABLE (1ull << 9)
#define X86_RFLAGS_DIRECTION (1ull << 10)
#define X86_CPU_KERNEL_ENTRY_STACK_TOP_OFFSET 40u
#define X86_CPU_SYSCALL_USER_STACK_OFFSET 48u

extern void x86_64_syscall_entry(void);

_Static_assert(offsetof(struct cpu, kernel_entry_stack_top) == X86_CPU_KERNEL_ENTRY_STACK_TOP_OFFSET,
               "x86_64 syscall asm kernel entry stack offset mismatch");
_Static_assert(offsetof(struct cpu, syscall_user_stack) == X86_CPU_SYSCALL_USER_STACK_OFFSET,
               "x86_64 syscall asm user stack offset mismatch");

void x86_64_syscall_init(void) {
	uint64_t star = ((uint64_t)X86_GDT_KERNEL_CODE_SELECTOR << 32) | ((uint64_t)X86_GDT_USER_COMPAT_SELECTOR << 48);

	write_msr(X86_IA32_STAR_MSR, star);
	write_msr(X86_IA32_LSTAR_MSR, (uint64_t)(uintptr_t)x86_64_syscall_entry);
	write_msr(X86_IA32_FMASK_MSR, X86_RFLAGS_INTERRUPT_ENABLE | X86_RFLAGS_DIRECTION);
	write_msr(X86_IA32_EFER_MSR, read_msr(X86_IA32_EFER_MSR) | X86_IA32_EFER_SCE);
}

bool x86_64_handle_syscall(struct interrupt_frame* frame) {
	syscall_result_t result;

	if (frame->vector != X86_SYSCALL_VECTOR) return false;

	result     = syscall_dispatch(frame->rax, frame->rdi, frame->rsi, frame->rdx, frame->rcx, frame->r8, frame->r9);
	frame->rax = result.value;
	frame->rdx = (uint64_t)result.status;
	return true;
}
