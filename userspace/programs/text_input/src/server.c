#include <base/cap.h>
#include <protocol/text_input.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#include "text_input.h"

struct server_argument {
	struct text_input* input;
};

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static size_t event_index(const struct text_input* input, size_t offset) {
	return (input->event_tail + offset) % TEXT_INPUT_EVENT_QUEUE_CAPACITY;
}

static bool dispatch_get_info(struct text_input* input, const struct cap_request* call) {
	struct text_input_get_info_response response;
	if (call->request_size != sizeof(struct text_input_get_info_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	text_input_lock(input);
	response = (struct text_input_get_info_response){
		.oldest_sequence         = input->event_count == 0u ? 0u : input->events[input->event_tail].sequence,
		.newest_sequence         = input->next_sequence - 1u,
		.queue_capacity          = TEXT_INPUT_EVENT_QUEUE_CAPACITY,
		.maximum_frame_events    = TEXT_INPUT_MAXIMUM_FRAME_EVENTS,
		.text_capacity_per_event = TEXT_INPUT_EVENT_TEXT_CAPACITY,
	};
	text_input_unlock(input);
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_read_events(struct text_input* input, const struct cap_request* call, const void* request_data,
                                 void* response_buffer) {
	struct text_input_read_events_request   request;
	struct text_input_read_events_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->events))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	text_input_lock(input);
	uint64_t newest = input->next_sequence - 1u;
	if (request.after_sequence > newest) {
		text_input_unlock(input);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	if (input->event_count == 0u || request.after_sequence == newest) {
		text_input_unlock(input);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	uint64_t oldest = input->events[input->event_tail].sequence;
	uint64_t wanted = request.after_sequence + 1u;
	bool     gap    = wanted < oldest;
	if (gap) wanted = oldest;
	size_t skip = (size_t)(wanted - oldest);
	if (skip >= input->event_count ||
	    (skip != 0u && (input->events[event_index(input, skip - 1u)].flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) == 0u)) {
		text_input_unlock(input);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}

	size_t available = input->event_count - skip;
	size_t limit     = available < (size_t)request.count ? available : (size_t)request.count;
	size_t returned  = 0u;
	for (size_t i = 0u; i < limit; i++)
		if ((input->events[event_index(input, skip + i)].flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) != 0u)
			returned = i + 1u;
	if (returned == 0u) {
		text_input_unlock(input);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	for (size_t i = 0u; i < returned; i++) response->events[i] = input->events[event_index(input, skip + i)];
	response->oldest_sequence = oldest;
	response->newest_sequence = newest;
	response->returned        = returned;
	response->flags           = gap ? TEXT_INPUT_READ_EVENT_FLAG_GAP : TEXT_INPUT_READ_EVENT_FLAG_NONE;
	response->reserved        = 0u;
	text_input_unlock(input);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->events), SYSCALL_STATUS_OK);
}

static bool dispatch_acquire_signal(struct text_input* input, const struct cap_request* call) {
	struct text_input_acquire_event_signal_response response = {.capability = CAP_ID_INVALID};
	if (call->request_size != sizeof(struct text_input_acquire_event_signal_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		cap_delegate(input->event_signal_cap, call->caller, TEXT_INPUT_EVENT_SIGNAL_RIGHTS, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch(struct text_input* input, const struct cap_request* call, const void* request_data,
                     void* response_buffer) {
	struct text_input_request_header header;
	if (call->object_id != TEXT_INPUT_OBJECT_ID) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((call->rights & CAP_CALL) == 0u || (call->rights & CAP_READ) == 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (request_data == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	switch (header.op) {
	case TEXT_INPUT_OP_GET_INFO:
		return dispatch_get_info(input, call);
	case TEXT_INPUT_OP_READ_EVENTS:
		return dispatch_read_events(input, call, request_data, response_buffer);
	case TEXT_INPUT_OP_ACQUIRE_EVENT_SIGNAL:
		return dispatch_acquire_signal(input, call);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static void server_thread(void* opaque) {
	const struct server_argument* argument        = opaque;
	struct text_input*            input           = argument->input;
	void*                         request_buffer  = malloc(CAP_MAX_REQUEST_SIZE);
	void*                         response_buffer = malloc(CAP_MAX_RESPONSE_SIZE);
	if (input == NULL || request_buffer == NULL || response_buffer == NULL)
		process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(input->endpoint, &call, request_buffer, CAP_MAX_REQUEST_SIZE, &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch(input, &call, request_buffer, response_buffer)))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(input->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		status = signal_wait(input->channel_activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

bool text_input_server_start(struct text_input* input) {
	const struct server_argument argument   = {.input = input};
	cap_id_t                     thread_cap = CAP_ID_INVALID;
	syscall_status_t             status     = process_spawn_thread(
		input->self_cap, (uintptr_t)server_thread, &argument, sizeof(argument), NULL, 0u, &thread_cap);
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status == SYSCALL_STATUS_OK;
}
