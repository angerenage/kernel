#pragma once

#include <base/cap.h>
#include <base/display.h>
#include <stdbool.h>
#include <stddef.h>

/* Serial console capability forwarded into _start's user_arg. */
extern cap_id_t serial_cap_id;

/* Write data to the serial console. */
bool display_write(const char* data, size_t length);
