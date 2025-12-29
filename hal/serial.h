#pragma once

#include <stddef.h>

void hal_serial_init(void);
void hal_serial_write_char(char ch);
void hal_serial_write(const char* data, size_t length);
