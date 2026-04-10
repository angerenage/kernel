#pragma once

#include <stddef.h>

/*
 * Early serial console used for boot logs and panic output.
 */

/* Initialize the boot console if the current platform exposes one. Safe to call more than once. */
void hal_serial_init(void);

/* Transmit one character, lazily initializing the console if needed. */
void hal_serial_write_char(char ch);

/* Transmit an arbitrary byte range in order. */
void hal_serial_write(const char* data, size_t length);
