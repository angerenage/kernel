#pragma once

#include <stdarg.h>
#include <stddef.h>

int format_to_display(const char* format, va_list* args);
int format_to_buffer(char* buffer, const char* format, va_list* args);
int format_to_buffer_size(char* buffer, size_t size, const char* format, va_list* args);
