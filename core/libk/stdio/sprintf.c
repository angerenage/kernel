#include <stdio.h>

#include "utils.h"

void sprintf(char* buffer, const char* restrict format, ...) {
	if (!buffer) return;

	struct buffer_ctx ctx = {
		.ptr   = buffer,
		.index = 0,
	};

	va_list args;
	va_start(args, format);
	format_emit(buffer_emit_adapter, &ctx, format, &args);
	va_end(args);

	ctx.ptr[ctx.index] = '\0';
}
