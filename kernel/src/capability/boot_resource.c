#include "boot_resource.h"

#include <base/boot_data.h>
#include <base/cap.h>
#include <base/framebuffer.h>
#include <base/kernel_resource.h>
#include <base/math.h>
#include <base/syscall.h>
#include <base/vmm.h>
#include <core/capability.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vm_space.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"

static cap_object_id_t rsdp_object_id        = CAP_OBJECT_ID_INVALID;
static cap_object_id_t dtb_object_id         = CAP_OBJECT_ID_INVALID;
static cap_object_id_t framebuffer_object_id = CAP_OBJECT_ID_INVALID;

struct external_mapping_layout {
	uintptr_t physical_base;
	size_t    page_offset;
	size_t    page_count;
};

struct external_mapping_result {
	cap_id_t        mapping_cap;
	struct vmm_info mapping;
	size_t          data_offset;
};

static bool external_mapping_layout(const void* address, size_t size, struct external_mapping_layout* out) {
	uintptr_t virtual_address;
	uintptr_t physical_address;
	size_t    mapped_size;

	if (address == NULL || size == 0u || out == NULL) return false;
	virtual_address = (uintptr_t)address;
	if (virtual_address < boot_info.direct_map_offset) return false;
	physical_address   = virtual_address - boot_info.direct_map_offset;
	out->physical_base = physical_address & ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
	out->page_offset   = (size_t)(physical_address - out->physical_base);
	if (add_overflow_size(out->page_offset, size, &mapped_size) ||
	    add_overflow_size(mapped_size, VMM_PAGE_SIZE - 1u, &mapped_size)) {
		return false;
	}
	out->page_count = mapped_size / VMM_PAGE_SIZE;
	return out->page_count != 0u;
}

static syscall_result_t external_mapping_create(const struct cap_request* req, const void* address, size_t size,
                                                vmm_prot_t prot, struct external_mapping_result* out) {
	struct external_mapping_layout layout;
	struct process*                caller;
	struct address_space*          space;
	struct memory_object*          memory     = NULL;
	vmm_id_t                       mapping_id = VMM_ID_INVALID;

	if (req == NULL || out == NULL || !external_mapping_layout(address, size, &layout)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	*out   = (struct external_mapping_result){.mapping_cap = CAP_ID_INVALID};
	caller = process_acquire(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	space = process_address_space(caller);
	if (space == NULL || !memory_object_create_external(layout.physical_base, layout.page_count, &memory) ||
	    !vm_space_map(space,
	                  &(const struct vm_map_request){
						  .memory = memory, .page_count = layout.page_count, .align_pages = 1u, .prot = prot},
	                  &mapping_id,
	                  NULL)) {
		memory_object_release(memory);
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	memory_object_release(memory);
	if (!vm_space_query_id(space, mapping_id, &out->mapping)) {
		(void)vm_space_unmap(space, mapping_id);
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	out->mapping.id  = VMM_ID_INVALID;
	out->mapping_cap = kernel_mapping_grant(caller, req->caller, mapping_id, CAP_DESTROY);
	out->data_offset = layout.page_offset;
	if (out->mapping_cap == CAP_ID_INVALID) {
		(void)vm_space_unmap(space, mapping_id);
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	process_release(caller);
	return syscall_result_ok(0u);
}

static bool boot_data_get(enum kernel_resource_type type, struct kernel_boot_data* out) {
	switch (type) {
	case KERNEL_RESOURCE_TYPE_RSDP:
		return kernel_boot_rsdp_get(out);
	case KERNEL_RESOURCE_TYPE_DTB:
		return kernel_boot_dtb_get(out);
	default:
		return false;
	}
}

static syscall_result_t boot_data_info_handler(const struct cap_request* req, enum kernel_resource_type type,
                                               const struct kernel_boot_data* data) {
	const struct boot_data_info_response response = {.type = type, .size = data->size};

	if (req->request_size < sizeof(struct boot_data_info_request) || !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t boot_data_read_handler(const struct cap_request* req, const struct kernel_boot_data* data) {
	struct boot_data_read_request request;

	if (req->request_size < sizeof(request)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.size == 0u && request.offset <= data->size) return syscall_result_ok(0u);
	if (request.size > CAP_MAX_RESPONSE_SIZE || request.offset > data->size ||
	    request.size > data->size - request.offset || !cap_kernel_response_fits(req, request.size)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, (const uint8_t*)data->address + (size_t)request.offset, request.size);
}

static syscall_result_t boot_data_handler(const struct cap_request* req) {
	enum kernel_resource_type type = (enum kernel_resource_type)req->object_id;
	struct kernel_boot_data   data;
	uint32_t                  operation;

	if (req->request == NULL || req->request_size < sizeof(operation))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!boot_data_get(type, &data)) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	memcpy(&operation, req->request, sizeof(operation));
	switch (operation) {
	case BOOT_DATA_OP_INFO:
		if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return boot_data_info_handler(req, type, &data);
	case BOOT_DATA_OP_READ:
		if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return boot_data_read_handler(req, &data);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static bool framebuffer_get(struct kernel_boot_framebuffer* out, size_t* out_size) {
	if (out == NULL || out_size == NULL || !kernel_boot_framebuffer_get(out) ||
	    (uint64_t)(size_t)out->pitch != out->pitch || (uint64_t)(size_t)out->height != out->height ||
	    mul_overflow_size((size_t)out->pitch, (size_t)out->height, out_size) || *out_size == 0u) {
		return false;
	}
	return true;
}

static syscall_result_t framebuffer_info_handler(const struct cap_request*             req,
                                                 const struct kernel_boot_framebuffer* framebuffer, size_t size) {
	const struct framebuffer_info_response response = {
		.size             = size,
		.width            = framebuffer->width,
		.height           = framebuffer->height,
		.pitch            = framebuffer->pitch,
		.bpp              = framebuffer->bpp,
		.memory_model     = framebuffer->memory_model,
		.red_mask_size    = framebuffer->red_mask_size,
		.red_mask_shift   = framebuffer->red_mask_shift,
		.green_mask_size  = framebuffer->green_mask_size,
		.green_mask_shift = framebuffer->green_mask_shift,
		.blue_mask_size   = framebuffer->blue_mask_size,
		.blue_mask_shift  = framebuffer->blue_mask_shift,
	};

	if (req->request_size < sizeof(struct framebuffer_info_request) ||
	    !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t framebuffer_map_handler(const struct cap_request*             req,
                                                const struct kernel_boot_framebuffer* framebuffer, size_t size) {
	struct external_mapping_result  mapping;
	struct framebuffer_map_response response;
	syscall_result_t                result;

	if (req->request_size < sizeof(struct framebuffer_map_request) ||
	    !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	result = external_mapping_create(req, framebuffer->address, size, VMM_PROT_READ | VMM_PROT_WRITE, &mapping);
	if (result.status != SYSCALL_STATUS_OK) return result;
	response = (struct framebuffer_map_response){
		.mapping_cap = mapping.mapping_cap, .mapping = mapping.mapping, .data_offset = mapping.data_offset};
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) {
		(void)kernel_mapping_discard_unpublished(response.mapping_cap, req->caller);
	}
	return result;
}

static syscall_result_t framebuffer_handler(const struct cap_request* req) {
	struct kernel_boot_framebuffer framebuffer;
	size_t                         size;
	uint32_t                       operation;

	if (req->request == NULL || req->request_size < sizeof(operation))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!framebuffer_get(&framebuffer, &size)) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	memcpy(&operation, req->request, sizeof(operation));
	switch (operation) {
	case FRAMEBUFFER_OP_INFO:
		if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return framebuffer_info_handler(req, &framebuffer, size);
	case FRAMEBUFFER_OP_MAP:
		if ((req->rights & (CAP_READ | CAP_WRITE | CAP_MAP)) != (CAP_READ | CAP_WRITE | CAP_MAP)) {
			return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		}
		return framebuffer_map_handler(req, &framebuffer, size);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

void kernel_capability_boot_resources_init(void) {
	rsdp_object_id        = cap_object_create_kernel(KERNEL_RESOURCE_TYPE_RSDP, boot_data_handler, NULL);
	dtb_object_id         = cap_object_create_kernel(KERNEL_RESOURCE_TYPE_DTB, boot_data_handler, NULL);
	framebuffer_object_id = cap_object_create_kernel(KERNEL_RESOURCE_TYPE_FRAMEBUFFER, framebuffer_handler, NULL);
}

bool kernel_capability_boot_data_available(enum kernel_resource_type type) {
	struct kernel_boot_data data;

	return boot_data_get(type, &data);
}

cap_id_t kernel_capability_boot_data_grant(enum kernel_resource_type type, process_id_t recipient) {
	struct kernel_boot_data data;
	cap_object_id_t         object_id;

	if (!boot_data_get(type, &data)) return CAP_ID_INVALID;
	object_id = type == KERNEL_RESOURCE_TYPE_RSDP ? rsdp_object_id : dtb_object_id;
	if (object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	return cap_create(object_id, recipient, CAP_CALL | CAP_READ | CAP_DELEGATE, NULL);
}

bool kernel_capability_framebuffer_available(void) {
	struct kernel_boot_framebuffer framebuffer;
	size_t                         size;

	return framebuffer_get(&framebuffer, &size);
}

cap_id_t kernel_capability_framebuffer_grant(process_id_t recipient) {
	struct kernel_boot_framebuffer framebuffer;
	size_t                         size;

	if (framebuffer_object_id == CAP_OBJECT_ID_INVALID || !framebuffer_get(&framebuffer, &size)) {
		return CAP_ID_INVALID;
	}
	return cap_create(framebuffer_object_id, recipient, CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE, NULL);
}
