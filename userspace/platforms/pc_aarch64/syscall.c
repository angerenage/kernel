#include <system/syscall.h>

syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5) {
	register uintptr_t x0 __asm__("x0") = arg0;
	register uintptr_t x1 __asm__("x1") = arg1;
	register uintptr_t x2 __asm__("x2") = arg2;
	register uintptr_t x3 __asm__("x3") = arg3;
	register uintptr_t x4 __asm__("x4") = arg4;
	register uintptr_t x5 __asm__("x5") = arg5;
	register uintptr_t x8 __asm__("x8") = number;

	__asm__ volatile("svc #0" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
	return (syscall_result_t){
		.value  = x0,
		.status = (syscall_status_t)x1,
	};
}
