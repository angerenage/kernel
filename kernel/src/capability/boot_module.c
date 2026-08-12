#include "boot_module.h"

#include <base/cap.h>
#include <base/math.h>
#include <base/module.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vmm.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"

static cap_object_id_t* boot_module_object_ids;
static size_t           boot_module_object_count;

struct boot_module_mapping_layout {
	uintptr_t physical_base;
	size_t    page_offset;
	size_t    page_count;
};

static bool boot_module_mapping_layout(const struct kernel_boot_module*   module,
                                       struct boot_module_mapping_layout* out_layout) {
	uintptr_t module_address;
	uintptr_t physical_address;
	size_t    mapped_size;

	if (module == NULL || out_layout == NULL) return false;
	module_address = (uintptr_t)module->address;
	if (module_address < boot_info.direct_map_offset) return false;
	physical_address          = module_address - boot_info.direct_map_offset;
	out_layout->physical_base = physical_address & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	out_layout->page_offset   = (size_t)(physical_address - out_layout->physical_base);
	if (add_overflow_size(out_layout->page_offset, module->size, &mapped_size) ||
	    add_overflow_size(mapped_size, PMM_PAGE_SIZE - 1u, &mapped_size)) {
		return false;
	}
	out_layout->page_count = mapped_size / PMM_PAGE_SIZE;
	return out_layout->page_count != 0u;
}

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
	struct boot_module_mapping_layout layout;
	struct module_map_response        response = {0};
	struct process*                   caller;
	struct address_space*             space;
	struct vmm_info                   mapping_info = {0};
	vmm_id_t                          mapping_id   = VMM_ID_INVALID;
	syscall_result_t                  result;

	if (req->request_size < sizeof(struct module_map_request) || !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	space = process_address_space(caller);
	if (space == NULL || !boot_module_mapping_layout(module, &layout)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	if (!vmm_alloc_phys(
			space, layout.physical_base, layout.page_count, VMM_PROT_READ | VMM_PROT_USER, NULL, &mapping_id, NULL)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	if (!vmm_query_id(space, mapping_id, &mapping_info)) {
		(void)vmm_free(space, mapping_id);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	mapping_info.id = VMM_ID_INVALID;
	response.mapping_cap =
		kernel_mapping_grant(caller, req->caller, mapping_id, VMM_KIND_PHYSICAL, CAP_READ | CAP_DESTROY);
	response.mapping     = mapping_info;
	response.data_offset = layout.page_offset;
	if (response.mapping_cap == CAP_ID_INVALID) {
		(void)vmm_free(space, mapping_id);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) {
		(void)kernel_mapping_discard_unpublished(response.mapping_cap, req->caller);
	}
	return result;
}

static syscall_result_t boot_module_read_handler(const struct cap_request*        req,
                                                 const struct kernel_boot_module* module) {
	struct module_read_request request;

	if (req->request_size < sizeof(request)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.size == 0u && request.offset <= module->size) return syscall_result_ok(0u);
	if (request.size > CAP_MAX_RESPONSE_SIZE || request.offset > module->size ||
	    request.size > module->size - request.offset || !cap_kernel_response_fits(req, request.size)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, (const uint8_t*)module->address + (size_t)request.offset, request.size);
}

static syscall_result_t boot_module_handler(const struct cap_request* req) {
	uint32_t    operation;
	module_id_t id = (module_id_t)req->object_id;

	if (id == MODULE_ID_INVALID || req->request_size < sizeof(operation)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&operation, req->request, sizeof(operation));
	const struct kernel_boot_module* module = kernel_boot_module_at((size_t)(id - 1u));
	if (module == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	switch (operation) {
	case MODULE_OP_INFO:
		if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return boot_module_info_handler(req, id, module);
	case MODULE_OP_MAP:
		if ((req->rights & CAP_MAP) != CAP_MAP) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return boot_module_map_handler(req, module);
	case MODULE_OP_READ:
		if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return boot_module_read_handler(req, module);
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
	cap         = cap_acquire(module_cap);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	result = cap_is_authorized(caller, cap);
	if (result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	result = cap_is_valid(cap);
	if (result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((cap_rights(cap) & required_rights) != required_rights) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL || object->handler != boot_module_handler) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	id = (module_id_t)object->object_id;
	if (id != MODULE_ID_INVALID) *out_module = kernel_boot_module_at((size_t)(id - 1u));
	cap_object_release(object);
	cap_release(cap);
	return *out_module == NULL ? syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u) : syscall_result_ok(0u);
}

cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t recipient, bool* out_created) {
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

	cap = cap_create(object_id, recipient, CAP_READ | CAP_MAP | CAP_CALL, NULL, out_created);
	return cap == NULL ? CAP_ID_INVALID : cap->cap_id;
}
