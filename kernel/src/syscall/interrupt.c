#include "interrupt.h"

#include <base/cap.h>
#include <base/interrupt.h>
#include <core/interrupt.h>
#include <core/process.h>
#include <core/signal.h>

#include "../capability/signal.h"

static syscall_result_t interrupt_result_to_syscall(enum interrupt_result result) {
	switch (result) {
	case INTERRUPT_OK:
		return syscall_result_ok(0u);
	case INTERRUPT_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case INTERRUPT_NOT_OWNER:
		return syscall_result_error(SYSCALL_STATUS_DENIED, (uintptr_t)result);
	case INTERRUPT_ALREADY_ATTACHED:
	case INTERRUPT_NOT_FOUND:
	case INTERRUPT_UNAVAILABLE:
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, (uintptr_t)result);
	case INTERRUPT_NO_MEMORY:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

syscall_result_t syscall_interrupt_attach(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                          uintptr_t arg4, uintptr_t arg5) {
	struct process*  process;
	struct signal*   signal;
	syscall_result_t result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;
	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = kernel_signal_retain_cap((cap_id_t)arg1, process_pid(process), CAP_SIGNAL, &signal);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = interrupt_result_to_syscall(interrupt_attach(process_pid(process), (interrupt_id_t)arg0, signal));
	signal_release(signal);
	return result;
}

syscall_result_t syscall_interrupt_detach(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                          uintptr_t arg4, uintptr_t arg5) {
	struct process* process;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;
	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return interrupt_result_to_syscall(interrupt_detach(process_pid(process), (interrupt_id_t)arg0));
}
