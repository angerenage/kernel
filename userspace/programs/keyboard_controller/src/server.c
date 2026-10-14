#include <base/cap.h>
#include <protocol/keyboard.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#include "keyboard_controller.h"

struct server_argument {
	struct keyboard_controller* controller;
};

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static struct managed_keyboard* find_keyboard(struct keyboard_controller* controller, uint64_t id) {
	for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (keyboard->id == id) return keyboard;
	return NULL;
}

static bool dispatch_get_info(struct keyboard_controller* controller, const struct cap_request* call) {
	struct keyboard_get_info_response response;
	if (call->request_size != sizeof(struct keyboard_get_info_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	keyboard_controller_lock(controller);
	response = (struct keyboard_get_info_response){
		.topology_generation = controller->topology_generation,
		.keyboard_count      = controller->keyboard_count,
		.oldest_sequence     = controller->event_count == 0u ? 0u : controller->events[controller->event_tail].sequence,
		.newest_sequence     = controller->next_sequence - 1u,
		.queue_capacity      = KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY,
		.maximum_frame_events = KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS,
		.repeat_delay_ms      = KEYBOARD_CONTROLLER_REPEAT_DELAY_MS,
		.repeat_interval_ms   = KEYBOARD_CONTROLLER_REPEAT_INTERVAL_MS,
	};
	keyboard_controller_unlock(controller);
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_enumerate(struct keyboard_controller* controller, const struct cap_request* call,
                               const void* request_data, void* response_buffer) {
	struct keyboard_enumerate_request   request;
	struct keyboard_enumerate_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.topology_generation == 0u ||
	    request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->keyboards))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	keyboard_controller_lock(controller);
	if (request.topology_generation != controller->topology_generation) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	if (request.offset > controller->keyboard_count) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	size_t skipped  = 0u;
	size_t returned = 0u;
	for (struct managed_keyboard* keyboard = controller->keyboards;
	     keyboard != NULL && returned < (size_t)request.count;
	     keyboard = keyboard->next) {
		if (skipped++ < (size_t)request.offset) continue;
		response->keyboards[returned++].id = keyboard->id;
	}
	response->topology_generation = controller->topology_generation;
	response->total               = controller->keyboard_count;
	response->returned            = returned;
	keyboard_controller_unlock(controller);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->keyboards), SYSCALL_STATUS_OK);
}

static bool dispatch_read_state(struct keyboard_controller* controller, const struct cap_request* call,
                                const void* request_data, void* response_buffer) {
	struct keyboard_read_state_request   request;
	struct keyboard_read_state_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.keyboard_id == 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	keyboard_controller_lock(controller);
	struct managed_keyboard* keyboard = find_keyboard(controller, request.keyboard_id);
	if (keyboard == NULL) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	size_t capacity = (call->response_capacity - sizeof(*response)) / sizeof(*response->pressed_keys);
	size_t count    = 0u;
	for (enum input_key_code key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++)
		if (keyboard->pressed[key]) count++;
	if (count > capacity) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	response->sequence      = controller->next_sequence - 1u;
	response->keyboard_id   = keyboard->id;
	response->modifiers     = keyboard->modifiers;
	response->locks         = keyboard->locks;
	response->pressed_count = (uint32_t)count;
	response->reserved      = 0u;
	size_t index            = 0u;
	for (enum input_key_code key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++)
		if (keyboard->pressed[key]) response->pressed_keys[index++] = key;
	keyboard_controller_unlock(controller);
	return reply(call, response, sizeof(*response) + count * sizeof(*response->pressed_keys), SYSCALL_STATUS_OK);
}

static size_t event_index(const struct keyboard_controller* controller, size_t offset) {
	return (controller->event_tail + offset) % KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY;
}

static bool dispatch_read_events(struct keyboard_controller* controller, const struct cap_request* call,
                                 const void* request_data, void* response_buffer) {
	struct keyboard_read_events_request   request;
	struct keyboard_read_events_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->events))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	keyboard_controller_lock(controller);
	uint64_t newest = controller->next_sequence - 1u;
	if (request.after_sequence > newest) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	if (controller->event_count == 0u || request.after_sequence == newest) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	uint64_t oldest = controller->events[controller->event_tail].sequence;
	uint64_t wanted = request.after_sequence + 1u;
	bool     gap    = wanted < oldest;
	if (gap) wanted = oldest;
	size_t skip = (size_t)(wanted - oldest);
	if (skip >= controller->event_count) {
		keyboard_controller_unlock(controller);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	size_t returned = controller->event_count - skip;
	if (returned > (size_t)request.count) returned = (size_t)request.count;
	for (size_t i = 0u; i < returned; i++) response->events[i] = controller->events[event_index(controller, skip + i)];
	response->oldest_sequence = oldest;
	response->newest_sequence = newest;
	response->returned        = returned;
	response->flags           = gap ? KEYBOARD_READ_EVENT_FLAG_GAP : KEYBOARD_READ_EVENT_FLAG_NONE;
	response->reserved        = 0u;
	keyboard_controller_unlock(controller);
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->events), SYSCALL_STATUS_OK);
}

static bool dispatch_acquire_signal(struct keyboard_controller* controller, const struct cap_request* call) {
	struct keyboard_acquire_event_signal_response response = {.capability = CAP_ID_INVALID};
	if (call->request_size != sizeof(struct keyboard_acquire_event_signal_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		cap_delegate(controller->event_signal_cap, call->caller, KEYBOARD_EVENT_SIGNAL_RIGHTS, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch(struct keyboard_controller* controller, const struct cap_request* call, const void* request_data,
                     void* response_buffer) {
	struct keyboard_request_header header;
	if (call->object_id != KEYBOARD_CONTROLLER_OBJECT_ID) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((call->rights & CAP_CALL) == 0u || (call->rights & CAP_READ) == 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (request_data == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	switch (header.op) {
	case KEYBOARD_OP_GET_INFO:
		return dispatch_get_info(controller, call);
	case KEYBOARD_OP_ENUMERATE:
		return dispatch_enumerate(controller, call, request_data, response_buffer);
	case KEYBOARD_OP_READ_STATE:
		return dispatch_read_state(controller, call, request_data, response_buffer);
	case KEYBOARD_OP_READ_EVENTS:
		return dispatch_read_events(controller, call, request_data, response_buffer);
	case KEYBOARD_OP_ACQUIRE_EVENT_SIGNAL:
		return dispatch_acquire_signal(controller, call);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static void server_thread(void* opaque) {
	const struct server_argument* argument        = opaque;
	struct keyboard_controller*   controller      = argument->controller;
	void*                         request_buffer  = malloc(CAP_MAX_REQUEST_SIZE);
	void*                         response_buffer = malloc(CAP_MAX_RESPONSE_SIZE);
	if (controller == NULL || request_buffer == NULL || response_buffer == NULL)
		process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(controller->endpoint, &call, request_buffer, CAP_MAX_REQUEST_SIZE, &received);
			if (status != SYSCALL_STATUS_OK ||
			    (received && !dispatch(controller, &call, request_buffer, response_buffer)))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(controller->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		status = signal_wait(controller->channel_activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

bool keyboard_controller_server_start(struct keyboard_controller* controller) {
	const struct server_argument argument   = {.controller = controller};
	cap_id_t                     thread_cap = CAP_ID_INVALID;
	syscall_status_t             status     = process_spawn_thread(
		controller->self_cap, (uintptr_t)server_thread, &argument, sizeof(argument), NULL, 0u, &thread_cap);
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status == SYSCALL_STATUS_OK;
}
