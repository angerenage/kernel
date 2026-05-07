#include <syscall.h>

syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5) {
	register uintptr_t rax __asm__("rax") = number;
	register uintptr_t rdi __asm__("rdi") = arg0;
	register uintptr_t rsi __asm__("rsi") = arg1;
	register uintptr_t rdx __asm__("rdx") = arg2;
	register uintptr_t r10 __asm__("r10") = arg3;
	register uintptr_t r8 __asm__("r8")   = arg4;
	register uintptr_t r9 __asm__("r9")   = arg5;

	__asm__ volatile("syscall"
	                 : "+r"(rax), "+r"(rdx)
	                 : "r"(rdi), "r"(rsi), "r"(r10), "r"(r8), "r"(r9)
	                 : "rcx", "r11", "memory");
	return (syscall_result_t){
		.value  = rax,
		.status = (syscall_status_t)rdx,
	};
}
