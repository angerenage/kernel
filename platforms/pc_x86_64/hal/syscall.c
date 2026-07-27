#include <core/cpu.h>
#include <core/exception.h>
#include <core/mm.h>
#include <core/syscall.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts_private.h"

#define X86_RFLAGS_CARRY (1ull << 0)
#define X86_RFLAGS_RESERVED_ONE (1ull << 1)
#define X86_RFLAGS_PARITY (1ull << 2)
#define X86_RFLAGS_AUXILIARY_CARRY (1ull << 4)
#define X86_RFLAGS_ZERO (1ull << 6)
#define X86_RFLAGS_SIGN (1ull << 7)
#define X86_RFLAGS_TRAP (1ull << 8)
#define X86_RFLAGS_INTERRUPT_ENABLE (1ull << 9)
#define X86_RFLAGS_DIRECTION (1ull << 10)
#define X86_RFLAGS_OVERFLOW (1ull << 11)
#define X86_RFLAGS_RESUME (1ull << 16)
#define X86_RFLAGS_ALIGNMENT_CHECK (1ull << 18)
#define X86_RFLAGS_ID (1ull << 21)

#define X86_USER_RFLAGS_ALLOWED                                                                                        \
	(X86_RFLAGS_CARRY | X86_RFLAGS_RESERVED_ONE | X86_RFLAGS_PARITY | X86_RFLAGS_AUXILIARY_CARRY | X86_RFLAGS_ZERO |   \
	 X86_RFLAGS_SIGN | X86_RFLAGS_TRAP | X86_RFLAGS_INTERRUPT_ENABLE | X86_RFLAGS_DIRECTION | X86_RFLAGS_OVERFLOW |    \
	 X86_RFLAGS_RESUME | X86_RFLAGS_ALIGNMENT_CHECK | X86_RFLAGS_ID)

#define X86_CPU_KERNEL_ENTRY_STACK_TOP_OFFSET 40u
#define X86_CPU_SYSCALL_USER_STACK_OFFSET 48u

extern void x86_64_syscall_entry(void);

_Static_assert(offsetof(struct interrupt_frame, rax) == 0u, "x86_64 frame rax offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rbx) == 8u, "x86_64 frame rbx offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rcx) == 16u, "x86_64 frame rcx offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rdx) == 24u, "x86_64 frame rdx offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rbp) == 32u, "x86_64 frame rbp offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rdi) == 40u, "x86_64 frame rdi offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rsi) == 48u, "x86_64 frame rsi offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r8) == 56u, "x86_64 frame r8 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r9) == 64u, "x86_64 frame r9 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r10) == 72u, "x86_64 frame r10 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r11) == 80u, "x86_64 frame r11 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r12) == 88u, "x86_64 frame r12 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r13) == 96u, "x86_64 frame r13 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r14) == 104u, "x86_64 frame r14 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, r15) == 112u, "x86_64 frame r15 offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, vector) == 120u, "x86_64 frame vector offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, error_code) == 128u, "x86_64 frame error offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rip) == 136u, "x86_64 frame rip offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, cs) == 144u, "x86_64 frame cs offset mismatch");
_Static_assert(offsetof(struct interrupt_frame, rflags) == 152u, "x86_64 frame rflags offset mismatch");
_Static_assert(sizeof(struct interrupt_frame) == 160u, "x86_64 interrupt frame size mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, rsp) == 160u, "x86_64 user frame rsp offset mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, ss) == 168u, "x86_64 user frame ss offset mismatch");
_Static_assert(sizeof(struct user_interrupt_frame) == 176u, "x86_64 user interrupt frame size mismatch");

_Static_assert(offsetof(struct cpu, kernel_entry_stack_top) == X86_CPU_KERNEL_ENTRY_STACK_TOP_OFFSET,
               "x86_64 syscall asm kernel entry stack offset mismatch");
_Static_assert(offsetof(struct cpu, syscall_user_stack) == X86_CPU_SYSCALL_USER_STACK_OFFSET,
               "x86_64 syscall asm user stack offset mismatch");

static bool x86_64_user_instruction_pointer(uint64_t address) {
	const uint64_t user_base = MM_USER_VMM_BASE;
	const uint64_t user_end  = MM_USER_VMM_BASE + MM_USER_VMM_SIZE;

	return address >= user_base && address < user_end;
}

static bool x86_64_user_stack_pointer(uint64_t address) {
	const uint64_t user_base = MM_USER_VMM_BASE;
	const uint64_t user_end  = MM_USER_VMM_BASE + MM_USER_VMM_SIZE;

	/* A stack pointer may legally point one byte beyond the last mapped stack byte. */
	return address >= user_base && address <= user_end;
}

static bool x86_64_user_rflags_valid(uint64_t rflags) {
	return (rflags & X86_RFLAGS_RESERVED_ONE) != 0u && (rflags & ~X86_USER_RFLAGS_ALLOWED) == 0u;
}

void x86_64_syscall_init(void) {
	uint64_t star = ((uint64_t)X86_GDT_KERNEL_CODE_SELECTOR << 32) | ((uint64_t)X86_GDT_USER_COMPAT_SELECTOR << 48);

	write_msr(X86_IA32_STAR_MSR, star);
	write_msr(X86_IA32_LSTAR_MSR, (uint64_t)(uintptr_t)x86_64_syscall_entry);
	write_msr(X86_IA32_FMASK_MSR, X86_RFLAGS_INTERRUPT_ENABLE | X86_RFLAGS_DIRECTION);
	write_msr(X86_IA32_EFER_MSR, read_msr(X86_IA32_EFER_MSR) | X86_IA32_EFER_SCE);
}

bool x86_64_handle_syscall(struct interrupt_frame* frame) {
	enum syscall_frame_action action;
	syscall_result_t          result;

	if (frame->vector != X86_SYSCALL_VECTOR) return false;

	action = syscall_dispatch_user_frame((struct hal_userspace_return_frame*)frame,
	                                     frame->rax,
	                                     frame->rdi,
	                                     frame->rsi,
	                                     frame->rdx,
	                                     frame->r10,
	                                     frame->r8,
	                                     frame->r9,
	                                     &result);
	if (action == SYSCALL_FRAME_RESTORED) return true;
	frame->rax = result.value;
	frame->rdx = (uint64_t)result.status;
	return true;
}

enum x86_user_return_kind x86_64_classify_user_return(const struct user_interrupt_frame* frame) {
	const uint64_t expected_cs = X86_GDT_USER_CODE_SELECTOR | 0x3u;
	const uint64_t expected_ss = X86_GDT_USER_DATA_SELECTOR | 0x3u;

	if (frame == NULL) return X86_USER_RETURN_INVALID;
	if (frame->frame.cs != expected_cs || frame->ss != expected_ss) return X86_USER_RETURN_INVALID;
	if (!x86_64_user_instruction_pointer(frame->frame.rip) || !x86_64_user_stack_pointer(frame->rsp)) {
		return X86_USER_RETURN_INVALID;
	}
	if (!x86_64_user_rflags_valid(frame->frame.rflags)) return X86_USER_RETURN_INVALID;

	/*
	 * SYSRETQ obtains RIP/RFLAGS from RCX/R11 and cannot independently restore
	 * those registers. IRETQ is required as soon as the architectural state
	 * represented by the frame differs from those SYSRET operands.
	 */
	if (frame->frame.rcx != frame->frame.rip || frame->frame.r11 != frame->frame.rflags) {
		return X86_USER_RETURN_IRET;
	}
	/* RF is restored precisely by IRETQ but is not used on the SYSRET fast path. */
	if ((frame->frame.rflags & X86_RFLAGS_RESUME) != 0u) return X86_USER_RETURN_IRET;

	return X86_USER_RETURN_SYSRET;
}

__attribute__((noreturn))
void x86_64_reject_user_return(void) {
	(void)core_handle_user_exception(CORE_EXCEPTION_PRIVILEGE_GENERAL_PROTECTION);
	hcf();
}
