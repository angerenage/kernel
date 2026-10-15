#include "text_input.h"

#include <protocol/input.h>
#include <protocol/keyboard.h>
#include <protocol/text_input.h>
#include <runtime/init.h>
#include <runtime/keyboard.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/time.h>
#include <utf8proc.h>

#include "layout.h"

#define TEXT_INPUT_PROTOCOL_VERSION_MAJOR 1u
#define TEXT_INPUT_PROTOCOL_VERSION_MINOR 0u

static const struct init_service_selector text_input_selector = {
	.namespace_path = TEXT_INPUT_NAMESPACE,
	.protocol       = TEXT_INPUT_PROTOCOL_NAME,
	.major          = TEXT_INPUT_PROTOCOL_VERSION_MAJOR,
	.service        = TEXT_INPUT_SERVICE_NAME,
};

void text_input_lock(struct text_input* input) {
	while (__atomic_test_and_set(&input->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

void text_input_unlock(struct text_input* input) {
	__atomic_clear(&input->locked, __ATOMIC_RELEASE);
}

static size_t event_index(const struct text_input* input, size_t offset) {
	return (input->event_tail + offset) % TEXT_INPUT_EVENT_QUEUE_CAPACITY;
}

static void evict_oldest_frame(struct text_input* input) {
	while (input->event_count != 0u) {
		bool frame_end    = (input->events[input->event_tail].flags & TEXT_INPUT_EVENT_FLAG_FRAME_END) != 0u;
		input->event_tail = (input->event_tail + 1u) % TEXT_INPUT_EVENT_QUEUE_CAPACITY;
		input->event_count--;
		if (frame_end) return;
	}
}

static bool append_frame_locked(struct text_input* input, struct text_input_event* events, size_t count) {
	if (count == 0u || count > TEXT_INPUT_MAXIMUM_FRAME_EVENTS || input->next_frame == 0u ||
	    input->next_frame == UINT64_MAX || input->next_sequence == 0u || input->next_sequence > UINT64_MAX - count)
		return false;
	while (TEXT_INPUT_EVENT_QUEUE_CAPACITY - input->event_count < count) evict_oldest_frame(input);
	uint64_t frame = input->next_frame++;
	for (size_t i = 0u; i < count; i++) {
		events[i].sequence = input->next_sequence++;
		events[i].frame    = frame;
		if (i + 1u == count) events[i].flags |= TEXT_INPUT_EVENT_FLAG_FRAME_END;
		input->events[event_index(input, input->event_count++)] = events[i];
	}
	return true;
}
#define TEXT_INPUT_RETRY_DELAY_MS 100u
#define TEXT_INPUT_KEYBOARD_PROTOCOL_VERSION_MAJOR 1u
#define TEXT_INPUT_KEYBOARD_PAGE_SIZE 16u

enum service_result {
	SERVICE_RESULT_RECONNECT,
	SERVICE_RESULT_FAILED,
};

enum drain_result {
	DRAIN_RESULT_IDLE,
	DRAIN_RESULT_RECONNECT,
	DRAIN_RESULT_FAILED,
};

static bool keyboard_handle_valid(const struct init_service_handle* handle) {
	return handle->capability != CAP_ID_INVALID && handle->owner != PROCESS_PID_INVALID &&
	       strcmp(handle->info.selector.namespace_path, KEYBOARD_NAMESPACE) == 0 &&
	       strcmp(handle->info.selector.protocol, KEYBOARD_PROTOCOL_NAME) == 0 &&
	       handle->info.selector.major == TEXT_INPUT_KEYBOARD_PROTOCOL_VERSION_MAJOR &&
	       strcmp(handle->info.selector.service, KEYBOARD_SERVICE_NAME) == 0 &&
	       handle->info.rights == KEYBOARD_CLIENT_RIGHTS;
}

static struct keyboard_text_state* find_keyboard(struct text_input* input, uint64_t id) {
	for (struct keyboard_text_state* keyboard = input->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (keyboard->id == id) return keyboard;
	return NULL;
}

static struct keyboard_text_state* ensure_keyboard(struct text_input* input, uint64_t id) {
	struct keyboard_text_state* keyboard = find_keyboard(input, id);
	if (keyboard != NULL) return keyboard;
	keyboard = calloc(1u, sizeof(*keyboard));
	if (keyboard == NULL) {
		free(keyboard);
		return NULL;
	}
	keyboard->source_id = text_input_allocate_source_id(input);
	if (keyboard->source_id == 0u) {
		free(keyboard);
		return NULL;
	}
	keyboard->id     = id;
	keyboard->next   = input->keyboards;
	input->keyboards = keyboard;
	return keyboard;
}

static void remove_keyboard(struct text_input* input, uint64_t id) {
	struct keyboard_text_state** cursor = &input->keyboards;
	while (*cursor != NULL && (*cursor)->id != id) cursor = &(*cursor)->next;
	if (*cursor == NULL) return;
	struct keyboard_text_state* keyboard = *cursor;
	*cursor                              = keyboard->next;
	free(keyboard);
}

static void clear_keyboards(struct text_input* input) {
	while (input->keyboards != NULL) {
		struct keyboard_text_state* next = input->keyboards->next;
		free(input->keyboards);
		input->keyboards = next;
	}
}

static bool cancel_composition(struct text_input* input, struct keyboard_text_state* keyboard) {
	const struct text_input_action action = {
		.type           = TEXT_INPUT_EVENT_PREEDIT,
		.source_id      = keyboard->source_id,
		.composition_id = keyboard->composition_id,
	};
	if (!text_input_publish_actions(input, &action, 1u)) return false;
	keyboard->composition_id = 0u;
	keyboard->context_size   = 0u;
	return true;
}

static bool publish_pending(struct text_input* input, struct keyboard_text_state* keyboard, const uint8_t* context,
                            size_t context_size) {
	if (keyboard->composition_id == 0u) {
		keyboard->composition_id = text_input_allocate_composition_id(input);
		if (keyboard->composition_id == 0u) return false;
	}
	const struct text_input_action action = {
		.type            = TEXT_INPUT_EVENT_PREEDIT,
		.source_id       = keyboard->source_id,
		.composition_id  = keyboard->composition_id,
		.text            = context,
		.text_size       = context_size,
		.selection_start = (uint32_t)context_size,
		.selection_end   = (uint32_t)context_size,
	};
	if (!text_input_publish_actions(input, &action, 1u)) return false;
	memcpy(keyboard->context, context, context_size);
	keyboard->context_size = context_size;
	return true;
}

static bool publish_commit(struct text_input* input, struct keyboard_text_state* keyboard, const uint8_t* text,
                           size_t text_size) {
	struct text_input_action actions[2];
	size_t                   count = 0u;
	if (keyboard->composition_id != 0u) {
		actions[count++] = (struct text_input_action){
			.type           = TEXT_INPUT_EVENT_PREEDIT,
			.source_id      = keyboard->source_id,
			.composition_id = keyboard->composition_id,
		};
	}
	actions[count++] = (struct text_input_action){
		.type           = TEXT_INPUT_EVENT_COMMIT,
		.source_id      = keyboard->source_id,
		.composition_id = keyboard->composition_id,
		.text           = text,
		.text_size      = text_size,
	};
	if (!text_input_publish_actions(input, actions, count)) return false;
	keyboard->composition_id = 0u;
	keyboard->context_size   = 0u;
	return true;
}

static bool translate_key(struct text_input* input, const struct keyboard_event* event) {
	if (event->action == KEYBOARD_KEY_ACTION_RELEASED) return true;
	struct keyboard_text_state* keyboard = ensure_keyboard(input, event->keyboard_id);
	if (keyboard == NULL) return false;
	if (keyboard->context_size != 0u && (event->key == INPUT_KEY_ESCAPE || event->key == INPUT_KEY_BACKSPACE))
		return cancel_composition(input, keyboard);
	const char* output = text_layout_map(&input->layout, event->key, event->modifiers, event->locks);
	if (output == NULL) return true;
	uint8_t*                committed = NULL;
	size_t                  committed_size;
	uint8_t                 next_context[TEXT_LAYOUT_CONTEXT_CAPACITY];
	size_t                  next_context_size;
	enum text_layout_result result = text_layout_apply(&input->layout,
	                                                   keyboard->context,
	                                                   keyboard->context_size,
	                                                   output,
	                                                   next_context,
	                                                   &next_context_size,
	                                                   &committed,
	                                                   &committed_size);
	if (result == TEXT_LAYOUT_RESULT_PENDING) return publish_pending(input, keyboard, next_context, next_context_size);
	if (result != TEXT_LAYOUT_RESULT_COMMIT || committed == NULL || committed_size == 0u) {
		if (committed != NULL) utf8proc_free(committed);
		return false;
	}
	bool success = publish_commit(input, keyboard, committed, committed_size);
	utf8proc_free(committed);
	return success;
}

static bool publish_reset(struct text_input* input, uint64_t source_id) {
	const struct text_input_action action = {
		.type      = TEXT_INPUT_EVENT_RESET,
		.source_id = source_id,
	};
	return text_input_publish_actions(input, &action, 1u);
}

static bool handle_event(struct text_input* input, const struct keyboard_event* event) {
	switch (event->type) {
	case KEYBOARD_EVENT_ADDED:
		if (ensure_keyboard(input, event->keyboard_id) == NULL) return false;
		printf("text input: attached keyboard %llu\n", (unsigned long long)event->keyboard_id);
		return true;
	case KEYBOARD_EVENT_REMOVED: {
		struct keyboard_text_state* keyboard = find_keyboard(input, event->keyboard_id);
		if (keyboard != NULL && !publish_reset(input, keyboard->source_id)) return false;
		remove_keyboard(input, event->keyboard_id);
		printf("text input: removed keyboard %llu\n", (unsigned long long)event->keyboard_id);
		return true;
	}
	case KEYBOARD_EVENT_RESET: {
		struct keyboard_text_state* keyboard = ensure_keyboard(input, event->keyboard_id);
		if (keyboard == NULL) return false;
		if (!publish_reset(input, keyboard->source_id)) return false;
		keyboard->composition_id = 0u;
		keyboard->context_size   = 0u;
		printf("text input: reset keyboard %llu\n", (unsigned long long)event->keyboard_id);
		return true;
	}
	case KEYBOARD_EVENT_KEY:
		return translate_key(input, event);
	default:
		return false;
	}
}

static enum drain_result drain_events(struct text_input* input) {
	for (;;) {
		struct keyboard_event_page page;
		syscall_status_t           status = keyboard_read_events(input->keyboard_cap,
		                                                         input->keyboard_cursor,
		                                                         input->keyboard_events,
		                                                         TEXT_INPUT_KEYBOARD_EVENT_PAGE_SIZE,
		                                                         &page);
		if (status == SYSCALL_STATUS_UNAVAILABLE) return DRAIN_RESULT_IDLE;
		if (status != SYSCALL_STATUS_OK) return DRAIN_RESULT_RECONNECT;
		if ((page.flags & KEYBOARD_READ_EVENT_FLAG_GAP) != 0u) {
			for (struct keyboard_text_state* keyboard = input->keyboards; keyboard != NULL; keyboard = keyboard->next)
				if (!publish_reset(input, keyboard->source_id)) return DRAIN_RESULT_FAILED;
			clear_keyboards(input);
			printf("text input: keyboard history gap; discarded composition state\n");
		}
		for (size_t i = 0u; i < page.returned; i++)
			if (!handle_event(input, &input->keyboard_events[i])) return DRAIN_RESULT_FAILED;
		input->keyboard_cursor = input->keyboard_events[page.returned - 1u].sequence;
	}
}

static enum service_result consume_service(struct text_input* input) {
	struct keyboard_get_info_response info;
	syscall_status_t                  status = keyboard_get_info(input->keyboard_cap, &info);
	if (status != SYSCALL_STATUS_OK) return SERVICE_RESULT_RECONNECT;
	status = keyboard_acquire_event_signal(input->keyboard_cap, &input->keyboard_signal_cap);
	if (status != SYSCALL_STATUS_OK) return SERVICE_RESULT_RECONNECT;
	uint64_t offset = 0u;
	while (offset < info.keyboard_count) {
		struct keyboard_info keyboards[TEXT_INPUT_KEYBOARD_PAGE_SIZE];
		size_t               returned;
		uint64_t             total;
		status = keyboard_enumerate(input->keyboard_cap,
		                            info.topology_generation,
		                            offset,
		                            keyboards,
		                            TEXT_INPUT_KEYBOARD_PAGE_SIZE,
		                            &returned,
		                            &total);
		if (status != SYSCALL_STATUS_OK || total != info.keyboard_count || returned == 0u)
			return SERVICE_RESULT_RECONNECT;
		for (size_t i = 0u; i < returned; i++) {
			if (find_keyboard(input, keyboards[i].id) != NULL || ensure_keyboard(input, keyboards[i].id) == NULL)
				return SERVICE_RESULT_FAILED;
			printf("text input: attached keyboard %llu\n", (unsigned long long)keyboards[i].id);
		}
		offset += returned;
	}
	input->keyboard_cursor = info.newest_sequence;
	printf("text input: connected to keyboard controller with %s layout\n", input->layout.locale);
	for (;;) {
		enum drain_result result = drain_events(input);
		if (result == DRAIN_RESULT_FAILED) return SERVICE_RESULT_FAILED;
		if (result == DRAIN_RESULT_RECONNECT) return SERVICE_RESULT_RECONNECT;
		struct signal_message message;
		do status = signal_wait(input->keyboard_signal_cap, &message);
		while (status == SYSCALL_STATUS_INTERRUPTED);
		if (status != SYSCALL_STATUS_OK) return SERVICE_RESULT_RECONNECT;
	}
}

static bool disconnect(struct text_input* input) {
	bool success = true;
	for (struct keyboard_text_state* keyboard = input->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (!publish_reset(input, keyboard->source_id)) success = false;
	if (input->keyboard_signal_cap != CAP_ID_INVALID) (void)cap_drop(input->keyboard_signal_cap);
	if (input->keyboard_cap != CAP_ID_INVALID) (void)cap_drop(input->keyboard_cap);
	input->keyboard_signal_cap = CAP_ID_INVALID;
	input->keyboard_cap        = CAP_ID_INVALID;
	input->keyboard_cursor     = 0u;
	clear_keyboards(input);
	return success;
}

int text_input_run(struct text_input* input) {
	bool waiting_logged = false;
	for (;;) {
		struct init_service_handle handle = {.capability = CAP_ID_INVALID};
		struct init_call_result    result = keyboard_acquire(&handle);
		if (result.transport_status != SYSCALL_STATUS_OK) return 1;
		if (result.status == INIT_REGISTRY_NOT_FOUND) {
			if (!waiting_logged) {
				printf("text input: waiting for keyboard controller\n");
				waiting_logged = true;
			}
			if (sleep_ms(TEXT_INPUT_RETRY_DELAY_MS) != SYSCALL_STATUS_OK) return 1;
			continue;
		}
		if (result.status != INIT_REGISTRY_OK || !keyboard_handle_valid(&handle)) {
			if (handle.capability != CAP_ID_INVALID) (void)cap_drop(handle.capability);
			return 1;
		}
		waiting_logged              = false;
		input->keyboard_cap         = handle.capability;
		enum service_result service = consume_service(input);
		if (!disconnect(input)) return 1;
		if (service == SERVICE_RESULT_FAILED) return 1;
		printf("text input: lost keyboard controller; reconnecting\n");
		if (sleep_ms(TEXT_INPUT_RETRY_DELAY_MS) != SYSCALL_STATUS_OK) return 1;
	}
}

static bool notify_consumers(struct text_input* input) {
	struct signal_send_response response;
	return signal_send(
			   input->event_signal_cap, input->next_sequence - 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
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

static bool action_valid(const struct text_input_action* action) {
	if (action->source_id == 0u || action->text_size > UINT32_MAX ||
	    (action->text_size != 0u && action->text == NULL) || !utf8_valid(action->text, action->text_size))
		return false;
	switch (action->type) {
	case TEXT_INPUT_EVENT_RESET:
		return action->composition_id == 0u && action->text_size == 0u && action->selection_start == 0u &&
		       action->selection_end == 0u && action->delete_before == 0u && action->delete_after == 0u;
	case TEXT_INPUT_EVENT_PREEDIT:
		return action->composition_id != 0u && action->selection_start <= action->selection_end &&
		       action->selection_end <= action->text_size && action->delete_before == 0u && action->delete_after == 0u;
	case TEXT_INPUT_EVENT_COMMIT:
		return action->text_size != 0u && action->selection_start == 0u && action->selection_end == 0u &&
		       action->delete_before == 0u && action->delete_after == 0u;
	case TEXT_INPUT_EVENT_DELETE_SURROUNDING:
		return action->text_size == 0u && action->selection_start == 0u && action->selection_end == 0u &&
		       (action->delete_before != 0u || action->delete_after != 0u);
	default:
		return false;
	}
}

static size_t utf8_chunk_size(const uint8_t* text, size_t remaining) {
	if (remaining <= TEXT_INPUT_EVENT_TEXT_CAPACITY) return remaining;
	size_t size = TEXT_INPUT_EVENT_TEXT_CAPACITY;
	while (size != 0u && (text[size] & 0xc0u) == 0x80u) size--;
	return size;
}

bool text_input_publish_actions(struct text_input* input, const struct text_input_action* actions,
                                size_t action_count) {
	if (input == NULL || actions == NULL || action_count == 0u) return false;
	size_t event_count = 0u;
	for (size_t i = 0u; i < action_count; i++) {
		if (!action_valid(&actions[i]) || (i != 0u && actions[i].source_id != actions[0].source_id)) return false;
		size_t offset = 0u;
		do {
			const uint8_t* chunk_text = actions[i].text_size == 0u ? NULL : actions[i].text + offset;
			size_t         chunk      = utf8_chunk_size(chunk_text, actions[i].text_size - offset);
			if (actions[i].text_size != 0u && chunk == 0u) return false;
			event_count++;
			if (event_count > TEXT_INPUT_MAXIMUM_FRAME_EVENTS) return false;
			offset += chunk;
		} while (offset < actions[i].text_size);
	}
	struct text_input_event* events = calloc(event_count, sizeof(*events));
	if (events == NULL) return false;
	size_t index = 0u;
	for (size_t i = 0u; i < action_count; i++) {
		size_t offset = 0u;
		do {
			const uint8_t* chunk_text = actions[i].text_size == 0u ? NULL : actions[i].text + offset;
			size_t         chunk      = utf8_chunk_size(chunk_text, actions[i].text_size - offset);
			events[index]             = (struct text_input_event){
				.source_id       = actions[i].source_id,
				.composition_id  = actions[i].composition_id,
				.type            = actions[i].type,
				.text_offset     = (uint32_t)offset,
				.text_size       = (uint32_t)chunk,
				.text_total_size = (uint32_t)actions[i].text_size,
				.selection_start = actions[i].selection_start,
				.selection_end   = actions[i].selection_end,
				.delete_before   = actions[i].delete_before,
				.delete_after    = actions[i].delete_after,
			};
			if (chunk != 0u) memcpy(events[index].text, chunk_text, chunk);
			offset += chunk;
			if (offset == actions[i].text_size) events[index].flags |= TEXT_INPUT_EVENT_FLAG_TEXT_END;
			index++;
		} while (offset < actions[i].text_size);
	}
	text_input_lock(input);
	bool success = append_frame_locked(input, events, event_count);
	text_input_unlock(input);
	free(events);
	return success && notify_consumers(input);
}

uint64_t text_input_allocate_source_id(struct text_input* input) {
	if (input == NULL || input->next_source_id == 0u) return 0u;
	return input->next_source_id++;
}

uint64_t text_input_allocate_composition_id(struct text_input* input) {
	if (input == NULL || input->next_composition_id == 0u) return 0u;
	return input->next_composition_id++;
}

static void initialize_invalid_capabilities(struct text_input* input) {
	input->self_cap             = CAP_ID_INVALID;
	input->channel_activity_cap = CAP_ID_INVALID;
	input->service_cap          = CAP_ID_INVALID;
	input->event_signal_cap     = CAP_ID_INVALID;
	input->keyboard_cap         = CAP_ID_INVALID;
	input->keyboard_signal_cap  = CAP_ID_INVALID;
	input->endpoint             = CHANNEL_ID_INVALID;
}

bool text_input_initialize(struct text_input* input, const char* layout_path) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	if (input == NULL || layout_path == NULL) return false;
	memset(input, 0, sizeof(*input));
	initialize_invalid_capabilities(input);
	if (!text_layout_load(&input->layout, layout_path)) return false;
	input->layout.decimal_comma = strncmp(input->layout.locale, "fr", 2u) == 0;
	input->next_sequence        = 1u;
	input->next_frame           = 1u;
	input->next_source_id       = 1u;
	input->next_composition_id  = 1u;
	input->events               = calloc(TEXT_INPUT_EVENT_QUEUE_CAPACITY, sizeof(*input->events));
	if (input->events == NULL) {
		text_layout_destroy(&input->layout);
		return false;
	}

	syscall_status_t status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK && self.pid == PROCESS_PID_INVALID) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		input->pid      = self.pid;
		input->self_cap = self.self_cap;
		self.self_cap   = CAP_ID_INVALID;
		status          = signal_create(&input->event_signal_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = channel_create(&input->endpoint, &input->channel_activity_cap);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(
			input->endpoint, TEXT_INPUT_OBJECT_ID, input->pid, CAP_CALL | CAP_READ | CAP_DELEGATE, &input->service_cap);
	if (status == SYSCALL_STATUS_OK && !text_input_server_start(input)) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		struct init_call_result result = init_advertise(
			&text_input_selector, TEXT_INPUT_PROTOCOL_VERSION_MINOR, input->service_cap, TEXT_INPUT_CLIENT_RIGHTS);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != INIT_REGISTRY_OK)
			status = SYSCALL_STATUS_FAILED;
	}
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (status != SYSCALL_STATUS_OK) {
		text_layout_destroy(&input->layout);
		return false;
	}
	printf("text input: loaded LDML layout %s from %s\n", input->layout.locale, layout_path);
	printf("text input: advertised %s/%s@%u.%u/%s\n",
	       TEXT_INPUT_NAMESPACE,
	       TEXT_INPUT_PROTOCOL_NAME,
	       TEXT_INPUT_PROTOCOL_VERSION_MAJOR,
	       TEXT_INPUT_PROTOCOL_VERSION_MINOR,
	       TEXT_INPUT_SERVICE_NAME);
	return true;
}
