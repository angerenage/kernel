#include <base/cap.h>
#include <base/module.h>
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

static syscall_result_t boot_module_info_handler(const struct cap_request* req, module_id_t id,
                                                 const struct kernel_boot_module* module) {
	struct module_info_response response = {
		.id         = id,
		.size       = module->size,
		.media_type = module->media_type,
	};

	if (req->request_size < sizeof(struct module_info_request) || !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	strlcpy(response.name, module->name != NULL ? module->name : "", sizeof(response.name));
	strlcpy(response.path, module->path != NULL ? module->path : "", sizeof(response.path));
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t boot_module_map_handler(const struct cap_request*        req,
                                                const struct kernel_boot_module* module) {
	if (req->request_size < sizeof(struct module_map_request) ||
	    !cap_kernel_response_fits(req, sizeof(struct module_map_response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	struct process*       caller;
	struct address_space* space;

	caller = process_lookup(req->caller);
	if (caller == NULL) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

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

	struct module_map_response response = {
		.mapped_base = (uintptr_t)mapped_address,
	};

	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t boot_module_handler(const struct cap_request* req) {
	struct module_request_header header;
	module_id_t                  id = (module_id_t)req->object_id;

	if (id == MODULE_ID_INVALID || req->request_size < sizeof(header)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&header, req->request, sizeof(header));

	const struct kernel_boot_module* module = kernel_boot_module_at((size_t)(id - 1u));
	if (module == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	switch (header.op) {
	case MODULE_OP_INFO:
		if ((req->rights & CAP_READ) != CAP_READ) {
			return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		}
		return boot_module_info_handler(req, id, module);
	case MODULE_OP_MAP:
		if ((req->rights & CAP_MAP) != CAP_MAP) {
			return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		}
		return boot_module_map_handler(req, module);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

syscall_result_t kernel_capability_boot_module_get(cap_id_t module_cap, process_id_t caller,
                                                   cap_rights_t                      required_rights,
                                                   const struct kernel_boot_module** out_module) {
	struct capability* cap;
	struct cap_object* object;
	enum cap_result    result;
	module_id_t        id;

	if (module_cap == CAP_ID_INVALID || caller == PROCESS_PID_INVALID || out_module == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	*out_module = NULL;

	cap = cap_lookup(module_cap);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	result = cap_is_authorized(caller, cap);
	if (result != CAP_OK) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	result = cap_is_valid(cap);
	if (result != CAP_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((cap->rights & required_rights) != required_rights) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL || object->handler != boot_module_handler) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	id = (module_id_t)object->object_id;
	if (id != MODULE_ID_INVALID) *out_module = kernel_boot_module_at((size_t)(id - 1u));
	cap_object_release(object);

	if (*out_module == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok(0u);
}

cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t recipient) {
	cap_object_id_t*   slot;
	cap_object_id_t    object_id;
	struct cap_object* object;
	struct capability* cap;

	slot = boot_module_id_slot(module_index);
	if (slot == NULL) return CAP_ID_INVALID;

	object_id = *slot;
	if (object_id == CAP_OBJECT_ID_INVALID) {
		const uint64_t raw_id = (uint64_t)module_index + 1u;
		if (kernel_boot_module_at(module_index) == NULL) return CAP_ID_INVALID;

		object = cap_object_create_kernel(raw_id, boot_module_handler);
		if (object == NULL) return CAP_ID_INVALID;

		object_id = object->cap_object_id;
		*slot     = object_id;
	}

	cap = cap_create(object_id, recipient, CAP_READ | CAP_MAP | CAP_CALL, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
