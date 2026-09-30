#pragma once

#if !defined(__x86_64__)
#error "I/O-port access is only available on x86_64"
#endif

#include <base/io_port.h>
#include <base/syscall.h>
#include <stdint.h>

/* Read the range represented by an I/O-port capability. */
syscall_status_t io_port_info(cap_id_t io_port_cap, struct io_port_info* out_info);

/* Derive a contained child range with reduced rights. */
syscall_status_t io_port_derive(cap_id_t io_port_cap, uint32_t offset, uint32_t count, cap_rights_t rights,
                                cap_id_t* out_io_port_cap);

/* Map the capability's complete range into the calling process. */
syscall_status_t io_port_map(cap_id_t io_port_cap);

/* Unmap the capability's complete range from the calling process. */
syscall_status_t io_port_unmap(cap_id_t io_port_cap);

/* Perform one syscall-backed read without mapping the range. */
syscall_status_t io_port_read(cap_id_t io_port_cap, uint32_t offset, enum io_port_width width, uint32_t* out_value);

/* Perform one syscall-backed write without mapping the range. */
syscall_status_t io_port_write(cap_id_t io_port_cap, uint32_t offset, enum io_port_width width, uint32_t value);
