#include <base/process.h>
#include <core/exception.h>
#include <core/process.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stdint.h>

static bool core_exception_exit_code(enum core_exception_kind kind, uintptr_t* out_code) {
	if (!out_code) return false;
	switch (kind) {
	case CORE_EXCEPTION_ARITHMETIC_DIVIDE_BY_ZERO:
		*out_code = PROCESS_EXIT_ARITHMETIC_DIVIDE_BY_ZERO;
		return true;
	case CORE_EXCEPTION_ARITHMETIC_OVERFLOW:
		*out_code = PROCESS_EXIT_ARITHMETIC_OVERFLOW;
		return true;
	case CORE_EXCEPTION_ARITHMETIC_BOUND_RANGE:
		*out_code = PROCESS_EXIT_ARITHMETIC_BOUND_RANGE;
		return true;
	case CORE_EXCEPTION_INSTRUCTION_ILLEGAL:
		*out_code = PROCESS_EXIT_INSTRUCTION_ILLEGAL;
		return true;
	case CORE_EXCEPTION_PRIVILEGE_GENERAL_PROTECTION:
		*out_code = PROCESS_EXIT_PRIVILEGE_GENERAL_PROTECTION;
		return true;
	case CORE_EXCEPTION_ALIGNMENT:
		*out_code = PROCESS_EXIT_ALIGNMENT_FAULT;
		return true;
	case CORE_EXCEPTION_ACCESS_INSTRUCTION_ABORT:
		*out_code = PROCESS_EXIT_ACCESS_INSTRUCTION_ABORT;
		return true;
	case CORE_EXCEPTION_ACCESS_DATA_ABORT:
		*out_code = PROCESS_EXIT_ACCESS_DATA_ABORT;
		return true;
	case CORE_EXCEPTION_ACCESS_ADDRESS_ERROR_FETCH:
		*out_code = PROCESS_EXIT_ACCESS_ADDRESS_ERROR_FETCH;
		return true;
	case CORE_EXCEPTION_ACCESS_ADDRESS_ERROR_MEMORY:
		*out_code = PROCESS_EXIT_ACCESS_ADDRESS_ERROR_MEMORY;
		return true;
	case CORE_EXCEPTION_BUS_ERROR:
		*out_code = PROCESS_EXIT_BUS_ERROR;
		return true;
	case CORE_EXCEPTION_FLOATING_POINT:
		*out_code = PROCESS_EXIT_FLOATING_POINT_ERROR;
		return true;
	case CORE_EXCEPTION_FLOATING_POINT_SIMD:
		*out_code = PROCESS_EXIT_FLOATING_POINT_SIMD;
		return true;
	case CORE_EXCEPTION_MEMORY_PROTECTION:
		*out_code = PROCESS_EXIT_MEMORY_PROTECTION;
		return true;
	case CORE_EXCEPTION_BREAKPOINT:
		*out_code = PROCESS_EXIT_BREAKPOINT;
		return true;
	case CORE_EXCEPTION_DEBUG:
		*out_code = PROCESS_EXIT_DEBUG;
		return true;
	case CORE_EXCEPTION_WATCHPOINT:
		*out_code = PROCESS_EXIT_WATCHPOINT;
		return true;
	default:
		return false;
	}
}

bool core_handle_user_exception(enum core_exception_kind kind) {
	struct process* process;
	uintptr_t       exit_code;

	if (!core_exception_exit_code(kind, &exit_code)) return false;
	process = process_current();
	if (process == NULL) return false;
	return process_terminate(process, exit_code);
}

static enum vmm_fault_kind core_exception_to_vmm_kind(enum core_exception_kind kind) {
	switch (kind) {
	case CORE_EXCEPTION_PAGE_FAULT_NOT_PRESENT:
		return VMM_FAULT_NOT_PRESENT;
	case CORE_EXCEPTION_PAGE_FAULT_PROTECTION:
		return VMM_FAULT_PROTECTION;
	case CORE_EXCEPTION_PAGE_FAULT_INVALID:
	default:
		return VMM_FAULT_INVALID;
	}
}

static enum vmm_fault_access core_exception_to_vmm_access(enum core_exception_access access) {
	switch (access) {
	case CORE_EXCEPTION_ACCESS_READ:
		return VMM_FAULT_ACCESS_READ;
	case CORE_EXCEPTION_ACCESS_WRITE:
		return VMM_FAULT_ACCESS_WRITE;
	case CORE_EXCEPTION_ACCESS_EXEC:
		return VMM_FAULT_ACCESS_EXEC;
	case CORE_EXCEPTION_ACCESS_UNKNOWN:
	default:
		return VMM_FAULT_ACCESS_UNKNOWN;
	}
}

bool core_handle_exception(enum core_exception_kind kind, enum core_exception_access access, uintptr_t addr,
                           bool user_mode) {
	switch (kind) {
	case CORE_EXCEPTION_PAGE_FAULT_NOT_PRESENT:
	case CORE_EXCEPTION_PAGE_FAULT_PROTECTION:
	case CORE_EXCEPTION_PAGE_FAULT_INVALID:
		return vmm_handle_current_page_fault(
			addr, core_exception_to_vmm_kind(kind), core_exception_to_vmm_access(access), user_mode);
	default:
		if (!user_mode) return false;
		return core_handle_user_exception(kind);
	}
}
