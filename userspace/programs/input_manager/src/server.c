#include <base/cap.h>
#include <protocol/input_manager.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#include "input_manager.h"

struct input_manager_server_argument {
	struct input_manager* manager;
};

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static struct managed_input_source* find_active_source(struct input_manager* manager, uint64_t id) {
	for (struct managed_input_source* source = manager->sources; source != NULL; source = source->next) {
		if (source->active && source->id == id) return source;
	}
	return NULL;
}

static bool dispatch_get_info(struct input_manager* manager, const struct cap_request* call) {
	struct input_manager_get_info_response response;

	if (call->request_size != sizeof(struct input_manager_get_info_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	input_manager_lock(manager);
	response = (struct input_manager_get_info_response){
		.topology_generation  = manager->topology_generation,
		.source_count         = manager->source_count,
		.oldest_sequence      = manager->event_count == 0u ? 0u : manager->events[manager->event_tail].sequence,
		.newest_sequence      = manager->next_sequence - 1u,
		.queue_capacity       = INPUT_MANAGER_EVENT_QUEUE_CAPACITY,
		.maximum_frame_events = INPUT_MANAGER_MAXIMUM_FRAME_EVENTS,
	};
	input_manager_unlock(manager);
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_enumerate_sources(struct input_manager* manager, const struct cap_request* call,
                                       const void* request_data, void* response_buffer) {
	struct input_manager_enumerate_sources_request   request;
	struct input_manager_enumerate_sources_response* response = response_buffer;

	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.topology_generation == 0u ||
	    request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->sources))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	input_manager_lock(manager);
	if (request.topology_generation != manager->topology_generation) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	if (request.offset > manager->source_count) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	size_t skipped  = 0u;
	size_t returned = 0u;
	for (struct managed_input_source* source = manager->sources; source != NULL && returned < (size_t)request.count;
	     source                              = source->next) {
		if (!source->active) continue;
		if (skipped < (size_t)request.offset) {
			skipped++;
			continue;
		}
		response->sources[returned++] = (struct input_manager_source){
			.id               = source->id,
			.device_class     = source->device_class,
			.descriptor_count = (uint32_t)source->descriptor_count,
		};
	}
	response->topology_generation = manager->topology_generation;
	response->total               = manager->source_count;
	response->returned            = returned;
	input_manager_unlock(manager);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->sources), SYSCALL_STATUS_OK);
}

static bool dispatch_read_descriptors(struct input_manager* manager, const struct cap_request* call,
                                      const void* request_data, void* response_buffer) {
	struct input_manager_read_descriptors_request   request;
	struct input_manager_read_descriptors_response* response = response_buffer;

	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.topology_generation == 0u ||
	    request.source_id == 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->descriptors))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	input_manager_lock(manager);
	if (request.topology_generation != manager->topology_generation) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	struct managed_input_source* source = find_active_source(manager, request.source_id);
	if (source == NULL) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	if (request.offset > source->descriptor_count) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	size_t returned = source->descriptor_count - (size_t)request.offset;
	if (returned > (size_t)request.count) returned = (size_t)request.count;
	if (returned != 0u)
		memcpy(response->descriptors,
		       source->descriptors + (size_t)request.offset,
		       returned * sizeof(*response->descriptors));
	response->topology_generation = manager->topology_generation;
	response->total               = source->descriptor_count;
	response->returned            = returned;
	input_manager_unlock(manager);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->descriptors), SYSCALL_STATUS_OK);
}

static size_t event_index(const struct input_manager* manager, size_t offset) {
	return (manager->event_tail + offset) % INPUT_MANAGER_EVENT_QUEUE_CAPACITY;
}

static bool dispatch_read_events(struct input_manager* manager, const struct cap_request* call,
                                 const void* request_data, void* response_buffer) {
	struct input_manager_read_events_request   request;
	struct input_manager_read_events_response* response = response_buffer;

	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->events))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	input_manager_lock(manager);
	uint64_t newest = manager->next_sequence - 1u;
	if (request.after_sequence > newest) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	if (manager->event_count == 0u || request.after_sequence == newest) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	uint64_t oldest = manager->events[manager->event_tail].sequence;
	uint64_t wanted = request.after_sequence + 1u;
	bool     gap    = wanted < oldest;
	if (gap) wanted = oldest;
	size_t skip = (size_t)(wanted - oldest);
	if (skip >= manager->event_count) {
		input_manager_unlock(manager);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	size_t returned = manager->event_count - skip;
	if (returned > (size_t)request.count) returned = (size_t)request.count;
	for (size_t i = 0u; i < returned; i++) response->events[i] = manager->events[event_index(manager, skip + i)];
	response->oldest_sequence = oldest;
	response->newest_sequence = newest;
	response->returned        = returned;
	response->flags           = gap ? INPUT_MANAGER_READ_EVENT_FLAG_GAP : INPUT_MANAGER_READ_EVENT_FLAG_NONE;
	response->reserved        = 0u;
	input_manager_unlock(manager);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->events), SYSCALL_STATUS_OK);
}

static bool dispatch_acquire_signal(struct input_manager* manager, const struct cap_request* call) {
	struct input_manager_acquire_event_signal_response response = {.capability = CAP_ID_INVALID};
	if (call->request_size != sizeof(struct input_manager_acquire_event_signal_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		cap_delegate(manager->event_signal_cap, call->caller, INPUT_MANAGER_EVENT_SIGNAL_RIGHTS, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch(struct input_manager* manager, const struct cap_request* call, const void* request_data,
                     void* response_buffer) {
	struct input_manager_request_header header;
	if (call->object_id != INPUT_MANAGER_OBJECT_ID) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((call->rights & (CAP_CALL | CAP_READ)) != (CAP_CALL | CAP_READ))
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (request_data == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	switch (header.op) {
	case INPUT_MANAGER_OP_GET_INFO:
		return dispatch_get_info(manager, call);
	case INPUT_MANAGER_OP_ENUMERATE_SOURCES:
		return dispatch_enumerate_sources(manager, call, request_data, response_buffer);
	case INPUT_MANAGER_OP_READ_DESCRIPTORS:
		return dispatch_read_descriptors(manager, call, request_data, response_buffer);
	case INPUT_MANAGER_OP_READ_EVENTS:
		return dispatch_read_events(manager, call, request_data, response_buffer);
	case INPUT_MANAGER_OP_ACQUIRE_EVENT_SIGNAL:
		return dispatch_acquire_signal(manager, call);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static void server_thread(void* opaque) {
	const struct input_manager_server_argument* argument        = opaque;
	struct input_manager*                       manager         = argument->manager;
	void*                                       request_buffer  = malloc(CAP_MAX_REQUEST_SIZE);
	void*                                       response_buffer = malloc(CAP_MAX_RESPONSE_SIZE);
	if (manager == NULL || request_buffer == NULL || response_buffer == NULL)
		process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(manager->endpoint, &call, request_buffer, CAP_MAX_REQUEST_SIZE, &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch(manager, &call, request_buffer, response_buffer)))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(manager->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		status = signal_wait(manager->channel_activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

bool input_manager_server_start(struct input_manager* manager) {
	const struct input_manager_server_argument argument   = {.manager = manager};
	cap_id_t                                   thread_cap = CAP_ID_INVALID;
	syscall_status_t                           status     = process_spawn_thread(
		manager->self_cap, (uintptr_t)server_thread, &argument, sizeof(argument), NULL, 0u, &thread_cap);
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status == SYSCALL_STATUS_OK;
}
