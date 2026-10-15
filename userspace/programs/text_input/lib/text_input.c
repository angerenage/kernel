#include <protocol/text_input.h>
#include <runtime/text_input.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <utf8proc.h>

#define TEXT_INPUT_PROTOCOL_VERSION_MAJOR 1u
#define TEXT_INPUT_PROTOCOL_VERSION_MINOR 0u

struct init_call_result text_input_acquire(struct init_service_handle* out_handle) {
	const struct init_protocol_query query = {
		.namespace_path = TEXT_INPUT_NAMESPACE,
		.protocol       = TEXT_INPUT_PROTOCOL_NAME,
		.major          = TEXT_INPUT_PROTOCOL_VERSION_MAJOR,
		.minor          = TEXT_INPUT_PROTOCOL_VERSION_MINOR,
	};
	if (out_handle == NULL)
		return (struct init_call_result){.status           = INIT_REGISTRY_INVALID_ARGUMENT,
		                                 .transport_status = SYSCALL_STATUS_BAD_ARGUMENT};
	*out_handle = (struct init_service_handle){.capability = CAP_ID_INVALID};
	return init_acquire(&query, TEXT_INPUT_SERVICE_NAME, out_handle);
}

syscall_status_t text_input_get_info(cap_id_t text_input_cap, struct text_input_get_info_response* out_info) {
	const struct text_input_get_info_request request = {.header = {.op = TEXT_INPUT_OP_GET_INFO}};
	struct text_input_get_info_response      response;
	size_t                                   response_size = 0u;
	if (text_input_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(text_input_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.queue_capacity == 0u || response.maximum_frame_events == 0u ||
	    response.maximum_frame_events > response.queue_capacity ||
	    response.text_capacity_per_event != TEXT_INPUT_EVENT_TEXT_CAPACITY || response.reserved != 0u ||
	    ((response.oldest_sequence == 0u) != (response.newest_sequence == 0u)) ||
	    (response.newest_sequence != 0u && response.oldest_sequence > response.newest_sequence))
		return SYSCALL_STATUS_FAILED;
	*out_info = response;
	return SYSCALL_STATUS_OK;
}

static bool utf8_valid(const uint8_t* text, size_t size) {
	size_t offset = 0u;
	while (offset < size) {
		utf8proc_int32_t codepoint;
		utf8proc_ssize_t consumed = utf8proc_iterate(text + offset, (utf8proc_ssize_t)(size - offset), &codepoint);
		if (consumed <= 0) return false;
		offset += (size_t)consumed;
	}
	return true;
}

static bool event_valid(const struct text_input_event* event) {
	if (event->sequence == 0u || event->frame == 0u || event->source_id == 0u ||
	    (event->flags & ~(uint32_t)(TEXT_INPUT_EVENT_FLAG_TEXT_END | TEXT_INPUT_EVENT_FLAG_FRAME_END)) != 0u ||
	    event->text_size > TEXT_INPUT_EVENT_TEXT_CAPACITY || event->text_offset > event->text_total_size ||
	    event->text_size > event->text_total_size - event->text_offset || !utf8_valid(event->text, event->text_size))
		return false;
	for (size_t i = event->text_size; i < TEXT_INPUT_EVENT_TEXT_CAPACITY; i++)
		if (event->text[i] != 0u) return false;
	switch (event->type) {
	case TEXT_INPUT_EVENT_RESET:
		return event->composition_id == 0u && event->text_offset == 0u && event->text_size == 0u &&
		       event->text_total_size == 0u && event->selection_start == 0u && event->selection_end == 0u &&
		       event->delete_before == 0u && event->delete_after == 0u &&
		       (event->flags & TEXT_INPUT_EVENT_FLAG_TEXT_END) != 0u;
	case TEXT_INPUT_EVENT_PREEDIT:
		return event->composition_id != 0u && event->selection_start <= event->selection_end &&
		       event->selection_end <= event->text_total_size && event->delete_before == 0u &&
		       event->delete_after == 0u;
	case TEXT_INPUT_EVENT_COMMIT:
		return event->text_total_size != 0u && event->selection_start == 0u && event->selection_end == 0u &&
		       event->delete_before == 0u && event->delete_after == 0u;
	case TEXT_INPUT_EVENT_DELETE_SURROUNDING:
		return event->text_offset == 0u && event->text_size == 0u && event->text_total_size == 0u &&
		       event->selection_start == 0u && event->selection_end == 0u &&
		       (event->delete_before != 0u || event->delete_after != 0u) &&
		       (event->flags & TEXT_INPUT_EVENT_FLAG_TEXT_END) != 0u;
	default:
		return false;
	}
}

static bool text_boundary(const struct text_input_event* events, size_t first, size_t last, uint32_t offset) {
	if (offset == events[first].text_total_size) return true;
	for (size_t i = first; i <= last; i++) {
		if (offset < events[i].text_offset || offset >= events[i].text_offset + events[i].text_size) continue;
		uint32_t local = offset - events[i].text_offset;
		return local == 0u || (events[i].text[local] & 0xc0u) != 0x80u;
	}
	return false;
}

static bool events_valid(const struct text_input_event* events, size_t count) {
	bool     action_open  = false;
	uint64_t frame        = 0u;
	uint64_t source_id    = 0u;
	uint32_t text_offset  = 0u;
	size_t   action_start = 0u;
	for (size_t i = 0u; i < count; i++) {
		const struct text_input_event* event = &events[i];
		if (!event_valid(event) || (i != 0u && event->sequence != events[i - 1u].sequence + 1u)) return false;
		if (i == 0u || (events[i - 1u].flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) != 0u) {
			if (action_open || event->text_offset != 0u || (i != 0u && event->frame <= frame)) return false;
			frame     = event->frame;
			source_id = event->source_id;
		}
		else if (event->frame != frame || event->source_id != source_id) {
			return false;
		}
		if (!action_open) {
			if (event->text_offset != 0u) return false;
			text_offset  = 0u;
			action_start = i;
			action_open  = true;
		}
		else {
			const struct text_input_event* previous = &events[i - 1u];
			if (event->type != previous->type || event->composition_id != previous->composition_id ||
			    event->text_total_size != previous->text_total_size ||
			    event->selection_start != previous->selection_start ||
			    event->selection_end != previous->selection_end || event->delete_before != previous->delete_before ||
			    event->delete_after != previous->delete_after || event->text_offset != text_offset)
				return false;
		}
		text_offset += event->text_size;
		if ((event->flags & TEXT_INPUT_EVENT_FLAG_TEXT_END) != 0u) {
			if (text_offset != event->text_total_size ||
			    (event->type == TEXT_INPUT_EVENT_PREEDIT &&
			     (!text_boundary(events, action_start, i, event->selection_start) ||
			      !text_boundary(events, action_start, i, event->selection_end))))
				return false;
			action_open = false;
		}
		if ((event->flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) != 0u && action_open) return false;
	}
	return !action_open && (events[count - 1u].flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) != 0u;
}

syscall_status_t text_input_read_events(cap_id_t text_input_cap, uint64_t after_sequence,
                                        struct text_input_event* events, size_t capacity,
                                        struct text_input_event_page* out_page) {
	const struct text_input_read_events_request request = {
		.header = {.op = TEXT_INPUT_OP_READ_EVENTS}, .after_sequence = after_sequence, .count = capacity};
	struct text_input_read_events_response* response;
	size_t                                  response_capacity;
	size_t                                  response_size = 0u;
	if (out_page != NULL) *out_page = (struct text_input_event_page){0};
	if (text_input_cap == CAP_ID_INVALID || events == NULL || capacity == 0u || out_page == NULL ||
	    capacity > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*events))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	response_capacity = sizeof(*response) + capacity * sizeof(*events);
	response          = malloc(response_capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	syscall_status_t status =
		cap_call(text_input_cap, &request, sizeof(request), response, response_capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->returned == 0u || response->returned > capacity ||
	    response->reserved != 0u || (response->flags & ~(uint32_t)TEXT_INPUT_READ_EVENT_FLAG_GAP) != 0u ||
	    response->oldest_sequence == 0u || response->newest_sequence < response->oldest_sequence ||
	    after_sequence > response->newest_sequence ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*events)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	bool gap = (response->flags & TEXT_INPUT_READ_EVENT_FLAG_GAP) != 0u;
	if ((!gap && response->events[0].sequence != after_sequence + 1u) ||
	    (gap && response->events[0].sequence != response->oldest_sequence) ||
	    (gap && (after_sequence == UINT64_MAX || after_sequence + 1u >= response->oldest_sequence)) ||
	    !events_valid(response->events, response->returned)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	memcpy(events, response->events, (size_t)response->returned * sizeof(*events));
	*out_page = (struct text_input_event_page){
		.oldest_sequence = response->oldest_sequence,
		.newest_sequence = response->newest_sequence,
		.returned        = (size_t)response->returned,
		.flags           = response->flags,
	};
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t text_input_acquire_event_signal(cap_id_t text_input_cap, cap_id_t* out_signal_cap) {
	const struct text_input_acquire_event_signal_request request = {
		.header = {.op = TEXT_INPUT_OP_ACQUIRE_EVENT_SIGNAL}};
	struct text_input_acquire_event_signal_response response      = {.capability = CAP_ID_INVALID};
	size_t                                          response_size = 0u;
	if (text_input_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	syscall_status_t status =
		cap_call(text_input_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}
