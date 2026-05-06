#include <syscall.h>

syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5) {
	register uintptr_t a0 __asm__("$a0") = arg0;
	register uintptr_t a1 __asm__("$a1") = arg1;
	register uintptr_t a2 __asm__("$a2") = arg2;
	register uintptr_t a3 __asm__("$a3") = arg3;
	register uintptr_t a4 __asm__("$a4") = arg4;
	register uintptr_t a5 __asm__("$a5") = arg5;
	register uintptr_t a7 __asm__("$a7") = number;

	__asm__ volatile("syscall 0" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
	return (syscall_result_t){
		.value  = a0,
		.status = (syscall_status_t)a1,
	};
}
