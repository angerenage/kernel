#pragma once

#include <stddef.h>

/*
 * Low-level display sink supplied by the environment using base.
 *
 * The kernel implementation writes to the serial console. A userspace runtime
 * provides the same symbol using the process stdout.
 */
void base_display_write(const char* data, size_t length);
