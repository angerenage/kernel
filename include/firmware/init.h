#pragma once

#include <boot/info.h>
#include <stdbool.h>

/* Initialize firmware parsers from the immutable boot snapshot. */
bool firmware_init(const struct boot_info* info);
