#include <base/cap.h>
#include <base/device.h>
#include <base/math.h>
#include <kernel/device.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct kernel_device_type_entry {
	enum kernel_device_type          type;
	size_t                           element_size;
	size_t                           count;
	size_t                           capacity;
	uint8_t*                         entries;
	struct kernel_device_type_entry* next;
};

static struct kernel_device_type_entry* device_types;
static struct kernel_device_type_entry* device_types_tail;
static bool                             device_inventory_frozen;

static struct kernel_device_type_entry* device_type_find(enum kernel_device_type type) {
	for (struct kernel_device_type_entry* entry = device_types; entry != NULL; entry = entry->next) {
		if (entry->type == type) return entry;
	}
	return NULL;
}

bool kernel_device_register_type(enum kernel_device_type type, size_t element_size) {
	struct kernel_device_type_entry* entry;

	if (device_inventory_frozen || type == KERNEL_DEVICE_TYPE_INVALID || element_size == 0u ||
	    element_size > CAP_MAX_RESPONSE_SIZE - sizeof(struct kernel_devices_list_response) ||
	    device_type_find(type) != NULL)
		return false;
	entry = calloc(1u, sizeof(*entry));
	if (entry == NULL) return false;
	entry->type         = type;
	entry->element_size = element_size;
	if (device_types_tail == NULL) device_types = entry;
	else device_types_tail->next = entry;
	device_types_tail = entry;
	return true;
}

bool kernel_device_register(enum kernel_device_type type, const void* descriptor, size_t descriptor_size) {
	struct kernel_device_type_entry* entry;
	size_t                           new_capacity;
	size_t                           allocation_size;
	uint8_t*                         resized;

	if (device_inventory_frozen || descriptor == NULL) return false;
	entry = device_type_find(type);
	if (entry == NULL || descriptor_size != entry->element_size) return false;
	if (entry->count == entry->capacity) {
		new_capacity = entry->capacity == 0u ? 4u : entry->capacity * 2u;
		if (new_capacity < entry->capacity || mul_overflow_size(new_capacity, entry->element_size, &allocation_size))
			return false;
		resized = realloc(entry->entries, allocation_size);
		if (resized == NULL) return false;
		entry->entries  = resized;
		entry->capacity = new_capacity;
	}
	memcpy(entry->entries + entry->count * entry->element_size, descriptor, entry->element_size);
	entry->count++;
	return true;
}

void kernel_device_freeze(void) {
	device_inventory_frozen = true;
}

bool kernel_device_type_size(enum kernel_device_type type, size_t* out_element_size) {
	struct kernel_device_type_entry* entry;

	if (type == KERNEL_DEVICE_TYPE_INVALID || out_element_size == NULL) return false;
	entry = device_type_find(type);
	if (entry == NULL) return false;
	*out_element_size = entry->element_size;
	return true;
}

bool kernel_device_count(enum kernel_device_type type, size_t* out_count) {
	struct kernel_device_type_entry* entry;

	if (type == KERNEL_DEVICE_TYPE_INVALID || out_count == NULL) return false;
	entry = device_type_find(type);
	if (entry == NULL) return false;
	*out_count = entry->count;
	return true;
}

bool kernel_device_list(enum kernel_device_type type, size_t offset, size_t length, size_t element_size,
                        void* out_entries, size_t* out_returned) {
	struct kernel_device_type_entry* entry;
	size_t                           returned;
	size_t                           source_offset;
	size_t                           copy_size;

	if (!device_inventory_frozen || type == KERNEL_DEVICE_TYPE_INVALID || out_returned == NULL ||
	    (length != 0u && out_entries == NULL))
		return false;
	entry = device_type_find(type);
	if (entry == NULL || element_size != entry->element_size) return false;
	returned = offset < entry->count ? entry->count - offset : 0u;
	if (returned > length) returned = length;
	if (returned != 0u) {
		if (mul_overflow_size(offset, element_size, &source_offset) ||
		    mul_overflow_size(returned, element_size, &copy_size))
			return false;
		memcpy(out_entries, entry->entries + source_offset, copy_size);
	}
	*out_returned = returned;
	return true;
}

#if defined(KERNEL_DEVICE_TEST)
void kernel_device_reset_for_test(void) {
	struct kernel_device_type_entry* entry = device_types;

	while (entry != NULL) {
		struct kernel_device_type_entry* next = entry->next;
		free(entry->entries);
		free(entry);
		entry = next;
	}
	device_types            = NULL;
	device_types_tail       = NULL;
	device_inventory_frozen = false;
}
#endif
