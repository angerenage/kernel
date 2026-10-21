#include "input_manager.h"

#include <limits.h>
#include <runtime/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#define INPUT_PROTOCOL_VERSION_MAJOR 1u
#define INPUT_PROTOCOL_VERSION_MINOR 0u
#define INPUT_MANAGER_PROTOCOL_VERSION_MAJOR 1u
#define INPUT_MANAGER_PROTOCOL_VERSION_MINOR 0u
#define INPUT_MANAGER_REGISTRY_PAGE_SIZE 8u
#define INPUT_MANAGER_DESCRIPTOR_PAGE_SIZE 64u

enum attach_result {
	ATTACH_RESULT_ATTACHED,
	ATTACH_RESULT_QUARANTINED,
	ATTACH_RESULT_RETRY,
	ATTACH_RESULT_FAILED,
};

struct source_waiter_argument {
	struct input_manager*        manager;
	struct managed_input_source* source;
};

static const struct registry_protocol_query input_query = {
	.namespace_path = INPUT_NAMESPACE,
	.protocol       = INPUT_PROTOCOL_NAME,
	.major          = INPUT_PROTOCOL_VERSION_MAJOR,
	.minor          = INPUT_PROTOCOL_VERSION_MINOR,
};

static const struct registry_service_selector manager_selector = {
	.namespace_path = INPUT_MANAGER_NAMESPACE,
	.protocol       = INPUT_MANAGER_PROTOCOL_NAME,
	.major          = INPUT_MANAGER_PROTOCOL_VERSION_MAJOR,
	.service        = INPUT_MANAGER_SERVICE_NAME,
};

void input_manager_lock(struct input_manager* manager) {
	while (__atomic_test_and_set(&manager->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

void input_manager_unlock(struct input_manager* manager) {
	__atomic_clear(&manager->locked, __ATOMIC_RELEASE);
}

static bool wake_manager(struct input_manager* manager) {
	struct signal_send_response response;
	return signal_send(manager->activity_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

static bool notify_consumers(struct input_manager* manager) {
	struct signal_send_response response;
	return signal_send(manager->event_signal_cap,
	                   manager->next_sequence - 1u,
	                   0u,
	                   0u,
	                   0u,
	                   SIGNAL_SEND_FLAG_COALESCE,
	                   &response) == SYSCALL_STATUS_OK;
}

static size_t event_index(const struct input_manager* manager, size_t offset) {
	return (manager->event_tail + offset) % INPUT_MANAGER_EVENT_QUEUE_CAPACITY;
}

static void evict_oldest_frame(struct input_manager* manager) {
	while (manager->event_count != 0u) {
		bool frame_end      = (manager->events[manager->event_tail].flags & INPUT_MANAGER_EVENT_FLAG_FRAME_END) != 0u;
		manager->event_tail = (manager->event_tail + 1u) % INPUT_MANAGER_EVENT_QUEUE_CAPACITY;
		manager->event_count--;
		if (frame_end) return;
	}
}

static bool append_frame_locked(struct input_manager* manager, struct input_manager_event* events, size_t count) {
	if (count == 0u || count > INPUT_MANAGER_MAXIMUM_FRAME_EVENTS || manager->next_sequence == 0u ||
	    manager->next_sequence > UINT64_MAX - count || manager->next_frame == 0u || manager->next_frame == UINT64_MAX)
		return false;
	while (INPUT_MANAGER_EVENT_QUEUE_CAPACITY - manager->event_count < count) evict_oldest_frame(manager);
	for (size_t i = 0u; i < count; i++) {
		events[i].sequence = manager->next_sequence++;
		events[i].frame    = manager->next_frame;
		events[i].flags    = i + 1u == count ? INPUT_MANAGER_EVENT_FLAG_FRAME_END : INPUT_MANAGER_EVENT_FLAG_NONE;
		manager->events[event_index(manager, manager->event_count++)] = events[i];
	}
	manager->next_frame++;
	return true;
}

static struct input_manager_event lifecycle_event(const struct managed_input_source* source,
                                                  enum input_manager_event_type      type) {
	return (struct input_manager_event){
		.source_id    = source->id,
		.type         = type,
		.device_class = source->device_class,
	};
}

static bool append_lifecycle_frame(struct input_manager* manager, const struct managed_input_source* source,
                                   enum input_manager_event_type type) {
	struct input_manager_event event = lifecycle_event(source, type);
	input_manager_lock(manager);
	bool success = append_frame_locked(manager, &event, 1u);
	input_manager_unlock(manager);
	return success && notify_consumers(manager);
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

static const struct input_control_descriptor* find_descriptor(const struct managed_input_source* source, uint32_t id) {
	for (size_t i = 0u; i < source->descriptor_count; i++)
		if (source->descriptors[i].id == id) return &source->descriptors[i];
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

static bool selector_equal(const struct registry_service_selector* left,
                           const struct registry_service_selector* right) {
	return left->major == right->major && strcmp(left->namespace_path, right->namespace_path) == 0 &&
	       strcmp(left->protocol, right->protocol) == 0 && strcmp(left->service, right->service) == 0;
}

static bool service_equal(const struct registry_service_info* left, const struct registry_service_info* right) {
	return selector_equal(&left->selector, &right->selector) && left->minor == right->minor &&
	       left->rights == right->rights;
}

static struct managed_input_source* find_source(struct input_manager*                   manager,
                                                const struct registry_service_selector* selector) {
	for (struct managed_input_source* source = manager->sources; source != NULL; source = source->next)
		if (selector_equal(&source->service.selector, selector)) return source;
	return NULL;
}

static void registry_waiter(void* opaque) {
	struct input_manager* manager = *(struct input_manager**)opaque;
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(manager->registry_signal_cap, &message);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) {
			__atomic_store_n(&manager->registry_failed, true, __ATOMIC_RELEASE);
			(void)wake_manager(manager);
			thread_exit(1u);
		}
		if (__atomic_add_fetch(&manager->registry_revision, 1u, __ATOMIC_ACQ_REL) == 0u) {
			__atomic_store_n(&manager->registry_failed, true, __ATOMIC_RELEASE);
			(void)wake_manager(manager);
			thread_exit(1u);
		}
		__atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
		if (!wake_manager(manager)) thread_exit(1u);
	}
}

static void source_waiter(void* opaque) {
	const struct source_waiter_argument* argument = opaque;
	struct input_manager*                manager  = argument->manager;
	struct managed_input_source*         source   = argument->source;
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(source->signal_cap, &message);
		if (__atomic_load_n(&source->stopping, __ATOMIC_ACQUIRE)) thread_exit(0u);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) {
			__atomic_store_n(&source->dead, true, __ATOMIC_RELEASE);
			(void)wake_manager(manager);
			thread_exit(1u);
		}
		__atomic_store_n(&source->dirty, true, __ATOMIC_RELEASE);
		if (!wake_manager(manager)) thread_exit(1u);
	}
}

static bool stop_source(struct managed_input_source* source) {
	bool success = true;
	__atomic_store_n(&source->stopping, true, __ATOMIC_RELEASE);
	if (source->waiter_cap != CAP_ID_INVALID) {
		(void)thread_cancel(source->waiter_cap);
		if (thread_join(source->waiter_cap, NULL) != SYSCALL_STATUS_OK) success = false;
		source->waiter_cap = CAP_ID_INVALID;
	}
	if (source->signal_cap != CAP_ID_INVALID) {
		if (cap_drop(source->signal_cap) != SYSCALL_STATUS_OK) success = false;
		source->signal_cap = CAP_ID_INVALID;
	}
	if (source->input_cap != CAP_ID_INVALID) {
		if (cap_drop(source->input_cap) != SYSCALL_STATUS_OK) success = false;
		source->input_cap = CAP_ID_INVALID;
	}
	free(source->descriptors);
	free(source->output_descriptors);
	source->descriptors             = NULL;
	source->descriptor_count        = 0u;
	source->output_descriptors      = NULL;
	source->output_descriptor_count = 0u;
	source->maximum_output_values   = 0u;
	source->pending_count           = 0u;
	source->pending_frame           = 0u;
	return success;
}

static bool link_quarantine(struct input_manager* manager, struct managed_input_source* source) {
	source->active            = false;
	source->quarantined       = true;
	source->seen              = true;
	source->identity_revision = __atomic_load_n(&manager->registry_revision, __ATOMIC_ACQUIRE);
	input_manager_lock(manager);
	source->next     = manager->sources;
	manager->sources = source;
	input_manager_unlock(manager);
	printf("input manager: quarantined malformed source %s\n", source->service.selector.service);
	return true;
}

static bool install_source(struct input_manager* manager, struct managed_input_source* source) {
	struct input_manager_event event;
	bool                       success;

	input_manager_lock(manager);
	if (manager->source_count >= INPUT_MANAGER_MAXIMUM_SOURCES || manager->next_source_id == 0u ||
	    manager->next_source_id == UINT64_MAX || manager->topology_generation == UINT64_MAX) {
		input_manager_unlock(manager);
		return false;
	}
	source->id                = manager->next_source_id++;
	source->active            = true;
	source->quarantined       = false;
	source->seen              = true;
	source->identity_revision = __atomic_load_n(&manager->registry_revision, __ATOMIC_ACQUIRE);
	source->next              = manager->sources;
	manager->sources          = source;
	manager->source_count++;
	manager->topology_generation++;
	event   = lifecycle_event(source, INPUT_MANAGER_EVENT_SOURCE_ADDED);
	success = append_frame_locked(manager, &event, 1u);
	if (!success) {
		manager->sources = source->next;
		manager->source_count--;
		manager->topology_generation--;
		manager->next_source_id--;
		source->next   = NULL;
		source->active = false;
	}
	input_manager_unlock(manager);
	if (!success || !notify_consumers(manager)) return false;
	printf("input manager: attached source %llu (%s), class=%u controls=%zu outputs=%zu\n",
	       (unsigned long long)source->id,
	       source->service.selector.service,
	       (unsigned)source->device_class,
	       source->descriptor_count,
	       source->output_descriptor_count);
	return true;
}

static bool deactivate_source(struct input_manager* manager, struct managed_input_source* source, bool quarantine) {
	bool success = true;
	if (source->active) {
		struct input_manager_event event = lifecycle_event(source, INPUT_MANAGER_EVENT_SOURCE_REMOVED);
		input_manager_lock(manager);
		if (manager->topology_generation == UINT64_MAX || manager->source_count == 0u) {
			success = false;
		}
		else {
			source->active = false;
			manager->source_count--;
			manager->topology_generation++;
			success = append_frame_locked(manager, &event, 1u);
		}
		input_manager_unlock(manager);
		if (success) success = notify_consumers(manager);
		printf("input manager: removed source %llu (%s)\n",
		       (unsigned long long)source->id,
		       source->service.selector.service);
	}
	if (!stop_source(source)) success = false;
	source->quarantined = quarantine;
	if (quarantine) {
		source->identity_revision = __atomic_load_n(&manager->registry_revision, __ATOMIC_ACQUIRE);
		__atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
		printf("input manager: quarantined source %s\n", source->service.selector.service);
	}
	return success;
}

static void unlink_source(struct input_manager* manager, struct managed_input_source* source) {
	input_manager_lock(manager);
	struct managed_input_source** cursor = &manager->sources;
	while (*cursor != NULL && *cursor != source) cursor = &(*cursor)->next;
	if (*cursor == source) *cursor = source->next;
	input_manager_unlock(manager);
	free(source);
}

static bool remove_source(struct input_manager* manager, struct managed_input_source* source) {
	bool success = deactivate_source(manager, source, false);
	unlink_source(manager, source);
	return success;
}

static bool descriptors_valid(const struct input_control_descriptor* descriptors, size_t count) {
	for (size_t i = 0u; i < count; i++) {
		if (!descriptor_valid(&descriptors[i])) return false;
		for (size_t previous = 0u; previous < i; previous++)
			if (descriptors[previous].id == descriptors[i].id) return false;
	}
	return true;
}

static bool output_descriptors_valid(const struct input_output_descriptor* descriptors, size_t count) {
	for (size_t i = 0u; i < count; i++) {
		if (!output_descriptor_valid(&descriptors[i])) return false;
		for (size_t previous = 0u; previous < i; previous++)
			if (descriptors[previous].id == descriptors[i].id) return false;
	}
	return true;
}

static enum attach_result quarantine_acquired_source(struct input_manager*        manager,
                                                     struct managed_input_source* source) {
	if (!stop_source(source)) {
		free(source);
		return ATTACH_RESULT_FAILED;
	}
	link_quarantine(manager, source);
	return ATTACH_RESULT_QUARANTINED;
}

static enum attach_result attach_source(struct input_manager* manager, const struct registry_service_info* service) {
	struct managed_input_source*   source = calloc(1u, sizeof(*source));
	struct registry_service_handle handle = {.capability = CAP_ID_INVALID};
	struct input_get_info_response info;
	struct registry_call_result    acquire;
	if (source == NULL) return ATTACH_RESULT_FAILED;
	source->service    = *service;
	source->owner      = PROCESS_PID_INVALID;
	source->input_cap  = CAP_ID_INVALID;
	source->signal_cap = CAP_ID_INVALID;
	source->waiter_cap = CAP_ID_INVALID;

	acquire = registry_acquire(&input_query, service->selector.service, &handle);
	if (acquire.transport_status != SYSCALL_STATUS_OK) {
		free(source);
		return ATTACH_RESULT_FAILED;
	}
	if (acquire.status == REGISTRY_STATUS_NOT_FOUND) {
		free(source);
		__atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
		return ATTACH_RESULT_RETRY;
	}
	if (acquire.status != REGISTRY_STATUS_OK) return quarantine_acquired_source(manager, source);
	source->owner     = handle.owner;
	source->input_cap = handle.capability;
	if (handle.capability == CAP_ID_INVALID || handle.owner == PROCESS_PID_INVALID ||
	    !service_equal(service, &handle.info) || handle.info.rights != INPUT_SERVICE_CLIENT_RIGHTS)
		return quarantine_acquired_source(manager, source);

	if (input_get_info(source->input_cap, &info) != SYSCALL_STATUS_OK || info.descriptor_count == 0u ||
	    info.descriptor_count > INPUT_MANAGER_MAXIMUM_DESCRIPTORS || info.queue_capacity == 0u ||
	    info.maximum_frame_events == 0u || info.maximum_frame_events > INPUT_MANAGER_MAXIMUM_FRAME_EVENTS ||
	    info.maximum_frame_events > info.queue_capacity ||
	    info.output_descriptor_count > INPUT_MANAGER_MAXIMUM_OUTPUT_DESCRIPTORS ||
	    info.maximum_output_values > INPUT_MANAGER_MAXIMUM_OUTPUT_VALUES ||
	    (info.output_descriptor_count == 0u) != (info.maximum_output_values == 0u) ||
	    info.maximum_output_values > info.output_descriptor_count)
		return quarantine_acquired_source(manager, source);
	source->device_class     = info.device_class;
	source->descriptor_count = info.descriptor_count;
	source->descriptors      = malloc(source->descriptor_count * sizeof(*source->descriptors));
	if (source->descriptors == NULL) {
		(void)stop_source(source);
		free(source);
		return ATTACH_RESULT_FAILED;
	}
	for (size_t offset = 0u; offset < source->descriptor_count;) {
		size_t capacity = source->descriptor_count - offset;
		if (capacity > INPUT_MANAGER_DESCRIPTOR_PAGE_SIZE) capacity = INPUT_MANAGER_DESCRIPTOR_PAGE_SIZE;
		size_t   returned = 0u;
		uint64_t total    = 0u;
		if (input_read_descriptors(
				source->input_cap, offset, source->descriptors + offset, capacity, &returned, &total) !=
		        SYSCALL_STATUS_OK ||
		    total != source->descriptor_count || returned == 0u || returned > capacity) {
			return quarantine_acquired_source(manager, source);
		}
		offset += returned;
	}
	if (!descriptors_valid(source->descriptors, source->descriptor_count))
		return quarantine_acquired_source(manager, source);
	source->output_descriptor_count = info.output_descriptor_count;
	source->maximum_output_values   = info.maximum_output_values;
	if (source->output_descriptor_count != 0u) {
		source->output_descriptors = malloc(source->output_descriptor_count * sizeof(*source->output_descriptors));
		if (source->output_descriptors == NULL) {
			(void)stop_source(source);
			free(source);
			return ATTACH_RESULT_FAILED;
		}
		for (size_t offset = 0u; offset < source->output_descriptor_count;) {
			size_t capacity = source->output_descriptor_count - offset;
			if (capacity > INPUT_MANAGER_DESCRIPTOR_PAGE_SIZE) capacity = INPUT_MANAGER_DESCRIPTOR_PAGE_SIZE;
			size_t   returned = 0u;
			uint64_t total    = 0u;
			if (input_read_output_descriptors(
					source->input_cap, offset, source->output_descriptors + offset, capacity, &returned, &total) !=
			        SYSCALL_STATUS_OK ||
			    total != source->output_descriptor_count || returned == 0u || returned > capacity)
				return quarantine_acquired_source(manager, source);
			offset += returned;
		}
		if (!output_descriptors_valid(source->output_descriptors, source->output_descriptor_count))
			return quarantine_acquired_source(manager, source);
	}
	if (input_acquire_event_signal(source->input_cap, &source->signal_cap) != SYSCALL_STATUS_OK ||
	    source->signal_cap == CAP_ID_INVALID)
		return quarantine_acquired_source(manager, source);

	const struct source_waiter_argument argument = {.manager = manager, .source = source};
	if (process_spawn_thread(manager->self_cap,
	                         (uintptr_t)source_waiter,
	                         &argument,
	                         sizeof(argument),
	                         "input source waiter",
	                         sizeof("input source waiter"),
	                         &source->waiter_cap) != SYSCALL_STATUS_OK) {
		(void)stop_source(source);
		free(source);
		return ATTACH_RESULT_FAILED;
	}
	__atomic_store_n(&source->dirty, true, __ATOMIC_RELEASE);
	if (!install_source(manager, source)) {
		(void)stop_source(source);
		free(source);
		return ATTACH_RESULT_FAILED;
	}
	return ATTACH_RESULT_ATTACHED;
}

static bool enumerate_sources(struct registry_service_info** out_services, size_t* out_count, bool* out_retry) {
	*out_services = NULL;
	*out_count    = 0u;
	*out_retry    = false;
	for (unsigned attempt = 0u; attempt < 3u; attempt++) {
		uint64_t                    total = 0u;
		uint64_t                    returned;
		struct registry_call_result result = registry_enumerate(&input_query, 0u, 0u, NULL, &returned, &total);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK ||
		    total > INPUT_MANAGER_MAXIMUM_SOURCES)
			return false;
		if (total == 0u) return true;
		struct registry_service_info* services = malloc((size_t)total * sizeof(*services));
		if (services == NULL) return false;
		uint64_t offset = 0u;
		bool     stable = true;
		while (offset < total) {
			uint64_t count = total - offset;
			if (count > INPUT_MANAGER_REGISTRY_PAGE_SIZE) count = INPUT_MANAGER_REGISTRY_PAGE_SIZE;
			uint64_t current_total = 0u;
			result =
				registry_enumerate(&input_query, offset, count, services + (size_t)offset, &returned, &current_total);
			if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK) {
				free(services);
				return false;
			}
			if (current_total != total || returned == 0u || returned > count) {
				stable = false;
				break;
			}
			offset += returned;
		}
		if (stable) {
			*out_services = services;
			*out_count    = (size_t)total;
			return true;
		}
		free(services);
	}
	*out_retry = true;
	return true;
}

static bool source_identity_is_same(struct input_manager* manager, struct managed_input_source* source,
                                    bool* out_same) {
	struct registry_service_handle handle = {.capability = CAP_ID_INVALID};
	struct registry_call_result    result = registry_acquire(&input_query, source->service.selector.service, &handle);
	*out_same                             = true;
	if (result.transport_status != SYSCALL_STATUS_OK) return false;
	if (result.status == REGISTRY_STATUS_NOT_FOUND) {
		*out_same = false;
		__atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
		return true;
	}
	if (result.status != REGISTRY_STATUS_OK) return true;
	*out_same = handle.owner == source->owner && service_equal(&handle.info, &source->service);
	if (handle.capability != CAP_ID_INVALID && cap_drop(handle.capability) != SYSCALL_STATUS_OK) return false;
	return true;
}

static bool reconcile_sources(struct input_manager* manager) {
	struct registry_service_info* services;
	size_t                        service_count;
	bool                          retry;
	uint64_t                      revision = __atomic_load_n(&manager->registry_revision, __ATOMIC_ACQUIRE);
	if (!enumerate_sources(&services, &service_count, &retry)) return false;
	if (retry) {
		__atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
		return true;
	}
	for (struct managed_input_source* source = manager->sources; source != NULL; source = source->next)
		source->seen = false;

	for (size_t i = 0u; i < service_count; i++) {
		struct managed_input_source* source = find_source(manager, &services[i].selector);
		if (source != NULL && !service_equal(&source->service, &services[i])) {
			if (!remove_source(manager, source)) {
				free(services);
				return false;
			}
			source = NULL;
		}
		if (source != NULL && source->identity_revision != revision) {
			bool same;
			if (!source_identity_is_same(manager, source, &same)) {
				free(services);
				return false;
			}
			source->identity_revision = revision;
			if (!same) {
				if (!remove_source(manager, source)) {
					free(services);
					return false;
				}
				source = NULL;
			}
		}
		if (source != NULL) {
			source->seen = true;
			continue;
		}
		enum attach_result result = attach_source(manager, &services[i]);
		if (result == ATTACH_RESULT_FAILED) {
			free(services);
			return false;
		}
	}
	free(services);

	struct managed_input_source* source = manager->sources;
	while (source != NULL) {
		struct managed_input_source* next = source->next;
		if (!source->seen && !remove_source(manager, source)) return false;
		source = next;
	}
	return true;
}

enum drain_result {
	DRAIN_RESULT_OK,
	DRAIN_RESULT_PROVIDER_FAILED,
	DRAIN_RESULT_MANAGER_FAILED,
};

static enum drain_result append_source_reset(struct input_manager* manager, const struct managed_input_source* source) {
	if (!append_lifecycle_frame(manager, source, INPUT_MANAGER_EVENT_SOURCE_RESET)) return DRAIN_RESULT_MANAGER_FAILED;
	printf("input manager: source %llu state reset\n", (unsigned long long)source->id);
	return DRAIN_RESULT_OK;
}

static enum drain_result append_source_controls(struct input_manager* manager, struct managed_input_source* source) {
	input_manager_lock(manager);
	bool success = append_frame_locked(manager, source->pending, source->pending_count);
	input_manager_unlock(manager);
	if (!success || !notify_consumers(manager)) return DRAIN_RESULT_MANAGER_FAILED;
	source->pending_count = 0u;
	source->pending_frame = 0u;
	return DRAIN_RESULT_OK;
}

static enum drain_result drain_source(struct input_manager* manager, struct managed_input_source* source) {
	struct input_event events[INPUT_MANAGER_MAXIMUM_FRAME_EVENTS];
	for (;;) {
		struct input_event_page page;
		syscall_status_t        status =
			input_read_events(source->input_cap, source->cursor, events, INPUT_MANAGER_MAXIMUM_FRAME_EVENTS, &page);
		if (status == SYSCALL_STATUS_UNAVAILABLE)
			return source->pending_count == 0u ? DRAIN_RESULT_OK : DRAIN_RESULT_PROVIDER_FAILED;
		if (status != SYSCALL_STATUS_OK) return DRAIN_RESULT_PROVIDER_FAILED;
		bool gap = (page.flags & INPUT_READ_EVENT_FLAG_GAP) != 0u;
		if (!gap && events[0].sequence != source->cursor + 1u) return DRAIN_RESULT_PROVIDER_FAILED;
		if (gap && events[0].sequence <= source->cursor + 1u) return DRAIN_RESULT_PROVIDER_FAILED;
		if (gap) {
			source->pending_count    = 0u;
			source->pending_frame    = 0u;
			source->last_frame       = 0u;
			enum drain_result result = append_source_reset(manager, source);
			if (result != DRAIN_RESULT_OK) return result;
		}
		for (size_t i = 0u; i < page.returned; i++) {
			const struct input_event* event = &events[i];
			if (i != 0u && event->sequence != source->cursor + 1u) return DRAIN_RESULT_PROVIDER_FAILED;
			source->cursor = event->sequence;
			if (event->type == INPUT_EVENT_TYPE_STATE_RESET) {
				if (source->pending_count != 0u || (event->flags & INPUT_EVENT_FLAG_FRAME_END) == 0u ||
				    event->frame == source->last_frame)
					return DRAIN_RESULT_PROVIDER_FAILED;
				source->last_frame       = event->frame;
				enum drain_result result = append_source_reset(manager, source);
				if (result != DRAIN_RESULT_OK) return result;
				continue;
			}
			const struct input_control_descriptor* descriptor = find_descriptor(source, event->control_id);
			if (descriptor == NULL || !value_valid(descriptor, event->value) ||
			    source->pending_count >= INPUT_MANAGER_MAXIMUM_FRAME_EVENTS)
				return DRAIN_RESULT_PROVIDER_FAILED;
			if (source->pending_count == 0u) {
				if (event->frame == source->last_frame) return DRAIN_RESULT_PROVIDER_FAILED;
				source->pending_frame = event->frame;
			}
			else if (source->pending_frame != event->frame) return DRAIN_RESULT_PROVIDER_FAILED;
			source->pending[source->pending_count++] = (struct input_manager_event){
				.source_id       = source->id,
				.type            = INPUT_MANAGER_EVENT_CONTROL,
				.device_class    = source->device_class,
				.control_id      = descriptor->id,
				.control_kind    = descriptor->kind,
				.control_code    = descriptor->code,
				.group_id        = descriptor->group_id,
				.value           = event->value,
				.logical_minimum = descriptor->logical_minimum,
				.logical_maximum = descriptor->logical_maximum,
			};
			if ((event->flags & INPUT_EVENT_FLAG_FRAME_END) != 0u) {
				source->last_frame       = event->frame;
				enum drain_result result = append_source_controls(manager, source);
				if (result != DRAIN_RESULT_OK) return result;
			}
		}
		if (source->cursor == page.newest_sequence) {
			return source->pending_count == 0u ? DRAIN_RESULT_OK : DRAIN_RESULT_PROVIDER_FAILED;
		}
	}
}

static bool process_dirty_sources(struct input_manager* manager) {
	for (struct managed_input_source* source = manager->sources; source != NULL; source = source->next) {
		if (!source->active) continue;
		if (__atomic_load_n(&source->dead, __ATOMIC_ACQUIRE)) {
			if (!deactivate_source(manager, source, true)) return false;
			continue;
		}
		if (!__atomic_exchange_n(&source->dirty, false, __ATOMIC_ACQ_REL)) continue;
		enum drain_result result = drain_source(manager, source);
		if (result == DRAIN_RESULT_MANAGER_FAILED) return false;
		if (result == DRAIN_RESULT_PROVIDER_FAILED && !deactivate_source(manager, source, true)) return false;
	}
	return true;
}

static void initialize_invalid_capabilities(struct input_manager* manager) {
	manager->self_cap             = CAP_ID_INVALID;
	manager->channel_activity_cap = CAP_ID_INVALID;
	manager->service_cap          = CAP_ID_INVALID;
	manager->event_signal_cap     = CAP_ID_INVALID;
	manager->activity_cap         = CAP_ID_INVALID;
	manager->registry_signal_cap  = CAP_ID_INVALID;
	manager->endpoint             = CHANNEL_ID_INVALID;
}

bool input_manager_initialize(struct input_manager* manager) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	cap_id_t registry_thread_cap = CAP_ID_INVALID;
	if (manager == NULL) return false;
	memset(manager, 0, sizeof(*manager));
	initialize_invalid_capabilities(manager);
	manager->topology_generation = 1u;
	manager->next_source_id      = 1u;
	manager->next_sequence       = 1u;
	manager->next_frame          = 1u;
	manager->registry_revision   = 1u;
	manager->events              = calloc(INPUT_MANAGER_EVENT_QUEUE_CAPACITY, sizeof(*manager->events));
	if (manager->events == NULL) return false;

	syscall_status_t status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK && self.pid == PROCESS_PID_INVALID) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		manager->pid      = self.pid;
		manager->self_cap = self.self_cap;
		self.self_cap     = CAP_ID_INVALID;
		status            = signal_create(&manager->event_signal_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = signal_create(&manager->activity_cap);
	if (status == SYSCALL_STATUS_OK) status = signal_create(&manager->registry_signal_cap);
	if (status == SYSCALL_STATUS_OK) status = channel_create(&manager->endpoint, &manager->channel_activity_cap);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(manager->endpoint,
		                     INPUT_MANAGER_OBJECT_ID,
		                     manager->pid,
		                     CAP_CALL | CAP_READ | CAP_WRITE | CAP_DELEGATE,
		                     &manager->service_cap);
	if (status == SYSCALL_STATUS_OK && !input_manager_server_start(manager)) status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) {
		struct registry_call_result result = registry_advertise(
			&manager_selector, INPUT_MANAGER_PROTOCOL_VERSION_MINOR, manager->service_cap, INPUT_MANAGER_CLIENT_RIGHTS);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK)
			status = SYSCALL_STATUS_FAILED;
	}
	if (status == SYSCALL_STATUS_OK) {
		uint64_t                    counter;
		struct registry_call_result result =
			registry_watch(&input_query, manager->registry_signal_cap, &manager->registry_subscription, &counter);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK || counter == 0u)
			status = SYSCALL_STATUS_FAILED;
		else __atomic_store_n(&manager->registry_dirty, true, __ATOMIC_RELEASE);
	}
	if (status == SYSCALL_STATUS_OK) {
		struct input_manager* argument = manager;
		status                         = process_spawn_thread(manager->self_cap,
		                                                      (uintptr_t)registry_waiter,
		                                                      &argument,
		                                                      sizeof(argument),
		                                                      "input registry waiter",
		                                                      sizeof("input registry waiter"),
		                                                      &registry_thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(registry_thread_cap);
	if (registry_thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(registry_thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	printf("input manager: advertised %s/%s@%u.%u/%s\n",
	       INPUT_MANAGER_NAMESPACE,
	       INPUT_MANAGER_PROTOCOL_NAME,
	       INPUT_MANAGER_PROTOCOL_VERSION_MAJOR,
	       INPUT_MANAGER_PROTOCOL_VERSION_MINOR,
	       INPUT_MANAGER_SERVICE_NAME);
	return true;
}

int input_manager_run(struct input_manager* manager) {
	if (manager == NULL) return 1;
	for (;;) {
		if (__atomic_load_n(&manager->registry_failed, __ATOMIC_ACQUIRE)) return 1;
		if (__atomic_exchange_n(&manager->registry_dirty, false, __ATOMIC_ACQ_REL) && !reconcile_sources(manager))
			return 1;
		if (!process_dirty_sources(manager)) return 1;
		if (__atomic_load_n(&manager->registry_dirty, __ATOMIC_ACQUIRE)) continue;
		bool source_dirty = false;
		for (struct managed_input_source* source = manager->sources; source != NULL; source = source->next) {
			if (source->active && (__atomic_load_n(&source->dirty, __ATOMIC_ACQUIRE) ||
			                       __atomic_load_n(&source->dead, __ATOMIC_ACQUIRE))) {
				source_dirty = true;
				break;
			}
		}
		if (source_dirty) continue;
		struct signal_message activity;
		syscall_status_t      status = signal_wait(manager->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return 1;
	}
}
