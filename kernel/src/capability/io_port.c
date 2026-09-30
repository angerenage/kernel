#include "io_port.h"

#include <base/io_port.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/io_port.h>
#include <core/process.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IO_PORT_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DERIVE | CAP_DELEGATE))

struct io_port_resource {
	uint32_t base;
	uint32_t count;
	bool     root;
};

static struct io_port_resource root_resource  = {.base = 0u, .count = IO_PORT_COUNT, .root = true};
static cap_object_id_t         root_object_id = CAP_OBJECT_ID_INVALID;

static syscall_result_t io_port_handler(const struct cap_request* req);

static bool range_valid(uint32_t base, uint32_t count) {
	return count != 0u && base < IO_PORT_COUNT && count <= IO_PORT_COUNT - base;
}

static bool access_valid(const struct io_port_resource* resource, uint32_t offset, enum io_port_width width,
                         uint16_t* out_port) {
	uint32_t size = (uint32_t)width;
	if (resource == NULL || out_port == NULL ||
	    (width != IO_PORT_WIDTH_8 && width != IO_PORT_WIDTH_16 && width != IO_PORT_WIDTH_32) ||
	    offset >= resource->count || size > resource->count - offset || resource->base + offset >= IO_PORT_COUNT ||
	    size > IO_PORT_COUNT - (resource->base + offset))
		return false;
	*out_port = (uint16_t)(resource->base + offset);
	return true;
}

static syscall_result_t copy_request(const struct cap_request* req, void* out, size_t size) {
	if (req == NULL || req->request == NULL || out == NULL || req->request_size != size)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(out, req->request, size);
	return syscall_result_ok(0u);
}

static bool grant_targets_caller(const struct cap_request* req) {
	struct capability* capability;
	bool               matches;
	if (req == NULL) return false;
	capability = cap_acquire(req->cap_id);
	if (capability == NULL) return false;
	matches = capability->target == req->caller;
	cap_release(capability);
	return matches;
}

static void io_port_resource_destroy(uint64_t object_id) {
	struct io_port_resource* resource = (struct io_port_resource*)(uintptr_t)object_id;
	if (resource != NULL && !resource->root) free(resource);
}

static void io_port_resource_event(struct cap_object* object, enum cap_object_event event) {
	struct io_port_resource* resource;
	if (object == NULL || event != CAP_OBJECT_EVENT_ZERO_GRANTS) return;
	resource = (struct io_port_resource*)(uintptr_t)object->object_id;
	if (resource != NULL && !resource->root) (void)cap_object_destroy_if_unused(object);
}

static void io_port_grant_change(uint64_t object_id, cap_id_t capability, process_id_t process, cap_rights_t rights,
                                 bool removed) {
	(void)object_id;
	if (removed || (rights & CAP_MAP) == 0u) (void)io_port_unmap_capability(process, capability);
}

static cap_id_t io_port_resource_publish(struct io_port_resource* resource, process_id_t recipient, cap_rights_t rights,
                                         struct capability* parent) {
	cap_object_id_t object_id;
	cap_id_t        cap;
	bool            created = false;
	if (resource == NULL || recipient == PROCESS_PID_INVALID || (rights & CAP_CALL) == 0u ||
	    (rights & ~IO_PORT_CAP_RIGHTS) != 0u)
		return CAP_ID_INVALID;
	object_id = cap_object_create_kernel_observed((uint64_t)(uintptr_t)resource,
	                                              io_port_handler,
	                                              NULL,
	                                              io_port_resource_destroy,
	                                              io_port_resource_event,
	                                              io_port_grant_change,
	                                              &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		if (!resource->root) free(resource);
		return CAP_ID_INVALID;
	}
	cap = cap_create(object_id, recipient, rights, parent);
	if (cap == CAP_ID_INVALID && created) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

static syscall_result_t io_port_info_handler(const struct cap_request* req, const struct io_port_resource* resource) {
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	const struct io_port_info response = {.base = resource->base, .count = resource->count};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t io_port_derive_handler(const struct cap_request* req, const struct io_port_resource* resource) {
	struct io_port_derive_request  request;
	struct io_port_derive_response response = {.io_port_cap = CAP_ID_INVALID};
	struct io_port_resource*       child;
	struct capability*             parent;
	syscall_result_t               result;

	if ((req->rights & CAP_DERIVE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (copy_request(req, &request, sizeof(request)).status != SYSCALL_STATUS_OK ||
	    !cap_kernel_response_fits(req, sizeof(response)) || request.count == 0u || request.offset >= resource->count ||
	    request.count > resource->count - request.offset || (request.rights & CAP_CALL) == 0u ||
	    (request.rights & ~req->rights) != 0u || (request.rights & ~IO_PORT_CAP_RIGHTS) != 0u)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	child = malloc(sizeof(*child));
	if (child == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	*child = (struct io_port_resource){
		.base  = resource->base + request.offset,
		.count = request.count,
		.root  = false,
	};
	parent = cap_acquire(req->cap_id);
	if (parent == NULL) {
		free(child);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	response.io_port_cap = io_port_resource_publish(child, req->caller, request.rights, parent);
	cap_release(parent);
	if (response.io_port_cap == CAP_ID_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.io_port_cap);
	return result;
}

static syscall_result_t io_port_map_handler(const struct cap_request* req, const struct io_port_resource* resource) {
	struct process*     process;
	enum io_port_result map_result;
	if ((req->rights & CAP_MAP) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!grant_targets_caller(req)) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	process = process_acquire(req->caller);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	map_result = io_port_map(process, req->cap_id, resource->base, resource->count);
	process_release(process);
	if (map_result == IO_PORT_OK) return syscall_result_ok(0u);
	return syscall_result_error(map_result == IO_PORT_NO_MEMORY ? SYSCALL_STATUS_FAILED : SYSCALL_STATUS_BAD_ARGUMENT,
	                            0u);
}

static syscall_result_t io_port_unmap_handler(const struct cap_request* req) {
	struct process*     process;
	enum io_port_result unmap_result;
	if ((req->rights & CAP_MAP) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!grant_targets_caller(req)) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	process = process_acquire(req->caller);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	unmap_result = io_port_unmap(process, req->cap_id);
	process_release(process);
	return unmap_result == IO_PORT_OK ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
}

static syscall_result_t io_port_read_handler(const struct cap_request* req, const struct io_port_resource* resource) {
	struct io_port_read_request  request;
	struct io_port_read_response response = {0};
	uint16_t                     port;
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (copy_request(req, &request, sizeof(request)).status != SYSCALL_STATUS_OK ||
	    !cap_kernel_response_fits(req, sizeof(response)) ||
	    !access_valid(resource, request.offset, request.width, &port))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	switch (request.width) {
	case IO_PORT_WIDTH_8:
		response.value = io_read8(port);
		break;
	case IO_PORT_WIDTH_16:
		response.value = io_read16(port);
		break;
	case IO_PORT_WIDTH_32:
		response.value = io_read32(port);
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t io_port_write_handler(const struct cap_request* req, const struct io_port_resource* resource) {
	struct io_port_write_request request;
	uint16_t                     port;
	if ((req->rights & CAP_WRITE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (copy_request(req, &request, sizeof(request)).status != SYSCALL_STATUS_OK ||
	    !access_valid(resource, request.offset, request.width, &port))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	switch (request.width) {
	case IO_PORT_WIDTH_8:
		io_write8(port, (uint8_t)request.value);
		break;
	case IO_PORT_WIDTH_16:
		io_write16(port, (uint16_t)request.value);
		break;
	case IO_PORT_WIDTH_32:
		io_write32(port, request.value);
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return syscall_result_ok(0u);
}

static syscall_result_t io_port_handler(const struct cap_request* req) {
	struct io_port_request_header header;
	struct io_port_resource*      resource;
	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	resource = (struct io_port_resource*)(uintptr_t)req->object_id;
	if (!range_valid(resource->base, resource->count)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	switch (header.op) {
	case IO_PORT_OP_INFO:
		if (req->request_size != sizeof(struct io_port_simple_request))
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		return io_port_info_handler(req, resource);
	case IO_PORT_OP_DERIVE:
		return io_port_derive_handler(req, resource);
	case IO_PORT_OP_MAP:
		if (req->request_size != sizeof(struct io_port_simple_request))
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		return io_port_map_handler(req, resource);
	case IO_PORT_OP_UNMAP:
		if (req->request_size != sizeof(struct io_port_simple_request))
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		return io_port_unmap_handler(req);
	case IO_PORT_OP_READ:
		return io_port_read_handler(req, resource);
	case IO_PORT_OP_WRITE:
		return io_port_write_handler(req, resource);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_io_ports_init(void) {
	bool created   = false;
	root_object_id = cap_object_create_kernel_observed((uint64_t)(uintptr_t)&root_resource,
	                                                   io_port_handler,
	                                                   NULL,
	                                                   io_port_resource_destroy,
	                                                   io_port_resource_event,
	                                                   io_port_grant_change,
	                                                   &created);
	(void)created;
	return root_object_id != CAP_OBJECT_ID_INVALID;
}

bool kernel_capability_io_ports_available(void) {
	struct cap_object* object = cap_object_acquire(root_object_id);
	if (object == NULL) return false;
	cap_object_release(object);
	return true;
}

cap_id_t kernel_capability_io_ports_grant(process_id_t recipient) {
	if (!kernel_capability_io_ports_available()) return CAP_ID_INVALID;
	return cap_create(root_object_id, recipient, IO_PORT_CAP_RIGHTS, NULL);
}
