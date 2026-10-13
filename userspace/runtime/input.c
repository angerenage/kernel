#include <base/channel.h>
#include <base/process.h>
#include <limits.h>
#include <protocol/input.h>
#include <runtime/init.h>
#include <runtime/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#define INPUT_PROVIDER_OBJECT_ID 1u
#define INPUT_PROVIDER_MAX_DESCRIPTORS 1024u
#define INPUT_PROVIDER_EVENT_QUEUE_CAPACITY 512u

/* Retained event state shared by a producer and its capability server. */
struct input_state {
	enum input_device_class          device_class;
	struct input_control_descriptor* descriptors;
	size_t                           descriptor_count;
	struct input_event               events[INPUT_PROVIDER_EVENT_QUEUE_CAPACITY];
	size_t                           event_tail;
	size_t                           event_count;
	uint64_t                         next_sequence;
	uint64_t                         next_frame;
	bool                             locked;
};

/* Metadata returned by one successful retained-event read. */
struct input_state_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Provider state owned for the lifetime of one producer process. */
struct input_provider {
	struct input_state           state;
	channel_id_t                 endpoint;
	cap_id_t                     activity_cap;
	cap_id_t                     event_signal_cap;
	cap_id_t                     service_cap;
	struct init_service_selector selector;
};

/* Copied onto the detached server thread's initial stack. */
struct input_server_argument {
	struct input_provider* provider;
};

static void input_state_lock(struct input_state* state) {
	while (__atomic_test_and_set(&state->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void input_state_unlock(struct input_state* state) {
	__atomic_clear(&state->locked, __ATOMIC_RELEASE);
}

static bool input_device_class_valid(enum input_device_class device_class) {
	return device_class > INPUT_DEVICE_CLASS_INVALID && device_class <= INPUT_DEVICE_CLASS_OTHER;
}

static bool input_descriptor_valid(const struct input_control_descriptor* descriptor) {
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

static bool input_descriptors_valid(const struct input_control_descriptor* descriptors, size_t count) {
	if (descriptors == NULL || count == 0u || count > INPUT_PROVIDER_MAX_DESCRIPTORS || count > UINT32_MAX)
		return false;
	for (size_t i = 0u; i < count; i++) {
		if (!input_descriptor_valid(&descriptors[i])) return false;
		for (size_t previous = 0u; previous < i; previous++)
			if (descriptors[previous].id == descriptors[i].id) return false;
	}
	return true;
}

static syscall_status_t input_state_initialize(struct input_state* state, enum input_device_class device_class,
                                               const struct input_control_descriptor* descriptors,
                                               size_t                                 descriptor_count) {
	if (state == NULL || !input_device_class_valid(device_class) ||
	    !input_descriptors_valid(descriptors, descriptor_count))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	struct input_control_descriptor* copy = malloc(descriptor_count * sizeof(*copy));
	if (copy == NULL) return SYSCALL_STATUS_FAILED;
	memcpy(copy, descriptors, descriptor_count * sizeof(*copy));
	memset(state, 0, sizeof(*state));
	state->device_class     = device_class;
	state->descriptors      = copy;
	state->descriptor_count = descriptor_count;
	state->next_sequence    = 1u;
	state->next_frame       = 1u;
	return SYSCALL_STATUS_OK;
}

static void input_state_destroy(struct input_state* state) {
	if (state == NULL) return;
	free(state->descriptors);
	memset(state, 0, sizeof(*state));
}

static syscall_status_t input_state_read_descriptors(struct input_state* state, uint64_t offset,
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

static const struct input_control_descriptor* input_find_descriptor(const struct input_state* state, uint32_t id) {
	for (size_t i = 0u; i < state->descriptor_count; i++)
		if (state->descriptors[i].id == id) return &state->descriptors[i];
	return NULL;
}

static bool input_value_valid(const struct input_control_descriptor* descriptor, int64_t value) {
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

static size_t input_event_index(const struct input_state* state, size_t offset) {
	return (state->event_tail + offset) % INPUT_PROVIDER_EVENT_QUEUE_CAPACITY;
}

static void input_evict_oldest_frame(struct input_state* state) {
	while (state->event_count != 0u) {
		bool frame_end    = (state->events[state->event_tail].flags & INPUT_EVENT_FLAG_FRAME_END) != 0u;
		state->event_tail = (state->event_tail + 1u) % INPUT_PROVIDER_EVENT_QUEUE_CAPACITY;
		state->event_count--;
		if (frame_end) return;
	}
}

static syscall_status_t input_append_frame(struct input_state* state, enum input_event_type type,
                                           const struct input_control_value* values, size_t count,
                                           uint64_t* out_newest_sequence) {
	if (out_newest_sequence != NULL) *out_newest_sequence = 0u;
	if (state == NULL || out_newest_sequence == NULL ||
	    (type == INPUT_EVENT_TYPE_CONTROL && (values == NULL || count == 0u)) ||
	    (type == INPUT_EVENT_TYPE_STATE_RESET && (values != NULL || count != 1u)) ||
	    count > INPUT_PROVIDER_MAX_FRAME_EVENTS)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	input_state_lock(state);
	if (type == INPUT_EVENT_TYPE_CONTROL) {
		for (size_t i = 0u; i < count; i++) {
			const struct input_control_descriptor* descriptor = input_find_descriptor(state, values[i].control_id);
			if (descriptor == NULL || !input_value_valid(descriptor, values[i].value)) {
				input_state_unlock(state);
				return SYSCALL_STATUS_BAD_ARGUMENT;
			}
			for (size_t previous = 0u; previous < i; previous++) {
				if (values[previous].control_id == values[i].control_id) {
					input_state_unlock(state);
					return SYSCALL_STATUS_BAD_ARGUMENT;
				}
			}
		}
	}
	if (state->next_sequence == 0u || state->next_sequence > UINT64_MAX - count || state->next_frame == 0u ||
	    state->next_frame == UINT64_MAX) {
		input_state_unlock(state);
		return SYSCALL_STATUS_FAILED;
	}
	while (INPUT_PROVIDER_EVENT_QUEUE_CAPACITY - state->event_count < count) input_evict_oldest_frame(state);
	for (size_t i = 0u; i < count; i++) {
		size_t index         = input_event_index(state, state->event_count);
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
	input_state_unlock(state);
	return SYSCALL_STATUS_OK;
}

static syscall_status_t input_state_read_events(struct input_state* state, uint64_t after_sequence,
                                                struct input_event* events, size_t capacity,
                                                struct input_state_event_page* out_page) {
	if (out_page != NULL) *out_page = (struct input_state_event_page){0};
	if (state == NULL || events == NULL || capacity == 0u || out_page == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	input_state_lock(state);
	uint64_t newest = state->next_sequence - 1u;
	if (after_sequence > newest) {
		input_state_unlock(state);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (state->event_count == 0u || after_sequence == newest) {
		input_state_unlock(state);
		return SYSCALL_STATUS_UNAVAILABLE;
	}
	uint64_t oldest = state->events[state->event_tail].sequence;
	uint64_t wanted = after_sequence + 1u;
	bool     gap    = wanted < oldest;
	if (gap) wanted = oldest;
	size_t skip = (size_t)(wanted - oldest);
	if (skip >= state->event_count) {
		input_state_unlock(state);
		return SYSCALL_STATUS_UNAVAILABLE;
	}
	size_t returned = state->event_count - skip;
	if (returned > capacity) returned = capacity;
	for (size_t i = 0u; i < returned; i++) events[i] = state->events[input_event_index(state, skip + i)];
	*out_page = (struct input_state_event_page){
		.oldest_sequence = oldest,
		.newest_sequence = newest,
		.returned        = returned,
		.flags           = gap ? INPUT_READ_EVENT_FLAG_GAP : INPUT_READ_EVENT_FLAG_NONE,
	};
	input_state_unlock(state);
	return SYSCALL_STATUS_OK;
}

static bool input_service_name_valid(const char* service) {
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

syscall_status_t input_provider_publish(struct input_provider* provider, const struct input_control_value* values,
                                        size_t count) {
	if (provider == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	uint64_t         newest;
	syscall_status_t status = input_append_frame(&provider->state, INPUT_EVENT_TYPE_CONTROL, values, count, &newest);
	if (status != SYSCALL_STATUS_OK) return status;
	struct signal_send_response response;
	return signal_send(provider->event_signal_cap, newest, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response);
}

syscall_status_t input_provider_reset(struct input_provider* provider) {
	if (provider == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	uint64_t         newest;
	syscall_status_t status = input_append_frame(&provider->state, INPUT_EVENT_TYPE_STATE_RESET, NULL, 1u, &newest);
	if (status != SYSCALL_STATUS_OK) return status;
	struct signal_send_response response;
	return signal_send(provider->event_signal_cap, newest, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response);
}

static bool input_reply(const struct cap_request* call, const void* response, size_t response_size,
                        syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool input_dispatch_get_info(struct input_provider* provider, const struct cap_request* call) {
	const struct input_get_info_response response = {
		.device_class         = provider->state.device_class,
		.descriptor_count     = (uint32_t)provider->state.descriptor_count,
		.queue_capacity       = INPUT_PROVIDER_EVENT_QUEUE_CAPACITY,
		.maximum_frame_events = INPUT_PROVIDER_MAX_FRAME_EVENTS,
	};
	if (call->request_size != sizeof(struct input_get_info_request) || call->response_capacity < sizeof(response))
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return input_reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool input_dispatch_read_descriptors(struct input_provider* provider, const struct cap_request* call,
                                            const void* request_data, void* response_buffer) {
	struct input_read_descriptors_request   request;
	struct input_read_descriptors_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->descriptors))
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	size_t   returned;
	uint64_t total;
	if (input_state_read_descriptors(
			&provider->state, request.offset, response->descriptors, (size_t)request.count, &returned, &total) !=
	    SYSCALL_STATUS_OK)
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	response->total    = total;
	response->returned = returned;
	return input_reply(
		call, response, sizeof(*response) + returned * sizeof(*response->descriptors), SYSCALL_STATUS_OK);
}

static bool input_dispatch_read_events(struct input_provider* provider, const struct cap_request* call,
                                       const void* request_data, void* response_buffer) {
	struct input_read_events_request   request;
	struct input_read_events_response* response = response_buffer;
	if (call->request_size != sizeof(request)) return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (call->response_capacity < sizeof(*response) || request.reserved != 0u || request.count == 0u ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(*response->events))
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct input_state_event_page page;
	syscall_status_t              status = input_state_read_events(
		&provider->state, request.after_sequence, response->events, (size_t)request.count, &page);
	if (status != SYSCALL_STATUS_OK) return input_reply(call, NULL, 0u, status);
	response->oldest_sequence = page.oldest_sequence;
	response->newest_sequence = page.newest_sequence;
	response->returned        = page.returned;
	response->flags           = page.flags;
	response->reserved        = 0u;
	return input_reply(
		call, response, sizeof(*response) + page.returned * sizeof(*response->events), SYSCALL_STATUS_OK);
}

static bool input_dispatch_acquire_signal(struct input_provider* provider, const struct cap_request* call) {
	struct input_acquire_event_signal_response response = {.capability = CAP_ID_INVALID};
	if (call->request_size != sizeof(struct input_acquire_event_signal_request) ||
	    call->response_capacity < sizeof(response))
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		cap_delegate(provider->event_signal_cap, call->caller, INPUT_EVENT_SIGNAL_RIGHTS, &response.capability);
	if (status != SYSCALL_STATUS_OK) return input_reply(call, NULL, 0u, status);
	if (input_reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool input_dispatch(struct input_provider* provider, const struct cap_request* call, const void* request_data,
                           void* response_buffer) {
	struct input_request_header header;
	if ((call->rights & CAP_READ) == 0u) return input_reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (request_data == NULL || call->request_size < sizeof(header))
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, request_data, sizeof(header));
	switch (header.op) {
	case INPUT_OP_GET_INFO:
		return input_dispatch_get_info(provider, call);
	case INPUT_OP_READ_DESCRIPTORS:
		return input_dispatch_read_descriptors(provider, call, request_data, response_buffer);
	case INPUT_OP_READ_EVENTS:
		return input_dispatch_read_events(provider, call, request_data, response_buffer);
	case INPUT_OP_ACQUIRE_EVENT_SIGNAL:
		return input_dispatch_acquire_signal(provider, call);
	default:
		return input_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static void input_server(void* opaque) {
	const struct input_server_argument* argument = opaque;
	struct input_provider*              provider = argument->provider;
	uint8_t                             request_buffer[sizeof(struct input_read_descriptors_request)];
	void*                               response_buffer = malloc(CAP_MAX_RESPONSE_SIZE);
	if (provider == NULL || response_buffer == NULL) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(provider->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK ||
			    (received && !input_dispatch(provider, &call, request_buffer, response_buffer)))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(provider->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		status = signal_wait(provider->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

static void input_drop_self_info(struct self_info* self) {
	if (self->self_cap != CAP_ID_INVALID) (void)cap_drop(self->self_cap);
	if (self->address_space_cap != CAP_ID_INVALID) (void)cap_drop(self->address_space_cap);
	if (self->main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self->main_thread_cap);
}

static void input_cleanup_unstarted(struct input_provider* provider) {
	if (provider == NULL) return;
	if (provider->service_cap != CAP_ID_INVALID) (void)cap_drop(provider->service_cap);
	if (provider->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(provider->endpoint);
	if (provider->event_signal_cap != CAP_ID_INVALID) (void)signal_destroy(provider->event_signal_cap);
	input_state_destroy(&provider->state);
	free(provider);
}

syscall_status_t input_provider_create(const char* service, uint32_t protocol_major, uint32_t protocol_minor,
                                       enum input_device_class                device_class,
                                       const struct input_control_descriptor* descriptors, size_t descriptor_count,
                                       struct input_provider** out_provider) {
	struct input_provider* provider;
	struct self_info       self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	cap_id_t         thread_cap = CAP_ID_INVALID;
	syscall_status_t status;
	bool             advertised = false;

	if (out_provider != NULL) *out_provider = NULL;
	if (!input_service_name_valid(service) || out_provider == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	provider = calloc(1u, sizeof(*provider));
	if (provider == NULL) return SYSCALL_STATUS_FAILED;
	status = input_state_initialize(&provider->state, device_class, descriptors, descriptor_count);
	if (status != SYSCALL_STATUS_OK) {
		free(provider);
		return status;
	}
	provider->endpoint         = CHANNEL_ID_INVALID;
	provider->activity_cap     = CAP_ID_INVALID;
	provider->event_signal_cap = CAP_ID_INVALID;
	provider->service_cap      = CAP_ID_INVALID;
	provider->selector         = (struct init_service_selector){
		.namespace_path = INPUT_NAMESPACE,
		.protocol       = INPUT_PROTOCOL_NAME,
		.major          = protocol_major,
	};
	(void)strlcpy(provider->selector.service, service, sizeof(provider->selector.service));

	status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK) status = signal_create(&provider->event_signal_cap);
	if (status == SYSCALL_STATUS_OK) status = channel_create(&provider->endpoint, &provider->activity_cap);
	if (status == SYSCALL_STATUS_OK) {
		status = cap_publish(provider->endpoint,
		                     INPUT_PROVIDER_OBJECT_ID,
		                     self.pid,
		                     CAP_CALL | CAP_READ | CAP_DELEGATE,
		                     &provider->service_cap);
	}
	if (status == SYSCALL_STATUS_OK) {
		struct init_call_result result =
			init_advertise(&provider->selector, protocol_minor, provider->service_cap, INPUT_SERVICE_CLIENT_RIGHTS);
		status     = result.transport_status;
		advertised = result.status == INIT_REGISTRY_OK;
		if (!advertised && status == SYSCALL_STATUS_OK) status = SYSCALL_STATUS_FAILED;
	}
	if (status == SYSCALL_STATUS_OK) {
		const struct input_server_argument argument = {.provider = provider};
		status                                      = process_spawn_thread(
			self.self_cap, (uintptr_t)input_server, &argument, sizeof(argument), NULL, 0u, &thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK && drop_status != SYSCALL_STATUS_OK) status = drop_status;
	}
	input_drop_self_info(&self);
	if (status != SYSCALL_STATUS_OK) {
		if (advertised) (void)init_withdraw(&provider->selector);
		/* A successfully spawned server retains provider until process exit. */
		if (thread_cap == CAP_ID_INVALID) input_cleanup_unstarted(provider);
		return status;
	}
	*out_provider = provider;
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_get_info(cap_id_t input_cap, struct input_get_info_response* out_info) {
	const struct input_get_info_request request = {.header = {.op = INPUT_OP_GET_INFO}};
	struct input_get_info_response      response;
	size_t                              response_size = 0u;

	if (input_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(input_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.device_class <= INPUT_DEVICE_CLASS_INVALID ||
	    response.device_class > INPUT_DEVICE_CLASS_OTHER || response.descriptor_count == 0u ||
	    response.queue_capacity == 0u || response.maximum_frame_events == 0u)
		return SYSCALL_STATUS_FAILED;
	*out_info = response;
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_read_descriptors(cap_id_t input_cap, uint64_t offset,
                                        struct input_control_descriptor* descriptors, size_t capacity,
                                        size_t* out_returned, uint64_t* out_total) {
	struct input_read_descriptors_response*     response;
	const struct input_read_descriptors_request request = {
		.header = {.op = INPUT_OP_READ_DESCRIPTORS},
		.offset = offset,
		.count  = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (input_cap == CAP_ID_INVALID || descriptors == NULL || capacity == 0u || out_returned == NULL ||
	    out_total == NULL || capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*descriptors))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*descriptors);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(input_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->returned > capacity || response->returned > SIZE_MAX ||
	    response->returned > response->total || offset > response->total ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*descriptors)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	if (response->returned != 0u)
		memcpy(descriptors, response->descriptors, (size_t)response->returned * sizeof(*descriptors));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_read_events(cap_id_t input_cap, uint64_t after_sequence, struct input_event* events,
                                   size_t capacity, struct input_event_page* out_page) {
	struct input_read_events_response*     response;
	const struct input_read_events_request request = {
		.header         = {.op = INPUT_OP_READ_EVENTS},
		.after_sequence = after_sequence,
		.count          = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_page != NULL) *out_page = (struct input_event_page){0};
	if (input_cap == CAP_ID_INVALID || events == NULL || capacity == 0u || out_page == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*events))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*events);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(input_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->reserved != 0u || response->returned == 0u ||
	    response->returned > capacity || response->returned > SIZE_MAX ||
	    (response->flags & ~(uint32_t)INPUT_READ_EVENT_FLAG_GAP) != 0u || response->oldest_sequence == 0u ||
	    response->newest_sequence < response->oldest_sequence ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*events)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		const struct input_event* event = &response->events[i];
		if (event->sequence == 0u || event->frame == 0u || event->reserved != 0u ||
		    event->type <= INPUT_EVENT_TYPE_INVALID || event->type > INPUT_EVENT_TYPE_STATE_RESET ||
		    (event->flags & ~(uint32_t)INPUT_EVENT_FLAG_FRAME_END) != 0u ||
		    (i != 0u && event->sequence != response->events[i - 1u].sequence + 1u) ||
		    event->sequence < response->oldest_sequence || event->sequence > response->newest_sequence ||
		    (event->type == INPUT_EVENT_TYPE_CONTROL && event->control_id == 0u) ||
		    (event->type == INPUT_EVENT_TYPE_STATE_RESET && (event->control_id != 0u || event->value != 0))) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
	}
	memcpy(events, response->events, (size_t)response->returned * sizeof(*events));
	*out_page = (struct input_event_page){
		.oldest_sequence = response->oldest_sequence,
		.newest_sequence = response->newest_sequence,
		.returned        = (size_t)response->returned,
		.flags           = response->flags,
	};
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_acquire_event_signal(cap_id_t input_cap, cap_id_t* out_signal_cap) {
	const struct input_acquire_event_signal_request request = {
		.header = {.op = INPUT_OP_ACQUIRE_EVENT_SIGNAL},
	};
	struct input_acquire_event_signal_response response      = {.capability = CAP_ID_INVALID};
	size_t                                     response_size = 0u;

	if (input_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	syscall_status_t status =
		cap_call(input_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}
