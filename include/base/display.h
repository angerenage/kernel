#pragma once

#include <stddef.h>

/*
 * Low-level display sink supplied by the environment using base.
 *
 * The kernel implementation writes to the serial console. A userspace runtime can
 * provide the same symbol using the temporary debug print syscall.
 */
void base_display_write(const char* data, size_t length);
