#include <base/cap.h>
#include <base/syscall.h>
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

static cap_object_id_t* boot_module_object_ids;
static size_t           boot_module_object_count;

static cap_object_id_t* boot_module_id_slot(size_t module_index) {
	if (boot_module_object_ids == NULL) {
		size_t count = kernel_boot_module_count();
		if (count == 0u || module_index >= count) return NULL;

		boot_module_object_ids = calloc(count, sizeof(*boot_module_object_ids));
		if (boot_module_object_ids == NULL) return NULL;
		boot_module_object_count = count;
	}

	if (module_index >= boot_module_object_count) return NULL;

	return &boot_module_object_ids[module_index];
}

static syscall_result_t boot_module_handler(const struct cap_request* req) {
	struct boot_module_map_request request;

	if (req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (!cap_kernel_response_fits(req, sizeof(struct boot_module_map_response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	const struct kernel_boot_module* module = (struct kernel_boot_module*)(uintptr_t)req->object_id;
	if (module == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	struct process*       caller;
	struct address_space* space;

	caller = process_lookup(req->caller);
	if (caller == NULL) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	space = process_address_space(caller);

	memcpy(&request, req->request, sizeof(request));

	space = process_address_space(caller);
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

	struct boot_module_map_response response = {
		.mapped_base = (uintptr_t)mapped_address,
	};

	(void)request.base_out_ptr;
	return cap_kernel_write_response(req, &response, sizeof(response));
}

cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t target) {
	cap_object_id_t*   slot;
	cap_object_id_t    object_id;
	struct cap_object* object;
	struct capability* cap;

	slot = boot_module_id_slot(module_index);
	if (slot == NULL) return CAP_ID_INVALID;

	object_id = *slot;
	if (object_id == CAP_OBJECT_ID_INVALID) {
		const uint64_t raw_id = (uint64_t)(uintptr_t)kernel_boot_module_at(module_index);
		if (raw_id == 0u) return CAP_ID_INVALID;

		object = cap_object_create_kernel(raw_id, boot_module_handler);
		if (object == NULL) return CAP_ID_INVALID;

		object_id = object->cap_object_id;
		*slot     = object_id;
	}

	cap = cap_create(object_id, target, CAP_READ | CAP_MAP | CAP_CALL, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
