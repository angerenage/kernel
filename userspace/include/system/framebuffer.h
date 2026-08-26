#pragma once

#include <base/cap.h>
#include <base/framebuffer.h>
#include <base/syscall.h>

/* Query framebuffer geometry, byte size, and pixel format. */
syscall_status_t framebuffer_get_info(cap_id_t framebuffer_cap, struct framebuffer_info_response* out_info);

/* Map the framebuffer read-write into the caller and return its mapping capability and layout. */
syscall_status_t framebuffer_map(cap_id_t framebuffer_cap, struct framebuffer_map_response* out_mapping);
