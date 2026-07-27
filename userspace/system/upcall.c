#include <base/syscall.h>
#include <system/upcall.h>

#include "syscall.h"

__attribute__((noreturn))
void upcall_return(void) {
	(void)syscall(SYSCALL_UPCALL_RETURN, 0u, 0u, 0u, 0u, 0u, 0u);
	__builtin_trap();
}
