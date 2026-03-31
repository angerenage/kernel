#pragma once

#include <stdarg.h>

void format_to_serial(const char* format, va_list* args);
void format_to_buffer(char* buffer, const char* format, va_list* args);
