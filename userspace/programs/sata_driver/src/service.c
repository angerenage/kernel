#include "service.h"

#include <base/channel.h>
#include <base/memory.h>
#include <base/process.h>
#include <base/signal.h>
#include <protocol/device.h>
#include <runtime/block.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "commands.h"
#include "identify.h"
#include "transport.h"

#define SATA_BLOCK_ROOT_OBJECT 1u
#define SATA_BLOCK_SESSION_OBJECT_BASE 0x100000000ull
#define SATA_BLOCK_MAX_SESSIONS 8u
#define SATA_BLOCK_MAX_BUFFERS 8u
#define SATA_BLOCK_PROVIDER_RIGHTS (BLOCK_DRIVER_RIGHTS | CAP_DELEGATE)
#define SATA_BLOCK_SESSION_RIGHTS BLOCK_DRIVER_RIGHTS

struct sata_block_buffer {
	bool               used;
	uint64_t           id;
	cap_id_t           client_cap;
	struct sata_buffer sata;
};

struct sata_block_session {
	bool                     active;
	uint64_t                 object_id;
	process_id_t             caller;
	uint64_t                 next_buffer_id;
	struct sata_block_buffer buffers[SATA_BLOCK_MAX_BUFFERS];
};

struct sata_block_service {
	channel_id_t                   endpoint;
	cap_id_t                       activity_cap;
	cap_id_t                       root_cap;
	process_id_t                   pid;
	const struct sata_transport*   transport;
	const struct sata_device_info* info;
	uint64_t                       next_session_id;
	struct sata_block_session      sessions[SATA_BLOCK_MAX_SESSIONS];
};

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static syscall_status_t add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t encoded[sizeof(value)];

	for (size_t index = 0u; index < sizeof(encoded); index++) encoded[index] = (uint8_t)(value >> (index * 8u));
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, encoded, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static syscall_status_t add_bool(const struct device_builder* builder, const char* name, bool value) {
	uint8_t          encoded = value ? 1u : 0u;
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_BOOLEAN, 1u, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, &encoded, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static uint32_t transfer_block_limit(const struct sata_transport* transport, const struct sata_device_info* info) {
	uint32_t count = transport->max_transfer_size / info->logical_sector_size;
	uint32_t limit = info->lba48 ? UINT16_MAX : 256u;
	return count < limit ? count : limit;
}

static bool create_child(cap_id_t parent, cap_id_t block_cap, const struct sata_transport* transport,
                         const struct sata_device_info* info) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	syscall_status_t      status  = device_builder_begin_child(parent, &builder);

	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, "disk", sizeof("disk") - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DISK, sizeof(BLOCK_COMPATIBLE_DISK) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DEVICE, sizeof(BLOCK_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_count", info->sector_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_size", info->logical_sector_size);
	if (status == SYSCALL_STATUS_OK)
		status = add_u64(&builder, "max_transfer_blocks", transfer_block_limit(transport, info));
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "writable", true);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "removable", false);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "flush_supported", info->flush_cache || info->flush_cache_ext);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(
			&builder, BLOCK_RESOURCE, sizeof(BLOCK_RESOURCE) - 1u, block_cap, BLOCK_DRIVER_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static struct sata_block_session* find_session(struct sata_block_service* service, uint64_t object_id) {
	for (size_t index = 0u; index < SATA_BLOCK_MAX_SESSIONS; index++)
		if (service->sessions[index].active && service->sessions[index].object_id == object_id)
			return &service->sessions[index];
	return NULL;
}

static struct sata_block_buffer* find_buffer(struct sata_block_session* session, uint64_t id) {
	for (size_t index = 0u; index < SATA_BLOCK_MAX_BUFFERS; index++)
		if (session->buffers[index].used && session->buffers[index].id == id) return &session->buffers[index];
	return NULL;
}

static void release_buffer(const struct sata_transport* transport, struct sata_block_buffer* buffer) {
	if (buffer == NULL || !buffer->used) return;
	if (buffer->client_cap != CAP_ID_INVALID) (void)cap_revoke(buffer->client_cap, 0u);
	sata_buffer_release(transport, &buffer->sata);
	*buffer = (struct sata_block_buffer){
		.client_cap = CAP_ID_INVALID,
		.sata = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
}

static void release_session(const struct sata_transport* transport, struct sata_block_session* session) {
	if (session == NULL || !session->active) return;
	for (size_t index = 0u; index < SATA_BLOCK_MAX_BUFFERS; index++)
		release_buffer(transport, &session->buffers[index]);
	*session = (struct sata_block_session){0};
}

static uint32_t max_transfer_blocks(const struct sata_block_service* service) {
	return transfer_block_limit(service->transport, service->info);
}

static bool dispatch_open(struct sata_block_service* service, const struct cap_request* call) {
	struct block_open_response response = {0};
	struct sata_block_session* session  = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_open_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < SATA_BLOCK_MAX_SESSIONS; index++)
		if (!service->sessions[index].active) {
			session = &service->sessions[index];
			break;
		}
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*session = (struct sata_block_session){.active    = true,
	                                       .object_id = SATA_BLOCK_SESSION_OBJECT_BASE + service->next_session_id++,
	                                       .caller    = call->caller,
	                                       .next_buffer_id = 1u};
	for (size_t index = 0u; index < SATA_BLOCK_MAX_BUFFERS; index++) {
		session->buffers[index].client_cap             = CAP_ID_INVALID;
		session->buffers[index].sata.memory_cap        = CAP_ID_INVALID;
		session->buffers[index].sata.address_space_cap = CAP_ID_INVALID;
		session->buffers[index].sata.mapping_cap       = CAP_ID_INVALID;
	}
	response.block_count         = service->info->sector_count;
	response.block_size          = service->info->logical_sector_size;
	response.max_transfer_blocks = max_transfer_blocks(service);
	response.flags               = (call->rights & CAP_WRITE) != 0u ? BLOCK_DEVICE_WRITABLE : 0u;
	if ((call->rights & CAP_WRITE) != 0u && (service->info->flush_cache || service->info->flush_cache_ext))
		response.flags |= BLOCK_DEVICE_FLUSH_SUPPORTED;
	if (cap_publish(service->endpoint,
	                session->object_id,
	                call->caller,
	                call->rights & SATA_BLOCK_SESSION_RIGHTS,
	                &response.session_cap) != SYSCALL_STATUS_OK) {
		release_session(service->transport, session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, session->object_id);
	release_session(service->transport, session);
	return false;
}

static bool dispatch_allocate(struct sata_block_service* service, struct sata_block_session* session,
                              const struct cap_request* call, const void* data) {
	struct block_allocate_buffer_request  request;
	struct block_allocate_buffer_response response = {0};
	struct sata_block_buffer*             buffer   = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.size == 0u || request.size > service->transport->max_transfer_size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < SATA_BLOCK_MAX_BUFFERS; index++)
		if (!session->buffers[index].used) {
			buffer = &session->buffers[index];
			break;
		}
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*buffer = (struct sata_block_buffer){
		.used       = true,
		.id         = session->next_buffer_id++,
		.client_cap = CAP_ID_INVALID,
		.sata = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
	if (!sata_buffer_allocate(service->transport, request.size, &buffer->sata) ||
	    cap_delegate(buffer->sata.memory_cap, call->caller, BLOCK_BUFFER_RIGHTS, &buffer->client_cap) !=
	        SYSCALL_STATUS_OK) {
		release_buffer(service->transport, buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	response.buffer_id  = buffer->id;
	response.memory_cap = buffer->client_cap;
	response.size       = buffer->sata.size;
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	release_buffer(service->transport, buffer);
	return false;
}

static bool transfer_valid(const struct sata_block_service* service, const struct block_transfer_request* request,
                           const struct sata_block_buffer* buffer) {
	if (request->reserved != 0u || request->block_count == 0u || request->block_count > max_transfer_blocks(service) ||
	    request->block_index >= service->info->sector_count ||
	    request->block_count > service->info->sector_count - request->block_index ||
	    request->block_count > UINT32_MAX / service->info->logical_sector_size)
		return false;
	uint32_t size = request->block_count * service->info->logical_sector_size;
	return request->buffer_offset <= buffer->sata.size && size <= buffer->sata.size - request->buffer_offset;
}

static bool dispatch_transfer(struct sata_block_service* service, struct sata_block_session* session,
                              const struct cap_request* call, const void* data, bool write) {
	struct block_transfer_request  request;
	struct block_transfer_response response = {0};
	struct sata_block_buffer*      buffer;
	bool                           success;

	if ((call->rights & (write ? CAP_WRITE : CAP_READ)) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (buffer == NULL || !transfer_valid(service, &request, buffer))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (service->info->lba48)
		success = write ? sata_write_dma_ext(service->transport,
		                                     &buffer->sata,
		                                     request.buffer_offset,
		                                     request.block_index,
		                                     (uint16_t)request.block_count,
		                                     service->info->logical_sector_size)
		                : sata_read_dma_ext(service->transport,
		                                    &buffer->sata,
		                                    request.buffer_offset,
		                                    request.block_index,
		                                    (uint16_t)request.block_count,
		                                    service->info->logical_sector_size);
	else
		success = write ? sata_write_dma(service->transport,
		                                 &buffer->sata,
		                                 request.buffer_offset,
		                                 (uint32_t)request.block_index,
		                                 (uint16_t)request.block_count,
		                                 service->info->logical_sector_size)
		                : sata_read_dma(service->transport,
		                                &buffer->sata,
		                                request.buffer_offset,
		                                (uint32_t)request.block_index,
		                                (uint16_t)request.block_count,
		                                service->info->logical_sector_size);
	if (!success) return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	response.transferred_blocks = request.block_count;
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_flush(struct sata_block_service* service, const struct cap_request* call) {
	bool supported = service->info->flush_cache || service->info->flush_cache_ext;
	bool success;

	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_flush_request))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!supported) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	if (service->info->lba48 && service->info->flush_cache_ext) success = sata_flush_cache_ext(service->transport);
	else success = sata_flush_cache(service->transport);
	return reply(call, NULL, 0u, success ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED);
}

static bool dispatch_release(struct sata_block_service* service, struct sata_block_session* session,
                             const struct cap_request* call, const void* data) {
	struct block_release_buffer_request request;
	struct sata_block_buffer*           buffer;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (request.reserved != 0u || buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	release_buffer(service->transport, buffer);
	return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch(struct sata_block_service* service, const struct cap_request* call, const void* data) {
	struct block_request_header header;
	struct sata_block_session*  session;

	if (data == NULL || call->request_size < sizeof(header)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	if (call->object_id == SATA_BLOCK_ROOT_OBJECT)
		return header.op == BLOCK_OP_OPEN ? dispatch_open(service, call)
		                                  : reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	session = find_session(service, call->object_id);
	if (session == NULL || session->caller != call->caller) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	switch (header.op) {
	case BLOCK_OP_ALLOCATE_BUFFER:
		return dispatch_allocate(service, session, call, data);
	case BLOCK_OP_READ:
		return dispatch_transfer(service, session, call, data, false);
	case BLOCK_OP_WRITE:
		return dispatch_transfer(service, session, call, data, true);
	case BLOCK_OP_FLUSH:
		return dispatch_flush(service, call);
	case BLOCK_OP_RELEASE_BUFFER:
		return dispatch_release(service, session, call, data);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool serve(struct sata_block_service* service) {
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(service->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && !dispatch(service, &call, request_buffer)) return false;
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(service->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && event.type == CHANNEL_EVENT_CAP_ZERO_GRANTS) {
				struct sata_block_session* session = find_session(service, event.object_id);
				if (session != NULL) {
					(void)cap_unpublish_if_unused(service->endpoint, session->object_id);
					release_session(service->transport, session);
				}
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(service->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

static void cleanup(struct sata_block_service* service) {
	for (size_t index = 0u; index < SATA_BLOCK_MAX_SESSIONS; index++) {
		if (!service->sessions[index].active) continue;
		(void)cap_unpublish(service->endpoint, service->sessions[index].object_id);
		release_session(service->transport, &service->sessions[index]);
	}
	if (service->root_cap != CAP_ID_INVALID) (void)cap_unpublish(service->endpoint, SATA_BLOCK_ROOT_OBJECT);
	if (service->activity_cap != CAP_ID_INVALID) (void)cap_drop(service->activity_cap);
	if (service->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(service->endpoint);
}

bool sata_block_service_run(cap_id_t device_cap, const struct sata_transport* transport,
                            const struct sata_device_info* info) {
	struct sata_block_service service = {.endpoint        = CHANNEL_ID_INVALID,
	                                     .activity_cap    = CAP_ID_INVALID,
	                                     .root_cap        = CAP_ID_INVALID,
	                                     .transport       = transport,
	                                     .info            = info,
	                                     .next_session_id = 1u};
	struct self_info          self    = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	bool success = false;

	if (transport == NULL || info == NULL || info->sector_count == 0u || info->logical_sector_size == 0u ||
	    transport->max_transfer_size < info->logical_sector_size || process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	service.pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&service.endpoint, &service.activity_cap) != SYSCALL_STATUS_OK) goto done;
	if (cap_publish(
			service.endpoint, SATA_BLOCK_ROOT_OBJECT, service.pid, SATA_BLOCK_PROVIDER_RIGHTS, &service.root_cap) !=
	    SYSCALL_STATUS_OK)
		goto done;
	if (!create_child(device_cap, service.root_cap, transport, info)) goto done;
	printf("sata: port %u published block:disk (%llu blocks of %u bytes)\n",
	       transport->port,
	       (unsigned long long)info->sector_count,
	       info->logical_sector_size);
	success = serve(&service);

done:
	cleanup(&service);
	return success;
}
