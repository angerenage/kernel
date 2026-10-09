#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Client-side handle for one temporary device-construction transaction. */
struct device_builder {
	cap_id_t     capability;
	process_id_t manager_pid;
};

/* Begin a root-device transaction through the manager's private construction capability. */
syscall_status_t device_builder_begin_root(cap_id_t root_cap, struct device_builder* out_builder);

/* Begin a direct-child transaction through a device capability with CAP_MANAGE. */
syscall_status_t device_builder_begin_child(cap_id_t device_cap, struct device_builder* out_builder);

/* Set the builder's optional length-delimited UTF-8 diagnostic name. */
syscall_status_t device_builder_set_name(const struct device_builder* builder, const char* name, size_t name_size);

/* Append one opaque UTF-8 compatible ID in most-specific-first provider order. */
syscall_status_t device_builder_add_compatible(const struct device_builder* builder, const char* id, size_t id_size);

/* Begin one named property with its complete encoded shape and byte size. */
syscall_status_t device_builder_begin_property(const struct device_builder* builder, const char* name, size_t name_size,
                                               enum device_property_type type, uint64_t element_count,
                                               uint64_t value_size);

/* Append the next contiguous encoded-value chunk to the active property. */
syscall_status_t device_builder_append_property(const struct device_builder* builder, uint64_t offset,
                                                const void* bytes, size_t size);

/* Validate and finish the active property after exactly value_size bytes. */
syscall_status_t device_builder_finish_property(const struct device_builder* builder);

/* Delegate a named resource to the manager and record its maximum driver rights. */
syscall_status_t device_builder_add_resource(const struct device_builder* builder, const char* name, size_t name_size,
                                             cap_id_t resource_cap, cap_rights_t rights);

/* Claim one physical MMIO range and attach its device-memory capability as a named resource. */
syscall_status_t device_builder_add_mmio_resource(const struct device_builder* builder, cap_id_t memory_allocator_cap,
                                                  const char* name, size_t name_size, uintptr_t physical_address,
                                                  size_t size);

/* Atomically install the immutable device and invalidate the builder on success. */
syscall_status_t device_builder_commit(struct device_builder* builder);

/* Idempotently abandon a builder and every resource already delegated through it. */
syscall_status_t device_builder_abort(struct device_builder* builder);

/* Read collection counts and the optional diagnostic-name size. */
syscall_status_t device_get_info(cap_id_t device_cap, struct device_info* out_info);

/* Read an exact byte range from the optional diagnostic name. */
syscall_status_t device_name_read(cap_id_t device_cap, uint64_t offset, void* buffer, size_t size);

/* Read the byte size of one provider-ordered compatible ID. */
syscall_status_t device_compatible_info(cap_id_t device_cap, uint64_t index, uint64_t* out_size);

/* Read an exact byte range from one provider-ordered compatible ID. */
syscall_status_t device_compatible_read(cap_id_t device_cap, uint64_t index, uint64_t offset, void* buffer,
                                        size_t size);

/* Read the encoded type and shape of one named property. */
syscall_status_t device_property_get_info(cap_id_t device_cap, const char* name, size_t name_size,
                                          struct device_property_info* out_info);

/* Read an exact encoded byte range from one named property. */
syscall_status_t device_property_read(cap_id_t device_cap, const char* name, size_t name_size, uint64_t offset,
                                      void* buffer, size_t size);

/* Read one scalar boolean property. */
syscall_status_t device_property_read_bool(cap_id_t device_cap, const char* name, bool* out_value);

/* Read one scalar unsigned 64-bit integer property. */
syscall_status_t device_property_read_u64(cap_id_t device_cap, const char* name, uint64_t* out_value);

/* Read one scalar signed 64-bit integer property. */
syscall_status_t device_property_read_i64(cap_id_t device_cap, const char* name, int64_t* out_value);

/* Decode count unsigned integer-array elements starting at first. */
syscall_status_t device_property_read_u64_array(cap_id_t device_cap, const char* name, uint64_t first, uint64_t* values,
                                                size_t count);

/* Decode count signed integer-array elements starting at first. */
syscall_status_t device_property_read_i64_array(cap_id_t device_cap, const char* name, uint64_t first, int64_t* values,
                                                size_t count);

/* Decode count boolean-array elements starting at first. */
syscall_status_t device_property_read_bool_array(cap_id_t device_cap, const char* name, uint64_t first, bool* values,
                                                 size_t count);

/* Read and NUL-terminate a UTF-8 string property, or query its size with a null buffer. */
syscall_status_t device_property_read_string(cap_id_t device_cap, const char* name, char* buffer, size_t capacity,
                                             size_t* out_size);

/* Read and NUL-terminate one UTF-8 string-array element, or query its size with a null buffer. */
syscall_status_t device_property_read_string_at(cap_id_t device_cap, const char* name, uint64_t index, char* buffer,
                                                size_t capacity, size_t* out_size);

/* Read an exact range from an opaque byte property. */
syscall_status_t device_property_read_bytes(cap_id_t device_cap, const char* name, uint64_t offset, void* buffer,
                                            size_t size);

/* Read one resource's name size and maximum acquirable rights by provider-order index. */
syscall_status_t device_resource_info(cap_id_t device_cap, uint64_t index, struct device_resource_info* out_info);

/* Read an exact byte range from one indexed resource name. */
syscall_status_t device_resource_name_read(cap_id_t device_cap, uint64_t index, uint64_t offset, void* buffer,
                                           size_t size);

/* Acquire a named resource capability with a subset of its recorded maximum rights. */
syscall_status_t device_resource_acquire(cap_id_t device_cap, const char* name, size_t name_size, cap_rights_t rights,
                                         cap_id_t* out_cap);
