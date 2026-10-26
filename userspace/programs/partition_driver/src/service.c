#include "service.h"

#include <base/channel.h>
#include <base/process.h>
#include <base/signal.h>
#include <protocol/device.h>
#include <runtime/block.h>
#include <runtime/device.h>
#include <runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "table.h"

#define PARTITION_BLOCK_SESSION_OBJECT_BASE 0x100000000ull
#define PARTITION_BLOCK_MAX_SESSIONS 16u
#define PARTITION_BLOCK_MAX_BUFFERS 8u

struct partition_block_buffer {
	bool                used;
	uint64_t            id;
	cap_id_t            client_cap;
	struct block_buffer parent;
};

struct partition_block_session {
	bool                          active;
	uint64_t                      object_id;
	process_id_t                  caller;
	const struct partition_entry* partition;
	uint64_t                      next_buffer_id;
	struct partition_block_buffer buffers[PARTITION_BLOCK_MAX_BUFFERS];
};

struct partition_block_service {
	channel_id_t                   endpoint;
	cap_id_t                       activity_cap;
	process_id_t                   pid;
	const struct block_device*     parent;
	const struct partition_table*  partitions;
	cap_id_t*                      root_caps;
	uint64_t                       next_session_id;
	struct partition_block_session sessions[PARTITION_BLOCK_MAX_SESSIONS];
};

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static cap_rights_t block_rights(const struct block_device* parent) {
	cap_rights_t rights = CAP_CALL | CAP_READ;
	if ((parent->flags & BLOCK_DEVICE_WRITABLE) != 0u) rights |= CAP_WRITE;
	return rights;
}

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

static syscall_status_t add_string(const struct device_builder* builder, const char* name, const char* value) {
	size_t           size = strlen(value);
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UTF8_STRING, 1u, size);
	if (status == SYSCALL_STATUS_OK && size != 0u) status = device_builder_append_property(builder, 0u, value, size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static bool create_child(cap_id_t parent, cap_id_t block_cap, const struct block_device* device,
                         const struct partition_entry* partition) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char                  name[32];
	char                  compatible[5u + PARTITION_GUID_STRING_SIZE];
	char                  type_guid[PARTITION_GUID_STRING_SIZE];
	char                  unique_guid[PARTITION_GUID_STRING_SIZE];
	int                   length;

	length = snprintf(name, sizeof(name), "partition-%u", partition->table_index);
	if (length <= 0 || (size_t)length >= sizeof(name)) return false;
	if (partition->scheme == PARTITION_SCHEME_GPT) {
		partition_format_guid(partition->gpt.type_guid, type_guid);
		partition_format_guid(partition->gpt.unique_guid, unique_guid);
		length = snprintf(compatible, sizeof(compatible), "gpt:%s", type_guid);
	}
	else {
		length = snprintf(compatible, sizeof(compatible), "mbr:%02x", partition->mbr.type);
	}
	if (length <= 0 || (size_t)length >= sizeof(compatible)) return false;

	syscall_status_t status = device_builder_begin_child(parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, strlen(name));
	if (status == SYSCALL_STATUS_OK) status = device_builder_add_compatible(&builder, compatible, strlen(compatible));
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(
			&builder, BLOCK_COMPATIBLE_PARTITION, sizeof(BLOCK_COMPATIBLE_PARTITION) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DEVICE, sizeof(BLOCK_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "partition_index", partition->table_index);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "first_lba", partition->first_lba);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_count", partition->block_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_size", device->block_size);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "max_transfer_blocks", device->max_transfer_blocks);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "writable", (device->flags & BLOCK_DEVICE_WRITABLE) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "removable", (device->flags & BLOCK_DEVICE_REMOVABLE) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "flush_supported", (device->flags & BLOCK_DEVICE_FLUSH_SUPPORTED) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_string(&builder, "partition_scheme", partition->scheme == PARTITION_SCHEME_GPT ? "gpt" : "mbr");
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_string(&builder, "type_guid", type_guid);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_string(&builder, "unique_guid", unique_guid);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_u64(&builder, "attributes", partition->gpt.attributes);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT && partition->gpt.name[0] != '\0')
		status = add_string(&builder, "partition_name", partition->gpt.name);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_MBR)
		status = add_u64(&builder, "partition_type", partition->mbr.type);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_MBR)
		status = add_bool(&builder, "bootable", partition->mbr.bootable);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(
			&builder, BLOCK_RESOURCE, sizeof(BLOCK_RESOURCE) - 1u, block_cap, block_rights(device));
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static const struct partition_entry* find_partition(const struct partition_block_service* service, uint64_t object_id) {
	if (object_id == 0u || object_id > service->partitions->count) return NULL;
	return &service->partitions->entries[object_id - 1u];
}

static struct partition_block_session* find_session(struct partition_block_service* service, uint64_t object_id) {
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_SESSIONS; index++)
		if (service->sessions[index].active && service->sessions[index].object_id == object_id)
			return &service->sessions[index];
	return NULL;
}

static struct partition_block_buffer* find_buffer(struct partition_block_session* session, uint64_t id) {
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_BUFFERS; index++)
		if (session->buffers[index].used && session->buffers[index].id == id) return &session->buffers[index];
	return NULL;
}

static void release_buffer(const struct block_device* parent, struct partition_block_buffer* buffer) {
	if (buffer == NULL || !buffer->used) return;
	if (buffer->client_cap != CAP_ID_INVALID) (void)cap_revoke(buffer->client_cap, 0u);
	block_buffer_release(parent, &buffer->parent);
	*buffer = (struct partition_block_buffer){
		.client_cap = CAP_ID_INVALID,
		.parent = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
}

static void release_session(const struct block_device* parent, struct partition_block_session* session) {
	if (session == NULL || !session->active) return;
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_BUFFERS; index++)
		release_buffer(parent, &session->buffers[index]);
	*session = (struct partition_block_session){0};
}

static bool dispatch_open(struct partition_block_service* service, const struct partition_entry* partition,
                          const struct cap_request* call) {
	struct block_open_response      response = {0};
	struct partition_block_session* session  = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_open_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_SESSIONS; index++)
		if (!service->sessions[index].active) {
			session = &service->sessions[index];
			break;
		}
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*session =
		(struct partition_block_session){.active    = true,
		                                 .object_id = PARTITION_BLOCK_SESSION_OBJECT_BASE + service->next_session_id++,
		                                 .caller    = call->caller,
		                                 .partition = partition,
		                                 .next_buffer_id = 1u};
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_BUFFERS; index++) {
		session->buffers[index].client_cap               = CAP_ID_INVALID;
		session->buffers[index].parent.memory_cap        = CAP_ID_INVALID;
		session->buffers[index].parent.address_space_cap = CAP_ID_INVALID;
		session->buffers[index].parent.mapping_cap       = CAP_ID_INVALID;
	}
	response.block_count         = partition->block_count;
	response.block_size          = service->parent->block_size;
	response.max_transfer_blocks = service->parent->max_transfer_blocks;
	response.flags               = service->parent->flags & BLOCK_DEVICE_REMOVABLE;
	if ((call->rights & CAP_WRITE) != 0u && (service->parent->flags & BLOCK_DEVICE_WRITABLE) != 0u) {
		response.flags |= BLOCK_DEVICE_WRITABLE;
		response.flags |= service->parent->flags & BLOCK_DEVICE_FLUSH_SUPPORTED;
	}
	if (cap_publish(service->endpoint,
	                session->object_id,
	                call->caller,
	                call->rights & block_rights(service->parent),
	                &response.session_cap) != SYSCALL_STATUS_OK) {
		release_session(service->parent, session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(service->endpoint, session->object_id);
	release_session(service->parent, session);
	return false;
}

static bool dispatch_allocate(struct partition_block_service* service, struct partition_block_session* session,
                              const struct cap_request* call, const void* data) {
	struct block_allocate_buffer_request  request;
	struct block_allocate_buffer_response response = {0};
	struct partition_block_buffer*        buffer   = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.size == 0u || request.size > service->parent->max_transfer_blocks * service->parent->block_size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_BUFFERS; index++)
		if (!session->buffers[index].used) {
			buffer = &session->buffers[index];
			break;
		}
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*buffer = (struct partition_block_buffer){
		.used       = true,
		.id         = session->next_buffer_id++,
		.client_cap = CAP_ID_INVALID,
		.parent = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID}
    };
	if (block_buffer_allocate(service->parent, request.size, &buffer->parent) != SYSCALL_STATUS_OK ||
	    cap_delegate(buffer->parent.memory_cap, call->caller, BLOCK_BUFFER_RIGHTS, &buffer->client_cap) !=
	        SYSCALL_STATUS_OK) {
		release_buffer(service->parent, buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	response.buffer_id  = buffer->id;
	response.memory_cap = buffer->client_cap;
	response.size       = buffer->parent.size;
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	release_buffer(service->parent, buffer);
	return false;
}

static bool transfer_valid(const struct partition_block_service* service, const struct partition_block_session* session,
                           const struct block_transfer_request* request, const struct partition_block_buffer* buffer) {
	if (request->reserved != 0u || request->block_count == 0u ||
	    request->block_count > service->parent->max_transfer_blocks ||
	    request->block_index >= session->partition->block_count ||
	    request->block_count > session->partition->block_count - request->block_index ||
	    request->block_count > UINT32_MAX / service->parent->block_size)
		return false;
	uint32_t size = request->block_count * service->parent->block_size;
	return request->buffer_offset <= buffer->parent.size && size <= buffer->parent.size - request->buffer_offset;
}

static bool dispatch_transfer(struct partition_block_service* service, struct partition_block_session* session,
                              const struct cap_request* call, const void* data, bool write) {
	struct block_transfer_request  request;
	struct block_transfer_response response = {0};
	struct partition_block_buffer* buffer;
	syscall_status_t               status;

	if ((call->rights & (write ? CAP_WRITE : CAP_READ)) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (buffer == NULL || !transfer_valid(service, session, &request, buffer))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	uint64_t parent_lba = session->partition->first_lba + request.block_index;
	status = write
	             ? block_write(service->parent, &buffer->parent, request.buffer_offset, parent_lba, request.block_count)
	             : block_read(service->parent, &buffer->parent, request.buffer_offset, parent_lba, request.block_count);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response.transferred_blocks = request.block_count;
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_flush(struct partition_block_service* service, const struct cap_request* call) {
	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct block_flush_request))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status = block_flush(service->parent);
	return reply(call, NULL, 0u, status);
}

static bool dispatch_release(struct partition_block_service* service, struct partition_block_session* session,
                             const struct cap_request* call, const void* data) {
	struct block_release_buffer_request request;
	struct partition_block_buffer*      buffer;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (request.reserved != 0u || buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	release_buffer(service->parent, buffer);
	return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch(struct partition_block_service* service, const struct cap_request* call, const void* data) {
	struct block_request_header     header;
	const struct partition_entry*   partition;
	struct partition_block_session* session;

	if (data == NULL || call->request_size < sizeof(header)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	partition = find_partition(service, call->object_id);
	if (partition != NULL)
		return header.op == BLOCK_OP_OPEN ? dispatch_open(service, partition, call)
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

static bool serve(struct partition_block_service* service) {
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
				struct partition_block_session* session = find_session(service, event.object_id);
				if (session != NULL) {
					(void)cap_unpublish_if_unused(service->endpoint, session->object_id);
					release_session(service->parent, session);
				}
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(service->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

static void cleanup(struct partition_block_service* service) {
	for (size_t index = 0u; index < PARTITION_BLOCK_MAX_SESSIONS; index++) {
		if (!service->sessions[index].active) continue;
		(void)cap_unpublish(service->endpoint, service->sessions[index].object_id);
		release_session(service->parent, &service->sessions[index]);
	}
	if (service->root_caps != NULL)
		for (size_t index = 0u; index < service->partitions->count; index++)
			if (service->root_caps[index] != CAP_ID_INVALID) (void)cap_unpublish(service->endpoint, index + 1u);
	free(service->root_caps);
	if (service->activity_cap != CAP_ID_INVALID) (void)cap_drop(service->activity_cap);
	if (service->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(service->endpoint);
}

bool partition_service_run(cap_id_t device_cap, const struct block_device* parent,
                           const struct partition_table* partitions) {
	struct partition_block_service service = {.endpoint        = CHANNEL_ID_INVALID,
	                                          .activity_cap    = CAP_ID_INVALID,
	                                          .parent          = parent,
	                                          .partitions      = partitions,
	                                          .next_session_id = 1u};
	struct self_info               self    = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	bool success = false;

	if (parent == NULL || partitions == NULL || partitions->count == 0u ||
	    partitions->count > SIZE_MAX / sizeof(*service.root_caps) ||
	    partitions->count >= PARTITION_BLOCK_SESSION_OBJECT_BASE || process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	service.pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	service.root_caps = malloc(partitions->count * sizeof(*service.root_caps));
	if (service.root_caps == NULL) return false;
	for (size_t index = 0u; index < partitions->count; index++) service.root_caps[index] = CAP_ID_INVALID;
	if (channel_create(&service.endpoint, &service.activity_cap) != SYSCALL_STATUS_OK) goto done;
	for (size_t index = 0u; index < partitions->count; index++) {
		if (cap_publish(service.endpoint,
		                index + 1u,
		                service.pid,
		                block_rights(parent) | CAP_DELEGATE,
		                &service.root_caps[index]) != SYSCALL_STATUS_OK ||
		    !create_child(device_cap, service.root_caps[index], parent, &partitions->entries[index]))
			goto done;
		char precise[5u + PARTITION_GUID_STRING_SIZE];
		if (partitions->entries[index].scheme == PARTITION_SCHEME_GPT) {
			char guid[PARTITION_GUID_STRING_SIZE];
			partition_format_guid(partitions->entries[index].gpt.type_guid, guid);
			(void)snprintf(precise, sizeof(precise), "gpt:%s", guid);
		}
		else {
			(void)snprintf(precise, sizeof(precise), "mbr:%02x", partitions->entries[index].mbr.type);
		}
		RUNTIME_DEBUG_LOG("partition: published partition-%u as block:partition (%s, first_lba=%llu, blocks=%llu)\n",
		                  partitions->entries[index].table_index,
		                  precise,
		                  (unsigned long long)partitions->entries[index].first_lba,
		                  (unsigned long long)partitions->entries[index].block_count);
	}
	printf("partition: published %zu partition device%s\n", partitions->count, partitions->count == 1u ? "" : "s");
	success = serve(&service);

done:
	cleanup(&service);
	return success;
}
