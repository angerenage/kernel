#include <core/address_transfer.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/vmm.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "syscall_private.h"

static bool syscall_vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0;
}

static bool syscall_vmm_kind_is_valid(enum vmm_kind kind) {
	switch (kind) {
	case VMM_KIND_GENERIC:
	case VMM_KIND_HEAP:
	case VMM_KIND_STACK:
		return true;
	case VMM_KIND_MMIO:
	case VMM_KIND_KERNEL_TEXT:
	case VMM_KIND_KERNEL_RODATA:
	case VMM_KIND_KERNEL_DATA:
	default:
		return false;
	}
}

static struct process* syscall_target_process(uintptr_t pid_arg) {
	if (pid_arg == 0u) return process_current();
	return process_lookup((process_id_t)pid_arg);
}

static struct address_space* syscall_target_address_space(uintptr_t pid_arg) {
	struct process* process = syscall_target_process(pid_arg);

	if (process == NULL) return NULL;
	return process_address_space(process);
}

static struct address_space* syscall_current_user_space(void) {
	struct thread* current = sched_current_thread();

	if (current == NULL || current->address_space == NULL || current->address_space == address_space_kernel()) {
		return NULL;
	}
	if (!address_space_is_initialized(current->address_space)) return NULL;
	return current->address_space;
}

syscall_result_t syscall_copy_out(uintptr_t ptr_arg_index, uintptr_t dst, const void* src, size_t size) {
	struct address_space*        caller_space;
	enum address_transfer_result transfer_result;

	if (dst == 0u || src == NULL || size == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, ptr_arg_index);

	caller_space = syscall_current_user_space();
	if (caller_space == NULL) {
		memcpy((void*)dst, src, size);
		return syscall_result_ok(0u);
	}

	transfer_result = address_space_copy_to(caller_space, dst, src, size);
	if (transfer_result == ADDRESS_TRANSFER_OK) return syscall_result_ok(0u);
	if (transfer_result == ADDRESS_TRANSFER_FAULT_FAILED) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, ptr_arg_index);
	}
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, ptr_arg_index);
}

static syscall_result_t syscall_result_from_address_transfer(enum address_transfer_result result, uintptr_t arg_index) {
	switch (result) {
	case ADDRESS_TRANSFER_OK:
		return syscall_result_ok(0u);
	case ADDRESS_TRANSFER_FAULT_FAILED:
		return syscall_result_error(SYSCALL_STATUS_FAILED, arg_index);
	case ADDRESS_TRANSFER_INVALID_ARGUMENTS:
	case ADDRESS_TRANSFER_ADDRESS_OVERFLOW:
	case ADDRESS_TRANSFER_NOT_MAPPED:
	case ADDRESS_TRANSFER_NOT_USER:
	case ADDRESS_TRANSFER_ACCESS_DENIED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);
	}
}

syscall_result_t syscall_vm_reserve(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct address_space*   space;
	struct vmm_alloc_params params;
	vmm_id_t                id   = VMM_ID_INVALID;
	void*                   base = NULL;
	size_t                  page_count;
	syscall_result_t        copy_result;

	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	page_count = (size_t)arg1;
	if ((uintptr_t)page_count != arg1) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (!syscall_vmm_prot_is_valid((vmm_prot_t)arg2)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (!syscall_vmm_kind_is_valid((enum vmm_kind)arg3)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if ((arg4 & ~((uintptr_t)VMM_MAP_LAZY)) != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 4u);

	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	params = (struct vmm_alloc_params){
		.page_count  = page_count,
		.align_pages = VMM_MIN_ALIGN_PAGES,
		.prot        = (vmm_prot_t)arg2,
		.kind        = (enum vmm_kind)arg3,
		.guard_pages = 0u,
		.map_flags   = (uint64_t)arg4,
	};
	if (!vmm_alloc(space, &params, &id, &base)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	if (arg5 != 0u) {
		uintptr_t base_value = (uintptr_t)base;

		copy_result = syscall_copy_out(5u, arg5, &base_value, sizeof(base_value));
		if (copy_result.status != SYSCALL_STATUS_OK) {
			(void)vmm_free(space, id);
			return copy_result;
		}
	}

	return syscall_result_ok((uintptr_t)id);
}

syscall_result_t syscall_vm_reserve_at(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	struct address_space*   space;
	struct vmm_alloc_params params;
	vmm_id_t                id = VMM_ID_INVALID;
	size_t                  page_count;

	if (arg1 == 0u || (arg1 & (PMM_PAGE_SIZE - 1u)) != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (arg2 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	page_count = (size_t)arg2;
	if ((uintptr_t)page_count != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (!syscall_vmm_prot_is_valid((vmm_prot_t)arg3)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if (!syscall_vmm_kind_is_valid((enum vmm_kind)arg4)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 4u);
	if ((arg5 & ~((uintptr_t)VMM_MAP_LAZY)) != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 5u);

	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	params = (struct vmm_alloc_params){
		.page_count  = page_count,
		.align_pages = VMM_MIN_ALIGN_PAGES,
		.prot        = (vmm_prot_t)arg3,
		.kind        = (enum vmm_kind)arg4,
		.guard_pages = 0u,
		.map_flags   = (uint64_t)arg5,
	};
	if (!vmm_alloc_at(space, (void*)arg1, &params, &id)) return syscall_result_error(SYSCALL_STATUS_FAILED, 1u);
	return syscall_result_ok((uintptr_t)id);
}

syscall_result_t syscall_vm_free(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                 uintptr_t arg5) {
	struct address_space* space;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg1 == (uintptr_t)VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_free(space, (vmm_id_t)arg1)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_vm_map(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                uintptr_t arg5) {
	struct address_space* space;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg1 == (uintptr_t)VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_map(space, (vmm_id_t)arg1)) return syscall_result_error(SYSCALL_STATUS_FAILED, 1u);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_vm_unmap(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct address_space* space;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg1 == (uintptr_t)VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_unmap(space, (vmm_id_t)arg1, arg2 != 0u)) return syscall_result_error(SYSCALL_STATUS_FAILED, 1u);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_vm_protect(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct address_space* space;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg1 == (uintptr_t)VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (!syscall_vmm_prot_is_valid((vmm_prot_t)arg2)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_protect(space, (vmm_id_t)arg1, (vmm_prot_t)arg2)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 1u);
	}
	return syscall_result_ok(0u);
}

syscall_result_t syscall_vm_query(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct address_space* space;
	struct vmm_info       info;
	syscall_result_t      copy_result;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg1 == (uintptr_t)VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (arg2 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	space = syscall_target_address_space(arg0);
	if (space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_query_id(space, (vmm_id_t)arg1, &info)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	copy_result = syscall_copy_out(2u, arg2, &info, sizeof(info));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	return syscall_result_ok(0u);
}

syscall_result_t syscall_vm_copy_from(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct address_space*        src_space;
	struct address_space*        dst_space;
	size_t                       size;
	enum address_transfer_result transfer_result;

	(void)arg4;
	(void)arg5;

	size = (size_t)arg3;
	if ((uintptr_t)size != arg3) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if (size == 0u) return syscall_result_ok(0u);

	src_space = syscall_target_address_space(arg0);
	if (src_space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	dst_space = syscall_current_user_space();
	if (dst_space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 2u);

	transfer_result = address_space_validate_range(
		src_space, arg1, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 1u);
	transfer_result = address_space_validate_range(
		dst_space, arg2, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 2u);
	transfer_result = address_space_copy_between(dst_space, arg2, src_space, arg1, size);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 3u);
	return syscall_result_ok((uintptr_t)size);
}

syscall_result_t syscall_vm_copy_to(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct address_space*        dst_space;
	struct address_space*        src_space;
	size_t                       size;
	enum address_transfer_result transfer_result;

	(void)arg4;
	(void)arg5;

	size = (size_t)arg3;
	if ((uintptr_t)size != arg3) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if (size == 0u) return syscall_result_ok(0u);

	dst_space = syscall_target_address_space(arg0);
	if (dst_space == NULL)
		return syscall_result_error(arg0 == 0u ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	src_space = syscall_current_user_space();
	if (src_space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 2u);

	transfer_result = address_space_validate_range(
		dst_space, arg1, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 1u);
	transfer_result = address_space_validate_range(
		src_space, arg2, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 2u);
	transfer_result = address_space_copy_between(dst_space, arg1, src_space, arg2, size);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 3u);
	return syscall_result_ok((uintptr_t)size);
}
