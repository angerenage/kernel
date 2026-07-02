#include "module.h"

#include <base/module.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/process.h>
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

	struct process* caller = process_current();
	if (caller == NULL) {
		free(name);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	if (arg2 == 0u) {
		free(name);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}

	size_t count = kernel_boot_module_count();
	for (size_t i = 0; i < count; i++) {
		const struct kernel_boot_module* module = kernel_boot_module_at(i);
		if (module == NULL) continue;

		if ((module->name != NULL && strcmp(module->name, name) == 0) ||
		    (module->path != NULL && strcmp(module->path, name) == 0)) {
			struct module_query_response response = {
				.id         = (module_id_t)i + 1u,
				.cap        = kernel_capability_boot_module_grant(i, process_pid(caller)),
				.size       = module->size,
				.media_type = module->media_type,
			};
			strlcpy(response.name, module->name != NULL ? module->name : "", sizeof(response.name));
			strlcpy(response.path, module->path != NULL ? module->path : "", sizeof(response.path));
			free(name);

			if (response.cap == CAP_ID_INVALID) {
				return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
			}

			syscall_result_t copy_result =
				syscall_copy_to_user(process_address_space(caller), arg2, &response, sizeof(response), 2u);
			if (copy_result.status != SYSCALL_STATUS_OK) {
				(void)cap_destroy_by_id(response.cap);
				return copy_result;
			}

			return syscall_result_ok(0u);
		}
	}

	free(name);
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0);
}
