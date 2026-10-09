#include "server.h"

#include <protocol/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* response, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, response, size, status) == SYSCALL_STATUS_OK;
}

static bool reply_status(const struct cap_request* call, syscall_status_t status) {
	return reply(call, NULL, 0u, status);
}

static bool has_rights(const struct cap_request* call, cap_rights_t rights) {
	return (call->rights & rights) == rights;
}

static bool exact_request(const struct cap_request* call, size_t size) {
	return call->request_size == size;
}

static bool variable_request(const struct cap_request* call, size_t header_size, size_t tail_size) {
	return tail_size <= SIZE_MAX - header_size && call->request_size == header_size + tail_size;
}

static bool begin_builder(struct device_server* server, const struct cap_request* call, struct dm_device* parent) {
	struct device_builder_begin_response response = {0};
	struct dm_builder*                   builder  = NULL;
	syscall_status_t                     status;
	if (call->response_capacity < sizeof(response)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	status = dm_builder_begin(&server->state, parent, &builder);
	if (status != SYSCALL_STATUS_OK) return reply_status(call, status);
	status = cap_publish(
		server->endpoint, builder->object_id, call->caller, DEVICE_BUILDER_CAP_RIGHTS, &response.builder_cap);
	if (status != SYSCALL_STATUS_OK) {
		dm_builder_abort(&server->state, builder);
		return reply_status(call, status);
	}
	response.manager_pid = server->pid;
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(server->endpoint, builder->object_id);
	dm_builder_abort(&server->state, builder);
	return false;
}

static bool read_slice(const struct cap_request* call, const void* value, size_t value_size, uint64_t offset,
                       uint64_t size) {
	if (offset > value_size || size > value_size - offset || size > CAP_MAX_RESPONSE_SIZE)
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (size > call->response_capacity) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	const void* response = size == 0u ? NULL : (const uint8_t*)value + (size_t)offset;
	return reply(call, response, (size_t)size, SYSCALL_STATUS_OK);
}

static bool dispatch_root(struct device_server* server, const struct cap_request* call, const void* data) {
	struct device_root_begin_request request;
	if (!has_rights(call, CAP_MANAGE)) return reply_status(call, SYSCALL_STATUS_DENIED);
	if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.op != DEVICE_ROOT_OP_BEGIN) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return begin_builder(server, call, NULL);
}

static bool builder_set_name(struct dm_builder* builder, const struct cap_request* call, const void* data) {
	struct device_builder_set_name_request request;
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (!variable_request(call, sizeof(request), request.name_size))
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_status(call, dm_builder_set_name(builder, (const uint8_t*)data + sizeof(request), request.name_size));
}

static bool builder_add_compatible(struct dm_builder* builder, const struct cap_request* call, const void* data) {
	struct device_builder_add_compatible_request request;
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (!variable_request(call, sizeof(request), request.id_size))
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_status(call,
	                    dm_builder_add_compatible(builder, (const uint8_t*)data + sizeof(request), request.id_size));
}

static bool builder_begin_property(struct dm_builder* builder, const struct cap_request* call, const void* data) {
	struct device_builder_begin_property_request request;
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || !variable_request(call, sizeof(request), request.name_size))
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_status(call,
	                    dm_builder_begin_property(builder,
	                                              (const uint8_t*)data + sizeof(request),
	                                              request.name_size,
	                                              request.type,
	                                              request.element_count,
	                                              request.value_size));
}

static bool builder_append_property(struct dm_builder* builder, const struct cap_request* call, const void* data) {
	struct device_builder_append_property_request request;
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_status(
		call,
		dm_builder_append_property(
			builder, request.offset, (const uint8_t*)data + sizeof(request), call->request_size - sizeof(request)));
}

static bool builder_add_resource(struct dm_builder* builder, const struct cap_request* call, const void* data) {
	struct device_builder_add_resource_request request;
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (!variable_request(call, sizeof(request), request.name_size))
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_status(
		call,
		dm_builder_add_resource(
			builder, (const uint8_t*)data + sizeof(request), request.name_size, request.capability, request.rights));
}

static bool builder_commit(struct device_server* server, struct dm_builder* builder, const struct cap_request* call) {
	struct dm_device* device            = NULL;
	uint64_t          builder_object_id = builder->object_id;
	syscall_status_t  status            = dm_builder_validate_commit(builder);
	if (status != SYSCALL_STATUS_OK) return reply_status(call, status);
	status = cap_unpublish(server->endpoint, builder_object_id);
	if (status != SYSCALL_STATUS_OK) return reply_status(call, status);
	status = dm_builder_commit(&server->state, builder, &device);
	if (status != SYSCALL_STATUS_OK) {
		dm_builder_abort(&server->state, builder);
		return reply_status(call, status);
	}
	status = cap_publish(server->endpoint, device->id, server->pid, DEVICE_CAP_DRIVER_RIGHTS, &device->manager_cap);
	if (status != SYSCALL_STATUS_OK) {
		if (device->manager_cap != CAP_ID_INVALID) (void)cap_unpublish(server->endpoint, device->id);
		dm_device_rollback(&server->state, device);
		return reply_status(call, status);
	}
	if (reply_status(call, SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(server->endpoint, device->id);
	dm_device_rollback(&server->state, device);
	return false;
}

static bool dispatch_builder(struct device_server* server, struct dm_builder* builder, const struct cap_request* call,
                             const void* data) {
	struct device_builder_request_header header;
	if (!has_rights(call, CAP_CALL)) return reply_status(call, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(header)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	switch (header.op) {
	case DEVICE_BUILDER_OP_SET_NAME:
		return builder_set_name(builder, call, data);
	case DEVICE_BUILDER_OP_ADD_COMPATIBLE:
		return builder_add_compatible(builder, call, data);
	case DEVICE_BUILDER_OP_BEGIN_PROPERTY:
		return builder_begin_property(builder, call, data);
	case DEVICE_BUILDER_OP_APPEND_PROPERTY:
		return builder_append_property(builder, call, data);
	case DEVICE_BUILDER_OP_FINISH_PROPERTY:
		if (!exact_request(call, sizeof(struct device_builder_simple_request)))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_status(call, dm_builder_finish_property(builder));
	case DEVICE_BUILDER_OP_ADD_RESOURCE:
		return builder_add_resource(builder, call, data);
	case DEVICE_BUILDER_OP_COMMIT:
		if (!exact_request(call, sizeof(struct device_builder_simple_request)))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return builder_commit(server, builder, call);
	case DEVICE_BUILDER_OP_ABORT:
		if (!exact_request(call, sizeof(struct device_builder_simple_request)))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		syscall_status_t status = cap_unpublish(server->endpoint, builder->object_id);
		if (status != SYSCALL_STATUS_OK) return reply_status(call, status);
		dm_builder_abort(&server->state, builder);
		return reply_status(call, SYSCALL_STATUS_OK);
	default:
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static const struct dm_property* property_from_request(const struct dm_device* device, const void* data,
                                                       size_t header_size, uint32_t name_size) {
	return dm_device_find_property(device, (const uint8_t*)data + header_size, name_size);
}

static const struct dm_resource* resource_at(const struct dm_device* device, uint64_t index) {
	const struct dm_resource* resource = device->resources;
	while (resource != NULL && index-- != 0u) resource = resource->next;
	return resource;
}

static bool dispatch_device_read(const struct dm_device* device, const struct cap_request* call, const void* data,
                                 enum device_object_op op) {
	if (!has_rights(call, CAP_READ)) return reply_status(call, SYSCALL_STATUS_DENIED);
	switch (op) {
	case DEVICE_OBJECT_OP_INFO: {
		const struct device_info info = {
			.name_size        = device->name.size,
			.compatible_count = device->compatible_count,
			.property_count   = device->property_count,
			.resource_count   = device->resource_count,
		};
		if (!exact_request(call, sizeof(struct device_object_simple_request)))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		if (call->response_capacity < sizeof(info)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply(call, &info, sizeof(info), SYSCALL_STATUS_OK);
	}
	case DEVICE_OBJECT_OP_READ_NAME: {
		struct device_object_read_index_request request;
		if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (request.reserved != 0u || request.index != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		if (device->name.data == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		return read_slice(call, device->name.data, device->name.size, request.offset, request.size);
	}
	case DEVICE_OBJECT_OP_COMPATIBLE_INFO: {
		struct device_object_index_request request;
		if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (request.reserved != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		if (request.index >= device->compatible_count) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		uint64_t size = device->compatible_ids[request.index].size;
		if (call->response_capacity < sizeof(size)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply(call, &size, sizeof(size), SYSCALL_STATUS_OK);
	}
	case DEVICE_OBJECT_OP_READ_COMPATIBLE: {
		struct device_object_read_index_request request;
		if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (request.reserved != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		if (request.index >= device->compatible_count) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		return read_slice(call,
		                  device->compatible_ids[request.index].data,
		                  device->compatible_ids[request.index].size,
		                  request.offset,
		                  request.size);
	}
	case DEVICE_OBJECT_OP_PROPERTY_INFO: {
		struct device_object_property_info_request request;
		if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (!variable_request(call, sizeof(request), request.name_size) ||
		    !dm_identifier_valid((const uint8_t*)data + sizeof(request), request.name_size))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		const struct dm_property* property = property_from_request(device, data, sizeof(request), request.name_size);
		if (property == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		const struct device_property_info info = {
			.type          = property->type,
			.element_count = property->element_count,
			.value_size    = property->value_size,
		};
		if (call->response_capacity < sizeof(info)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply(call, &info, sizeof(info), SYSCALL_STATUS_OK);
	}
	case DEVICE_OBJECT_OP_READ_PROPERTY: {
		struct device_object_property_read_request request;
		if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (!variable_request(call, sizeof(request), request.name_size) ||
		    !dm_identifier_valid((const uint8_t*)data + sizeof(request), request.name_size))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		const struct dm_property* property = property_from_request(device, data, sizeof(request), request.name_size);
		if (property == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		return read_slice(call, property->value, property->value_size, request.offset, request.size);
	}
	case DEVICE_OBJECT_OP_RESOURCE_INFO: {
		struct device_object_index_request request;
		if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (request.reserved != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		const struct dm_resource* resource = resource_at(device, request.index);
		if (resource == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		const struct device_resource_info info = {.name_size = resource->name.size, .rights = resource->rights};
		if (call->response_capacity < sizeof(info)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply(call, &info, sizeof(info), SYSCALL_STATUS_OK);
	}
	case DEVICE_OBJECT_OP_READ_RESOURCE_NAME: {
		struct device_object_read_index_request request;
		if (!exact_request(call, sizeof(request))) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&request, data, sizeof(request));
		if (request.reserved != 0u) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		const struct dm_resource* resource = resource_at(device, request.index);
		if (resource == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
		return read_slice(call, resource->name.data, resource->name.size, request.offset, request.size);
	}
	default:
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool acquire_resource(const struct dm_device* device, const struct cap_request* call, const void* data) {
	struct device_object_resource_acquire_request  request;
	struct device_object_resource_acquire_response response = {.capability = CAP_ID_INVALID};
	if (!has_rights(call, CAP_READ)) return reply_status(call, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(request)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.rights == 0u || !variable_request(call, sizeof(request), request.name_size) ||
	    !dm_identifier_valid((const uint8_t*)data + sizeof(request), request.name_size))
		return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->response_capacity < sizeof(response)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	const struct dm_resource* resource =
		dm_device_find_resource(device, (const uint8_t*)data + sizeof(request), request.name_size);
	if (resource == NULL) return reply_status(call, SYSCALL_STATUS_UNAVAILABLE);
	if ((request.rights & ~resource->rights) != 0u) return reply_status(call, SYSCALL_STATUS_DENIED);
	syscall_status_t status = cap_delegate(resource->capability, call->caller, request.rights, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply_status(call, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch_device(struct device_server* server, struct dm_device* device, const struct cap_request* call,
                            const void* data) {
	struct device_object_request_header header;
	if (!has_rights(call, CAP_CALL)) return reply_status(call, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(header)) return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	if (header.op == DEVICE_OBJECT_OP_BEGIN_CHILD) {
		if (!has_rights(call, CAP_MANAGE)) return reply_status(call, SYSCALL_STATUS_DENIED);
		if (!exact_request(call, sizeof(struct device_object_simple_request)))
			return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return begin_builder(server, call, device);
	}
	if (header.op == DEVICE_OBJECT_OP_ACQUIRE_RESOURCE) return acquire_resource(device, call, data);
	return dispatch_device_read(device, call, data, header.op);
}

bool device_server_dispatch(struct device_server* server, const struct cap_request* call, const void* request) {
	struct dm_builder* builder;
	struct dm_device*  device;
	if (server == NULL || call == NULL || (request == NULL && call->request_size != 0u)) return false;
	if (!has_rights(call, CAP_CALL)) return reply_status(call, SYSCALL_STATUS_DENIED);
	if (call->object_id == DEVICE_ROOT_OBJECT_ID) return dispatch_root(server, call, request);
	builder = dm_state_find_builder(&server->state, call->object_id);
	if (builder != NULL) return dispatch_builder(server, builder, call, request);
	device = dm_state_find_device(&server->state, call->object_id);
	if (device != NULL) return dispatch_device(server, device, call, request);
	return reply_status(call, SYSCALL_STATUS_BAD_ARGUMENT);
}

void device_server_handle_event(struct device_server* server, const struct channel_event* event) {
	struct dm_builder* builder;
	if (server == NULL || event == NULL || event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->reserved != 0u)
		return;
	if (event->object_id == DEVICE_ROOT_OBJECT_ID) {
		size_t root_count = 0u;
		if (!server->root_published ||
		    cap_unpublish_if_unused(server->endpoint, DEVICE_ROOT_OBJECT_ID) != SYSCALL_STATUS_OK)
			return;
		server->root_cap       = CAP_ID_INVALID;
		server->root_published = false;
		for (const struct dm_device* device = server->state.devices; device != NULL; device = device->next)
			if (device->parent == NULL) root_count++;
		printf("device-manager: parser completed with %zu root device(s)\n", root_count);
		return;
	}
	builder = dm_state_find_builder(&server->state, event->object_id);
	if (builder == NULL || cap_unpublish_if_unused(server->endpoint, event->object_id) != SYSCALL_STATUS_OK) return;
	dm_builder_abort(&server->state, builder);
}

bool device_server_init(struct device_server* server) {
	struct self_info self;
	syscall_status_t status;
	if (server == NULL) return false;
	*server = (struct device_server){.endpoint = CHANNEL_ID_INVALID,
	                                 .activity = CAP_ID_INVALID,
	                                 .root_cap = CAP_ID_INVALID,
	                                 .pid      = PROCESS_PID_INVALID};
	dm_state_init(&server->state);
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) goto fail;
	server->pid = self.pid;
	status      = channel_create(&server->endpoint, &server->activity);
	if (status != SYSCALL_STATUS_OK) goto fail;
	status =
		cap_publish(server->endpoint, DEVICE_ROOT_OBJECT_ID, server->pid, DEVICE_ROOT_CAP_RIGHTS, &server->root_cap);
	if (status != SYSCALL_STATUS_OK) goto fail;
	server->root_published = true;
	return true;
fail:
	device_server_deinit(server);
	return false;
}

void device_server_deinit(struct device_server* server) {
	if (server == NULL) return;
	if (server->endpoint != CHANNEL_ID_INVALID) {
		for (struct dm_builder* builder = server->state.builders; builder != NULL; builder = builder->next)
			(void)cap_unpublish(server->endpoint, builder->object_id);
		for (struct dm_device* device = server->state.devices; device != NULL; device = device->next)
			(void)cap_unpublish(server->endpoint, device->id);
		if (server->root_published) (void)cap_unpublish(server->endpoint, DEVICE_ROOT_OBJECT_ID);
	}
	dm_state_deinit(&server->state);
	if (server->activity != CAP_ID_INVALID) (void)cap_drop(server->activity);
	if (server->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(server->endpoint);
	*server = (struct device_server){.endpoint = CHANNEL_ID_INVALID,
	                                 .activity = CAP_ID_INVALID,
	                                 .root_cap = CAP_ID_INVALID,
	                                 .pid      = PROCESS_PID_INVALID};
}

syscall_status_t device_server_grant(struct device_server* server, uint64_t device_id, process_id_t target,
                                     cap_rights_t rights, cap_id_t* out_cap) {
	struct dm_device* device;
	if (server == NULL || target == PROCESS_PID_INVALID || out_cap == NULL || (rights & CAP_CALL) == 0u ||
	    (rights & ~DEVICE_CAP_DRIVER_RIGHTS) != 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	device = dm_state_find_device(&server->state, device_id);
	if (device == NULL) return SYSCALL_STATUS_UNAVAILABLE;
	return cap_delegate(device->manager_cap, target, rights, out_cap);
}

int device_server_run(struct device_server* server) {
	for (;;) {
		struct cap_request call;
		bool               received;
		syscall_status_t   status;
		do {
			received = false;
			status   = channel_recv(server->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received && !device_server_dispatch(server, &call, request_buffer))
				printf("device-manager: channel reply failed\n");
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(server->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received) device_server_handle_event(server, &event);
		} while (received);
		struct signal_message activity;
		status = signal_wait(server->activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}
