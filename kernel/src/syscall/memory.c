#include "memory.h"

#include <base/cap.h>
#include <base/memory.h>
#include <base/vmm.h>
#include <core/capability.h>
#include <core/syscall.h>

#include "../capability/memory.h"

static bool vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0u;
}

static bool user_vmm_prot_is_valid(vmm_prot_t prot) {
	return vmm_prot_is_valid(prot) && (prot & VMM_PROT_GLOBAL) == 0u;
}

syscall_result_t syscall_memory_allocate(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5) {
	cap_id_t     cap_id;
	cap_rights_t rights;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!user_vmm_prot_is_valid((vmm_prot_t)arg1)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	rights = CAP_CALL | CAP_MAP | CAP_DESTROY | CAP_READ | CAP_WRITE | CAP_DELEGATE;

	cap_id = kernel_allocate_memory(rights, (size_t)arg0, (vmm_prot_t)arg1);
	if (cap_id == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok((uintptr_t)cap_id);
}
