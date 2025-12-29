#include "utils.h"

#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void serial_out_char(char ch) {
	if (ch == '\n') {
		hal_serial_write_char('\r');
	}
	hal_serial_write_char(ch);
}

void serial_emit_adapter(char ch, void* ctx) {
	(void)ctx;
	serial_out_char(ch);
}

void buffer_emit_adapter(char ch, void* context) {
	struct buffer_ctx* buf = context;
	buf->ptr[buf->index++] = ch;
}

static void emit_string(void (*emit)(char, void*), void* ctx, const char* str) {
	if (!str) {
		str = "(null)";
	}

	while (*str) {
		emit(*str++, ctx);
	}
}

static void emit_padding(void (*emit)(char, void*), void* ctx, int count, char pad) {
	while (count-- > 0) {
		emit(pad, ctx);
	}
}

static void emit_unsigned_number(void (*emit)(char, void*), void* ctx, unsigned long long value, unsigned base,
                                 bool uppercase, int width, char pad) {
	char   buffer[32];
	size_t idx = 0;

	if (value == 0) {
		buffer[idx++] = '0';
	}
	else {
		while (value > 0) {
			unsigned digit = value % base;
			value /= base;
			if (digit < 10) {
				buffer[idx++] = (char)('0' + digit);
			}
			else {
				buffer[idx++] = (char)((uppercase ? 'A' : 'a') + (digit - 10));
			}
		}
	}

	int padding = width - (int)idx;
	if (padding > 0) {
		emit_padding(emit, ctx, padding, pad);
	}

	while (idx > 0) {
		emit(buffer[--idx], ctx);
	}
}

static void emit_signed_number(void (*emit)(char, void*), void* ctx, long long value, int width, bool zero_pad) {
	char               pad       = zero_pad ? '0' : ' ';
	bool               negative  = value < 0;
	unsigned long long magnitude = (unsigned long long)(negative ? -value : value);
	char               buffer[32];
	size_t             idx = 0;

	if (magnitude == 0) {
		buffer[idx++] = '0';
	}
	else {
		while (magnitude > 0) {
			unsigned digit = magnitude % 10u;
			magnitude /= 10u;
			buffer[idx++] = (char)('0' + digit);
		}
	}

	int total_digits = (int)idx + (negative ? 1 : 0);
	int padding      = width - total_digits;

	if (negative && pad == '0') {
		emit('-', ctx);
		negative = false;
	}

	if (padding > 0) {
		emit_padding(emit, ctx, padding, pad);
	}

	if (negative) {
		emit('-', ctx);
	}

	while (idx > 0) {
		emit(buffer[--idx], ctx);
	}
}

static unsigned long long read_unsigned(va_list* args, int length) {
	switch (length) {
	case 2:
		return va_arg(*args, unsigned long long);
	case 1:
		return va_arg(*args, unsigned long);
	default:
		return va_arg(*args, unsigned int);
	}
}

static long long read_signed(va_list* args, int length) {
	switch (length) {
	case 2:
		return va_arg(*args, long long);
	case 1:
		return va_arg(*args, long);
	default:
		return va_arg(*args, int);
	}
}

// Minimal printf-format parser; supports width and zero padding for integers.
void format_emit(void (*emit)(char, void*), void* ctx, const char* format, va_list* args) {
	while (*format) {
		if (*format != '%') {
			emit(*format++, ctx);
			continue;
		}

		format++;
		if (*format == '%') {
			emit('%', ctx);
			format++;
			continue;
		}

		bool zero_pad = false;
		int  width    = 0;

		if (*format == '0') {
			zero_pad = true;
			format++;
		}

		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}

		int  length      = 0;
		bool length_size = false;
		if (*format == 'l') {
			format++;
			length = 1;
			if (*format == 'l') {
				length = 2;
				format++;
			}
		}
		else if (*format == 'z') {
			length_size = true;
			format++;
		}

		char spec = *format ? *format++ : '\0';
		switch (spec) {
		case 'c': {
			char ch = (char)va_arg(*args, int);
			emit(ch, ctx);
			break;
		}
		case 's': {
			const char* str = va_arg(*args, const char*);
			emit_string(emit, ctx, str);
			break;
		}
		case 'd':
		case 'i': {
			long long value;
			if (length_size) {
				ptrdiff_t diff_value = va_arg(*args, ptrdiff_t);
				value                = (long long)diff_value;
			}
			else {
				value = read_signed(args, length);
			}
			emit_signed_number(emit, ctx, value, width, zero_pad);
			break;
		}
		case 'u': {
			unsigned long long value;
			if (length_size) {
				size_t size_value = va_arg(*args, size_t);
				value             = (unsigned long long)size_value;
			}
			else {
				value = read_unsigned(args, length);
			}
			emit_unsigned_number(emit, ctx, value, 10u, false, width, zero_pad ? '0' : ' ');
			break;
		}
		case 'x':
		case 'X': {
			unsigned long long value;
			if (length_size) {
				size_t size_value = va_arg(*args, size_t);
				value             = (unsigned long long)size_value;
			}
			else {
				value = read_unsigned(args, length);
			}
			emit_unsigned_number(emit, ctx, value, 16u, spec == 'X', width, zero_pad ? '0' : ' ');
			break;
		}
		case 'p': {
			uintptr_t value = (uintptr_t)va_arg(*args, void*);
			emit('0', ctx);
			emit('x', ctx);
			emit_unsigned_number(emit, ctx, value, 16u, false, width ? width : (int)(sizeof(uintptr_t) * 2), '0');
			break;
		}
		default:
			emit('%', ctx);
			if (spec) {
				emit(spec, ctx);
			}
			break;
		}
	}
}
