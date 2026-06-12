#include "module.h"

#include <base/syscall.h>
#include <core/capability.h>
#include <core/syscall.h>
#include <kernel/boot.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <string.h>

#include "../capability/boot_module.h"

syscall_result_t syscall_module_resolve(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	(void)arg3;
	(void)arg4;
	(void)arg5;

	char*            name   = NULL;
	syscall_result_t result = syscall_copy_string_arg(0, arg0, 1, arg1, &name);
	if (!syscall_status_is_success(result.status)) {
		return result;
	}

	size_t count = kernel_boot_module_count();
	for (size_t i = 0; i < count; i++) {
		const struct kernel_boot_module* module = kernel_boot_module_at(i);
		if (module == NULL) continue;

		if (strcmp(module->name, name) == 0 || strcmp(module->path, name) == 0) {
			cap_id_t cap_id = kernel_capability_boot_module_grant(i, (process_id_t)arg2);
			free(name);

			if (cap_id == CAP_ID_INVALID) {
				return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
			}

			return syscall_result_ok((uintptr_t)cap_id);
		}
	}

	free(name);
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0);
}
