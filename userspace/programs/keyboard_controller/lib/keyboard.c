#include <protocol/keyboard.h>
#include <runtime/keyboard.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

#define KEYBOARD_PROTOCOL_VERSION_MAJOR 1u
#define KEYBOARD_PROTOCOL_VERSION_MINOR 0u

static const uint32_t modifier_mask = KEYBOARD_MODIFIER_LEFT_SHIFT | KEYBOARD_MODIFIER_RIGHT_SHIFT |
                                      KEYBOARD_MODIFIER_LEFT_CONTROL | KEYBOARD_MODIFIER_RIGHT_CONTROL |
                                      KEYBOARD_MODIFIER_LEFT_ALT | KEYBOARD_MODIFIER_RIGHT_ALT |
                                      KEYBOARD_MODIFIER_LEFT_META | KEYBOARD_MODIFIER_RIGHT_META;
static const uint32_t lock_mask     = KEYBOARD_LOCK_CAPS_LOCK | KEYBOARD_LOCK_NUM_LOCK | KEYBOARD_LOCK_SCROLL_LOCK;

struct registry_call_result keyboard_acquire(struct registry_service_handle* out_handle) {
	const struct registry_protocol_query query = {
		.namespace_path = KEYBOARD_NAMESPACE,
		.protocol       = KEYBOARD_PROTOCOL_NAME,
		.major          = KEYBOARD_PROTOCOL_VERSION_MAJOR,
		.minor          = KEYBOARD_PROTOCOL_VERSION_MINOR,
	};
	if (out_handle == NULL)
		return (struct registry_call_result){.status           = REGISTRY_STATUS_INVALID_ARGUMENT,
		                                     .transport_status = SYSCALL_STATUS_BAD_ARGUMENT};
	*out_handle = (struct registry_service_handle){.capability = CAP_ID_INVALID};
	return registry_acquire(&query, KEYBOARD_SERVICE_NAME, out_handle);
}

syscall_status_t keyboard_get_info(cap_id_t keyboard_cap, struct keyboard_get_info_response* out_info) {
	const struct keyboard_get_info_request request = {.header = {.op = KEYBOARD_OP_GET_INFO}};
	struct keyboard_get_info_response      response;
	size_t                                 response_size = 0u;
	if (keyboard_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(keyboard_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.topology_generation == 0u || response.queue_capacity == 0u ||
	    response.maximum_frame_events == 0u || response.maximum_frame_events > response.queue_capacity ||
	    response.repeat_delay_ms == 0u || response.repeat_interval_ms == 0u ||
	    ((response.oldest_sequence == 0u) != (response.newest_sequence == 0u)) ||
	    (response.newest_sequence != 0u && response.oldest_sequence > response.newest_sequence))
		return SYSCALL_STATUS_FAILED;
	*out_info = response;
	return SYSCALL_STATUS_OK;
}

syscall_status_t keyboard_enumerate(cap_id_t keyboard_cap, uint64_t topology_generation, uint64_t offset,
                                    struct keyboard_info* keyboards, size_t capacity, size_t* out_returned,
                                    uint64_t* out_total) {
	struct keyboard_enumerate_response*     response;
	const struct keyboard_enumerate_request request = {.header              = {.op = KEYBOARD_OP_ENUMERATE},
	                                                   .topology_generation = topology_generation,
	                                                   .offset              = offset,
	                                                   .count               = capacity};
	size_t                                  response_capacity;
	size_t                                  response_size = 0u;
	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (keyboard_cap == CAP_ID_INVALID || topology_generation == 0u || keyboards == NULL || capacity == 0u ||
	    out_returned == NULL || out_total == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*keyboards))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*keyboards);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	syscall_status_t status =
		cap_call(keyboard_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->topology_generation != topology_generation ||
	    response->returned > capacity || response->returned > response->total || offset > response->total ||
	    (offset < response->total && response->returned == 0u) ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*keyboards)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		if (response->keyboards[i].id == 0u) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
		for (size_t previous = 0u; previous < i; previous++) {
			if (response->keyboards[previous].id == response->keyboards[i].id) {
				free(response);
				return SYSCALL_STATUS_FAILED;
			}
		}
	}
	memcpy(keyboards, response->keyboards, (size_t)response->returned * sizeof(*keyboards));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t keyboard_read_state(cap_id_t keyboard_cap, uint64_t keyboard_id, enum input_key_code* pressed_keys,
                                     size_t capacity, struct keyboard_state* out_state) {
	const struct keyboard_read_state_request request = {.header      = {.op = KEYBOARD_OP_READ_STATE},
	                                                    .keyboard_id = keyboard_id};
	struct keyboard_read_state_response*     response;
	size_t                                   response_capacity;
	size_t                                   response_size = 0u;
	if (out_state != NULL) *out_state = (struct keyboard_state){0};
	if (keyboard_cap == CAP_ID_INVALID || keyboard_id == 0u || pressed_keys == NULL || capacity == 0u ||
	    out_state == NULL || capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*pressed_keys))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*pressed_keys);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	syscall_status_t status =
		cap_call(keyboard_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->sequence == 0u || response->keyboard_id != keyboard_id ||
	    response->reserved != 0u || response->pressed_count > capacity ||
	    (response->modifiers & ~modifier_mask) != 0u || (response->locks & ~lock_mask) != 0u ||
	    response_size != sizeof(*response) + (size_t)response->pressed_count * sizeof(*pressed_keys)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < response->pressed_count; i++) {
		if (response->pressed_keys[i] <= INPUT_KEY_INVALID || response->pressed_keys[i] > INPUT_KEY_KEYPAD_DECIMAL ||
		    (i != 0u && response->pressed_keys[i] <= response->pressed_keys[i - 1u])) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
	}
	memcpy(pressed_keys, response->pressed_keys, response->pressed_count * sizeof(*pressed_keys));
	*out_state = (struct keyboard_state){
		.sequence      = response->sequence,
		.keyboard_id   = response->keyboard_id,
		.modifiers     = response->modifiers,
		.locks         = response->locks,
		.pressed_count = response->pressed_count,
	};
	free(response);
	return SYSCALL_STATUS_OK;
}

static bool event_valid(const struct keyboard_event* event) {
	if (event->sequence == 0u || event->frame == 0u || event->keyboard_id == 0u || event->reserved != 0u ||
	    (event->modifiers & ~modifier_mask) != 0u || (event->locks & ~lock_mask) != 0u ||
	    (event->flags & ~(uint32_t)KEYBOARD_EVENT_FLAG_FRAME_END) != 0u)
		return false;
	if (event->type == KEYBOARD_EVENT_KEY)
		return event->action >= KEYBOARD_KEY_ACTION_PRESSED && event->action <= KEYBOARD_KEY_ACTION_REPEATED &&
		       event->key > INPUT_KEY_INVALID && event->key <= INPUT_KEY_KEYPAD_DECIMAL;
	return event->type >= KEYBOARD_EVENT_ADDED && event->type <= KEYBOARD_EVENT_RESET &&
	       event->action == KEYBOARD_KEY_ACTION_INVALID && event->key == INPUT_KEY_INVALID &&
	       (event->flags & KEYBOARD_EVENT_FLAG_FRAME_END) != 0u;
}

syscall_status_t keyboard_read_events(cap_id_t keyboard_cap, uint64_t after_sequence, struct keyboard_event* events,
                                      size_t capacity, struct keyboard_event_page* out_page) {
	const struct keyboard_read_events_request request = {
		.header = {.op = KEYBOARD_OP_READ_EVENTS}, .after_sequence = after_sequence, .count = capacity};
	struct keyboard_read_events_response* response;
	size_t                                response_capacity;
	size_t                                response_size = 0u;
	if (out_page != NULL) *out_page = (struct keyboard_event_page){0};
	if (keyboard_cap == CAP_ID_INVALID || events == NULL || capacity == 0u || out_page == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*events))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*events);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	syscall_status_t status =
		cap_call(keyboard_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->returned == 0u || response->returned > capacity ||
	    response->reserved != 0u || (response->flags & ~(uint32_t)KEYBOARD_READ_EVENT_FLAG_GAP) != 0u ||
	    response->oldest_sequence == 0u || response->newest_sequence < response->oldest_sequence ||
	    after_sequence > response->newest_sequence ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*events)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	bool gap = (response->flags & KEYBOARD_READ_EVENT_FLAG_GAP) != 0u;
	if ((!gap && response->events[0].sequence != after_sequence + 1u) ||
	    (gap && response->events[0].sequence != response->oldest_sequence) ||
	    (gap && (after_sequence == UINT64_MAX || after_sequence + 1u >= response->oldest_sequence))) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < response->returned; i++) {
		if (!event_valid(&response->events[i]) ||
		    (i != 0u && response->events[i].sequence != response->events[i - 1u].sequence + 1u) ||
		    (i != 0u && (response->events[i - 1u].flags & KEYBOARD_EVENT_FLAG_FRAME_END) == 0u &&
		     (response->events[i].frame != response->events[i - 1u].frame ||
		      response->events[i].keyboard_id != response->events[i - 1u].keyboard_id ||
		      response->events[i].type != KEYBOARD_EVENT_KEY || response->events[i - 1u].type != KEYBOARD_EVENT_KEY)) ||
		    (i != 0u && (response->events[i - 1u].flags & KEYBOARD_EVENT_FLAG_FRAME_END) != 0u &&
		     response->events[i].frame <= response->events[i - 1u].frame)) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
	}
	memcpy(events, response->events, (size_t)response->returned * sizeof(*events));
	*out_page = (struct keyboard_event_page){
		.oldest_sequence = response->oldest_sequence,
		.newest_sequence = response->newest_sequence,
		.returned        = (size_t)response->returned,
		.flags           = response->flags,
	};
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t keyboard_acquire_event_signal(cap_id_t keyboard_cap, cap_id_t* out_signal_cap) {
	const struct keyboard_acquire_event_signal_request request  = {.header = {.op = KEYBOARD_OP_ACQUIRE_EVENT_SIGNAL}};
	struct keyboard_acquire_event_signal_response      response = {.capability = CAP_ID_INVALID};
	size_t                                             response_size = 0u;
	if (keyboard_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	syscall_status_t status =
		cap_call(keyboard_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}
