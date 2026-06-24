#include <base/cap.h>
#include <base/syscall.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct boot_module_map_request {
	uintptr_t base_out_ptr;
};

struct boot_module_map_response {
	uintptr_t mapped_base;
};

static struct cap_object** boot_module_objects;
static size_t              boot_module_object_count;

static struct address_space* caller_space(process_id_t caller_pid) {
	struct process* process = process_lookup(caller_pid);
	if (process == NULL) return NULL;
	return process_address_space(process);
}

static syscall_result_t copy_to_user(process_id_t caller_pid, uintptr_t user_ptr, const void* kernel_buf, size_t size) {
	struct address_space*        space;
	enum address_transfer_result result;

	if (user_ptr == 0u || kernel_buf == NULL || size == 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	space = caller_space(caller_pid);
	if (space == NULL) {
		memcpy((void*)user_ptr, kernel_buf, size);
		return syscall_result_ok(0u);
	}

	result = address_space_copy_to(space, user_ptr, kernel_buf, size);
	if (result == ADDRESS_TRANSFER_OK) return syscall_result_ok(0u);
	if (result == ADDRESS_TRANSFER_FAULT_FAILED) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
}

static syscall_result_t boot_module_handler(const struct cap_request* req) {
	struct boot_module_map_request request;
	syscall_result_t               result;

	if (req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	const struct kernel_boot_module* module = (struct kernel_boot_module*)(uintptr_t)req->object_id;
	if (module == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	{
		struct address_space*        space;
		enum address_transfer_result transfer_result;

		space = caller_space(req->caller);
		if (space == NULL) {
			memcpy(&request, req->request, sizeof(request));
		}
		else {
			transfer_result = address_space_copy_from(space, (uintptr_t)req->request, &request, sizeof(request));
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				if (transfer_result == ADDRESS_TRANSFER_FAULT_FAILED) {
					return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
				}
				return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
			}
		}
	}

	struct address_space* space = caller_space(req->caller);
	if (space == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	size_t page_count = (module->size + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;

	void*    mapped_address = NULL;
	vmm_id_t mapping_id     = VMM_ID_INVALID;

	if (!vmm_alloc_phys(space,
	                    (uintptr_t)module->address,
	                    page_count,
	                    VMM_PROT_READ | VMM_PROT_USER,
	                    NULL,
	                    &mapping_id,
	                    &mapped_address)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	if (request.base_out_ptr != 0u) {
		struct boot_module_map_response response = {
			.mapped_base = (uintptr_t)mapped_address,
		};
		result = copy_to_user(req->caller, request.base_out_ptr, &response, sizeof(response));
		if (result.status != SYSCALL_STATUS_OK) return result;
	}

	return syscall_result_ok((uintptr_t)mapped_address);
}

void kernel_capability_boot_module_init(void) {
	size_t count = kernel_boot_module_count();

	if (count == 0u) return;

	boot_module_objects = malloc(count * sizeof(*boot_module_objects));
	if (boot_module_objects == NULL) return;
	boot_module_object_count = count;
}

cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t target) {
	struct capability* cap;

	if (module_index >= boot_module_object_count) {
		return CAP_ID_INVALID;
	}

	if (boot_module_objects[module_index] == NULL) {
		const uint64_t object_id = (uint64_t)(uintptr_t)kernel_boot_module_at(module_index);
		if (object_id == 0u) {
			return CAP_ID_INVALID;
		}

		boot_module_objects[module_index] = cap_object_create_kernel(object_id, boot_module_handler);
		if (boot_module_objects[module_index] == NULL) {
			return CAP_ID_INVALID;
		}
	}

	cap = cap_create(boot_module_objects[module_index]->cap_object_id, target, CAP_READ | CAP_MAP | CAP_CALL, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
