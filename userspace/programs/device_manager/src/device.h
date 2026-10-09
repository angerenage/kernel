#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_ROOT_OBJECT_ID UINT64_MAX
#define DEVICE_BUILDER_OBJECT_BIT (1ull << 63)

/* Owned length-delimited byte string used by internal immutable state. */
struct dm_string {
	char*  data;
	size_t size;
};

/* One immutable named property in provider insertion order. */
struct dm_property {
	struct dm_string          name;
	enum device_property_type type;
	uint64_t                  element_count;
	size_t                    value_size;
	uint8_t*                  value;
	struct dm_property*       next;
};

/* One manager-owned named resource and its maximum driver rights. */
struct dm_resource {
	struct dm_string    name;
	cap_id_t            capability;
	cap_rights_t        rights;
	struct dm_resource* next;
};

/* One committed immutable device and its fixed parent/child links. */
struct dm_device {
	uint64_t            id;          /* Private, monotonically assigned manager ID. */
	cap_id_t            manager_cap; /* Manager-owned source for future driver grants. */
	struct dm_device*   parent;
	struct dm_device*   first_child;
	struct dm_device*   last_child;
	struct dm_device*   next_sibling;
	struct dm_device*   next;
	struct dm_string    name;
	struct dm_string*   compatible_ids;
	size_t              compatible_count;
	struct dm_property* properties;
	struct dm_property* properties_tail;
	size_t              property_count;
	struct dm_resource* resources;
	struct dm_resource* resources_tail;
	size_t              resource_count;
};

/* Mutable state retained only for the lifetime of one construction transaction. */
struct dm_builder {
	uint64_t            object_id;
	struct dm_device*   parent;
	struct dm_string    name;
	struct dm_string*   compatible_ids;
	size_t              compatible_count;
	struct dm_property* properties;
	struct dm_property* properties_tail;
	size_t              property_count;
	struct dm_property* active_property;
	size_t              active_property_written;
	struct dm_resource* resources;
	struct dm_resource* resources_tail;
	size_t              resource_count;
	size_t              accounted_size;
	struct dm_builder*  next;
};

/* Complete independently testable device tree and in-progress builder state. */
struct dm_state {
	uint64_t           next_device_id;
	uint64_t           next_builder_id;
	struct dm_device*  devices;
	struct dm_device*  devices_tail;
	struct dm_builder* builders;
};

/* Initialize an empty state with monotonic ID sequences beginning at one. */
void dm_state_init(struct dm_state* state);

/* Release all builders, devices, metadata, and retained resource capabilities. */
void dm_state_deinit(struct dm_state* state);

/* Find one committed device by its private manager ID. */
struct dm_device* dm_state_find_device(const struct dm_state* state, uint64_t id);

/* Find one unfinished builder by its channel object ID. */
struct dm_builder* dm_state_find_builder(const struct dm_state* state, uint64_t object_id);

/* Allocate and link a root or direct-child builder. */
syscall_status_t dm_builder_begin(struct dm_state* state, struct dm_device* parent, struct dm_builder** out_builder);

/* Set a builder's optional UTF-8 diagnostic name. */
syscall_status_t dm_builder_set_name(struct dm_builder* builder, const void* name, size_t name_size);

/* Append one unique UTF-8 compatible ID in provider order. */
syscall_status_t dm_builder_add_compatible(struct dm_builder* builder, const void* id, size_t id_size);

/* Allocate one property and make it the builder's active property. */
syscall_status_t dm_builder_begin_property(struct dm_builder* builder, const void* name, size_t name_size,
                                           enum device_property_type type, uint64_t element_count, uint64_t value_size);

/* Append the next contiguous chunk to the active property. */
syscall_status_t dm_builder_append_property(struct dm_builder* builder, uint64_t offset, const void* bytes,
                                            size_t size);

/* Validate the completed encoding and close the active property. */
syscall_status_t dm_builder_finish_property(struct dm_builder* builder);

/* Transfer one delegated named resource into builder ownership. */
syscall_status_t dm_builder_add_resource(struct dm_builder* builder, const void* name, size_t name_size,
                                         cap_id_t capability, cap_rights_t rights);

/* Check whether a builder has the minimum complete state required by commit. */
syscall_status_t dm_builder_validate_commit(const struct dm_builder* builder);

/* Consume a valid builder and atomically link its new immutable device. */
syscall_status_t dm_builder_commit(struct dm_state* state, struct dm_builder* builder, struct dm_device** out_device);

/* Unlink and release one unfinished builder and its retained resources. */
void dm_builder_abort(struct dm_state* state, struct dm_builder* builder);

/* Remove a newly committed leaf when the surrounding capability transaction fails. */
void dm_device_rollback(struct dm_state* state, struct dm_device* device);

/* Find an immutable property by its exact lower-snake-case name. */
const struct dm_property* dm_device_find_property(const struct dm_device* device, const void* name, size_t name_size);

/* Find an immutable resource by its exact lower-snake-case name. */
const struct dm_resource* dm_device_find_resource(const struct dm_device* device, const void* name, size_t name_size);

/* Validate a nonempty lower-snake-case property or resource identifier. */
bool dm_identifier_valid(const void* name, size_t size);

/* Validate structurally correct, length-delimited UTF-8 without embedded NULs. */
bool dm_utf8_valid(const void* value, size_t size);
