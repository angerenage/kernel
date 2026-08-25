#include "memory.h"

#include <base/cap.h>
#include <base/memory.h>
#include <core/memory_object.h>
#include <core/syscall.h>

#include "../capability/memory.h"

syscall_result_t syscall_memory_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	struct memory_create_params params;
	struct address_space*       space;
	cap_rights_t                rights;
	cap_id_t                    cap_id;
	syscall_result_t            result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;
	if (arg0 == 0u || arg1 != sizeof(params)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	space  = syscall_current_user_space();
	result = syscall_copy_from_user(space, arg0, &params, sizeof(params), 0u);
	if (result.status != SYSCALL_STATUS_OK) return result;
	if (!memory_object_create_params_valid(&params)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	rights = CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE;
	if (params.memory_type == MEMORY_TYPE_NORMAL) rights |= CAP_EXEC;
	cap_id = kernel_memory_create(rights, &params);
	return cap_id == CAP_ID_INVALID ? syscall_result_error(SYSCALL_STATUS_FAILED, 0u)
	                                : syscall_result_ok((uintptr_t)cap_id);
}
