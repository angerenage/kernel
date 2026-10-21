#include "keyboard_controller.h"

#include <limits.h>
#include <protocol/input_manager.h>
#include <runtime/input_manager.h>
#include <runtime/registry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>
#include <system/time.h>

#define INPUT_MANAGER_PROTOCOL_VERSION_MAJOR 1u
#define INPUT_MANAGER_PROTOCOL_VERSION_MINOR 0u
#define KEYBOARD_PROTOCOL_VERSION_MAJOR 1u
#define KEYBOARD_PROTOCOL_VERSION_MINOR 0u
#define KEYBOARD_CONTROLLER_SOURCE_PAGE_SIZE 16u
#define KEYBOARD_CONTROLLER_DESCRIPTOR_PAGE_SIZE 64u
#define KEYBOARD_CONTROLLER_EVENT_PAGE_SIZE 64u

enum attach_result {
	ATTACH_RESULT_OK,
	ATTACH_RESULT_REJECTED,
	ATTACH_RESULT_RETRY,
	ATTACH_RESULT_FAILED,
};

struct manager_waiter_argument {
	struct keyboard_controller* controller;
};

static const struct registry_protocol_query manager_query = {
	.namespace_path = INPUT_MANAGER_NAMESPACE,
	.protocol       = INPUT_MANAGER_PROTOCOL_NAME,
	.major          = INPUT_MANAGER_PROTOCOL_VERSION_MAJOR,
	.minor          = INPUT_MANAGER_PROTOCOL_VERSION_MINOR,
};

static const struct registry_service_selector keyboard_selector = {
	.namespace_path = KEYBOARD_NAMESPACE,
	.protocol       = KEYBOARD_PROTOCOL_NAME,
	.major          = KEYBOARD_PROTOCOL_VERSION_MAJOR,
	.service        = KEYBOARD_SERVICE_NAME,
};

void keyboard_controller_lock(struct keyboard_controller* controller) {
	while (__atomic_test_and_set(&controller->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

void keyboard_controller_unlock(struct keyboard_controller* controller) {
	__atomic_clear(&controller->locked, __ATOMIC_RELEASE);
}

static bool wake_controller(struct keyboard_controller* controller) {
	struct signal_send_response response;
	return signal_send(controller->activity_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

static bool wake_repeat_thread(struct keyboard_controller* controller) {
	struct signal_send_response response;
	return signal_send(controller->repeat_activity_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

static bool notify_consumers(struct keyboard_controller* controller) {
	struct signal_send_response response;
	return signal_send(controller->event_signal_cap,
	                   controller->next_sequence - 1u,
	                   0u,
	                   0u,
	                   0u,
	                   SIGNAL_SEND_FLAG_COALESCE,
	                   &response) == SYSCALL_STATUS_OK;
}

static size_t event_index(const struct keyboard_controller* controller, size_t offset) {
	return (controller->event_tail + offset) % KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY;
}

static void evict_oldest_frame(struct keyboard_controller* controller) {
	while (controller->event_count != 0u) {
		bool frame_end = (controller->events[controller->event_tail].flags & KEYBOARD_EVENT_FLAG_FRAME_END) != 0u;
		controller->event_tail = (controller->event_tail + 1u) % KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY;
		controller->event_count--;
		if (frame_end) return;
	}
}

static bool append_frame_locked(struct keyboard_controller* controller, struct keyboard_event* events, size_t count) {
	if (count == 0u || count > KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS || controller->next_frame == 0u ||
	    controller->next_frame == UINT64_MAX || controller->next_sequence == 0u ||
	    controller->next_sequence > UINT64_MAX - count)
		return false;
	for (size_t i = 1u; i < count; i++)
		if (events[i].timestamp_ns != events[0].timestamp_ns) return false;
	while (KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY - controller->event_count < count) evict_oldest_frame(controller);
	uint64_t frame = controller->next_frame++;
	for (size_t i = 0u; i < count; i++) {
		events[i].sequence = controller->next_sequence++;
		events[i].frame    = frame;
		events[i].flags    = i + 1u == count ? KEYBOARD_EVENT_FLAG_FRAME_END : KEYBOARD_EVENT_FLAG_NONE;
		controller->events[event_index(controller, controller->event_count++)] = events[i];
	}
	return true;
}

static uint64_t timestamp_now(void) {
	uint64_t timestamp_ns = 0u;
	(void)monotonic_time_ns(&timestamp_ns);
	return timestamp_ns;
}

static struct managed_keyboard* find_keyboard_by_source(struct keyboard_controller* controller, uint64_t source_id) {
	for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (keyboard->source_id == source_id) return keyboard;
	return NULL;
}

static struct rejected_keyboard_source* find_rejected_source(struct keyboard_controller* controller,
                                                             uint64_t                    source_id) {
	for (struct rejected_keyboard_source* source = controller->rejected_sources; source != NULL; source = source->next)
		if (source->source_id == source_id) return source;
	return NULL;
}

static uint32_t modifier_for_key(enum input_key_code key) {
	switch (key) {
	case INPUT_KEY_LEFT_SHIFT:
		return KEYBOARD_MODIFIER_LEFT_SHIFT;
	case INPUT_KEY_RIGHT_SHIFT:
		return KEYBOARD_MODIFIER_RIGHT_SHIFT;
	case INPUT_KEY_LEFT_CONTROL:
		return KEYBOARD_MODIFIER_LEFT_CONTROL;
	case INPUT_KEY_RIGHT_CONTROL:
		return KEYBOARD_MODIFIER_RIGHT_CONTROL;
	case INPUT_KEY_LEFT_ALT:
		return KEYBOARD_MODIFIER_LEFT_ALT;
	case INPUT_KEY_RIGHT_ALT:
		return KEYBOARD_MODIFIER_RIGHT_ALT;
	case INPUT_KEY_LEFT_META:
		return KEYBOARD_MODIFIER_LEFT_META;
	case INPUT_KEY_RIGHT_META:
		return KEYBOARD_MODIFIER_RIGHT_META;
	default:
		return KEYBOARD_MODIFIER_NONE;
	}
}

static uint32_t lock_for_key(enum input_key_code key) {
	switch (key) {
	case INPUT_KEY_CAPS_LOCK:
		return KEYBOARD_LOCK_CAPS_LOCK;
	case INPUT_KEY_NUM_LOCK:
		return KEYBOARD_LOCK_NUM_LOCK;
	case INPUT_KEY_SCROLL_LOCK:
		return KEYBOARD_LOCK_SCROLL_LOCK;
	default:
		return KEYBOARD_LOCK_NONE;
	}
}

static bool key_repeatable(enum input_key_code key) {
	return modifier_for_key(key) == KEYBOARD_MODIFIER_NONE && lock_for_key(key) == KEYBOARD_LOCK_NONE &&
	       key != INPUT_KEY_PAUSE && key != INPUT_KEY_PRINT_SCREEN;
}

static uint32_t recompute_modifiers(const struct managed_keyboard* keyboard) {
	uint32_t modifiers = 0u;
	for (enum input_key_code key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++)
		if (keyboard->pressed[key]) modifiers |= modifier_for_key(key);
	return modifiers;
}

static enum input_key_code most_recent_repeatable_key(const struct managed_keyboard* keyboard) {
	enum input_key_code selected = INPUT_KEY_INVALID;
	uint64_t            order    = 0u;
	for (enum input_key_code key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++) {
		if (keyboard->pressed[key] && key_repeatable(key) && keyboard->press_order[key] > order) {
			selected = key;
			order    = keyboard->press_order[key];
		}
	}
	return selected;
}

static struct keyboard_event lifecycle_event(const struct managed_keyboard* keyboard, enum keyboard_event_type type,
                                             uint64_t timestamp_ns) {
	return (struct keyboard_event){
		.timestamp_ns = timestamp_ns,
		.keyboard_id  = keyboard->id,
		.type         = type,
		.modifiers    = type == KEYBOARD_EVENT_REMOVED ? 0u : keyboard->modifiers,
		.locks        = type == KEYBOARD_EVENT_REMOVED ? 0u : keyboard->locks,
	};
}

static void free_keyboard(struct managed_keyboard* keyboard) {
	if (keyboard == NULL) return;
	free(keyboard->outputs);
	free(keyboard);
}

static bool remove_keyboard(struct keyboard_controller* controller, struct managed_keyboard* keyboard) {
	bool success;
	keyboard_controller_lock(controller);
	struct managed_keyboard** cursor = &controller->keyboards;
	while (*cursor != NULL && *cursor != keyboard) cursor = &(*cursor)->next;
	if (*cursor != keyboard || controller->keyboard_count == 0u || controller->topology_generation == UINT64_MAX) {
		keyboard_controller_unlock(controller);
		return false;
	}
	struct keyboard_event event = lifecycle_event(keyboard, KEYBOARD_EVENT_REMOVED, timestamp_now());
	*cursor                     = keyboard->next;
	controller->keyboard_count--;
	controller->topology_generation++;
	success = append_frame_locked(controller, &event, 1u);
	keyboard_controller_unlock(controller);
	if (success) success = notify_consumers(controller);
	printf("keyboard controller: removed keyboard %llu\n", (unsigned long long)keyboard->id);
	free_keyboard(keyboard);
	return success;
}

static bool reset_keyboard(struct keyboard_controller* controller, struct managed_keyboard* keyboard, bool clear_locks,
                           uint64_t timestamp_ns) {
	keyboard_controller_lock(controller);
	memset(keyboard->pressed, 0, sizeof(keyboard->pressed));
	memset(keyboard->press_order, 0, sizeof(keyboard->press_order));
	keyboard->modifiers           = 0u;
	keyboard->repeat_key          = INPUT_KEY_INVALID;
	keyboard->repeat_remaining_ms = 0u;
	if (clear_locks) keyboard->locks = 0u;
	keyboard->led_dirty           = true;
	struct keyboard_event event   = lifecycle_event(keyboard, KEYBOARD_EVENT_RESET, timestamp_ns);
	bool                  success = append_frame_locked(controller, &event, 1u);
	keyboard_controller_unlock(controller);
	if (!wake_repeat_thread(controller)) success = false;
	if (success) success = notify_consumers(controller);
	if (success)
		printf("keyboard controller: reset keyboard %llu%s\n",
		       (unsigned long long)keyboard->id,
		       clear_locks ? " after an input-history gap" : "");
	return success;
}

static bool reset_all_keyboards(struct keyboard_controller* controller, bool clear_locks, uint64_t timestamp_ns) {
	for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (!reset_keyboard(controller, keyboard, clear_locks, timestamp_ns)) return false;
	return true;
}

static void registry_waiter(void* opaque) {
	struct keyboard_controller* controller = *(struct keyboard_controller**)opaque;
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(controller->registry_signal_cap, &message);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) {
			__atomic_store_n(&controller->registry_failed, true, __ATOMIC_RELEASE);
			(void)wake_controller(controller);
			thread_exit(1u);
		}
		__atomic_store_n(&controller->registry_dirty, true, __ATOMIC_RELEASE);
		if (!wake_controller(controller)) thread_exit(1u);
	}
}

static void manager_waiter(void* opaque) {
	const struct manager_waiter_argument* argument   = opaque;
	struct keyboard_controller*           controller = argument->controller;
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(controller->manager_signal_cap, &message);
		if (__atomic_load_n(&controller->manager_stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) {
			__atomic_store_n(&controller->manager_dead, true, __ATOMIC_RELEASE);
			(void)wake_controller(controller);
			thread_exit(1u);
		}
		__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
		if (!wake_controller(controller)) thread_exit(1u);
	}
}

static bool stop_manager(struct keyboard_controller* controller) {
	bool success = true;
	__atomic_store_n(&controller->manager_stopping, true, __ATOMIC_RELEASE);
	if (controller->manager_waiter_cap != CAP_ID_INVALID) {
		(void)thread_cancel(controller->manager_waiter_cap);
		if (thread_join(controller->manager_waiter_cap, NULL) != SYSCALL_STATUS_OK) success = false;
		controller->manager_waiter_cap = CAP_ID_INVALID;
	}
	if (controller->manager_signal_cap != CAP_ID_INVALID) {
		if (cap_drop(controller->manager_signal_cap) != SYSCALL_STATUS_OK) success = false;
		controller->manager_signal_cap = CAP_ID_INVALID;
	}
	if (controller->manager_cap != CAP_ID_INVALID) {
		if (cap_drop(controller->manager_cap) != SYSCALL_STATUS_OK) success = false;
		controller->manager_cap = CAP_ID_INVALID;
	}
	controller->manager_owner               = PROCESS_PID_INVALID;
	controller->manager_topology_generation = 0u;
	controller->manager_cursor              = 0u;
	controller->pending_count               = 0u;
	controller->pending_frame               = 0u;
	controller->pending_source              = 0u;
	__atomic_store_n(&controller->manager_stopping, false, __ATOMIC_RELEASE);
	return success;
}

static bool remove_all_keyboards(struct keyboard_controller* controller) {
	while (controller->keyboards != NULL)
		if (!remove_keyboard(controller, controller->keyboards)) return false;
	while (controller->rejected_sources != NULL) {
		struct rejected_keyboard_source* next = controller->rejected_sources->next;
		free(controller->rejected_sources);
		controller->rejected_sources = next;
	}
	return true;
}

static bool manager_handle_valid(const struct registry_service_handle* handle) {
	return handle->capability != CAP_ID_INVALID && handle->owner != PROCESS_PID_INVALID &&
	       strcmp(handle->info.selector.namespace_path, INPUT_MANAGER_NAMESPACE) == 0 &&
	       strcmp(handle->info.selector.protocol, INPUT_MANAGER_PROTOCOL_NAME) == 0 &&
	       handle->info.selector.major == INPUT_MANAGER_PROTOCOL_VERSION_MAJOR &&
	       strcmp(handle->info.selector.service, INPUT_MANAGER_SERVICE_NAME) == 0 &&
	       handle->info.rights == INPUT_MANAGER_CLIENT_RIGHTS;
}

static bool connect_manager(struct keyboard_controller* controller) {
	struct registry_service_handle handle = {.capability = CAP_ID_INVALID};
	struct registry_call_result    result = registry_acquire(&manager_query, INPUT_MANAGER_SERVICE_NAME, &handle);
	if (result.transport_status != SYSCALL_STATUS_OK) return false;
	if (result.status == REGISTRY_STATUS_NOT_FOUND) {
		if (controller->manager_cap != CAP_ID_INVALID) {
			if (!stop_manager(controller) || !remove_all_keyboards(controller)) return false;
			printf("keyboard controller: input manager unavailable\n");
		}
		return true;
	}
	if (result.status != REGISTRY_STATUS_OK || !manager_handle_valid(&handle)) {
		if (handle.capability != CAP_ID_INVALID) (void)cap_drop(handle.capability);
		return false;
	}
	if (controller->manager_cap != CAP_ID_INVALID && controller->manager_owner == handle.owner) {
		return cap_drop(handle.capability) == SYSCALL_STATUS_OK;
	}
	if (controller->manager_cap != CAP_ID_INVALID && (!stop_manager(controller) || !remove_all_keyboards(controller))) {
		(void)cap_drop(handle.capability);
		return false;
	}
	struct input_manager_get_info_response info;
	cap_id_t                               signal_cap = CAP_ID_INVALID;
	if (input_manager_get_info(handle.capability, &info) != SYSCALL_STATUS_OK ||
	    input_manager_acquire_event_signal(handle.capability, &signal_cap) != SYSCALL_STATUS_OK) {
		if (signal_cap != CAP_ID_INVALID) (void)cap_drop(signal_cap);
		(void)cap_drop(handle.capability);
		return true;
	}
	controller->manager_cap        = handle.capability;
	controller->manager_signal_cap = signal_cap;
	controller->manager_owner      = handle.owner;
	controller->manager_cursor     = 0u;
	__atomic_store_n(&controller->manager_stopping, false, __ATOMIC_RELEASE);
	const struct manager_waiter_argument argument = {.controller = controller};
	if (process_spawn_thread(controller->self_cap,
	                         (uintptr_t)manager_waiter,
	                         &argument,
	                         sizeof(argument),
	                         "keyboard input waiter",
	                         sizeof("keyboard input waiter"),
	                         &controller->manager_waiter_cap) != SYSCALL_STATUS_OK) {
		(void)stop_manager(controller);
		return false;
	}
	__atomic_store_n(&controller->manager_dead, false, __ATOMIC_RELEASE);
	__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
	printf("keyboard controller: connected to input manager\n");
	return true;
}

static bool reject_source(struct keyboard_controller* controller, uint64_t source_id) {
	struct rejected_keyboard_source* source = calloc(1u, sizeof(*source));
	if (source == NULL) return false;
	source->source_id            = source_id;
	source->seen                 = true;
	source->next                 = controller->rejected_sources;
	controller->rejected_sources = source;
	printf("keyboard controller: quarantined keyboard source %llu\n", (unsigned long long)source_id);
	return true;
}

static enum attach_result attach_keyboard(struct keyboard_controller*        controller,
                                          const struct input_manager_source* source, uint64_t generation) {
	struct input_control_descriptor* descriptors = malloc(source->descriptor_count * sizeof(*descriptors));
	if (descriptors == NULL) return ATTACH_RESULT_FAILED;
	for (size_t offset = 0u; offset < source->descriptor_count;) {
		size_t capacity = source->descriptor_count - offset;
		if (capacity > KEYBOARD_CONTROLLER_DESCRIPTOR_PAGE_SIZE) capacity = KEYBOARD_CONTROLLER_DESCRIPTOR_PAGE_SIZE;
		size_t           returned = 0u;
		uint64_t         total    = 0u;
		syscall_status_t status   = input_manager_read_descriptors(
			controller->manager_cap, generation, source->id, offset, descriptors + offset, capacity, &returned, &total);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			free(descriptors);
			return ATTACH_RESULT_RETRY;
		}
		if (status != SYSCALL_STATUS_OK || total != source->descriptor_count || returned == 0u) {
			free(descriptors);
			return ATTACH_RESULT_FAILED;
		}
		offset += returned;
	}
	bool   keys[INPUT_KEY_KEYPAD_DECIMAL + 1u] = {0};
	size_t key_count                           = 0u;
	for (size_t i = 0u; i < source->descriptor_count; i++) {
		if (descriptors[i].kind != INPUT_CONTROL_KIND_KEY) continue;
		if (descriptors[i].code <= INPUT_KEY_INVALID || descriptors[i].code > INPUT_KEY_KEYPAD_DECIMAL ||
		    keys[descriptors[i].code]) {
			free(descriptors);
			return ATTACH_RESULT_REJECTED;
		}
		keys[descriptors[i].code] = true;
		key_count++;
	}
	free(descriptors);
	if (key_count == 0u) return ATTACH_RESULT_REJECTED;

	struct keyboard_output* outputs      = NULL;
	size_t                  output_count = 0u;
	if (source->output_descriptor_count != 0u) {
		struct input_output_descriptor* descriptors_out =
			malloc(source->output_descriptor_count * sizeof(*descriptors_out));
		if (descriptors_out == NULL) return ATTACH_RESULT_FAILED;
		for (size_t offset = 0u; offset < source->output_descriptor_count;) {
			size_t capacity = source->output_descriptor_count - offset;
			if (capacity > KEYBOARD_CONTROLLER_DESCRIPTOR_PAGE_SIZE)
				capacity = KEYBOARD_CONTROLLER_DESCRIPTOR_PAGE_SIZE;
			size_t           returned = 0u;
			uint64_t         total    = 0u;
			syscall_status_t status   = input_manager_read_output_descriptors(controller->manager_cap,
			                                                                  generation,
			                                                                  source->id,
			                                                                  offset,
			                                                                  descriptors_out + offset,
			                                                                  capacity,
			                                                                  &returned,
			                                                                  &total);
			if (status == SYSCALL_STATUS_UNAVAILABLE) {
				free(descriptors_out);
				return ATTACH_RESULT_RETRY;
			}
			if (status != SYSCALL_STATUS_OK || total != source->output_descriptor_count || returned == 0u) {
				free(descriptors_out);
				return ATTACH_RESULT_FAILED;
			}
			offset += returned;
		}
		outputs = malloc(source->output_descriptor_count * sizeof(*outputs));
		if (outputs == NULL) {
			free(descriptors_out);
			return ATTACH_RESULT_FAILED;
		}
		for (size_t i = 0u; i < source->output_descriptor_count; i++) {
			uint32_t lock = 0u;
			if (descriptors_out[i].kind != INPUT_OUTPUT_KIND_BOOLEAN) continue;
			switch (descriptors_out[i].code) {
			case INPUT_OUTPUT_CODE_CAPS_LOCK_INDICATOR:
				lock = KEYBOARD_LOCK_CAPS_LOCK;
				break;
			case INPUT_OUTPUT_CODE_NUM_LOCK_INDICATOR:
				lock = KEYBOARD_LOCK_NUM_LOCK;
				break;
			case INPUT_OUTPUT_CODE_SCROLL_LOCK_INDICATOR:
				lock = KEYBOARD_LOCK_SCROLL_LOCK;
				break;
			default:
				break;
			}
			if (lock != 0u)
				outputs[output_count++] = (struct keyboard_output){.output_id = descriptors_out[i].id, .lock = lock};
		}
		free(descriptors_out);
		if (output_count > source->maximum_output_values) {
			free(outputs);
			return ATTACH_RESULT_REJECTED;
		}
	}

	struct managed_keyboard* keyboard = calloc(1u, sizeof(*keyboard));
	if (keyboard == NULL) {
		free(outputs);
		return ATTACH_RESULT_FAILED;
	}
	keyboard->source_id    = source->id;
	keyboard->outputs      = outputs;
	keyboard->output_count = output_count;
	keyboard->led_dirty    = output_count != 0u;
	keyboard_controller_lock(controller);
	if (controller->keyboard_count >= KEYBOARD_CONTROLLER_MAXIMUM_KEYBOARDS || controller->next_keyboard_id == 0u ||
	    controller->next_keyboard_id == UINT64_MAX || controller->topology_generation == UINT64_MAX) {
		keyboard_controller_unlock(controller);
		free_keyboard(keyboard);
		return ATTACH_RESULT_FAILED;
	}
	keyboard->id          = controller->next_keyboard_id++;
	keyboard->seen        = true;
	keyboard->next        = controller->keyboards;
	controller->keyboards = keyboard;
	controller->keyboard_count++;
	controller->topology_generation++;
	struct keyboard_event event   = lifecycle_event(keyboard, KEYBOARD_EVENT_ADDED, timestamp_now());
	bool                  success = append_frame_locked(controller, &event, 1u);
	if (!success) {
		controller->keyboards = keyboard->next;
		controller->keyboard_count--;
		controller->topology_generation--;
		controller->next_keyboard_id--;
	}
	keyboard_controller_unlock(controller);
	if (!success || !notify_consumers(controller)) {
		if (!success) free_keyboard(keyboard);
		return ATTACH_RESULT_FAILED;
	}
	printf("keyboard controller: attached keyboard %llu from source %llu\n",
	       (unsigned long long)keyboard->id,
	       (unsigned long long)keyboard->source_id);
	return ATTACH_RESULT_OK;
}

static bool reconcile_keyboards(struct keyboard_controller* controller) {
	struct input_manager_get_info_response info;
	syscall_status_t                       status = input_manager_get_info(controller->manager_cap, &info);
	if (status != SYSCALL_STATUS_OK) {
		printf("keyboard controller: input-manager metadata failed: %u\n", (unsigned)status);
		__atomic_store_n(&controller->manager_dead, true, __ATOMIC_RELEASE);
		return true;
	}
	if (info.source_count > KEYBOARD_CONTROLLER_MAXIMUM_KEYBOARDS) return false;
	for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next)
		keyboard->seen = false;
	for (struct rejected_keyboard_source* source = controller->rejected_sources; source != NULL; source = source->next)
		source->seen = false;
	uint64_t offset = 0u;
	while (offset < info.source_count) {
		struct input_manager_source sources[KEYBOARD_CONTROLLER_SOURCE_PAGE_SIZE];
		size_t                      capacity = (size_t)(info.source_count - offset);
		if (capacity > KEYBOARD_CONTROLLER_SOURCE_PAGE_SIZE) capacity = KEYBOARD_CONTROLLER_SOURCE_PAGE_SIZE;
		size_t   returned = 0u;
		uint64_t total    = 0u;
		status            = input_manager_enumerate_sources(
			controller->manager_cap, info.topology_generation, offset, sources, capacity, &returned, &total);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
			return true;
		}
		if (status != SYSCALL_STATUS_OK || total != info.source_count || returned == 0u) {
			__atomic_store_n(&controller->manager_dead, true, __ATOMIC_RELEASE);
			return true;
		}
		for (size_t i = 0u; i < returned; i++) {
			if (sources[i].device_class != INPUT_DEVICE_CLASS_KEYBOARD) continue;
			struct managed_keyboard* keyboard = find_keyboard_by_source(controller, sources[i].id);
			if (keyboard != NULL) {
				keyboard->seen = true;
				continue;
			}
			struct rejected_keyboard_source* rejected = find_rejected_source(controller, sources[i].id);
			if (rejected != NULL) {
				rejected->seen = true;
				continue;
			}
			enum attach_result attach = attach_keyboard(controller, &sources[i], info.topology_generation);
			if (attach == ATTACH_RESULT_RETRY) {
				__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
				return true;
			}
			if (attach == ATTACH_RESULT_REJECTED) {
				if (!reject_source(controller, sources[i].id)) return false;
			}
			else if (attach == ATTACH_RESULT_FAILED) return false;
		}
		offset += returned;
	}
	struct managed_keyboard* keyboard = controller->keyboards;
	while (keyboard != NULL) {
		struct managed_keyboard* next = keyboard->next;
		if (!keyboard->seen && !remove_keyboard(controller, keyboard)) return false;
		keyboard = next;
	}
	struct rejected_keyboard_source** rejected = &controller->rejected_sources;
	while (*rejected != NULL) {
		if ((*rejected)->seen) {
			rejected = &(*rejected)->next;
			continue;
		}
		struct rejected_keyboard_source* removed = *rejected;
		*rejected                                = removed->next;
		free(removed);
	}
	controller->manager_topology_generation = info.topology_generation;
	return true;
}

static bool process_key_frame(struct keyboard_controller* controller) {
	struct managed_keyboard* keyboard = find_keyboard_by_source(controller, controller->pending_source);
	if (keyboard == NULL) return true;
	struct keyboard_event output[KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS];
	size_t                output_count  = 0u;
	bool                  lock_changed  = false;
	enum input_key_code   newest_repeat = INPUT_KEY_INVALID;
	keyboard_controller_lock(controller);
	for (size_t i = 0u; i < controller->pending_count; i++) {
		const struct input_manager_event* input = &controller->pending[i];
		if (input->control_kind != INPUT_CONTROL_KIND_KEY) continue;
		if (input->control_code <= INPUT_KEY_INVALID || input->control_code > INPUT_KEY_KEYPAD_DECIMAL ||
		    (input->value != 0 && input->value != 1)) {
			keyboard_controller_unlock(controller);
			return false;
		}
		enum input_key_code key     = (enum input_key_code)input->control_code;
		bool                pressed = input->value != 0;
		if (keyboard->pressed[key] == pressed) continue;
		keyboard->pressed[key] = pressed;
		if (pressed) {
			if (controller->next_press_order == 0u || controller->next_press_order == UINT64_MAX) {
				keyboard_controller_unlock(controller);
				return false;
			}
			keyboard->press_order[key] = controller->next_press_order++;
			uint32_t lock              = lock_for_key(key);
			if (lock != 0u) {
				keyboard->locks ^= lock;
				lock_changed = true;
			}
			if (key_repeatable(key)) newest_repeat = key;
		}
		else keyboard->press_order[key] = 0u;
		output[output_count++] = (struct keyboard_event){
			.timestamp_ns = controller->pending[0].timestamp_ns,
			.keyboard_id  = keyboard->id,
			.type         = KEYBOARD_EVENT_KEY,
			.action       = pressed ? KEYBOARD_KEY_ACTION_PRESSED : KEYBOARD_KEY_ACTION_RELEASED,
			.key          = key,
		};
	}
	keyboard->modifiers = recompute_modifiers(keyboard);
	if (newest_repeat != INPUT_KEY_INVALID) {
		keyboard->repeat_key          = newest_repeat;
		keyboard->repeat_remaining_ms = KEYBOARD_CONTROLLER_REPEAT_DELAY_MS;
	}
	else if (keyboard->repeat_key != INPUT_KEY_INVALID && !keyboard->pressed[keyboard->repeat_key]) {
		keyboard->repeat_key = most_recent_repeatable_key(keyboard);
		keyboard->repeat_remaining_ms =
			keyboard->repeat_key == INPUT_KEY_INVALID ? 0u : KEYBOARD_CONTROLLER_REPEAT_DELAY_MS;
	}
	if (lock_changed) keyboard->led_dirty = true;
	for (size_t i = 0u; i < output_count; i++) {
		output[i].modifiers = keyboard->modifiers;
		output[i].locks     = keyboard->locks;
	}
	bool success = output_count == 0u || append_frame_locked(controller, output, output_count);
	keyboard_controller_unlock(controller);
	if (!wake_repeat_thread(controller)) return false;
	if (success && output_count != 0u) {
		success = notify_consumers(controller);
	}
	return success;
}

static bool handle_manager_event(struct keyboard_controller* controller, const struct input_manager_event* event) {
	switch (event->type) {
	case INPUT_MANAGER_EVENT_SOURCE_ADDED:
	case INPUT_MANAGER_EVENT_SOURCE_REMOVED:
		if (controller->pending_count != 0u) return false;
		__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
		return reconcile_keyboards(controller);
	case INPUT_MANAGER_EVENT_SOURCE_RESET: {
		if (controller->pending_count != 0u) return false;
		struct managed_keyboard* keyboard = find_keyboard_by_source(controller, event->source_id);
		return keyboard == NULL || reset_keyboard(controller, keyboard, false, event->timestamp_ns);
	}
	case INPUT_MANAGER_EVENT_CONTROL:
		if (controller->pending_count == 0u) {
			controller->pending_frame  = event->frame;
			controller->pending_source = event->source_id;
		}
		else if (controller->pending_frame != event->frame || controller->pending_source != event->source_id ||
		         controller->pending[0].timestamp_ns != event->timestamp_ns)
			return false;
		if (controller->pending_count >= KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS) return false;
		controller->pending[controller->pending_count++] = *event;
		if ((event->flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) != 0u) {
			bool success               = process_key_frame(controller);
			controller->pending_count  = 0u;
			controller->pending_frame  = 0u;
			controller->pending_source = 0u;
			return success;
		}
		return true;
	default:
		return false;
	}
}

static bool drain_manager(struct keyboard_controller* controller) {
	struct input_manager_event events[KEYBOARD_CONTROLLER_EVENT_PAGE_SIZE];
	for (;;) {
		struct input_manager_event_page page;
		syscall_status_t                status = input_manager_read_events(
			controller->manager_cap, controller->manager_cursor, events, KEYBOARD_CONTROLLER_EVENT_PAGE_SIZE, &page);
		if (status == SYSCALL_STATUS_UNAVAILABLE) return controller->pending_count == 0u;
		if (status != SYSCALL_STATUS_OK) {
			printf("keyboard controller: input-manager event read failed: %u\n", (unsigned)status);
			__atomic_store_n(&controller->manager_dead, true, __ATOMIC_RELEASE);
			return true;
		}
		if ((page.flags & INPUT_MANAGER_READ_EVENT_FLAG_GAP) != 0u) {
			controller->pending_count  = 0u;
			controller->pending_frame  = 0u;
			controller->pending_source = 0u;
			if (!reset_all_keyboards(controller, true, timestamp_now())) return false;
		}
		for (size_t i = 0u; i < page.returned; i++) {
			if (!handle_manager_event(controller, &events[i])) return false;
			controller->manager_cursor = events[i].sequence;
		}
		if (controller->manager_cursor == page.newest_sequence) return controller->pending_count == 0u;
	}
}

static bool process_led_outputs(struct keyboard_controller* controller) {
	for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next) {
		if (!keyboard->led_dirty) continue;
		keyboard->led_dirty = false;
		if (keyboard->output_count == 0u) continue;
		struct input_output_value* values = malloc(keyboard->output_count * sizeof(*values));
		if (values == NULL) return false;
		for (size_t i = 0u; i < keyboard->output_count; i++) {
			values[i] = (struct input_output_value){
				.output_id = keyboard->outputs[i].output_id,
				.value     = (keyboard->locks & keyboard->outputs[i].lock) != 0u,
			};
		}
		syscall_status_t status = input_manager_set_outputs(controller->manager_cap,
		                                                    controller->manager_topology_generation,
		                                                    keyboard->source_id,
		                                                    values,
		                                                    keyboard->output_count);
		free(values);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			keyboard->led_dirty = true;
			__atomic_store_n(&controller->manager_dirty, true, __ATOMIC_RELEASE);
			return true;
		}
		if (status != SYSCALL_STATUS_OK) {
			keyboard->led_dirty = true;
			printf("keyboard controller: keyboard %llu LED update failed: %u\n",
			       (unsigned long long)keyboard->id,
			       (unsigned)status);
		}
	}
	return true;
}

static bool any_repeat_active_locked(const struct keyboard_controller* controller) {
	for (const struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next)
		if (keyboard->repeat_key != INPUT_KEY_INVALID) return true;
	return false;
}

static void repeat_thread(void* opaque) {
	struct keyboard_controller* controller = *(struct keyboard_controller**)opaque;
	for (;;) {
		keyboard_controller_lock(controller);
		bool active = any_repeat_active_locked(controller);
		keyboard_controller_unlock(controller);
		if (!active) {
			struct signal_message message;
			syscall_status_t      status = signal_wait(controller->repeat_activity_cap, &message);
			if (status == SYSCALL_STATUS_INTERRUPTED) continue;
			if (status != SYSCALL_STATUS_OK) break;
			continue;
		}
		if (sleep_ms(KEYBOARD_CONTROLLER_REPEAT_QUANTUM_MS) != SYSCALL_STATUS_OK) break;
		bool published = false;
		bool success   = true;
		keyboard_controller_lock(controller);
		for (struct managed_keyboard* keyboard = controller->keyboards; keyboard != NULL; keyboard = keyboard->next) {
			if (keyboard->repeat_key == INPUT_KEY_INVALID) continue;
			if (keyboard->repeat_remaining_ms > KEYBOARD_CONTROLLER_REPEAT_QUANTUM_MS) {
				keyboard->repeat_remaining_ms -= KEYBOARD_CONTROLLER_REPEAT_QUANTUM_MS;
				continue;
			}
			uint32_t remainder            = KEYBOARD_CONTROLLER_REPEAT_QUANTUM_MS - keyboard->repeat_remaining_ms;
			keyboard->repeat_remaining_ms = KEYBOARD_CONTROLLER_REPEAT_INTERVAL_MS > remainder
			                                    ? KEYBOARD_CONTROLLER_REPEAT_INTERVAL_MS - remainder
			                                    : 1u;
			struct keyboard_event event   = {
				.timestamp_ns = timestamp_now(),
				.keyboard_id  = keyboard->id,
				.type         = KEYBOARD_EVENT_KEY,
				.action       = KEYBOARD_KEY_ACTION_REPEATED,
				.key          = keyboard->repeat_key,
				.modifiers    = keyboard->modifiers,
				.locks        = keyboard->locks,
			};
			if (!append_frame_locked(controller, &event, 1u)) {
				success = false;
				break;
			}
			published = true;
		}
		keyboard_controller_unlock(controller);
		if (published && !notify_consumers(controller)) success = false;
		if (!success) break;
	}
	__atomic_store_n(&controller->failed, true, __ATOMIC_RELEASE);
	(void)wake_controller(controller);
	thread_exit(1u);
}

static void initialize_invalid_capabilities(struct keyboard_controller* controller) {
	controller->self_cap             = CAP_ID_INVALID;
	controller->channel_activity_cap = CAP_ID_INVALID;
	controller->service_cap          = CAP_ID_INVALID;
	controller->event_signal_cap     = CAP_ID_INVALID;
	controller->activity_cap         = CAP_ID_INVALID;
	controller->registry_signal_cap  = CAP_ID_INVALID;
	controller->repeat_activity_cap  = CAP_ID_INVALID;
	controller->manager_cap          = CAP_ID_INVALID;
	controller->manager_signal_cap   = CAP_ID_INVALID;
	controller->manager_waiter_cap   = CAP_ID_INVALID;
	controller->manager_owner        = PROCESS_PID_INVALID;
	controller->endpoint             = CHANNEL_ID_INVALID;
}

bool keyboard_controller_initialize(struct keyboard_controller* controller) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	cap_id_t registry_thread_cap = CAP_ID_INVALID;
	cap_id_t repeat_thread_cap   = CAP_ID_INVALID;
	if (controller == NULL) return false;
	memset(controller, 0, sizeof(*controller));
	initialize_invalid_capabilities(controller);
	controller->topology_generation = 1u;
	controller->next_keyboard_id    = 1u;
	controller->next_sequence       = 1u;
	controller->next_frame          = 1u;
	controller->next_press_order    = 1u;
	controller->events              = calloc(KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY, sizeof(*controller->events));
	if (controller->events == NULL) return false;

	syscall_status_t status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK && self.pid == PROCESS_PID_INVALID) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		controller->pid      = self.pid;
		controller->self_cap = self.self_cap;
		self.self_cap        = CAP_ID_INVALID;
		status               = signal_create(&controller->event_signal_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = signal_create(&controller->activity_cap);
	if (status == SYSCALL_STATUS_OK) status = signal_create(&controller->registry_signal_cap);
	if (status == SYSCALL_STATUS_OK) status = signal_create(&controller->repeat_activity_cap);
	if (status == SYSCALL_STATUS_OK) status = channel_create(&controller->endpoint, &controller->channel_activity_cap);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(controller->endpoint,
		                     KEYBOARD_CONTROLLER_OBJECT_ID,
		                     controller->pid,
		                     CAP_CALL | CAP_READ | CAP_DELEGATE,
		                     &controller->service_cap);
	if (status == SYSCALL_STATUS_OK && !keyboard_controller_server_start(controller)) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		struct registry_call_result result = registry_advertise(
			&keyboard_selector, KEYBOARD_PROTOCOL_VERSION_MINOR, controller->service_cap, KEYBOARD_CLIENT_RIGHTS);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK)
			status = SYSCALL_STATUS_FAILED;
	}
	if (status == SYSCALL_STATUS_OK) {
		uint64_t                    counter;
		struct registry_call_result result = registry_watch(
			&manager_query, controller->registry_signal_cap, &controller->registry_subscription, &counter);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK || counter == 0u)
			status = SYSCALL_STATUS_FAILED;
		else __atomic_store_n(&controller->registry_dirty, true, __ATOMIC_RELEASE);
	}
	if (status == SYSCALL_STATUS_OK) {
		struct keyboard_controller* argument = controller;
		status                               = process_spawn_thread(controller->self_cap,
		                                                            (uintptr_t)registry_waiter,
		                                                            &argument,
		                                                            sizeof(argument),
		                                                            "keyboard registry waiter",
		                                                            sizeof("keyboard registry waiter"),
		                                                            &registry_thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(registry_thread_cap);
	if (status == SYSCALL_STATUS_OK) {
		struct keyboard_controller* argument = controller;
		status                               = process_spawn_thread(controller->self_cap,
		                                                            (uintptr_t)repeat_thread,
		                                                            &argument,
		                                                            sizeof(argument),
		                                                            "keyboard repeat",
		                                                            sizeof("keyboard repeat"),
		                                                            &repeat_thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(repeat_thread_cap);
	if (registry_thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(registry_thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	if (repeat_thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(repeat_thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	printf("keyboard controller: advertised %s/%s@%u.%u/%s\n",
	       KEYBOARD_NAMESPACE,
	       KEYBOARD_PROTOCOL_NAME,
	       KEYBOARD_PROTOCOL_VERSION_MAJOR,
	       KEYBOARD_PROTOCOL_VERSION_MINOR,
	       KEYBOARD_SERVICE_NAME);
	return true;
}

int keyboard_controller_run(struct keyboard_controller* controller) {
	if (controller == NULL) return 1;
	for (;;) {
		if (__atomic_load_n(&controller->registry_failed, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&controller->failed, __ATOMIC_ACQUIRE))
			return 1;
		if (__atomic_exchange_n(&controller->manager_dead, false, __ATOMIC_ACQ_REL)) {
			if (!stop_manager(controller) || !remove_all_keyboards(controller)) return 1;
			printf("keyboard controller: lost input manager\n");
		}
		if (__atomic_exchange_n(&controller->registry_dirty, false, __ATOMIC_ACQ_REL) && !connect_manager(controller))
			return 1;
		if (controller->manager_cap != CAP_ID_INVALID &&
		    __atomic_exchange_n(&controller->manager_dirty, false, __ATOMIC_ACQ_REL)) {
			if (!reconcile_keyboards(controller)) return 1;
			if (__atomic_load_n(&controller->manager_dead, __ATOMIC_ACQUIRE)) continue;
			if (!drain_manager(controller)) return 1;
		}
		if (controller->manager_cap != CAP_ID_INVALID && !process_led_outputs(controller)) return 1;
		if (__atomic_load_n(&controller->registry_dirty, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&controller->manager_dirty, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&controller->manager_dead, __ATOMIC_ACQUIRE))
			continue;
		struct signal_message activity;
		syscall_status_t      status = signal_wait(controller->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return 1;
	}
}
