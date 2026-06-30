#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/module.h>

#include "syscall.h"

syscall_status_t module_resolve(const char* name, size_t name_length, process_id_t target, cap_id_t* out_module_cap) {
	syscall_result_t result;

	if (out_module_cap == NULL || name == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	result = syscall(SYSCALL_MODULE_RESOLVE, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)target, 0u, 0u, 0u);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	*out_module_cap = (cap_id_t)result.value;
	return SYSCALL_STATUS_OK;
}

syscall_status_t module_map(cap_id_t module_cap, uintptr_t* out_mapped_base) {
	struct {
		uintptr_t base_out_ptr;
	} request;

	struct {
		uintptr_t mapped_base;
	} response;

	syscall_result_t result;

	request.base_out_ptr = (uintptr_t)out_mapped_base;
	result               = cap_call_syscall(module_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (out_mapped_base != NULL) *out_mapped_base = response.mapped_base;
	return SYSCALL_STATUS_OK;
}
