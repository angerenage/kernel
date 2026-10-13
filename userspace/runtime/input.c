#include <protocol/input.h>
#include <runtime/input.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

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
