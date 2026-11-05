#include <ata/block_service.h>
#include <ata/transport.h>
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

#define ATA_BLOCK_ROOT_OBJECT 1u
#define ATA_BLOCK_SLICE_OBJECT_BASE 0x10000ull
#define ATA_BLOCK_SESSION_OBJECT_BASE 0x100000000ull
#define ATA_BLOCK_MAX_SLICES 32u
#define ATA_BLOCK_MAX_SESSIONS 8u
#define ATA_BLOCK_MAX_BUFFERS 8u
#define ATA_BLOCK_PROVIDER_RIGHTS (BLOCK_DRIVER_RIGHTS | CAP_DELEGATE)
#define ATA_BLOCK_SESSION_RIGHTS BLOCK_DRIVER_RIGHTS

struct ata_block_buffer {
	bool              used;
	uint64_t          id;
	cap_id_t          client_cap;
	struct ata_buffer sata;
};

struct ata_block_session {
	bool                    active;
	uint64_t                object_id;
	process_id_t            caller;
	uint64_t                block_offset;
	uint64_t                block_count;
	uint64_t                next_buffer_id;
	struct ata_block_buffer buffers[ATA_BLOCK_MAX_BUFFERS];
};

struct ata_block_slice {
	bool     active;
	uint64_t object_id;
	uint64_t block_offset;
	uint64_t block_count;
};

struct ata_block_service {
	channel_id_t                    endpoint;
	cap_id_t                        activity_cap;
	cap_id_t                        root_cap;
	process_id_t                    pid;
	const struct ata_transport*     transport;
	const struct ata_block_info*    info;
	const struct ata_block_backend* backend;
	uint64_t                        next_slice_id;
	uint64_t                        next_session_id;
	struct ata_block_slice          slices[ATA_BLOCK_MAX_SLICES];
	struct ata_block_session        sessions[ATA_BLOCK_MAX_SESSIONS];
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

static bool create_child(cap_id_t parent, cap_id_t block_cap, const struct ata_block_info* info) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	syscall_status_t      status  = device_builder_begin_child(parent, &builder);

	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, info->name, strlen(info->name));
	if (status == SYSCALL_STATUS_OK && info->compatible != NULL)
		status = device_builder_add_compatible(&builder, info->compatible, strlen(info->compatible));
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DISK, sizeof(BLOCK_COMPATIBLE_DISK) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DEVICE, sizeof(BLOCK_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_count", info->block_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_size", info->block_size);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "max_transfer_blocks", info->max_transfer_blocks);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "writable", info->writable);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "removable", info->removable);
	if (status == SYSCALL_STATUS_OK) status = add_bool(&builder, "flush_supported", info->flush_supported);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
		                                     BLOCK_RESOURCE,
		                                     sizeof(BLOCK_RESOURCE) - 1u,
		                                     block_cap,
		                                     info->writable ? BLOCK_DRIVER_RIGHTS : BLOCK_READER_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static struct ata_block_session* find_session(struct ata_block_service* service, uint64_t object_id) {
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SESSIONS; index++)
		if (service->sessions[index].active && service->sessions[index].object_id == object_id)
			return &service->sessions[index];
	return NULL;
}

static bool find_region(struct ata_block_service* service, uint64_t object_id, uint64_t* out_offset,
                        uint64_t* out_count) {
	if (object_id == ATA_BLOCK_ROOT_OBJECT) {
		*out_offset = 0u;
		*out_count  = service->info->block_count;
		return true;
	}
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SLICES; index++) {
		const struct ata_block_slice* slice = &service->slices[index];
		if (slice->active && slice->object_id == object_id) {
			*out_offset = slice->block_offset;
			*out_count  = slice->block_count;
			return true;
		}
	}
	return false;
}

static struct ata_block_slice* find_slice(struct ata_block_service* service, uint64_t object_id) {
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SLICES; index++)
		if (service->slices[index].active && service->slices[index].object_id == object_id)
			return &service->slices[index];
	return NULL;
}

static struct ata_block_buffer* find_buffer(struct ata_block_session* session, uint64_t id) {
	for (size_t index = 0u; index < ATA_BLOCK_MAX_BUFFERS; index++)
		if (session->buffers[index].used && session->buffers[index].id == id) return &session->buffers[index];
	return NULL;
}

static void release_buffer(const struct ata_transport* transport, struct ata_block_buffer* buffer) {
	if (buffer == NULL || !buffer->used) return;
	if (buffer->client_cap != CAP_ID_INVALID) (void)cap_revoke(buffer->client_cap, 0u);
	ata_buffer_release(transport, &buffer->sata);
	*buffer = (struct ata_block_buffer){
		.client_cap = CAP_ID_INVALID,
		.sata = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
}

static void release_session(const struct ata_transport* transport, struct ata_block_session* session) {
	if (session == NULL || !session->active) return;
	for (size_t index = 0u; index < ATA_BLOCK_MAX_BUFFERS; index++) release_buffer(transport, &session->buffers[index]);
	*session = (struct ata_block_session){0};
}

static uint32_t max_transfer_blocks(const struct ata_block_service* service) {
	return service->info->max_transfer_blocks;
}

static bool dispatch_open(struct ata_block_service* service, uint64_t block_offset, uint64_t block_count,
                          const struct cap_request* call) {
	struct block_open_response response = {0};
	struct ata_block_session*  session  = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_open_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SESSIONS; index++)
		if (!service->sessions[index].active) {
			session = &service->sessions[index];
			break;
		}
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*session = (struct ata_block_session){.active         = true,
	                                      .object_id      = ATA_BLOCK_SESSION_OBJECT_BASE + service->next_session_id++,
	                                      .caller         = call->caller,
	                                      .block_offset   = block_offset,
	                                      .block_count    = block_count,
	                                      .next_buffer_id = 1u};
	for (size_t index = 0u; index < ATA_BLOCK_MAX_BUFFERS; index++) {
		session->buffers[index].client_cap             = CAP_ID_INVALID;
		session->buffers[index].sata.memory_cap        = CAP_ID_INVALID;
		session->buffers[index].sata.address_space_cap = CAP_ID_INVALID;
		session->buffers[index].sata.mapping_cap       = CAP_ID_INVALID;
	}
	response.block_count         = block_count;
	response.block_size          = service->info->block_size;
	response.max_transfer_blocks = max_transfer_blocks(service);
	response.flags = service->info->writable && (call->rights & CAP_WRITE) != 0u ? BLOCK_DEVICE_WRITABLE : 0u;
	if (service->info->removable) response.flags |= BLOCK_DEVICE_REMOVABLE;
	if (service->info->writable && (call->rights & CAP_WRITE) != 0u && service->info->flush_supported)
		response.flags |= BLOCK_DEVICE_FLUSH_SUPPORTED;
	if (cap_publish(service->endpoint,
	                session->object_id,
	                call->caller,
	                call->rights & (service->info->writable ? ATA_BLOCK_SESSION_RIGHTS : BLOCK_READER_RIGHTS),
	                &response.session_cap) != SYSCALL_STATUS_OK) {
		release_session(service->transport, session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, session->object_id);
	release_session(service->transport, session);
	return false;
}

static bool dispatch_slice(struct ata_block_service* service, uint64_t parent_offset, uint64_t parent_count,
                           const struct cap_request* call, const void* data) {
	struct block_slice_request  request;
	struct block_slice_response response = {.root_cap = CAP_ID_INVALID};
	struct ata_block_slice*     slice    = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.block_count == 0u || request.block_index >= parent_count ||
	    request.block_count > parent_count - request.block_index)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SLICES; index++)
		if (!service->slices[index].active) {
			slice = &service->slices[index];
			break;
		}
	if (slice == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*slice = (struct ata_block_slice){.active       = true,
	                                  .object_id    = ATA_BLOCK_SLICE_OBJECT_BASE + service->next_slice_id++,
	                                  .block_offset = parent_offset + request.block_index,
	                                  .block_count  = request.block_count};
	if (cap_publish(service->endpoint,
	                slice->object_id,
	                call->caller,
	                (call->rights & ATA_BLOCK_SESSION_RIGHTS) | CAP_DELEGATE,
	                &response.root_cap) != SYSCALL_STATUS_OK) {
		*slice = (struct ata_block_slice){0};
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, slice->object_id);
	*slice = (struct ata_block_slice){0};
	return false;
}

static bool dispatch_allocate(struct ata_block_service* service, struct ata_block_session* session,
                              const struct cap_request* call, const void* data) {
	struct block_allocate_buffer_request  request;
	struct block_allocate_buffer_response response = {0};
	struct ata_block_buffer*              buffer   = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.size == 0u || request.size > service->transport->max_transfer_size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < ATA_BLOCK_MAX_BUFFERS; index++)
		if (!session->buffers[index].used) {
			buffer = &session->buffers[index];
			break;
		}
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*buffer = (struct ata_block_buffer){
		.used       = true,
		.id         = session->next_buffer_id++,
		.client_cap = CAP_ID_INVALID,
		.sata = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
	if (!ata_buffer_allocate(service->transport, request.size, &buffer->sata) ||
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

static bool transfer_valid(const struct ata_block_service* service, const struct ata_block_session* session,
                           const struct block_transfer_request* request, const struct ata_block_buffer* buffer) {
	if (request->reserved != 0u || request->block_count == 0u || request->block_count > max_transfer_blocks(service) ||
	    request->block_index >= session->block_count ||
	    request->block_count > session->block_count - request->block_index ||
	    request->block_count > UINT32_MAX / service->info->block_size)
		return false;
	uint32_t size = request->block_count * service->info->block_size;
	return request->buffer_offset <= buffer->sata.size && size <= buffer->sata.size - request->buffer_offset;
}

static bool dispatch_transfer(struct ata_block_service* service, struct ata_block_session* session,
                              const struct cap_request* call, const void* data, bool write) {
	struct block_transfer_request  request;
	struct block_transfer_response response = {0};
	struct ata_block_buffer*       buffer;
	bool                           success;

	if ((call->rights & (write ? CAP_WRITE : CAP_READ)) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (buffer == NULL || !transfer_valid(service, session, &request, buffer))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	uint64_t block_index = session->block_offset + request.block_index;
	if (write)
		success =
			service->backend->write != NULL &&
			service->backend->write(
				service->backend->context, &buffer->sata, request.buffer_offset, block_index, request.block_count);
	else
		success = service->backend->read(
			service->backend->context, &buffer->sata, request.buffer_offset, block_index, request.block_count);
	if (!success) return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	response.transferred_blocks = request.block_count;
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_flush(struct ata_block_service* service, const struct cap_request* call) {
	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_flush_request))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!service->info->flush_supported || service->backend->flush == NULL)
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	bool success = service->backend->flush(service->backend->context);
	return reply(call, NULL, 0u, success ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED);
}

static bool dispatch_release(struct ata_block_service* service, struct ata_block_session* session,
                             const struct cap_request* call, const void* data) {
	struct block_release_buffer_request request;
	struct ata_block_buffer*            buffer;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (request.reserved != 0u || buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	release_buffer(service->transport, buffer);
	return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch(struct ata_block_service* service, const struct cap_request* call, const void* data) {
	struct block_request_header header;
	struct ata_block_session*   session;
	uint64_t                    block_offset;
	uint64_t                    block_count;

	if (data == NULL || call->request_size < sizeof(header)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	if (find_region(service, call->object_id, &block_offset, &block_count)) {
		if (header.op == BLOCK_OP_OPEN) return dispatch_open(service, block_offset, block_count, call);
		if (header.op == BLOCK_OP_SLICE) return dispatch_slice(service, block_offset, block_count, call, data);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
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

static bool serve(struct ata_block_service* service) {
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
				struct ata_block_session* session = find_session(service, event.object_id);
				if (session != NULL) {
					(void)cap_unpublish_if_unused(service->endpoint, session->object_id);
					release_session(service->transport, session);
				}
				else {
					struct ata_block_slice* slice = find_slice(service, event.object_id);
					if (slice != NULL &&
					    cap_unpublish_if_unused(service->endpoint, slice->object_id) == SYSCALL_STATUS_OK)
						*slice = (struct ata_block_slice){0};
				}
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(service->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

static void cleanup(struct ata_block_service* service) {
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SESSIONS; index++) {
		if (!service->sessions[index].active) continue;
		(void)cap_unpublish(service->endpoint, service->sessions[index].object_id);
		release_session(service->transport, &service->sessions[index]);
	}
	for (size_t index = 0u; index < ATA_BLOCK_MAX_SLICES; index++)
		if (service->slices[index].active) (void)cap_unpublish(service->endpoint, service->slices[index].object_id);
	if (service->root_cap != CAP_ID_INVALID) (void)cap_unpublish(service->endpoint, ATA_BLOCK_ROOT_OBJECT);
	if (service->activity_cap != CAP_ID_INVALID) (void)cap_drop(service->activity_cap);
	if (service->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(service->endpoint);
}

bool ata_block_service_run(cap_id_t device_cap, const struct ata_transport* transport,
                           const struct ata_block_info* info, const struct ata_block_backend* backend) {
	struct ata_block_service service = {.endpoint        = CHANNEL_ID_INVALID,
	                                    .activity_cap    = CAP_ID_INVALID,
	                                    .root_cap        = CAP_ID_INVALID,
	                                    .transport       = transport,
	                                    .info            = info,
	                                    .backend         = backend,
	                                    .next_slice_id   = 1u,
	                                    .next_session_id = 1u};
	struct self_info         self    = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	bool success = false;

	if (transport == NULL || info == NULL || backend == NULL || backend->read == NULL || info->name == NULL ||
	    info->block_count == 0u || info->block_size == 0u || info->max_transfer_blocks == 0u ||
	    info->max_transfer_blocks > UINT32_MAX / info->block_size ||
	    info->max_transfer_blocks > transport->max_transfer_size / info->block_size ||
	    process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	service.pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&service.endpoint, &service.activity_cap) != SYSCALL_STATUS_OK) goto done;
	if (cap_publish(
			service.endpoint, ATA_BLOCK_ROOT_OBJECT, service.pid, ATA_BLOCK_PROVIDER_RIGHTS, &service.root_cap) !=
	    SYSCALL_STATUS_OK)
		goto done;
	if (!create_child(device_cap, service.root_cap, info)) goto done;
	printf("%s: port %u published read%s block:%s (%llu blocks of %u bytes)\n",
	       info->name,
	       transport->port,
	       info->writable ? "/write" : "-only",
	       info->name,
	       (unsigned long long)info->block_count,
	       info->block_size);
	success = serve(&service);

done:
	cleanup(&service);
	return success;
}
