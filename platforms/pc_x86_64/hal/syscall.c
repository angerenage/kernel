#include <core/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

#define X86_RFLAGS_INTERRUPT_ENABLE (1ull << 9)
#define X86_RFLAGS_DIRECTION (1ull << 10)

extern void x86_64_syscall_entry(void);

void x86_64_syscall_init(void) {
	uint64_t star = ((uint64_t)X86_GDT_KERNEL_CODE_SELECTOR << 32) | ((uint64_t)X86_GDT_USER_COMPAT_SELECTOR << 48);

	write_msr(X86_IA32_STAR_MSR, star);
	write_msr(X86_IA32_LSTAR_MSR, (uint64_t)(uintptr_t)x86_64_syscall_entry);
	write_msr(X86_IA32_FMASK_MSR, X86_RFLAGS_INTERRUPT_ENABLE | X86_RFLAGS_DIRECTION);
	write_msr(X86_IA32_EFER_MSR, read_msr(X86_IA32_EFER_MSR) | X86_IA32_EFER_SCE);
}

bool x86_64_handle_syscall(struct interrupt_frame* frame) {
	if (frame->vector != X86_SYSCALL_VECTOR) return false;

	frame->rax = syscall_dispatch(frame->rax, frame->rdi, frame->rsi, frame->rdx, frame->rcx, frame->r8, frame->r9);
	return true;
}
