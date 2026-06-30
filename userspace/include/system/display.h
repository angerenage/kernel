#pragma once

#include <base/cap.h>
#include <base/display.h>
#include <base/syscall.h>
#include <stddef.h>

/* Serial console capability installed from process_startup_info by _start. */
extern cap_id_t serial_cap_id;

/* Write data to the serial console. */
syscall_status_t display_write(const char* data, size_t length);
