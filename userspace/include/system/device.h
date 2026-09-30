#pragma once

#include <base/cap.h>
#include <base/device.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Read the number of descriptors registered for one kernel device type. */
syscall_status_t kernel_devices_count(cap_id_t devices_cap, enum kernel_device_type type, uint64_t* out_count);

/* Read at most length type-specific descriptors beginning at offset. */
syscall_status_t kernel_devices_list(cap_id_t devices_cap, enum kernel_device_type type, uint64_t offset, void* entries,
                                     size_t element_size, size_t length, size_t* out_returned);
