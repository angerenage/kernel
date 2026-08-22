#include "memory.h"

#include <base/cap.h>
#include <core/syscall.h>

#include "../capability/memory.h"

syscall_result_t syscall_memory_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	const cap_rights_t rights = CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DELEGATE;
	cap_id_t           cap_id = kernel_memory_create(rights, (size_t)arg0);
	return cap_id == CAP_ID_INVALID ? syscall_result_error(SYSCALL_STATUS_FAILED, 0u)
	                                : syscall_result_ok((uintptr_t)cap_id);
}
