#include "input.h"

#include <base/process.h>
#include <protocol/input_manager.h>
#include <protocol/terminal.h>
#include <protocol/text_input.h>
#include <runtime/init.h>
#include <runtime/input_manager.h>
#include <runtime/stream.h>
#include <runtime/text_input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>
#include <system/time.h>

#include "terminal.h"

#define TERMINAL_INPUT_BUFFER_CAPACITY (64u * 1024u)
#define TERMINAL_INPUT_PENDING_CAPACITY 64u
#define TERMINAL_INPUT_RETRY_DELAY_MS 100u
#define TERMINAL_INPUT_MANAGER_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_INPUT_MANAGER_PROTOCOL_VERSION_MINOR 0u
#define TERMINAL_INPUT_MANAGER_EVENT_PAGE_SIZE                                                                         \
	((CAP_MAX_RESPONSE_SIZE - sizeof(struct input_manager_read_events_response)) / sizeof(struct input_manager_event))
#define TERMINAL_TEXT_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_TEXT_EVENT_PAGE_SIZE                                                                                  \
	((CAP_MAX_RESPONSE_SIZE - sizeof(struct text_input_read_events_response)) / sizeof(struct text_input_event))
#define TERMINAL_WHEEL_LINES 3u
#define TERMINAL_KEY_SEQUENCE_CAPACITY 32u

struct pending_read {
	cap_call_id_t call_id;
	size_t        size;
};

struct terminal_input {
	struct terminal*    terminal;
	uint8_t             bytes[TERMINAL_INPUT_BUFFER_CAPACITY];
	size_t              byte_head;
	size_t              byte_count;
	struct pending_read pending[TERMINAL_INPUT_PENDING_CAPACITY];
	size_t              pending_head;
	size_t              pending_count;
	bool                locked;
	bool                stopping;
	cap_id_t            text_thread_cap;
	cap_id_t            pointer_thread_cap;
	cap_id_t            interrupt_signal_cap;
	uint64_t            interrupt_counter;
};

static const struct init_protocol_query input_manager_query = {
	.namespace_path = INPUT_MANAGER_NAMESPACE,
	.protocol       = INPUT_MANAGER_PROTOCOL_NAME,
	.major          = TERMINAL_INPUT_MANAGER_PROTOCOL_VERSION_MAJOR,
	.minor          = TERMINAL_INPUT_MANAGER_PROTOCOL_VERSION_MINOR,
};

static void input_lock(struct terminal_input* input) {
	while (__atomic_test_and_set(&input->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void input_unlock(struct terminal_input* input) {
	__atomic_clear(&input->locked, __ATOMIC_RELEASE);
}

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static void consume_bytes(struct terminal_input* input, size_t count) {
	input->byte_head = (input->byte_head + count) % TERMINAL_INPUT_BUFFER_CAPACITY;
	input->byte_count -= count;
}

static size_t copy_bytes(const struct terminal_input* input, uint8_t* output, size_t capacity) {
	size_t count = input->byte_count < capacity ? input->byte_count : capacity;
	for (size_t i = 0u; i < count; i++)
		output[i] = input->bytes[(input->byte_head + i) % TERMINAL_INPUT_BUFFER_CAPACITY];
	return count;
}

static void service_pending_reads(struct terminal_input* input) {
	uint8_t response[CAP_MAX_RESPONSE_SIZE];
	while (input->pending_count != 0u && input->byte_count != 0u) {
		struct pending_read* pending = &input->pending[input->pending_head];
		size_t               count   = copy_bytes(input, response, pending->size);
		if (channel_reply(pending->call_id, response, count, SYSCALL_STATUS_OK) == SYSCALL_STATUS_OK)
			consume_bytes(input, count);
		input->pending_head = (input->pending_head + 1u) % TERMINAL_INPUT_PENDING_CAPACITY;
		input->pending_count--;
	}
}

static bool publish_bytes(struct terminal_input* input, const uint8_t* bytes, size_t size) {
	if (input == NULL || (size != 0u && bytes == NULL)) return false;
	input_lock(input);
	service_pending_reads(input);
	if (size > TERMINAL_INPUT_BUFFER_CAPACITY - input->byte_count) {
		input_unlock(input);
		return true;
	}
	for (size_t i = 0u; i < size; i++) {
		size_t tail        = (input->byte_head + input->byte_count) % TERMINAL_INPUT_BUFFER_CAPACITY;
		input->bytes[tail] = bytes[i];
		input->byte_count++;
	}
	service_pending_reads(input);
	input_unlock(input);
	return true;
}

static bool publish_interrupt(struct terminal_input* input) {
	struct signal_send_response response;
	uint64_t                    counter;

	input_lock(input);
	input->interrupt_counter++;
	if (input->interrupt_counter == 0u) input->interrupt_counter = 1u;
	counter = input->interrupt_counter;
	input_unlock(input);
	return signal_send(input->interrupt_signal_cap, counter, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

bool terminal_input_read(struct terminal_input* input, const struct cap_request* call, const uint8_t* request_data) {
	struct stream_read_request request;
	uint8_t                    response[CAP_MAX_RESPONSE_SIZE];

	if (call == NULL) return false;
	if (input == NULL || request_data == NULL || call->request_size != sizeof(request))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, request_data, sizeof(request));
	if (request.header.op != STREAM_OP_READ || request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE ||
	    request.size > call->response_capacity)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.size == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_OK);

	input_lock(input);
	service_pending_reads(input);
	if (input->byte_count != 0u) {
		size_t count   = copy_bytes(input, response, (size_t)request.size);
		bool   success = reply(call, response, count, SYSCALL_STATUS_OK);
		if (success) consume_bytes(input, count);
		input_unlock(input);
		return success;
	}
	if (input->pending_count == TERMINAL_INPUT_PENDING_CAPACITY) {
		input_unlock(input);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	size_t tail          = (input->pending_head + input->pending_count) % TERMINAL_INPUT_PENDING_CAPACITY;
	input->pending[tail] = (struct pending_read){.call_id = call->call_id, .size = (size_t)request.size};
	input->pending_count++;
	input_unlock(input);
	return true;
}

bool terminal_input_acquire_interrupt_signal(struct terminal_input* input, process_id_t caller,
                                             cap_id_t* out_signal_cap) {
	if (input == NULL || caller == PROCESS_PID_INVALID || out_signal_cap == NULL) return false;
	*out_signal_cap = CAP_ID_INVALID;
	return cap_delegate(input->interrupt_signal_cap, caller, TERMINAL_INTERRUPT_SIGNAL_RIGHTS, out_signal_cap) ==
	       SYSCALL_STATUS_OK;
}

uint64_t terminal_input_discard(struct terminal_input* input) {
	uint64_t interrupt_generation;

	if (input == NULL) return 0u;
	input_lock(input);
	input->byte_head  = 0u;
	input->byte_count = 0u;
	while (input->pending_count != 0u) {
		struct pending_read* pending = &input->pending[input->pending_head];
		(void)channel_reply(pending->call_id, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
		input->pending_head = (input->pending_head + 1u) % TERMINAL_INPUT_PENDING_CAPACITY;
		input->pending_count--;
	}
	interrupt_generation = input->interrupt_counter;
	input_unlock(input);
	return interrupt_generation;
}

static bool text_handle_valid(const struct init_service_handle* handle) {
	return handle->capability != CAP_ID_INVALID && handle->owner != PROCESS_PID_INVALID &&
	       strcmp(handle->info.selector.namespace_path, TEXT_INPUT_NAMESPACE) == 0 &&
	       strcmp(handle->info.selector.protocol, TEXT_INPUT_PROTOCOL_NAME) == 0 &&
	       handle->info.selector.major == TERMINAL_TEXT_PROTOCOL_VERSION_MAJOR &&
	       strcmp(handle->info.selector.service, TEXT_INPUT_SERVICE_NAME) == 0 &&
	       handle->info.rights == TEXT_INPUT_CLIENT_RIGHTS;
}

static bool input_manager_handle_valid(const struct init_service_handle* handle) {
	return handle->capability != CAP_ID_INVALID && handle->owner != PROCESS_PID_INVALID &&
	       strcmp(handle->info.selector.namespace_path, INPUT_MANAGER_NAMESPACE) == 0 &&
	       strcmp(handle->info.selector.protocol, INPUT_MANAGER_PROTOCOL_NAME) == 0 &&
	       handle->info.selector.major == TERMINAL_INPUT_MANAGER_PROTOCOL_VERSION_MAJOR &&
	       strcmp(handle->info.selector.service, INPUT_MANAGER_SERVICE_NAME) == 0 &&
	       handle->info.rights == INPUT_MANAGER_CLIENT_RIGHTS;
}

static bool wait_for_retry(struct terminal_input* input) {
	return !__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE) &&
	       sleep_ms(TERMINAL_INPUT_RETRY_DELAY_MS) == SYSCALL_STATUS_OK;
}

static bool process_logical_key(struct terminal_input* input, const struct text_input_event* event, const uint8_t* text,
                                size_t text_size, bool text_generated, bool* handled);

static bool frame_generates_text(const struct text_input_event* events, size_t count, size_t first) {
	uint64_t frame = events[first].frame;

	for (size_t i = first; i < count && events[i].frame == frame; i++) {
		if (events[i].type == TEXT_INPUT_EVENT_PREEDIT || events[i].type == TEXT_INPUT_EVENT_COMMIT) return true;
	}
	return false;
}

static bool process_text_events(struct terminal_input* input, const struct text_input_event* events, size_t count,
                                uint64_t* active_source, uint64_t* active_composition) {
	uint8_t  text[CAP_MAX_RESPONSE_SIZE];
	uint64_t frame          = 0u;
	bool     key_handled    = false;
	bool     text_generated = false;
	for (size_t first = 0u; first < count;) {
		const struct text_input_event* action = &events[first];
		size_t                         last   = first;
		size_t                         size   = 0u;
		if (action->frame != frame) {
			frame          = action->frame;
			key_handled    = false;
			text_generated = frame_generates_text(events, count, first);
		}
		while (last < count) {
			if (size > sizeof(text) - events[last].text_size) return false;
			memcpy(text + size, events[last].text, events[last].text_size);
			size += events[last].text_size;
			if ((events[last].flags & TEXT_INPUT_EVENT_FLAG_TEXT_END) != 0u) break;
			last++;
		}
		if (last == count || size != action->text_total_size) return false;
		switch (action->type) {
		case TEXT_INPUT_EVENT_RESET:
			if (*active_source == action->source_id) {
				if (!terminal_set_preedit(input->terminal, NULL, 0u, 0u)) return false;
				*active_source      = 0u;
				*active_composition = 0u;
			}
			break;
		case TEXT_INPUT_EVENT_PREEDIT:
			if (size == 0u) {
				if (*active_source == action->source_id && *active_composition == action->composition_id) {
					if (!terminal_set_preedit(input->terminal, NULL, 0u, 0u)) return false;
					*active_source      = 0u;
					*active_composition = 0u;
				}
			}
			else {
				if (!terminal_set_preedit(input->terminal, text, size, action->selection_end)) return false;
				*active_source      = action->source_id;
				*active_composition = action->composition_id;
			}
			break;
		case TEXT_INPUT_EVENT_COMMIT:
			if (*active_source == action->source_id && *active_composition == action->composition_id) {
				if (!terminal_set_preedit(input->terminal, NULL, 0u, 0u)) return false;
				*active_source      = 0u;
				*active_composition = 0u;
			}
			if (!key_handled && !publish_bytes(input, text, size)) return false;
			break;
		case TEXT_INPUT_EVENT_DELETE_SURROUNDING:
			/* A sequential terminal stream cannot express arbitrary surrounding-text edits. */
			break;
		case TEXT_INPUT_EVENT_KEY: {
			bool handled;
			if (!process_logical_key(input, action, text, size, text_generated, &handled)) return false;
			key_handled = key_handled || handled;
			break;
		}
		default:
			return false;
		}
		first = last + 1u;
	}
	return true;
}

static void text_thread(void* opaque) {
	struct terminal_input* input              = *(struct terminal_input**)opaque;
	uint64_t               active_source      = 0u;
	uint64_t               active_composition = 0u;
	for (;;) {
		struct init_service_handle handle   = {.capability = CAP_ID_INVALID};
		struct init_call_result    acquired = text_input_acquire(&handle);
		if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK ||
		    !text_handle_valid(&handle)) {
			if (handle.capability != CAP_ID_INVALID) (void)cap_drop(handle.capability);
			if (!wait_for_retry(input)) thread_exit(1u);
			continue;
		}
		struct text_input_get_info_response info       = {0};
		cap_id_t                            signal_cap = CAP_ID_INVALID;
		syscall_status_t                    status     = text_input_get_info(handle.capability, &info);
		if (status == SYSCALL_STATUS_OK) status = text_input_acquire_event_signal(handle.capability, &signal_cap);
		uint64_t cursor = info.newest_sequence;
		while (status == SYSCALL_STATUS_OK && !__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) {
			struct text_input_event      events[TERMINAL_TEXT_EVENT_PAGE_SIZE];
			struct text_input_event_page page;
			status = text_input_read_events(handle.capability, cursor, events, TERMINAL_TEXT_EVENT_PAGE_SIZE, &page);
			if (status == SYSCALL_STATUS_UNAVAILABLE) {
				struct signal_message message;
				do {
					status = signal_wait(signal_cap, &message);
					if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) break;
				} while (status == SYSCALL_STATUS_INTERRUPTED);
				continue;
			}
			if (status != SYSCALL_STATUS_OK) break;
			if ((page.flags & TEXT_INPUT_READ_EVENT_FLAG_GAP) != 0u) {
				if (!terminal_set_preedit(input->terminal, NULL, 0u, 0u)) status = SYSCALL_STATUS_FAILED;
				active_source      = 0u;
				active_composition = 0u;
			}
			if (status == SYSCALL_STATUS_OK &&
			    !process_text_events(input, events, page.returned, &active_source, &active_composition))
				status = SYSCALL_STATUS_FAILED;
			if (status == SYSCALL_STATUS_OK) cursor = events[page.returned - 1u].sequence;
		}
		if (signal_cap != CAP_ID_INVALID) (void)cap_drop(signal_cap);
		(void)cap_drop(handle.capability);
		if (active_source != 0u) (void)terminal_set_preedit(input->terminal, NULL, 0u, 0u);
		active_source      = 0u;
		active_composition = 0u;
		if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (!wait_for_retry(input)) thread_exit(1u);
	}
}

static int64_t add_saturated(int64_t left, int64_t right) {
	if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
	if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
	return left + right;
}

static int64_t wheel_lines(int64_t value) {
	if (value > INT64_MAX / TERMINAL_WHEEL_LINES) return INT64_MIN;
	if (value < INT64_MIN / TERMINAL_WHEEL_LINES) return INT64_MAX;
	return -(value * TERMINAL_WHEEL_LINES);
}

struct pointer_frame {
	uint64_t frame;
	uint64_t source_id;
	int64_t  delta_x;
	int64_t  delta_y;
	int64_t  wheel;
	bool     primary_changed;
	bool     primary_pressed;
	bool     active;
};

static bool process_pointer_event(struct terminal_input* input, struct pointer_frame* frame,
                                  const struct input_manager_event* event) {
	if (event->device_class != INPUT_DEVICE_CLASS_POINTER) return true;
	if (event->type == INPUT_MANAGER_EVENT_SOURCE_REMOVED || event->type == INPUT_MANAGER_EVENT_SOURCE_RESET) {
		terminal_pointer_cancel(input->terminal, event->source_id);
		return true;
	}
	if (event->type != INPUT_MANAGER_EVENT_CONTROL) return true;
	if (!frame->active) {
		*frame = (struct pointer_frame){.frame = event->frame, .source_id = event->source_id, .active = true};
	}
	else if (event->frame != frame->frame || event->source_id != frame->source_id) return false;

	if (event->control_kind == INPUT_CONTROL_KIND_RELATIVE_AXIS) {
		switch ((enum input_axis_code)event->control_code) {
		case INPUT_AXIS_X:
			frame->delta_x = add_saturated(frame->delta_x, event->value);
			break;
		case INPUT_AXIS_Y:
			frame->delta_y = add_saturated(frame->delta_y, event->value);
			break;
		case INPUT_AXIS_VERTICAL_SCROLL:
			frame->wheel = add_saturated(frame->wheel, event->value);
			break;
		default:
			break;
		}
	}
	else if (event->control_kind == INPUT_CONTROL_KIND_BUTTON && event->control_code == INPUT_BUTTON_PRIMARY &&
	         (event->value == 0 || event->value == 1)) {
		frame->primary_changed = true;
		frame->primary_pressed = event->value != 0;
	}
	if ((event->flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) == 0u) return true;
	if (frame->wheel != 0 && !terminal_scroll_view(input->terminal, wheel_lines(frame->wheel))) return false;
	if ((frame->delta_x != 0 || frame->delta_y != 0 || frame->wheel != 0 || frame->primary_changed) &&
	    !terminal_pointer_update(input->terminal,
	                             frame->source_id,
	                             frame->delta_x,
	                             frame->delta_y,
	                             frame->primary_changed,
	                             frame->primary_pressed))
		return false;
	*frame = (struct pointer_frame){0};
	return true;
}

static void pointer_thread(void* opaque) {
	struct terminal_input* input = *(struct terminal_input**)opaque;
	for (;;) {
		struct pointer_frame       frame    = {0};
		struct init_service_handle handle   = {.capability = CAP_ID_INVALID};
		struct init_call_result    acquired = init_acquire(&input_manager_query, INPUT_MANAGER_SERVICE_NAME, &handle);
		if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK ||
		    !input_manager_handle_valid(&handle)) {
			if (handle.capability != CAP_ID_INVALID) (void)cap_drop(handle.capability);
			if (!wait_for_retry(input)) thread_exit(1u);
			continue;
		}
		struct input_manager_get_info_response info;
		cap_id_t                               signal_cap = CAP_ID_INVALID;
		syscall_status_t                       status     = input_manager_get_info(handle.capability, &info);
		if (status == SYSCALL_STATUS_OK) status = input_manager_acquire_event_signal(handle.capability, &signal_cap);
		uint64_t cursor = info.newest_sequence;
		while (status == SYSCALL_STATUS_OK && !__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) {
			struct input_manager_event      events[TERMINAL_INPUT_MANAGER_EVENT_PAGE_SIZE];
			struct input_manager_event_page page;
			status = input_manager_read_events(
				handle.capability, cursor, events, TERMINAL_INPUT_MANAGER_EVENT_PAGE_SIZE, &page);
			if (status == SYSCALL_STATUS_UNAVAILABLE) {
				struct signal_message message;
				do {
					status = signal_wait(signal_cap, &message);
					if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) break;
				} while (status == SYSCALL_STATUS_INTERRUPTED);
				continue;
			}
			if (status != SYSCALL_STATUS_OK) break;
			if ((page.flags & INPUT_MANAGER_READ_EVENT_FLAG_GAP) != 0u) {
				frame = (struct pointer_frame){0};
				terminal_pointer_cancel(input->terminal, 0u);
			}
			for (size_t i = 0u; i < page.returned; i++) {
				if (!process_pointer_event(input, &frame, &events[i])) {
					status = SYSCALL_STATUS_FAILED;
					break;
				}
			}
			if (status == SYSCALL_STATUS_OK) cursor = events[page.returned - 1u].sequence;
		}
		terminal_pointer_cancel(input->terminal, 0u);
		if (signal_cap != CAP_ID_INVALID) (void)cap_drop(signal_cap);
		(void)cap_drop(handle.capability);
		if (__atomic_load_n(&input->stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (!wait_for_retry(input)) thread_exit(1u);
	}
}

static uint32_t terminal_modifier_parameter(uint32_t modifiers) {
	const uint32_t shift   = TEXT_INPUT_KEY_MODIFIER_LEFT_SHIFT | TEXT_INPUT_KEY_MODIFIER_RIGHT_SHIFT;
	const uint32_t control = TEXT_INPUT_KEY_MODIFIER_LEFT_CONTROL | TEXT_INPUT_KEY_MODIFIER_RIGHT_CONTROL;
	const uint32_t alt     = TEXT_INPUT_KEY_MODIFIER_LEFT_ALT | TEXT_INPUT_KEY_MODIFIER_RIGHT_ALT;
	const uint32_t meta    = TEXT_INPUT_KEY_MODIFIER_LEFT_META | TEXT_INPUT_KEY_MODIFIER_RIGHT_META;
	uint32_t       result  = 1u;

	if ((modifiers & shift) != 0u) result += 1u;
	if ((modifiers & alt) != 0u) result += 2u;
	if ((modifiers & control) != 0u) result += 4u;
	if ((modifiers & meta) != 0u) result += 8u;
	return result;
}

static bool terminal_key_sequence(enum text_input_key_code key, uint32_t modifiers, uint8_t* output, size_t capacity,
                                  size_t* out_size) {
	const char* legacy_sequence = NULL;
	uint32_t    parameter       = 0u;
	uint32_t    unicode         = 0u;
	char        final           = '\0';
	bool        ss3_unmodified  = false;
	uint32_t    modifier        = terminal_modifier_parameter(modifiers);

	if (output == NULL || out_size == NULL) return false;
	*out_size = 0u;
	switch (key) {
	case TEXT_INPUT_KEY_ESCAPE:
		if (modifier == 1u) legacy_sequence = "\x1b";
		else unicode = 27u;
		break;
	case TEXT_INPUT_KEY_BACKSPACE:
		if (modifier == 1u) legacy_sequence = "\x7f";
		else unicode = 127u;
		break;
	case TEXT_INPUT_KEY_TAB:
		if (modifier != 1u) unicode = 9u;
		break;
	case TEXT_INPUT_KEY_ENTER:
		if (modifier != 1u) unicode = 13u;
		break;
	case TEXT_INPUT_KEY_INSERT:
		parameter = 2u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_HOME:
		parameter = 1u;
		final     = 'H';
		break;
	case TEXT_INPUT_KEY_PAGE_UP:
		parameter = 5u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_DELETE:
		parameter = 3u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_END:
		parameter = 1u;
		final     = 'F';
		break;
	case TEXT_INPUT_KEY_PAGE_DOWN:
		parameter = 6u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_UP:
		parameter = 1u;
		final     = 'A';
		break;
	case TEXT_INPUT_KEY_LEFT:
		parameter = 1u;
		final     = 'D';
		break;
	case TEXT_INPUT_KEY_DOWN:
		parameter = 1u;
		final     = 'B';
		break;
	case TEXT_INPUT_KEY_RIGHT:
		parameter = 1u;
		final     = 'C';
		break;
	case TEXT_INPUT_KEY_CENTER:
		parameter = 1u;
		final     = 'E';
		break;
	case TEXT_INPUT_KEY_F1:
		parameter      = 1u;
		final          = 'P';
		ss3_unmodified = true;
		break;
	case TEXT_INPUT_KEY_F2:
		parameter      = 1u;
		final          = 'Q';
		ss3_unmodified = true;
		break;
	case TEXT_INPUT_KEY_F3:
		parameter      = 1u;
		final          = 'R';
		ss3_unmodified = true;
		break;
	case TEXT_INPUT_KEY_F4:
		parameter      = 1u;
		final          = 'S';
		ss3_unmodified = true;
		break;
	case TEXT_INPUT_KEY_F5:
		parameter = 15u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F6:
		parameter = 17u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F7:
		parameter = 18u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F8:
		parameter = 19u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F9:
		parameter = 20u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F10:
		parameter = 21u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F11:
		parameter = 23u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_F12:
		parameter = 24u;
		final     = '~';
		break;
	case TEXT_INPUT_KEY_CAPS_LOCK:
		unicode = 57358u;
		break;
	case TEXT_INPUT_KEY_SCROLL_LOCK:
		unicode = 57359u;
		break;
	case TEXT_INPUT_KEY_NUM_LOCK:
		unicode = 57360u;
		break;
	case TEXT_INPUT_KEY_PRINT_SCREEN:
		unicode = 57361u;
		break;
	case TEXT_INPUT_KEY_PAUSE:
		unicode = 57362u;
		break;
	case TEXT_INPUT_KEY_MENU:
		unicode = 57363u;
		break;
	default:
		return true;
	}
	int length;
	if (legacy_sequence != NULL) length = snprintf((char*)output, capacity, "%s", legacy_sequence);
	else if (unicode != 0u) {
		if (modifier == 1u) length = snprintf((char*)output, capacity, "\x1b[%uu", (unsigned)unicode);
		else length = snprintf((char*)output, capacity, "\x1b[%u;%uu", (unsigned)unicode, (unsigned)modifier);
	}
	else if (final == '\0') return true;
	else if (modifier == 1u) {
		if (ss3_unmodified) length = snprintf((char*)output, capacity, "\x1bO%c", final);
		else if (parameter == 1u) length = snprintf((char*)output, capacity, "\x1b[%c", final);
		else length = snprintf((char*)output, capacity, "\x1b[%u%c", (unsigned)parameter, final);
	}
	else length = snprintf((char*)output, capacity, "\x1b[%u;%u%c", (unsigned)parameter, (unsigned)modifier, final);
	if (length < 0 || (size_t)length >= capacity) return false;
	*out_size = (size_t)length;
	return true;
}

static bool control_byte(const uint8_t* text, size_t size, uint8_t* output) {
	if (text == NULL || size != 1u || output == NULL) return false;
	uint8_t character = text[0];
	if (character >= 'a' && character <= 'z') character = (uint8_t)(character - ('a' - 'A'));
	if (character >= 'A' && character <= 'Z') {
		*output = (uint8_t)(character - 'A' + 1u);
		return true;
	}
	switch (character) {
	case ' ':
	case '@':
		*output = 0x00u;
		return true;
	case '[':
		*output = 0x1bu;
		return true;
	case '\\':
		*output = 0x1cu;
		return true;
	case ']':
		*output = 0x1du;
		return true;
	case '^':
		*output = 0x1eu;
		return true;
	case '_':
		*output = 0x1fu;
		return true;
	case '?':
		*output = 0x7fu;
		return true;
	default:
		return false;
	}
}

static bool single_codepoint(const uint8_t* text, size_t size, uint32_t* out_codepoint) {
	uint32_t codepoint;
	size_t   length;

	if (text == NULL || size == 0u || out_codepoint == NULL) return false;
	if (text[0] < 0x80u) {
		codepoint = text[0];
		length    = 1u;
	}
	else if (text[0] >= 0xc2u && text[0] <= 0xdfu) {
		codepoint = text[0] & 0x1fu;
		length    = 2u;
	}
	else if (text[0] >= 0xe0u && text[0] <= 0xefu) {
		codepoint = text[0] & 0x0fu;
		length    = 3u;
	}
	else if (text[0] >= 0xf0u && text[0] <= 0xf4u) {
		codepoint = text[0] & 0x07u;
		length    = 4u;
	}
	else return false;
	if (length != size) return false;
	for (size_t i = 1u; i < length; i++) codepoint = (codepoint << 6u) | (text[i] & 0x3fu);
	*out_codepoint = codepoint;
	return true;
}

static bool publish_text_shortcut(struct terminal_input* input, const uint8_t* text, size_t text_size,
                                  uint32_t modifiers, bool* handled) {
	const uint32_t shift    = TEXT_INPUT_KEY_MODIFIER_LEFT_SHIFT | TEXT_INPUT_KEY_MODIFIER_RIGHT_SHIFT;
	const uint32_t control  = TEXT_INPUT_KEY_MODIFIER_LEFT_CONTROL | TEXT_INPUT_KEY_MODIFIER_RIGHT_CONTROL;
	const uint32_t alt      = TEXT_INPUT_KEY_MODIFIER_LEFT_ALT | TEXT_INPUT_KEY_MODIFIER_RIGHT_ALT;
	const uint32_t meta     = TEXT_INPUT_KEY_MODIFIER_LEFT_META | TEXT_INPUT_KEY_MODIFIER_RIGHT_META;
	uint32_t       relevant = modifiers & (shift | control | alt | meta);

	if (relevant == 0u || (relevant & ~(uint32_t)shift) == 0u) return true;
	if ((relevant & alt) != 0u && (relevant & meta) == 0u && ((relevant & control) == 0u || (relevant & shift) == 0u)) {
		uint8_t escape = 0x1bu;
		uint8_t byte;

		*handled = true;
		if (!publish_bytes(input, &escape, 1u)) return false;
		if ((relevant & control) != 0u && control_byte(text, text_size, &byte)) return publish_bytes(input, &byte, 1u);
		return publish_bytes(input, text, text_size);
	}
	uint32_t codepoint;
	if (!single_codepoint(text, text_size, &codepoint)) return true;
	uint8_t sequence[TERMINAL_KEY_SEQUENCE_CAPACITY];
	int     length = snprintf((char*)sequence,
	                          sizeof(sequence),
	                          "\x1b[%u;%uu",
	                          (unsigned)codepoint,
	                          (unsigned)terminal_modifier_parameter(relevant));
	if (length < 0 || (size_t)length >= sizeof(sequence)) return false;
	*handled = true;
	return publish_bytes(input, sequence, (size_t)length);
}

static bool paste_clipboard(struct terminal_input* input) {
	uint8_t* clipboard;
	size_t   clipboard_size;
	size_t   text_size = 0u;

	if (!terminal_clipboard_snapshot(input->terminal, &clipboard, &clipboard_size)) return false;
	for (size_t i = 0u; i < clipboard_size; i++) {
		uint8_t byte = clipboard[i];
		if (byte < 0x20u || byte == 0x7fu) continue;
		clipboard[text_size++] = byte;
	}
	bool success = publish_bytes(input, clipboard, text_size);
	free(clipboard);
	return success;
}

static bool process_logical_key(struct terminal_input* input, const struct text_input_event* event, const uint8_t* text,
                                size_t text_size, bool text_generated, bool* handled) {
	const uint32_t shift   = TEXT_INPUT_KEY_MODIFIER_LEFT_SHIFT | TEXT_INPUT_KEY_MODIFIER_RIGHT_SHIFT;
	const uint32_t control = TEXT_INPUT_KEY_MODIFIER_LEFT_CONTROL | TEXT_INPUT_KEY_MODIFIER_RIGHT_CONTROL;
	const uint32_t command = TEXT_INPUT_KEY_MODIFIER_LEFT_ALT | TEXT_INPUT_KEY_MODIFIER_RIGHT_ALT |
	                         TEXT_INPUT_KEY_MODIFIER_LEFT_META | TEXT_INPUT_KEY_MODIFIER_RIGHT_META;
	if (handled == NULL) return false;
	*handled = false;
	if (event->key_action == TEXT_INPUT_KEY_ACTION_RELEASED) return true;
	if (event->key == TEXT_INPUT_KEY_TEXT && (event->key_modifiers & control) != 0u &&
	    (event->key_modifiers & command) == 0u) {
		uint8_t byte;
		if (control_byte(text, text_size, &byte)) {
			if (byte == 0x03u) {
				*handled                                   = true;
				enum terminal_clipboard_copy_result result = terminal_clipboard_copy_selection(input->terminal);
				if (result == TERMINAL_CLIPBOARD_COPY_UNAVAILABLE) return publish_interrupt(input);
				return true;
			}
			if (byte == 0x16u) {
				*handled = true;
				return paste_clipboard(input);
			}
			if ((event->key_modifiers & shift) == 0u) {
				*handled = true;
				return publish_bytes(input, &byte, 1u);
			}
		}
	}
	if (event->key == TEXT_INPUT_KEY_TEXT) {
		if (text_generated) return true;
		return publish_text_shortcut(input, text, text_size, event->key_modifiers, handled);
	}
	uint8_t sequence[TERMINAL_KEY_SEQUENCE_CAPACITY];
	size_t  size;
	if (!terminal_key_sequence(event->key, event->key_modifiers, sequence, sizeof(sequence), &size)) return false;
	if (size == 0u) return true;
	*handled = true;
	return publish_bytes(input, sequence, size);
}

struct terminal_input* terminal_input_create(struct terminal* terminal) {
	if (terminal == NULL) return NULL;
	struct terminal_input* input = calloc(1u, sizeof(*input));
	if (input == NULL) return NULL;
	input->terminal             = terminal;
	input->text_thread_cap      = CAP_ID_INVALID;
	input->pointer_thread_cap   = CAP_ID_INVALID;
	input->interrupt_signal_cap = CAP_ID_INVALID;
	if (signal_create(&input->interrupt_signal_cap) != SYSCALL_STATUS_OK) {
		free(input);
		return NULL;
	}
	return input;
}

bool terminal_input_start(struct terminal_input* input) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	if (input == NULL) return false;
	syscall_status_t status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) printf("terminal: process query for input workers failed: %u\n", (unsigned)status);
	if (status == SYSCALL_STATUS_OK) {
		struct terminal_input* argument = input;
		status                          = process_spawn_thread(self.self_cap,
		                                                       (uintptr_t)text_thread,
		                                                       &argument,
		                                                       sizeof(argument),
		                                                       "terminal-text-input",
		                                                       sizeof("terminal-text-input"),
		                                                       &input->text_thread_cap);
		if (status != SYSCALL_STATUS_OK) printf("terminal: text input worker spawn failed: %u\n", (unsigned)status);
	}
	if (status == SYSCALL_STATUS_OK) {
		struct terminal_input* argument = input;
		status                          = process_spawn_thread(self.self_cap,
		                                                       (uintptr_t)pointer_thread,
		                                                       &argument,
		                                                       sizeof(argument),
		                                                       "terminal-pointer-input",
		                                                       sizeof("terminal-pointer-input"),
		                                                       &input->pointer_thread_cap);
		if (status != SYSCALL_STATUS_OK) printf("terminal: pointer input worker spawn failed: %u\n", (unsigned)status);
	}
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		return false;
	}
	return true;
}

void terminal_input_destroy(struct terminal_input* input) {
	if (input == NULL) return;
	__atomic_store_n(&input->stopping, true, __ATOMIC_RELEASE);
	if (input->text_thread_cap != CAP_ID_INVALID) {
		(void)thread_cancel(input->text_thread_cap);
		(void)thread_join(input->text_thread_cap, NULL);
	}
	if (input->pointer_thread_cap != CAP_ID_INVALID) {
		(void)thread_cancel(input->pointer_thread_cap);
		(void)thread_join(input->pointer_thread_cap, NULL);
	}
	input_lock(input);
	while (input->pending_count != 0u) {
		struct pending_read* pending = &input->pending[input->pending_head];
		(void)channel_reply(pending->call_id, NULL, 0u, SYSCALL_STATUS_FAILED);
		input->pending_head = (input->pending_head + 1u) % TERMINAL_INPUT_PENDING_CAPACITY;
		input->pending_count--;
	}
	input_unlock(input);
	if (input->interrupt_signal_cap != CAP_ID_INVALID) (void)signal_destroy(input->interrupt_signal_cap);
	free(input);
}
