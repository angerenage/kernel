#include "input.h"

#include <base/channel.h>
#include <base/process.h>
#include <limits.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#define PS2_INPUT_PROTOCOL_VERSION_MAJOR 1u
#define PS2_INPUT_PROTOCOL_VERSION_MINOR 0u
#define PS2_INPUT_SERVER_OBJECT_ID 1u
#define PS2_INPUT_MAXIMUM_DESCRIPTORS 1024u
#define PS2_INPUT_EVENT_QUEUE_CAPACITY 512u
#define PS2_INPUT_MAXIMUM_FRAME_EVENTS 128u
#define PS2_INPUT_MAXIMUM_OUTPUT_VALUES 128u

/* Retained input state shared by the PS/2 decoder and its server thread. */
struct ps2_input_state {
	enum input_device_class          device_class;
	struct input_control_descriptor* descriptors;
	size_t                           descriptor_count;
	struct input_output_descriptor*  output_descriptors;
	size_t                           output_descriptor_count;
	struct input_event               events[PS2_INPUT_EVENT_QUEUE_CAPACITY];
	size_t                           event_tail;
	size_t                           event_count;
	uint64_t                         next_sequence;
	uint64_t                         next_frame;
	bool                             locked;
};

/* Metadata returned by one successful retained-event read. */
struct ps2_input_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Input-protocol server state owned by one PS/2 driver process. */
struct ps2_input_server {
	struct ps2_input_state       state;
	channel_id_t                 endpoint;
	cap_id_t                     activity_cap;
	cap_id_t                     event_signal_cap;
	cap_id_t                     driver_activity_cap;
	cap_id_t                     output_completion_cap;
	cap_id_t                     service_cap;
	struct init_service_selector selector;
	struct input_output_value    pending_outputs[PS2_INPUT_MAXIMUM_OUTPUT_VALUES];
	size_t                       pending_output_count;
	syscall_status_t             pending_output_status;
	bool                         output_pending;
	bool                         output_taken;
	bool                         output_complete;
};

/* Copied onto the detached server thread's initial stack. */
struct ps2_input_server_argument {
	struct ps2_input_server* server;
};

static void state_lock(struct ps2_input_state* state) {
	while (__atomic_test_and_set(&state->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void state_unlock(struct ps2_input_state* state) {
	__atomic_clear(&state->locked, __ATOMIC_RELEASE);
}

static bool device_class_valid(enum input_device_class device_class) {
	return device_class > INPUT_DEVICE_CLASS_INVALID && device_class <= INPUT_DEVICE_CLASS_OTHER;
}

static bool descriptor_valid(const struct input_control_descriptor* descriptor) {
	if (descriptor->id == 0u || descriptor->code == 0u) return false;
	switch (descriptor->kind) {
	case INPUT_CONTROL_KIND_KEY:
		return descriptor->code <= INPUT_KEY_KEYPAD_DECIMAL && descriptor->logical_minimum == 0 &&
		       descriptor->logical_maximum == 1;
	case INPUT_CONTROL_KIND_BUTTON:
		return descriptor->code <= INPUT_BUTTON_FORWARD && descriptor->logical_minimum == 0 &&
		       descriptor->logical_maximum == 1;
	case INPUT_CONTROL_KIND_RELATIVE_AXIS:
		return descriptor->code <= INPUT_AXIS_HORIZONTAL_SCROLL && descriptor->logical_minimum == 0 &&
		       descriptor->logical_maximum == 0;
	case INPUT_CONTROL_KIND_ABSOLUTE_AXIS:
		return descriptor->code <= INPUT_AXIS_HORIZONTAL_SCROLL &&
		       descriptor->logical_minimum < descriptor->logical_maximum;
	default:
		return false;
	}
}

static bool descriptors_valid(const struct input_control_descriptor* descriptors, size_t count) {
	if (descriptors == NULL || count == 0u || count > PS2_INPUT_MAXIMUM_DESCRIPTORS || count > UINT32_MAX) return false;
	for (size_t i = 0u; i < count; i++) {
		if (!descriptor_valid(&descriptors[i])) return false;
		for (size_t previous = 0u; previous < i; previous++)
			if (descriptors[previous].id == descriptors[i].id) return false;
	}
	return true;
}

static bool output_descriptor_valid(const struct input_output_descriptor* descriptor) {
	if (descriptor->id == 0u || descriptor->code <= INPUT_OUTPUT_CODE_INVALID ||
	    descriptor->code > INPUT_OUTPUT_CODE_SCROLL_LOCK_INDICATOR)
		return false;
	switch (descriptor->kind) {
	case INPUT_OUTPUT_KIND_BOOLEAN:
		return descriptor->logical_minimum == 0 && descriptor->logical_maximum == 1;
	case INPUT_OUTPUT_KIND_BOUNDED_VALUE:
		return descriptor->logical_minimum < descriptor->logical_maximum;
	default:
		return false;
	}
}

static bool output_descriptors_valid(const struct input_output_descriptor* descriptors, size_t count) {
	if ((descriptors == NULL) != (count == 0u) || count > PS2_INPUT_MAXIMUM_OUTPUT_VALUES) return false;
	for (size_t i = 0u; i < count; i++) {
		if (!output_descriptor_valid(&descriptors[i])) return false;
		for (size_t previous = 0u; previous < i; previous++)
			if (descriptors[previous].id == descriptors[i].id) return false;
	}
	return true;
}

static syscall_status_t state_initialize(struct ps2_input_state* state, enum input_device_class device_class,
                                         const struct input_control_descriptor* descriptors, size_t descriptor_count,
                                         const struct input_output_descriptor* output_descriptors,
                                         size_t                                output_descriptor_count) {
	if (state == NULL || !device_class_valid(device_class) || !descriptors_valid(descriptors, descriptor_count) ||
	    !output_descriptors_valid(output_descriptors, output_descriptor_count))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	struct input_control_descriptor* copy = malloc(descriptor_count * sizeof(*copy));
	if (copy == NULL) return SYSCALL_STATUS_FAILED;
	struct input_output_descriptor* output_copy = NULL;
	if (output_descriptor_count != 0u) {
		output_copy = malloc(output_descriptor_count * sizeof(*output_copy));
		if (output_copy == NULL) {
			free(copy);
			return SYSCALL_STATUS_FAILED;
		}
		memcpy(output_copy, output_descriptors, output_descriptor_count * sizeof(*output_copy));
	}
	memcpy(copy, descriptors, descriptor_count * sizeof(*copy));
	memset(state, 0, sizeof(*state));
	state->device_class            = device_class;
	state->descriptors             = copy;
	state->descriptor_count        = descriptor_count;
	state->output_descriptors      = output_copy;
	state->output_descriptor_count = output_descriptor_count;
	state->next_sequence           = 1u;
	state->next_frame              = 1u;
	return SYSCALL_STATUS_OK;
}

static void state_destroy(struct ps2_input_state* state) {
	if (state == NULL) return;
	free(state->descriptors);
	free(state->output_descriptors);
	memset(state, 0, sizeof(*state));
}

static syscall_status_t state_read_descriptors(struct ps2_input_state* state, uint64_t offset,
                                               struct input_control_descriptor* descriptors, size_t capacity,
                                               size_t* out_returned, uint64_t* out_total) {
	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (state == NULL || descriptors == NULL || capacity == 0u || out_returned == NULL || out_total == NULL ||
	    offset > state->descriptor_count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	size_t returned = state->descriptor_count - (size_t)offset;
	if (returned > capacity) returned = capacity;
	if (returned != 0u) memcpy(descriptors, state->descriptors + (size_t)offset, returned * sizeof(*descriptors));
	*out_returned = returned;
	*out_total    = state->descriptor_count;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t state_read_output_descriptors(struct ps2_input_state* state, uint64_t offset,
                                                      struct input_output_descriptor* descriptors, size_t capacity,
                                                      size_t* out_returned, uint64_t* out_total) {
	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (state == NULL || descriptors == NULL || capacity == 0u || out_returned == NULL || out_total == NULL ||
	    offset > state->output_descriptor_count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	size_t returned = state->output_descriptor_count - (size_t)offset;
	if (returned > capacity) returned = capacity;
	if (returned != 0u)
		memcpy(descriptors, state->output_descriptors + (size_t)offset, returned * sizeof(*descriptors));
	*out_returned = returned;
	*out_total    = state->output_descriptor_count;
	return SYSCALL_STATUS_OK;
}

static const struct input_output_descriptor* find_output_descriptor(const struct ps2_input_state* state, uint32_t id) {
	for (size_t i = 0u; i < state->output_descriptor_count; i++)
		if (state->output_descriptors[i].id == id) return &state->output_descriptors[i];
	return NULL;
}

static bool output_value_valid(const struct input_output_descriptor* descriptor, int64_t value) {
	return value >= descriptor->logical_minimum && value <= descriptor->logical_maximum;
}

static const struct input_control_descriptor* find_descriptor(const struct ps2_input_state* state, uint32_t id) {
	for (size_t i = 0u; i < state->descriptor_count; i++)
		if (state->descriptors[i].id == id) return &state->descriptors[i];
	return NULL;
}

static bool value_valid(const struct input_control_descriptor* descriptor, int64_t value) {
	switch (descriptor->kind) {
	case INPUT_CONTROL_KIND_KEY:
	case INPUT_CONTROL_KIND_BUTTON:
		return value == 0 || value == 1;
	case INPUT_CONTROL_KIND_RELATIVE_AXIS:
		return true;
	case INPUT_CONTROL_KIND_ABSOLUTE_AXIS:
		return value >= descriptor->logical_minimum && value <= descriptor->logical_maximum;
	default:
		return false;
	}
}

static size_t event_index(const struct ps2_input_state* state, size_t offset) {
	return (state->event_tail + offset) % PS2_INPUT_EVENT_QUEUE_CAPACITY;
}

static void evict_oldest_frame(struct ps2_input_state* state) {
	while (state->event_count != 0u) {
		bool frame_end    = (state->events[state->event_tail].flags & INPUT_EVENT_FLAG_FRAME_END) != 0u;
		state->event_tail = (state->event_tail + 1u) % PS2_INPUT_EVENT_QUEUE_CAPACITY;
		state->event_count--;
		if (frame_end) return;
	}
}

static syscall_status_t append_frame(struct ps2_input_state* state, enum input_event_type type,
                                     const struct ps2_input_value* values, size_t count,
                                     uint64_t* out_newest_sequence) {
	if (out_newest_sequence != NULL) *out_newest_sequence = 0u;
	if (state == NULL || out_newest_sequence == NULL ||
	    (type == INPUT_EVENT_TYPE_CONTROL && (values == NULL || count == 0u)) ||
	    (type == INPUT_EVENT_TYPE_STATE_RESET && (values != NULL || count != 1u)) ||
	    count > PS2_INPUT_MAXIMUM_FRAME_EVENTS)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	state_lock(state);
	if (type == INPUT_EVENT_TYPE_CONTROL) {
		for (size_t i = 0u; i < count; i++) {
			const struct input_control_descriptor* descriptor = find_descriptor(state, values[i].control_id);
			if (descriptor == NULL || !value_valid(descriptor, values[i].value)) {
				state_unlock(state);
				return SYSCALL_STATUS_BAD_ARGUMENT;
			}
			for (size_t previous = 0u; previous < i; previous++) {
				if (values[previous].control_id == values[i].control_id) {
					state_unlock(state);
					return SYSCALL_STATUS_BAD_ARGUMENT;
				}
			}
		}
	}
	if (state->next_sequence == 0u || state->next_sequence > UINT64_MAX - count || state->next_frame == 0u ||
	    state->next_frame == UINT64_MAX) {
		state_unlock(state);
		return SYSCALL_STATUS_FAILED;
	}
	while (PS2_INPUT_EVENT_QUEUE_CAPACITY - state->event_count < count) evict_oldest_frame(state);
	for (size_t i = 0u; i < count; i++) {
		size_t index         = event_index(state, state->event_count);
		state->events[index] = (struct input_event){
			.sequence   = state->next_sequence++,
			.frame      = state->next_frame,
			.type       = type,
			.control_id = type == INPUT_EVENT_TYPE_CONTROL ? values[i].control_id : 0u,
			.value      = type == INPUT_EVENT_TYPE_CONTROL ? values[i].value : 0,
			.flags      = i + 1u == count ? INPUT_EVENT_FLAG_FRAME_END : INPUT_EVENT_FLAG_NONE,
		};
		state->event_count++;
	}
	state->next_frame++;
	*out_newest_sequence = state->next_sequence - 1u;
	state_unlock(state);
	return SYSCALL_STATUS_OK;
}

static syscall_status_t state_read_events(struct ps2_input_state* state, uint64_t after_sequence,
                                          struct input_event* events, size_t capacity,
                                          struct ps2_input_event_page* out_page) {
	if (out_page != NULL) *out_page = (struct ps2_input_event_page){0};
	if (state == NULL || events == NULL || capacity == 0u || out_page == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	state_lock(state);
	uint64_t newest = state->next_sequence - 1u;
	if (after_sequence > newest) {
		state_unlock(state);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (state->event_count == 0u || after_sequence == newest) {
		state_unlock(state);
		return SYSCALL_STATUS_UNAVAILABLE;
	}
	uint64_t oldest = state->events[state->event_tail].sequence;
	uint64_t wanted = after_sequence + 1u;
	bool     gap    = wanted < oldest;
	if (gap) wanted = oldest;
	size_t skip = (size_t)(wanted - oldest);
	if (skip >= state->event_count) {
		state_unlock(state);
		return SYSCALL_STATUS_UNAVAILABLE;
	}
	size_t returned = state->event_count - skip;
	if (returned > capacity) returned = capacity;
	for (size_t i = 0u; i < returned; i++) events[i] = state->events[event_index(state, skip + i)];
	*out_page = (struct ps2_input_event_page){
		.oldest_sequence = oldest,
		.newest_sequence = newest,
		.returned        = returned,
		.flags           = gap ? INPUT_READ_EVENT_FLAG_GAP : INPUT_READ_EVENT_FLAG_NONE,
	};
	state_unlock(state);
	return SYSCALL_STATUS_OK;
}

static bool service_name_valid(const char* service) {
	if (service == NULL || service[0] == '\0') return false;
	for (size_t i = 0u; i <= INIT_NAME_MAX; i++) {
		char value = service[i];
		if (value == '\0') return i != 0u;
		if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
		      value == '_' || value == '-' || value == '.'))
			return false;
	}
	return false;
}

syscall_status_t ps2_input_server_publish(struct ps2_input_server* server, const struct ps2_input_value* values,
                                          size_t count) {
	if (server == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	uint64_t         newest;
	syscall_status_t status = append_frame(&server->state, INPUT_EVENT_TYPE_CONTROL, values, count, &newest);
	if (status != SYSCALL_STATUS_OK) return status;
	struct signal_send_response response;
	return signal_send(server->event_signal_cap, newest, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response);
}

syscall_status_t ps2_input_server_reset(struct ps2_input_server* server) {
	if (server == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	uint64_t         newest;
	syscall_status_t status = append_frame(&server->state, INPUT_EVENT_TYPE_STATE_RESET, NULL, 1u, &newest);
	if (status != SYSCALL_STATUS_OK) return status;
	struct signal_send_response response;
	return signal_send(server->event_signal_cap, newest, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response);
}

bool ps2_input_server_take_outputs(struct ps2_input_server* server, struct input_output_value* values, size_t capacity,
                                   size_t* out_count) {
	if (out_count != NULL) *out_count = 0u;
	if (server == NULL || values == NULL || capacity == 0u || out_count == NULL) return false;
	state_lock(&server->state);
	if (!server->output_pending || server->output_taken || server->pending_output_count > capacity) {
		state_unlock(&server->state);
		return false;
	}
	memcpy(values, server->pending_outputs, server->pending_output_count * sizeof(*values));
	*out_count           = server->pending_output_count;
	server->output_taken = true;
	state_unlock(&server->state);
	return true;
}

bool ps2_input_server_complete_outputs(struct ps2_input_server* server, syscall_status_t status) {
	if (server == NULL || (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_FAILED)) return false;
	state_lock(&server->state);
	if (!server->output_pending || !server->output_taken || server->output_complete) {
		state_unlock(&server->state);
		return false;
	}
	server->pending_output_status = status;
	server->output_complete       = true;
	state_unlock(&server->state);
	struct signal_send_response response;
	return signal_send(server->output_completion_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool dispatch_get_info(struct ps2_input_server* server, const struct cap_request* call) {
	const struct input_get_info_response response = {
		.device_class            = server->state.device_class,
		.descriptor_count        = (uint32_t)server->state.descriptor_count,
		.output_descriptor_count = (uint32_t)server->state.output_descriptor_count,
		.queue_capacity          = PS2_INPUT_EVENT_QUEUE_CAPACITY,
		.maximum_frame_events    = PS2_INPUT_MAXIMUM_FRAME_EVENTS,
		.maximum_output_values   = (uint32_t)server->state.output_descriptor_count,
	};
	if (call->request_size != sizeof(struct input_get_info_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_read_descriptors(struct ps2_input_server* server, const struct cap_request* call,
                                      const void* request_data, void* response_buffer) {
	struct input_read_descriptors_request   request;
	struct input_read_descriptors_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->descriptors))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	size_t   returned;
	uint64_t total;
	if (state_read_descriptors(
			&server->state, request.offset, response->descriptors, (size_t)request.count, &returned, &total) !=
	    SYSCALL_STATUS_OK)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	response->total    = total;
	response->returned = returned;
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->descriptors), SYSCALL_STATUS_OK);
}

static bool dispatch_read_events(struct ps2_input_server* server, const struct cap_request* call,
                                 const void* request_data, void* response_buffer) {
	struct input_read_events_request   request;
	struct input_read_events_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->events))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct ps2_input_event_page page;
	syscall_status_t            status =
		state_read_events(&server->state, request.after_sequence, response->events, (size_t)request.count, &page);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response->oldest_sequence = page.oldest_sequence;
	response->newest_sequence = page.newest_sequence;
	response->returned        = page.returned;
	response->flags           = page.flags;
	response->reserved        = 0u;
	return reply(call, response, sizeof(*response) + page.returned * sizeof(*response->events), SYSCALL_STATUS_OK);
}

static bool dispatch_acquire_signal(struct ps2_input_server* server, const struct cap_request* call) {
	struct input_acquire_event_signal_response response = {.capability = CAP_ID_INVALID};
	if (call->request_size != sizeof(struct input_acquire_event_signal_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		cap_delegate(server->event_signal_cap, call->caller, INPUT_EVENT_SIGNAL_RIGHTS, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch_read_output_descriptors(struct ps2_input_server* server, const struct cap_request* call,
                                             const void* request_data, void* response_buffer) {
	struct input_read_output_descriptors_request   request;
	struct input_read_output_descriptors_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->descriptors))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	size_t   returned;
	uint64_t total;
	if (state_read_output_descriptors(
			&server->state, request.offset, response->descriptors, (size_t)request.count, &returned, &total) !=
	    SYSCALL_STATUS_OK)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	response->total    = total;
	response->returned = returned;
	return reply(call, response, sizeof(*response) + returned * sizeof(*response->descriptors), SYSCALL_STATUS_OK);
}

static bool dispatch_set_outputs(struct ps2_input_server* server, const struct cap_request* call,
                                 const void* request_data) {
	struct input_set_outputs_request header;
	if (call->request_size < sizeof(header)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	if (header.reserved != 0u || header.count == 0u || header.count > server->state.output_descriptor_count ||
	    header.count > (CAP_MAX_REQUEST_SIZE - sizeof(header)) / sizeof(struct input_output_value) ||
	    call->request_size != sizeof(header) + (size_t)header.count * sizeof(struct input_output_value))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	const struct input_set_outputs_request* request = request_data;
	for (size_t i = 0u; i < (size_t)request->count; i++) {
		const struct input_output_value*      value      = &request->values[i];
		const struct input_output_descriptor* descriptor = find_output_descriptor(&server->state, value->output_id);
		if (value->reserved != 0u || descriptor == NULL || !output_value_valid(descriptor, value->value))
			return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		for (size_t previous = 0u; previous < i; previous++)
			if (request->values[previous].output_id == value->output_id)
				return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	state_lock(&server->state);
	if (server->output_pending) {
		state_unlock(&server->state);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	memcpy(server->pending_outputs, request->values, (size_t)request->count * sizeof(*request->values));
	server->pending_output_count  = (size_t)request->count;
	server->pending_output_status = SYSCALL_STATUS_FAILED;
	server->output_pending        = true;
	server->output_taken          = false;
	server->output_complete       = false;
	state_unlock(&server->state);
	struct signal_send_response send_response;
	if (signal_send(server->driver_activity_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &send_response) !=
	    SYSCALL_STATUS_OK) {
		state_lock(&server->state);
		server->output_pending = false;
		state_unlock(&server->state);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(server->output_completion_cap, &message);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) {
			state_lock(&server->state);
			server->pending_output_count = 0u;
			server->output_pending       = false;
			server->output_taken         = false;
			server->output_complete      = false;
			state_unlock(&server->state);
			return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
		}
		state_lock(&server->state);
		if (!server->output_complete) {
			state_unlock(&server->state);
			continue;
		}
		status                       = server->pending_output_status;
		server->pending_output_count = 0u;
		server->output_pending       = false;
		server->output_taken         = false;
		server->output_complete      = false;
		state_unlock(&server->state);
		return reply(call, NULL, 0u, status);
	}
}

static bool dispatch(struct ps2_input_server* server, const struct cap_request* call, const void* request_data,
                     void* response_buffer) {
	struct input_request_header header;
	if ((call->rights & CAP_CALL) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (request_data == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	if (header.op == INPUT_OP_SET_OUTPUTS) {
		if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	}
	else if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	switch (header.op) {
	case INPUT_OP_GET_INFO:
		return dispatch_get_info(server, call);
	case INPUT_OP_READ_DESCRIPTORS:
		return dispatch_read_descriptors(server, call, request_data, response_buffer);
	case INPUT_OP_READ_EVENTS:
		return dispatch_read_events(server, call, request_data, response_buffer);
	case INPUT_OP_ACQUIRE_EVENT_SIGNAL:
		return dispatch_acquire_signal(server, call);
	case INPUT_OP_READ_OUTPUT_DESCRIPTORS:
		return dispatch_read_output_descriptors(server, call, request_data, response_buffer);
	case INPUT_OP_SET_OUTPUTS:
		return dispatch_set_outputs(server, call, request_data);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static void run_server(void* opaque) {
	const struct ps2_input_server_argument* argument        = opaque;
	struct ps2_input_server*                server          = argument->server;
	void*                                   request_buffer  = malloc(CAP_MAX_REQUEST_SIZE);
	void*                                   response_buffer = malloc(CAP_MAX_RESPONSE_SIZE);
	if (server == NULL || request_buffer == NULL || response_buffer == NULL)
		process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(server->endpoint, &call, request_buffer, CAP_MAX_REQUEST_SIZE, &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch(server, &call, request_buffer, response_buffer)))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(server->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		status = signal_wait(server->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

static void drop_self_info(struct self_info* self) {
	if (self->self_cap != CAP_ID_INVALID) (void)cap_drop(self->self_cap);
	if (self->address_space_cap != CAP_ID_INVALID) (void)cap_drop(self->address_space_cap);
	if (self->main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self->main_thread_cap);
}

static void cleanup_unstarted(struct ps2_input_server* server) {
	if (server == NULL) return;
	if (server->service_cap != CAP_ID_INVALID) (void)cap_drop(server->service_cap);
	if (server->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(server->endpoint);
	if (server->event_signal_cap != CAP_ID_INVALID) (void)signal_destroy(server->event_signal_cap);
	if (server->output_completion_cap != CAP_ID_INVALID) (void)signal_destroy(server->output_completion_cap);
	state_destroy(&server->state);
	free(server);
}

syscall_status_t ps2_input_server_create(const char* service, enum input_device_class device_class,
                                         const struct input_control_descriptor* descriptors, size_t descriptor_count,
                                         const struct input_output_descriptor* output_descriptors,
                                         size_t output_descriptor_count, cap_id_t driver_activity_cap,
                                         struct ps2_input_server** out_server) {
	struct ps2_input_server* server;
	struct self_info         self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	cap_id_t         thread_cap = CAP_ID_INVALID;
	syscall_status_t status;
	bool             advertised     = false;
	bool             server_started = false;

	if (out_server != NULL) *out_server = NULL;
	if (!service_name_valid(service) || out_server == NULL ||
	    (output_descriptor_count != 0u && driver_activity_cap == CAP_ID_INVALID))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	server = calloc(1u, sizeof(*server));
	if (server == NULL) return SYSCALL_STATUS_FAILED;
	status = state_initialize(
		&server->state, device_class, descriptors, descriptor_count, output_descriptors, output_descriptor_count);
	if (status != SYSCALL_STATUS_OK) {
		free(server);
		return status;
	}
	server->endpoint              = CHANNEL_ID_INVALID;
	server->activity_cap          = CAP_ID_INVALID;
	server->event_signal_cap      = CAP_ID_INVALID;
	server->driver_activity_cap   = driver_activity_cap;
	server->output_completion_cap = CAP_ID_INVALID;
	server->service_cap           = CAP_ID_INVALID;
	server->selector              = (struct init_service_selector){
		.namespace_path = INPUT_NAMESPACE,
		.protocol       = INPUT_PROTOCOL_NAME,
		.major          = PS2_INPUT_PROTOCOL_VERSION_MAJOR,
	};
	(void)strlcpy(server->selector.service, service, sizeof(server->selector.service));

	status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK) status = signal_create(&server->event_signal_cap);
	if (status == SYSCALL_STATUS_OK && output_descriptor_count != 0u)
		status = signal_create(&server->output_completion_cap);
	if (status == SYSCALL_STATUS_OK) status = channel_create(&server->endpoint, &server->activity_cap);
	if (status == SYSCALL_STATUS_OK) {
		status = cap_publish(server->endpoint,
		                     PS2_INPUT_SERVER_OBJECT_ID,
		                     self.pid,
		                     CAP_CALL | CAP_READ | CAP_WRITE | CAP_DELEGATE,
		                     &server->service_cap);
	}
	if (status == SYSCALL_STATUS_OK) {
		const struct ps2_input_server_argument argument = {.server = server};
		status                                          = process_spawn_thread(
			self.self_cap, (uintptr_t)run_server, &argument, sizeof(argument), NULL, 0u, &thread_cap);
		server_started = status == SYSCALL_STATUS_OK;
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK && drop_status != SYSCALL_STATUS_OK) status = drop_status;
	}
	if (status == SYSCALL_STATUS_OK) {
		struct init_call_result result = init_advertise(
			&server->selector, PS2_INPUT_PROTOCOL_VERSION_MINOR, server->service_cap, INPUT_SERVICE_CLIENT_RIGHTS);
		status     = result.transport_status;
		advertised = result.status == INIT_REGISTRY_OK;
		if (!advertised && status == SYSCALL_STATUS_OK) status = SYSCALL_STATUS_FAILED;
	}
	drop_self_info(&self);
	if (status != SYSCALL_STATUS_OK) {
		if (advertised) (void)init_withdraw(&server->selector);
		/* A successfully spawned server retains its state until the process exits. */
		if (!server_started) cleanup_unstarted(server);
		return status;
	}
	*out_server = server;
	return SYSCALL_STATUS_OK;
}
