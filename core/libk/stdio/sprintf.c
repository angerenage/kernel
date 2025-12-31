#include <stdarg.h>
#include <stdio.h>
#include <stdio/utils.h>

void sprintf(char* buffer, const char* __restrict format, ...) {
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
