#pragma once

#include <base/device.h>
#include <stdbool.h>
#include <stddef.h>

/* Register the fixed descriptor size associated with one device type. */
bool kernel_device_register_type(enum kernel_device_type type, size_t element_size);

/* Copy one descriptor into the boot-time inventory for its registered type. */
bool kernel_device_register(enum kernel_device_type type, const void* descriptor, size_t descriptor_size);

/* Make the device inventory immutable. Repeated calls are harmless. */
void kernel_device_freeze(void);

/* Query a registered type's fixed descriptor size. */
bool kernel_device_type_size(enum kernel_device_type type, size_t* out_element_size);

/* Query the current number of descriptors registered for one type. */
bool kernel_device_count(enum kernel_device_type type, size_t* out_count);

/* Copy a type-local range of descriptors from the frozen inventory. */
bool kernel_device_list(enum kernel_device_type type, size_t offset, size_t length, size_t element_size,
                        void* out_entries, size_t* out_returned);

#if defined(KERNEL_DEVICE_TEST)
/* Clear all inventory state between hosted tests. */
void kernel_device_reset_for_test(void);
#endif
