#pragma once

#include <base/boot_data.h>
#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Query the stable kernel-resource type and total readable byte size. */
syscall_status_t boot_data_get_info(cap_id_t boot_data_cap, struct boot_data_info_response* out_info);

/* Copy an in-bounds resource range into buffer; a zero-sized read permits a null buffer. */
syscall_status_t boot_data_read(cap_id_t boot_data_cap, uint64_t offset, void* buffer, size_t size);
