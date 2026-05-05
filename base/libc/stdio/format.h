#pragma once

#include <stdarg.h>

int format_to_display(const char* format, va_list* args);
int format_to_buffer(char* buffer, const char* format, va_list* args);
