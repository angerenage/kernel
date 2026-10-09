#include "device.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

static bool add_size(size_t left, size_t right, size_t* out) {
	if (right > SIZE_MAX - left) return false;
	*out = left + right;
	return true;
}

static bool reserve(struct dm_builder* builder, size_t size) {
	size_t total;
	if (!add_size(builder->accounted_size, size, &total) || total > DEVICE_MAX_STATE_SIZE) return false;
	builder->accounted_size = total;
	return true;
}

static bool string_equal(const struct dm_string* string, const void* data, size_t size) {
	return string->size == size && (size == 0u || memcmp(string->data, data, size) == 0);
}

static bool string_copy(struct dm_string* string, const void* data, size_t size) {
	string->data = malloc(size);
	if (string->data == NULL && size != 0u) return false;
	if (size != 0u) memcpy(string->data, data, size);
	string->size = size;
	return true;
}

bool dm_identifier_valid(const void* name, size_t size) {
	const uint8_t* bytes = name;
	if (bytes == NULL || size == 0u || size > DEVICE_IDENTIFIER_MAX || bytes[0] < 'a' || bytes[0] > 'z') return false;
	for (size_t index = 1u; index < size; index++) {
		uint8_t byte = bytes[index];
		if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '_') return false;
	}
	return true;
}

bool dm_utf8_valid(const void* value, size_t size) {
	const uint8_t* bytes = value;
	if (bytes == NULL && size != 0u) return false;
	for (size_t index = 0u; index < size;) {
		uint8_t  first = bytes[index++];
		uint32_t codepoint;
		size_t   trailing;
		if (first == 0u) return false;
		if (first < 0x80u) continue;
		if (first >= 0xc2u && first <= 0xdfu) {
			codepoint = first & 0x1fu;
			trailing  = 1u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			codepoint = first & 0x0fu;
			trailing  = 2u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			codepoint = first & 0x07u;
			trailing  = 3u;
		}
		else return false;
		if (trailing > size - index) return false;
		for (size_t part = 0u; part < trailing; part++) {
			uint8_t byte = bytes[index++];
			if ((byte & 0xc0u) != 0x80u) return false;
			codepoint = (codepoint << 6u) | (byte & 0x3fu);
		}
		if ((trailing == 1u && codepoint < 0x80u) || (trailing == 2u && codepoint < 0x800u) ||
		    (trailing == 3u && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
		    (codepoint >= 0xd800u && codepoint <= 0xdfffu))
			return false;
	}
	return true;
}

static void free_properties(struct dm_property* property) {
	while (property != NULL) {
		struct dm_property* next = property->next;
		free(property->name.data);
		free(property->value);
		free(property);
		property = next;
	}
}

static void free_resources(struct dm_resource* resource) {
	while (resource != NULL) {
		struct dm_resource* next = resource->next;
		if (resource->capability != CAP_ID_INVALID) (void)cap_drop(resource->capability);
		free(resource->name.data);
		free(resource);
		resource = next;
	}
}

static void free_strings(struct dm_string* strings, size_t count) {
	for (size_t index = 0u; index < count; index++) free(strings[index].data);
	free(strings);
}

static void release_builder(struct dm_builder* builder) {
	if (builder == NULL) return;
	free(builder->name.data);
	free_strings(builder->compatible_ids, builder->compatible_count);
	free_properties(builder->properties);
	free_resources(builder->resources);
	free(builder);
}

static void release_device(struct dm_device* device) {
	if (device == NULL) return;
	free(device->name.data);
	free_strings(device->compatible_ids, device->compatible_count);
	free_properties(device->properties);
	free_resources(device->resources);
	free(device);
}

void dm_state_init(struct dm_state* state) {
	*state = (struct dm_state){.next_device_id = 1u, .next_builder_id = 1u};
}

void dm_state_deinit(struct dm_state* state) {
	while (state->builders != NULL) {
		struct dm_builder* next = state->builders->next;
		release_builder(state->builders);
		state->builders = next;
	}
	while (state->devices != NULL) {
		struct dm_device* next = state->devices->next;
		release_device(state->devices);
		state->devices = next;
	}
	*state = (struct dm_state){0};
}

struct dm_device* dm_state_find_device(const struct dm_state* state, uint64_t id) {
	for (struct dm_device* device = state->devices; device != NULL; device = device->next)
		if (device->id == id) return device;
	return NULL;
}

struct dm_builder* dm_state_find_builder(const struct dm_state* state, uint64_t object_id) {
	for (struct dm_builder* builder = state->builders; builder != NULL; builder = builder->next)
		if (builder->object_id == object_id) return builder;
	return NULL;
}

syscall_status_t dm_builder_begin(struct dm_state* state, struct dm_device* parent, struct dm_builder** out_builder) {
	struct dm_builder* builder;
	uint64_t           id;
	if (state == NULL || out_builder == NULL || state->next_builder_id >= DEVICE_BUILDER_OBJECT_BIT - 1u)
		return SYSCALL_STATUS_FAILED;
	builder = calloc(1u, sizeof(*builder));
	if (builder == NULL) return SYSCALL_STATUS_FAILED;
	id                      = state->next_builder_id++;
	builder->object_id      = DEVICE_BUILDER_OBJECT_BIT | id;
	builder->parent         = parent;
	builder->accounted_size = sizeof(*builder);
	builder->next           = state->builders;
	state->builders         = builder;
	*out_builder            = builder;
	return SYSCALL_STATUS_OK;
}

syscall_status_t dm_builder_set_name(struct dm_builder* builder, const void* name, size_t name_size) {
	if (builder == NULL || builder->name.data != NULL || name_size == 0u || name_size > DEVICE_NAME_MAX ||
	    !dm_utf8_valid(name, name_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (!reserve(builder, name_size)) return SYSCALL_STATUS_UNAVAILABLE;
	if (!string_copy(&builder->name, name, name_size)) {
		builder->accounted_size -= name_size;
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t dm_builder_add_compatible(struct dm_builder* builder, const void* id, size_t id_size) {
	struct dm_string* ids;
	if (builder == NULL || id_size == 0u || id_size > DEVICE_COMPATIBLE_ID_MAX || !dm_utf8_valid(id, id_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (builder->compatible_count >= DEVICE_MAX_COMPATIBLE_IDS) return SYSCALL_STATUS_UNAVAILABLE;
	for (size_t index = 0u; index < builder->compatible_count; index++)
		if (string_equal(&builder->compatible_ids[index], id, id_size)) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (!reserve(builder, sizeof(*ids) + id_size)) return SYSCALL_STATUS_UNAVAILABLE;
	ids = realloc(builder->compatible_ids, (builder->compatible_count + 1u) * sizeof(*ids));
	if (ids == NULL) {
		builder->accounted_size -= sizeof(*ids) + id_size;
		return SYSCALL_STATUS_FAILED;
	}
	builder->compatible_ids        = ids;
	ids[builder->compatible_count] = (struct dm_string){0};
	if (!string_copy(&ids[builder->compatible_count], id, id_size)) {
		builder->accounted_size -= sizeof(*ids) + id_size;
		return SYSCALL_STATUS_FAILED;
	}
	builder->compatible_count++;
	return SYSCALL_STATUS_OK;
}

const struct dm_property* dm_device_find_property(const struct dm_device* device, const void* name, size_t name_size) {
	for (const struct dm_property* property = device->properties; property != NULL; property = property->next)
		if (string_equal(&property->name, name, name_size)) return property;
	return NULL;
}

static struct dm_property* builder_find_property(const struct dm_builder* builder, const void* name, size_t size) {
	for (struct dm_property* property = builder->properties; property != NULL; property = property->next)
		if (string_equal(&property->name, name, size)) return property;
	return NULL;
}

static bool property_shape_valid(enum device_property_type type, uint64_t count, uint64_t size) {
	switch (type) {
	case DEVICE_PROPERTY_BOOLEAN:
		return count == 1u && size == 1u;
	case DEVICE_PROPERTY_UNSIGNED:
	case DEVICE_PROPERTY_SIGNED:
		return count == 1u && size == 8u;
	case DEVICE_PROPERTY_UTF8_STRING:
		return count == 1u;
	case DEVICE_PROPERTY_BYTES:
		return count == size;
	case DEVICE_PROPERTY_BOOLEAN_ARRAY:
		return count == size;
	case DEVICE_PROPERTY_UNSIGNED_ARRAY:
	case DEVICE_PROPERTY_SIGNED_ARRAY:
		return count <= UINT64_MAX / 8u && size == count * 8u;
	case DEVICE_PROPERTY_UTF8_STRING_ARRAY:
		return count <= size / 4u;
	default:
		return false;
	}
}

syscall_status_t dm_builder_begin_property(struct dm_builder* builder, const void* name, size_t name_size,
                                           enum device_property_type type, uint64_t element_count,
                                           uint64_t value_size) {
	struct dm_property* property;
	if (builder == NULL || builder->active_property != NULL || !dm_identifier_valid(name, name_size) ||
	    value_size > SIZE_MAX || !property_shape_valid(type, element_count, value_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (builder->property_count >= DEVICE_MAX_PROPERTIES) return SYSCALL_STATUS_UNAVAILABLE;
	if (builder_find_property(builder, name, name_size) != NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (!reserve(builder, sizeof(*property) + name_size + (size_t)value_size)) return SYSCALL_STATUS_UNAVAILABLE;
	property = calloc(1u, sizeof(*property));
	if (property == NULL) {
		builder->accounted_size -= sizeof(*property) + name_size + (size_t)value_size;
		return SYSCALL_STATUS_FAILED;
	}
	if (!string_copy(&property->name, name, name_size)) {
		builder->accounted_size -= sizeof(*property) + name_size + (size_t)value_size;
		free(property);
		return SYSCALL_STATUS_FAILED;
	}
	property->value = malloc((size_t)value_size);
	if (property->value == NULL && value_size != 0u) {
		builder->accounted_size -= sizeof(*property) + name_size + (size_t)value_size;
		free(property->name.data);
		free(property);
		return SYSCALL_STATUS_FAILED;
	}
	property->type          = type;
	property->element_count = element_count;
	property->value_size    = (size_t)value_size;
	if (builder->properties_tail == NULL) builder->properties = property;
	else builder->properties_tail->next = property;
	builder->properties_tail = property;
	builder->property_count++;
	builder->active_property         = property;
	builder->active_property_written = 0u;
	return SYSCALL_STATUS_OK;
}

syscall_status_t dm_builder_append_property(struct dm_builder* builder, uint64_t offset, const void* bytes,
                                            size_t size) {
	if (builder == NULL || builder->active_property == NULL || offset != builder->active_property_written ||
	    size > builder->active_property->value_size - builder->active_property_written || (size != 0u && bytes == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (size != 0u) memcpy(builder->active_property->value + builder->active_property_written, bytes, size);
	builder->active_property_written += size;
	return SYSCALL_STATUS_OK;
}

static uint32_t read_u32_le(const uint8_t* bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static bool property_value_valid(const struct dm_property* property) {
	if (property->type == DEVICE_PROPERTY_BOOLEAN) return property->value[0] <= 1u;
	if (property->type == DEVICE_PROPERTY_BOOLEAN_ARRAY) {
		for (size_t index = 0u; index < property->value_size; index++)
			if (property->value[index] > 1u) return false;
		return true;
	}
	if (property->type == DEVICE_PROPERTY_UTF8_STRING) return dm_utf8_valid(property->value, property->value_size);
	if (property->type != DEVICE_PROPERTY_UTF8_STRING_ARRAY) return true;
	size_t offset = 0u;
	for (uint64_t index = 0u; index < property->element_count; index++) {
		if (property->value_size - offset < 4u) return false;
		uint32_t length = read_u32_le(property->value + offset);
		offset += 4u;
		if (length > property->value_size - offset || !dm_utf8_valid(property->value + offset, length)) return false;
		offset += length;
	}
	return offset == property->value_size;
}

syscall_status_t dm_builder_finish_property(struct dm_builder* builder) {
	if (builder == NULL || builder->active_property == NULL ||
	    builder->active_property_written != builder->active_property->value_size)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (!property_value_valid(builder->active_property)) return SYSCALL_STATUS_BAD_ARGUMENT;
	builder->active_property         = NULL;
	builder->active_property_written = 0u;
	return SYSCALL_STATUS_OK;
}

const struct dm_resource* dm_device_find_resource(const struct dm_device* device, const void* name, size_t name_size) {
	for (const struct dm_resource* resource = device->resources; resource != NULL; resource = resource->next)
		if (string_equal(&resource->name, name, name_size)) return resource;
	return NULL;
}

static struct dm_resource* builder_find_resource(const struct dm_builder* builder, const void* name, size_t size) {
	for (struct dm_resource* resource = builder->resources; resource != NULL; resource = resource->next)
		if (string_equal(&resource->name, name, size)) return resource;
	return NULL;
}

syscall_status_t dm_builder_add_resource(struct dm_builder* builder, const void* name, size_t name_size,
                                         cap_id_t capability, cap_rights_t rights) {
	struct dm_resource* resource;
	if (builder == NULL || !dm_identifier_valid(name, name_size) || capability == CAP_ID_INVALID || rights == 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (builder->resource_count >= DEVICE_MAX_RESOURCES) return SYSCALL_STATUS_UNAVAILABLE;
	if (builder_find_resource(builder, name, name_size) != NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (!reserve(builder, sizeof(*resource) + name_size)) return SYSCALL_STATUS_UNAVAILABLE;
	resource = calloc(1u, sizeof(*resource));
	if (resource == NULL) {
		builder->accounted_size -= sizeof(*resource) + name_size;
		return SYSCALL_STATUS_FAILED;
	}
	if (!string_copy(&resource->name, name, name_size)) {
		builder->accounted_size -= sizeof(*resource) + name_size;
		free(resource);
		return SYSCALL_STATUS_FAILED;
	}
	resource->capability = capability;
	resource->rights     = rights;
	if (builder->resources_tail == NULL) builder->resources = resource;
	else builder->resources_tail->next = resource;
	builder->resources_tail = resource;
	builder->resource_count++;
	return SYSCALL_STATUS_OK;
}

static void unlink_builder(struct dm_state* state, struct dm_builder* builder) {
	struct dm_builder** cursor = &state->builders;
	while (*cursor != NULL) {
		if (*cursor == builder) {
			*cursor       = builder->next;
			builder->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

syscall_status_t dm_builder_validate_commit(const struct dm_builder* builder) {
	if (builder == NULL || builder->active_property != NULL || builder->compatible_count == 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	return SYSCALL_STATUS_OK;
}

syscall_status_t dm_builder_commit(struct dm_state* state, struct dm_builder* builder, struct dm_device** out_device) {
	struct dm_device* device;
	if (state == NULL || out_device == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t validation = dm_builder_validate_commit(builder);
	if (validation != SYSCALL_STATUS_OK) return validation;
	if (state->next_device_id >= DEVICE_BUILDER_OBJECT_BIT) return SYSCALL_STATUS_FAILED;
	device = calloc(1u, sizeof(*device));
	if (device == NULL) return SYSCALL_STATUS_FAILED;
	device->id               = state->next_device_id++;
	device->parent           = builder->parent;
	device->name             = builder->name;
	device->compatible_ids   = builder->compatible_ids;
	device->compatible_count = builder->compatible_count;
	device->properties       = builder->properties;
	device->properties_tail  = builder->properties_tail;
	device->property_count   = builder->property_count;
	device->resources        = builder->resources;
	device->resources_tail   = builder->resources_tail;
	device->resource_count   = builder->resource_count;
	if (device->parent != NULL) {
		if (device->parent->last_child == NULL) device->parent->first_child = device;
		else device->parent->last_child->next_sibling = device;
		device->parent->last_child = device;
	}
	if (state->devices_tail == NULL) state->devices = device;
	else state->devices_tail->next = device;
	state->devices_tail = device;
	unlink_builder(state, builder);
	builder->name             = (struct dm_string){0};
	builder->compatible_ids   = NULL;
	builder->compatible_count = 0u;
	builder->properties       = NULL;
	builder->resources        = NULL;
	release_builder(builder);
	*out_device = device;
	return SYSCALL_STATUS_OK;
}

void dm_builder_abort(struct dm_state* state, struct dm_builder* builder) {
	if (state == NULL || builder == NULL) return;
	unlink_builder(state, builder);
	release_builder(builder);
}

void dm_device_rollback(struct dm_state* state, struct dm_device* device) {
	struct dm_device** cursor;
	if (state == NULL || device == NULL || device->first_child != NULL) return;
	if (device->parent != NULL) {
		struct dm_device** child = &device->parent->first_child;
		while (*child != NULL) {
			if (*child == device) {
				*child = device->next_sibling;
				break;
			}
			child = &(*child)->next_sibling;
		}
		device->parent->last_child = NULL;
		for (struct dm_device* item = device->parent->first_child; item != NULL; item = item->next_sibling)
			device->parent->last_child = item;
	}
	cursor = &state->devices;
	while (*cursor != NULL) {
		if (*cursor == device) {
			*cursor = device->next;
			break;
		}
		cursor = &(*cursor)->next;
	}
	state->devices_tail = NULL;
	for (struct dm_device* item = state->devices; item != NULL; item = item->next) state->devices_tail = item;
	release_device(device);
}
