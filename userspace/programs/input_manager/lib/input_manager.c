#include <protocol/input_manager.h>
#include <runtime/input_manager.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

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

static bool control_value_valid(const struct input_manager_event* event) {
	switch (event->control_kind) {
	case INPUT_CONTROL_KIND_KEY:
	case INPUT_CONTROL_KIND_BUTTON:
		return event->value == 0 || event->value == 1;
	case INPUT_CONTROL_KIND_RELATIVE_AXIS:
		return true;
	case INPUT_CONTROL_KIND_ABSOLUTE_AXIS:
		return event->value >= event->logical_minimum && event->value <= event->logical_maximum;
	default:
		return false;
	}
}

syscall_status_t input_manager_get_info(cap_id_t manager_cap, struct input_manager_get_info_response* out_info) {
	const struct input_manager_get_info_request request = {.header = {.op = INPUT_MANAGER_OP_GET_INFO}};
	struct input_manager_get_info_response      response;
	size_t                                      response_size = 0u;

	if (manager_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(manager_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.topology_generation == 0u || response.queue_capacity == 0u ||
	    response.maximum_frame_events == 0u || response.maximum_frame_events > response.queue_capacity ||
	    ((response.newest_sequence == 0u) != (response.oldest_sequence == 0u)) ||
	    (response.newest_sequence != 0u && response.oldest_sequence > response.newest_sequence))
		return SYSCALL_STATUS_FAILED;
	*out_info = response;
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_manager_enumerate_sources(cap_id_t manager_cap, uint64_t topology_generation, uint64_t offset,
                                                 struct input_manager_source* sources, size_t capacity,
                                                 size_t* out_returned, uint64_t* out_total) {
	struct input_manager_enumerate_sources_response*     response;
	const struct input_manager_enumerate_sources_request request = {
		.header              = {.op = INPUT_MANAGER_OP_ENUMERATE_SOURCES},
		.topology_generation = topology_generation,
		.offset              = offset,
		.count               = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (manager_cap == CAP_ID_INVALID || topology_generation == 0u || sources == NULL || capacity == 0u ||
	    out_returned == NULL || out_total == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*sources))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*sources);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(manager_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->topology_generation != topology_generation ||
	    response->returned > capacity || response->returned > SIZE_MAX || response->returned > response->total ||
	    offset > response->total || (offset < response->total && response->returned == 0u) ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*sources)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		const struct input_manager_source* source = &response->sources[i];
		if (source->id == 0u || !device_class_valid(source->device_class) || source->descriptor_count == 0u ||
		    (source->output_descriptor_count == 0u) != (source->maximum_output_values == 0u) ||
		    source->maximum_output_values > source->output_descriptor_count) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
		for (size_t previous = 0u; previous < i; previous++) {
			if (response->sources[previous].id == source->id) {
				free(response);
				return SYSCALL_STATUS_FAILED;
			}
		}
	}
	memcpy(sources, response->sources, (size_t)response->returned * sizeof(*sources));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_manager_read_descriptors(cap_id_t manager_cap, uint64_t topology_generation, uint64_t source_id,
                                                uint64_t offset, struct input_control_descriptor* descriptors,
                                                size_t capacity, size_t* out_returned, uint64_t* out_total) {
	struct input_manager_read_descriptors_response*     response;
	const struct input_manager_read_descriptors_request request = {
		.header              = {.op = INPUT_MANAGER_OP_READ_DESCRIPTORS},
		.topology_generation = topology_generation,
		.source_id           = source_id,
		.offset              = offset,
		.count               = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (manager_cap == CAP_ID_INVALID || topology_generation == 0u || source_id == 0u || descriptors == NULL ||
	    capacity == 0u || out_returned == NULL || out_total == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*descriptors))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*descriptors);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(manager_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->topology_generation != topology_generation ||
	    response->returned > capacity || response->returned > SIZE_MAX || response->returned > response->total ||
	    offset > response->total || (offset < response->total && response->returned == 0u) ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*descriptors)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		if (!descriptor_valid(&response->descriptors[i])) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
		for (size_t previous = 0u; previous < i; previous++) {
			if (response->descriptors[previous].id == response->descriptors[i].id) {
				free(response);
				return SYSCALL_STATUS_FAILED;
			}
		}
	}
	memcpy(descriptors, response->descriptors, (size_t)response->returned * sizeof(*descriptors));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return SYSCALL_STATUS_OK;
}

static bool lifecycle_event_valid(const struct input_manager_event* event) {
	return event->control_id == 0u && event->control_kind == INPUT_CONTROL_KIND_INVALID && event->control_code == 0u &&
	       event->group_id == 0u && event->value == 0 && event->logical_minimum == 0 && event->logical_maximum == 0;
}

static bool manager_event_valid(const struct input_manager_event* event) {
	if (event->sequence == 0u || event->frame == 0u || event->source_id == 0u ||
	    !device_class_valid(event->device_class) || event->reserved != 0u ||
	    (event->flags & ~(uint32_t)INPUT_MANAGER_EVENT_FLAG_FRAME_END) != 0u)
		return false;
	switch (event->type) {
	case INPUT_MANAGER_EVENT_SOURCE_ADDED:
	case INPUT_MANAGER_EVENT_SOURCE_REMOVED:
	case INPUT_MANAGER_EVENT_SOURCE_RESET:
		return lifecycle_event_valid(event);
	case INPUT_MANAGER_EVENT_CONTROL: {
		const struct input_control_descriptor descriptor = {
			.id              = event->control_id,
			.kind            = event->control_kind,
			.code            = event->control_code,
			.group_id        = event->group_id,
			.logical_minimum = event->logical_minimum,
			.logical_maximum = event->logical_maximum,
		};
		return descriptor_valid(&descriptor) && control_value_valid(event);
	}
	default:
		return false;
	}
}

syscall_status_t input_manager_read_events(cap_id_t manager_cap, uint64_t after_sequence,
                                           struct input_manager_event* events, size_t capacity,
                                           struct input_manager_event_page* out_page) {
	struct input_manager_read_events_response*     response;
	const struct input_manager_read_events_request request = {
		.header         = {.op = INPUT_MANAGER_OP_READ_EVENTS},
		.after_sequence = after_sequence,
		.count          = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_page != NULL) *out_page = (struct input_manager_event_page){0};
	if (manager_cap == CAP_ID_INVALID || events == NULL || capacity == 0u || out_page == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*events))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*events);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(manager_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->reserved != 0u || response->returned == 0u ||
	    response->returned > capacity || response->returned > SIZE_MAX ||
	    (response->flags & ~(uint32_t)INPUT_MANAGER_READ_EVENT_FLAG_GAP) != 0u || response->oldest_sequence == 0u ||
	    response->newest_sequence < response->oldest_sequence || after_sequence > response->newest_sequence ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*events)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	bool gap = (response->flags & INPUT_MANAGER_READ_EVENT_FLAG_GAP) != 0u;
	if ((!gap && response->events[0].sequence != after_sequence + 1u) ||
	    (gap && response->events[0].sequence != response->oldest_sequence) ||
	    (gap && (after_sequence == UINT64_MAX || after_sequence + 1u >= response->oldest_sequence))) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		const struct input_manager_event* event = &response->events[i];
		if (!manager_event_valid(event) || event->sequence < response->oldest_sequence ||
		    event->sequence > response->newest_sequence ||
		    (event->type != INPUT_MANAGER_EVENT_CONTROL && (event->flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) == 0u) ||
		    (i != 0u && event->sequence != response->events[i - 1u].sequence + 1u) ||
		    (i != 0u && (response->events[i - 1u].flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) == 0u &&
		     (event->frame != response->events[i - 1u].frame ||
		      event->timestamp_ns != response->events[i - 1u].timestamp_ns ||
		      event->source_id != response->events[i - 1u].source_id || event->type != INPUT_MANAGER_EVENT_CONTROL ||
		      response->events[i - 1u].type != INPUT_MANAGER_EVENT_CONTROL)) ||
		    (i != 0u && (response->events[i - 1u].flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) != 0u &&
		     event->frame <= response->events[i - 1u].frame)) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
	}
	memcpy(events, response->events, (size_t)response->returned * sizeof(*events));
	*out_page = (struct input_manager_event_page){
		.oldest_sequence = response->oldest_sequence,
		.newest_sequence = response->newest_sequence,
		.returned        = (size_t)response->returned,
		.flags           = response->flags,
	};
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_manager_acquire_event_signal(cap_id_t manager_cap, cap_id_t* out_signal_cap) {
	const struct input_manager_acquire_event_signal_request request = {
		.header = {.op = INPUT_MANAGER_OP_ACQUIRE_EVENT_SIGNAL},
	};
	struct input_manager_acquire_event_signal_response response      = {.capability = CAP_ID_INVALID};
	size_t                                             response_size = 0u;

	if (manager_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	syscall_status_t status =
		cap_call(manager_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_manager_read_output_descriptors(cap_id_t manager_cap, uint64_t topology_generation,
                                                       uint64_t source_id, uint64_t offset,
                                                       struct input_output_descriptor* descriptors, size_t capacity,
                                                       size_t* out_returned, uint64_t* out_total) {
	struct input_manager_read_output_descriptors_response*     response;
	const struct input_manager_read_output_descriptors_request request = {
		.header              = {.op = INPUT_MANAGER_OP_READ_OUTPUT_DESCRIPTORS},
		.topology_generation = topology_generation,
		.source_id           = source_id,
		.offset              = offset,
		.count               = capacity,
	};
	size_t           response_capacity;
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_returned != NULL) *out_returned = 0u;
	if (out_total != NULL) *out_total = 0u;
	if (manager_cap == CAP_ID_INVALID || topology_generation == 0u || source_id == 0u || descriptors == NULL ||
	    capacity == 0u || out_returned == NULL || out_total == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*descriptors))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*descriptors);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(manager_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->topology_generation != topology_generation ||
	    response->returned > capacity || response->returned > SIZE_MAX || response->returned > response->total ||
	    offset > response->total || (offset < response->total && response->returned == 0u) ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*descriptors)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		if (!output_descriptor_valid(&response->descriptors[i])) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
		for (size_t previous = 0u; previous < i; previous++) {
			if (response->descriptors[previous].id == response->descriptors[i].id) {
				free(response);
				return SYSCALL_STATUS_FAILED;
			}
		}
	}
	memcpy(descriptors, response->descriptors, (size_t)response->returned * sizeof(*descriptors));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t input_manager_set_outputs(cap_id_t manager_cap, uint64_t topology_generation, uint64_t source_id,
                                           const struct input_output_value* values, size_t count) {
	struct input_manager_set_outputs_request* request;
	size_t                                    request_size;
	size_t                                    response_size = 0u;

	if (manager_cap == CAP_ID_INVALID || topology_generation == 0u || source_id == 0u || values == NULL ||
	    count == 0u || count > (CAP_MAX_REQUEST_SIZE - sizeof(*request)) / sizeof(*values))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	for (size_t i = 0u; i < count; i++) {
		if (values[i].output_id == 0u || values[i].reserved != 0u) return SYSCALL_STATUS_BAD_ARGUMENT;
		for (size_t previous = 0u; previous < i; previous++)
			if (values[previous].output_id == values[i].output_id) return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	request_size = sizeof(*request) + count * sizeof(*values);
	request      = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header              = (struct input_manager_request_header){.op = INPUT_MANAGER_OP_SET_OUTPUTS};
	request->reserved            = 0u;
	request->topology_generation = topology_generation;
	request->source_id           = source_id;
	request->count               = count;
	memcpy(request->values, values, count * sizeof(*values));
	syscall_status_t status = cap_call(manager_cap, request, request_size, NULL, 0u, &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}
