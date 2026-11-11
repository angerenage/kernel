#include "service.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <base/address_space.h>
#include <base/channel.h>
#include <base/memory.h>
#include <base/process.h>
#include <base/signal.h>

#include <protocol/device.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/memory.h>
#include <system/process.h>
#include <system/signal.h>
#include <runtime/block.h>
#include <runtime/device.h>
#include <runtime/heap.h>

#include "bot.h"

#define MSC_BLOCK_ROOT_OBJECT 1u
#define MSC_BLOCK_SLICE_OBJECT_BASE 0x10000ull
#define MSC_BLOCK_SESSION_OBJECT_BASE 0x100000000ull
#define MSC_BLOCK_MAX_SLICES 32u
#define MSC_BLOCK_MAX_SESSIONS 8u
#define MSC_BLOCK_MAX_BUFFERS 8u
#define MSC_BLOCK_PROVIDER_RIGHTS (BLOCK_DRIVER_RIGHTS | CAP_DELEGATE)

struct msc_block_buffer {
	bool used;
	uint64_t id;
	uint32_t size;
	cap_id_t memory_cap;
	cap_id_t client_cap;
	cap_id_t mapping_cap;
	uint8_t* address;
};

struct msc_block_session {
	bool active;
	uint64_t object_id;
	uint64_t block_offset;
	uint64_t block_count;
	uint64_t next_buffer_id;
	struct msc_block_buffer buffers[MSC_BLOCK_MAX_BUFFERS];
};

struct msc_block_slice {
	bool active;
	uint64_t object_id;
	uint64_t block_offset;
	uint64_t block_count;
};

struct msc_block_service {
	channel_id_t endpoint;
	cap_id_t activity_cap;
	cap_id_t root_cap;
	cap_id_t address_space_cap;
	process_id_t pid;
	struct usb_msc_device* device;
	uint64_t next_slice_id;
	uint64_t next_session_id;
	struct msc_block_slice slices[MSC_BLOCK_MAX_SLICES];
	struct msc_block_session sessions[MSC_BLOCK_MAX_SESSIONS];
};

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static syscall_status_t add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t bytes[sizeof(value)];
	for (unsigned i = 0u; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(value >> (8u * i));
	return device_builder_add_property(builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, bytes, sizeof(bytes));
}

static syscall_status_t add_bool(const struct device_builder* builder, const char* name, bool value) {
	uint8_t byte = value ? 1u : 0u;
	return device_builder_add_property(builder, name, strlen(name), DEVICE_PROPERTY_BOOLEAN, 1u, &byte, sizeof(byte));
}

static bool create_child(cap_id_t parent, cap_id_t block_cap, const struct usb_msc_device* device) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	syscall_status_t status = device_builder_begin_child(parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, "disk", sizeof("disk") - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DISK, sizeof(BLOCK_COMPATIBLE_DISK) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DEVICE, sizeof(BLOCK_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_count", device->block_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_size", device->block_size);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "max_transfer_blocks", device->max_transfer_blocks);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "writable", true);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "removable", device->removable);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "flush_supported", true);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(
				&builder, BLOCK_RESOURCE, sizeof(BLOCK_RESOURCE) - 1u, block_cap, BLOCK_DRIVER_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static struct msc_block_session* find_session(struct msc_block_service* service, uint64_t object_id) {
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SESSIONS; i++)
		if (service->sessions[i].active && service->sessions[i].object_id == object_id) return &service->sessions[i];
	return NULL;
}

static struct msc_block_slice* find_slice(struct msc_block_service* service, uint64_t object_id) {
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SLICES; i++)
		if (service->slices[i].active && service->slices[i].object_id == object_id) return &service->slices[i];
	return NULL;
}

static bool find_region(struct msc_block_service* service, uint64_t object_id, uint64_t* offset, uint64_t* count) {
	if (object_id == MSC_BLOCK_ROOT_OBJECT) {
		*offset = 0u;
		*count = service->device->block_count;
		return true;
	}
	struct msc_block_slice* slice = find_slice(service, object_id);
	if (slice == NULL) return false;
	*offset = slice->block_offset;
	*count = slice->block_count;
	return true;
}

static struct msc_block_buffer* find_buffer(struct msc_block_session* session, uint64_t id) {
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_BUFFERS; i++)
		if (session->buffers[i].used && session->buffers[i].id == id) return &session->buffers[i];
	return NULL;
}

static void release_buffer(struct msc_block_buffer* buffer) {
	if (buffer == NULL || !buffer->used) return;
	if (buffer->client_cap != CAP_ID_INVALID) (void)cap_revoke(buffer->client_cap, 0u);
	if (buffer->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(buffer->mapping_cap);
	if (buffer->memory_cap != CAP_ID_INVALID) (void)cap_drop(buffer->memory_cap);
	*buffer = (struct msc_block_buffer){
		.memory_cap = CAP_ID_INVALID, .client_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID
	};
}

static void release_session(struct msc_block_session* session) {
	if (session == NULL || !session->active) return;
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_BUFFERS; i++) release_buffer(&session->buffers[i]);
	*session = (struct msc_block_session){0};
}

static bool dispatch_open(
		struct msc_block_service* service, uint64_t offset, uint64_t count, const struct cap_request* call) {
	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_open_request) ||
			call->response_capacity < sizeof(struct block_open_response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct msc_block_session* session = NULL;
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SESSIONS; i++)
		if (!service->sessions[i].active) {
			session = &service->sessions[i];
			break;
		}
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*session = (struct msc_block_session){
		.active = true,
		.object_id = MSC_BLOCK_SESSION_OBJECT_BASE + service->next_session_id++,
		.block_offset = offset,
		.block_count = count,
		.next_buffer_id = 1u
	};
	struct block_open_response response = {
		.block_count = count,
		.block_size = service->device->block_size,
		.max_transfer_blocks = service->device->max_transfer_blocks,
		.flags = BLOCK_DEVICE_WRITABLE | BLOCK_DEVICE_FLUSH_SUPPORTED |
		         (service->device->removable ? BLOCK_DEVICE_REMOVABLE : 0u)
	};
	if (cap_publish(service->endpoint,
				session->object_id,
				call->caller,
				call->rights & BLOCK_DRIVER_RIGHTS,
				&response.session_cap) != SYSCALL_STATUS_OK) {
		release_session(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, session->object_id);
	release_session(session);
	return false;
}

static bool dispatch_slice(struct msc_block_service* service, uint64_t offset, uint64_t count,
		const struct cap_request* call, const void* data) {
	struct block_slice_request request;
	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(struct block_slice_response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.block_count == 0u || request.block_index >= count ||
			request.block_count > count - request.block_index)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct msc_block_slice* slice = NULL;
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SLICES; i++)
		if (!service->slices[i].active) {
			slice = &service->slices[i];
			break;
		}
	if (slice == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*slice = (struct msc_block_slice){
		.active = true,
		.object_id = MSC_BLOCK_SLICE_OBJECT_BASE + service->next_slice_id++,
		.block_offset = offset + request.block_index,
		.block_count = request.block_count
	};
	struct block_slice_response response = {.root_cap = CAP_ID_INVALID};
	if (cap_publish(service->endpoint,
				slice->object_id,
				call->caller,
				(call->rights & BLOCK_DRIVER_RIGHTS) | CAP_DELEGATE,
				&response.root_cap) != SYSCALL_STATUS_OK) {
		*slice = (struct msc_block_slice){0};
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, slice->object_id);
	*slice = (struct msc_block_slice){0};
	return false;
}

static bool dispatch_allocate(struct msc_block_service* service, struct msc_block_session* session,
		const struct cap_request* call, const void* data) {
	struct block_allocate_buffer_request request;
	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) ||
			call->response_capacity < sizeof(struct block_allocate_buffer_response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	uint64_t maximum = (uint64_t)service->device->max_transfer_blocks * service->device->block_size;
	if (request.size == 0u || request.size > maximum) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct msc_block_buffer* buffer = NULL;
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_BUFFERS; i++)
		if (!session->buffers[i].used) {
			buffer = &session->buffers[i];
			break;
		}
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*buffer = (struct msc_block_buffer){
		.used = true,
		.id = session->next_buffer_id++,
		.size = request.size,
		.memory_cap = CAP_ID_INVALID,
		.client_cap = CAP_ID_INVALID,
		.mapping_cap = CAP_ID_INVALID
	};
	struct address_space_map_response mapping = {.mapping_cap = CAP_ID_INVALID};
	size_t allocation_size = (request.size + runtime_heap_granule - 1u) & ~(runtime_heap_granule - 1u);
	syscall_status_t status =
			memory_allocator_alloc(runtime_heap_memory_allocator_cap, allocation_size, &buffer->memory_cap);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "usb-storage: block buffer allocation failed: %u\n", status);
		release_buffer(buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	status = address_space_map(service->address_space_cap,
			buffer->memory_cap,
			MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
			0u,
			0u,
			0u,
			0u,
			&mapping);
	if (status != SYSCALL_STATUS_OK || mapping.mapping_cap == CAP_ID_INVALID || mapping.address == 0u) {
		fprintf(stderr, "usb-storage: block buffer mapping failed: %u\n", status);
		release_buffer(buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	buffer->mapping_cap = mapping.mapping_cap;
	buffer->address = (uint8_t*)mapping.address;
	status = cap_delegate(buffer->memory_cap, call->caller, BLOCK_BUFFER_RIGHTS, &buffer->client_cap);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "usb-storage: block buffer delegation failed: %u\n", status);
		release_buffer(buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	struct block_allocate_buffer_response response = {
		.buffer_id = buffer->id, .memory_cap = buffer->client_cap, .size = buffer->size
	};
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	release_buffer(buffer);
	return false;
}

static bool dispatch_transfer(struct msc_block_service* service, struct msc_block_session* session,
		const struct cap_request* call, const void* data, bool write) {
	struct block_transfer_request request;
	if ((call->rights & (write ? CAP_WRITE : CAP_READ)) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(struct block_transfer_response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	struct msc_block_buffer* buffer = find_buffer(session, request.buffer_id);
	if (buffer == NULL || request.reserved != 0u || request.block_count == 0u ||
			request.block_count > service->device->max_transfer_blocks || request.block_index >= session->block_count ||
			request.block_count > session->block_count - request.block_index ||
			request.block_count > UINT32_MAX / service->device->block_size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	uint32_t size = request.block_count * service->device->block_size;
	if (request.buffer_offset > buffer->size || size > buffer->size - request.buffer_offset)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	uint64_t block = session->block_offset + request.block_index;
	bool success =
			write ? usb_msc_write(service->device, block, request.block_count, buffer->address + request.buffer_offset)
			      : usb_msc_read(service->device, block, request.block_count, buffer->address + request.buffer_offset);
	if (!success) return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	struct block_transfer_response response = {.transferred_blocks = request.block_count};
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_session(struct msc_block_service* service, struct msc_block_session* session,
		const struct cap_request* call, const void* data, enum block_op op) {
	switch (op) {
	case BLOCK_OP_ALLOCATE_BUFFER:
		return dispatch_allocate(service, session, call, data);
	case BLOCK_OP_READ:
		return dispatch_transfer(service, session, call, data, false);
	case BLOCK_OP_WRITE:
		return dispatch_transfer(service, session, call, data, true);
	case BLOCK_OP_FLUSH:
		if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct block_flush_request))
			return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply(call, NULL, 0u, usb_msc_flush(service->device) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED);
	case BLOCK_OP_RELEASE_BUFFER: {
		struct block_release_buffer_request request;
		if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		struct msc_block_buffer* buffer = find_buffer(session, request.buffer_id);
		if (request.reserved != 0u || buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		release_buffer(buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	}
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch(struct msc_block_service* service, const struct cap_request* call, const void* data) {
	if ((call->rights & CAP_CALL) == 0u || call->request_size < sizeof(struct block_request_header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	struct block_request_header header;
	memcpy(&header, data, sizeof(header));
	uint64_t offset, count;
	if (find_region(service, call->object_id, &offset, &count)) {
		if (header.op == BLOCK_OP_OPEN) return dispatch_open(service, offset, count, call);
		if (header.op == BLOCK_OP_SLICE) return dispatch_slice(service, offset, count, call, data);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	struct msc_block_session* session = find_session(service, call->object_id);
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return dispatch_session(service, session, call, data, header.op);
}

static bool serve(struct msc_block_service* service) {
	for (;;) {
		bool received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status = channel_recv(service->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch(service, &call, request_buffer))) return false;
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status = channel_event_recv(service->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && event.type == CHANNEL_EVENT_CAP_ZERO_GRANTS) {
				struct msc_block_session* session = find_session(service, event.object_id);
				if (session != NULL) {
					(void)cap_unpublish_if_unused(service->endpoint, session->object_id);
					release_session(session);
				}
				else {
					struct msc_block_slice* slice = find_slice(service, event.object_id);
					if (slice != NULL &&
							cap_unpublish_if_unused(service->endpoint, slice->object_id) == SYSCALL_STATUS_OK)
						*slice = (struct msc_block_slice){0};
				}
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(service->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

static void cleanup(struct msc_block_service* service) {
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SESSIONS; i++) {
		if (!service->sessions[i].active) continue;
		(void)cap_unpublish(service->endpoint, service->sessions[i].object_id);
		release_session(&service->sessions[i]);
	}
	for (unsigned i = 0u; i < MSC_BLOCK_MAX_SLICES; i++)
		if (service->slices[i].active) (void)cap_unpublish(service->endpoint, service->slices[i].object_id);
	if (service->root_cap != CAP_ID_INVALID) (void)cap_unpublish(service->endpoint, MSC_BLOCK_ROOT_OBJECT);
	if (service->address_space_cap != CAP_ID_INVALID) (void)cap_drop(service->address_space_cap);
	if (service->activity_cap != CAP_ID_INVALID) (void)cap_drop(service->activity_cap);
	if (service->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(service->endpoint);
}

bool usb_msc_block_service_run(cap_id_t device_cap, struct usb_msc_device* device) {
	struct msc_block_service service = {
		.endpoint = CHANNEL_ID_INVALID,
		.activity_cap = CAP_ID_INVALID,
		.root_cap = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.device = device,
		.next_slice_id = 1u,
		.next_session_id = 1u
	};
	struct self_info self;
	if (device == NULL || device->block_count == 0u || device->block_size == 0u ||
			process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	service.pid = self.pid;
	service.address_space_cap = self.address_space_cap;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	bool success = channel_create(&service.endpoint, &service.activity_cap) == SYSCALL_STATUS_OK &&
	               cap_publish(service.endpoint,
						   MSC_BLOCK_ROOT_OBJECT,
						   service.pid,
						   MSC_BLOCK_PROVIDER_RIGHTS,
						   &service.root_cap) == SYSCALL_STATUS_OK &&
	               create_child(device_cap, service.root_cap, device);
	if (success) {
		printf("usb-storage: published block:disk (%llu blocks of %u bytes)\n",
				(unsigned long long)device->block_count,
				device->block_size);
		success = serve(&service);
	}
	cleanup(&service);
	return success;
}
