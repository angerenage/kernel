#include <runtime/diagnostic.h>
#include <system/interrupt.h>

#include "syscall.h"

syscall_status_t interrupt_attach(interrupt_id_t id, cap_id_t signal_cap) {
	syscall_result_t result;

	if (id == INTERRUPT_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(id);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (signal_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(signal_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_INTERRUPT_ATTACH, (uintptr_t)id, (uintptr_t)signal_cap, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_INTERRUPT_ATTACH, result);
	return result.status;
}

syscall_status_t interrupt_detach(interrupt_id_t id) {
	syscall_result_t result;

	if (id == INTERRUPT_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(id);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_INTERRUPT_DETACH, (uintptr_t)id, 0u, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_INTERRUPT_DETACH, result);
	return result.status;
}
