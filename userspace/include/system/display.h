#pragma once

#include <base/cap.h>
#include <base/display.h>
#include <base/syscall.h>
#include <stddef.h>

/* Install the standard Stream capabilities extracted from process_startup_info. */
void display_set_standard_streams(cap_id_t stdin_cap, cap_id_t stdout_cap, cap_id_t stderr_cap);

/* Read from the process standard input Stream. */
syscall_status_t display_read(void* data, size_t capacity, size_t* out_read);

/* Write all data to the process standard output Stream. */
syscall_status_t display_write(const char* data, size_t length);

/* Write all data to the process standard error Stream. */
syscall_status_t display_error_write(const char* data, size_t length);
