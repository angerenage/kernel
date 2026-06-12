#include <base/module.h>
#include <base/syscall.h>
#include <base/vmm.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <kernel/boot.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <string.h>

#include "../../core/syscall/syscall_private.h"

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
			if (arg2 != 0) {
				struct module_info info = {
					.size = module->size,
				};

				size_t name_len = strlen(module->name);
				if (name_len >= sizeof(info.name)) {
					name_len = sizeof(info.name) - 1;
				}
				memcpy(info.name, module->name, name_len);
				info.name[name_len] = '\0';

				syscall_copy_out(2, arg2, &info, sizeof(info));
			}
			free(name);
			return syscall_result_ok(i);
		}
	}

	free(name);
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0);
}

syscall_result_t syscall_module_map(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	size_t index = (size_t)arg0;

	const struct kernel_boot_module* module = kernel_boot_module_at(index);
	if (module == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0);
	}

	struct thread* current_thread = sched_current_thread();
	if (current_thread == NULL || current_thread->address_space == NULL) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0);
	}
	struct address_space* space = current_thread->address_space;

	size_t page_count = (module->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

	void*    mapped_address = NULL;
	vmm_id_t mapping_id     = VMM_ID_INVALID;

	bool success = vmm_alloc_phys(space,
	                              (uintptr_t)module->address,
	                              page_count,
	                              VMM_PROT_READ | VMM_PROT_USER,
	                              NULL,
	                              &mapping_id,
	                              &mapped_address);

	if (!success) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0);
	}

	return syscall_result_ok((uintptr_t)mapped_address);
}
