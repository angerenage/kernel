#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/module.h>

#include "syscall.h"

bool module_resolve(const char* name, size_t name_length, process_id_t target, cap_id_t* out_module_cap) {
	syscall_result_t result;

	if (out_module_cap == NULL || name == NULL) return false;
	result = syscall(SYSCALL_MODULE_RESOLVE, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)target, 0u, 0u, 0u);
	if (result.status != SYSCALL_STATUS_OK) return false;
	*out_module_cap = (cap_id_t)result.value;
	return true;
}

bool module_map(cap_id_t module_cap, uintptr_t* out_mapped_base) {
	struct {
		uintptr_t base_out_ptr;
	} request;

	struct {
		uintptr_t mapped_base;
	} response;

	syscall_result_t result;

	request.base_out_ptr = (uintptr_t)out_mapped_base;
	result               = cap_call_syscall(module_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) return false;
	if (out_mapped_base != NULL) *out_mapped_base = response.mapped_base;
	return true;
}
