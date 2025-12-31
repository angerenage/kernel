#pragma once

#include <stdarg.h>
#include <stddef.h>

struct buffer_ctx {
	char*  ptr;
	size_t index;
};

void serial_out_char(char ch);

void serial_emit_adapter(char ch, void* ctx);
void buffer_emit_adapter(char ch, void* context);

void format_emit(void (*emit)(char, void*), void* ctx, const char* format, va_list* args);
