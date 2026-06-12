#include <base/display.h>
#include <stddef.h>
#include <stdio.h>

#include "syscall_private.h"

syscall_result_t syscall_nop(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                             uintptr_t arg5) {
	printf("syscall: nop called with args %p %p %p %p %p %p\n",
	       (void*)arg0,
	       (void*)arg1,
	       (void*)arg2,
	       (void*)arg3,
	       (void*)arg4,
	       (void*)arg5);

	return syscall_result_ok(0u);
}
