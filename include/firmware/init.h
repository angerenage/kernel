#pragma once

#include <stdbool.h>

#include <boot/info.h>

/* Initialize firmware parsers from the immutable boot snapshot. */
bool firmware_init(const struct boot_info* info);
